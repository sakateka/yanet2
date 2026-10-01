package ring

// One internal lock covers every request that reads or changes the
// registry, including admitting or dropping a lease, for its whole duration.
//
// A ring recreated under a name whose deletion is in progress, or a lease
// admitted against a handle being deleted, could otherwise let a consumer
// bind by handle to a ring the dataplane no longer has.

import (
	"context"
	"errors"
	"fmt"
	"slices"
	"strings"
	"sync"

	"go.uber.org/zap"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	"github.com/yanet-platform/yanet2/bindings/go/cerrors"
	"github.com/yanet-platform/yanet2/controlplane/ffi"
	"github.com/yanet-platform/yanet2/objects/ring/bindings/go/cring"
	ringpb "github.com/yanet-platform/yanet2/objects/ring/controlplane/ringpb/v1"
)

// Handle identifies one ring across its create-to-delete lifetime.
//
// A ring recreated under the same name after a delete gets a fresh handle,
// so a lease acquired against the old handle never matches the new ring.
type Handle uint64

// Lease pins one ring handle against deletion until Release.
//
// Acquired from RingService.Acquire; Release is the only way to drop the
// pin, is idempotent, and only ever affects the handle it was acquired
// for.
type Lease struct {
	handle  Handle
	once    sync.Once
	release func()
}

// Handle returns the handle this lease pins.
func (m *Lease) Handle() Handle {
	return m.handle
}

// Release drops this lease's pin. Safe to call more than once.
func (m *Lease) Release() {
	m.once.Do(m.release)
}

// ringEntry is one registered ring: owner data set once at create and never
// mutated.
//
// Only the service decides, under its lock, when an entry is replaced or
// removed from the registry.
type ringEntry struct {
	Handle Handle
	Name   string
	Object *cring.Object
}

// Option configures a RingService.
type Option func(*options)

type options struct {
	Log *zap.Logger
}

func newOptions() *options {
	return &options{
		Log: zap.NewNop(),
	}
}

// WithLog sets the ring service logger.
func WithLog(log *zap.Logger) Option {
	return func(o *options) {
		o.Log = log
	}
}

// RingService implements the gRPC service for standalone named rings and
// owns every ring it creates.
//
// It is the only caller of a ring's free, and it hands out the leases a
// consumer uses to bind to a ring by handle.
type RingService struct {
	ringpb.UnimplementedRingServiceServer

	mu    sync.Mutex
	agent *ffi.Agent
	rings map[string]*ringEntry
	// leases counts active leases per handle and holds only positive
	// counts: a missing key means zero.
	//
	// Each lease decrements at most once, and a delete never removes a
	// handle whose count is positive, so every decrement lands on a count
	// a matching increment raised.
	leases     map[Handle]int
	nextHandle Handle
	// deferred holds deleted entries whose free was refused because a live
	// configuration generation still referenced them.
	//
	// Nothing else remembers them; the service retries them on every delete
	// and on explicit reclamation.
	deferred []*ringEntry
	log      *zap.Logger
}

// NewRingService creates a new RingService.
func NewRingService(agent *ffi.Agent, opts ...Option) *RingService {
	o := newOptions()
	for _, opt := range opts {
		opt(o)
	}

	return &RingService{
		agent:  agent,
		rings:  map[string]*ringEntry{},
		leases: map[Handle]int{},
		log:    o.Log,
	}
}

// CreateRing creates a new named ring and publishes it to the dataplane.
func (m *RingService) CreateRing(
	ctx context.Context,
	req *ringpb.CreateRingRequest,
) (*ringpb.CreateRingResponse, error) {
	// Validated here as well as by the gateway, so an in-process caller
	// cannot slip a capacity past the 32-bit truncation below.
	if err := req.Validate(); err != nil {
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}
	name := req.GetName()
	capacity := uint32(req.GetCapacity())
	publishBatch := req.PublishBatchOrDefault()

	m.mu.Lock()
	defer m.mu.Unlock()

	if _, exists := m.rings[name]; exists {
		return nil, status.Errorf(codes.AlreadyExists, "ring %q already exists", name)
	}
	if cring.Exists(m.agent, name) {
		return nil, status.Errorf(codes.AlreadyExists, "ring %q already exists", name)
	}

	object, err := cring.NewObject(m.agent, name, capacity, publishBatch)
	if err != nil {
		if errors.Is(err, cerrors.InvalidArgument) {
			return nil, status.Errorf(codes.InvalidArgument, "failed to create ring %q: %v", name, err)
		}
		m.log.Error("failed to create ring object", zap.String("ring", name), zap.Error(err))
		return nil, status.Errorf(codes.Internal, "failed to create ring %q: %v", name, err)
	}

	if err := object.Publish(); err != nil {
		if err := object.Free(); err != nil {
			m.log.Error("failed to free unpublished ring", zap.String("ring", name), zap.Error(err))
		}
		m.log.Error("failed to publish ring", zap.String("ring", name), zap.Error(err))
		return nil, status.Errorf(codes.Internal, "failed to publish ring %q: %v", name, err)
	}

	m.nextHandle++
	entry := &ringEntry{Handle: m.nextHandle, Name: name, Object: object}
	m.rings[name] = entry

	m.log.Info("created ring",
		zap.String("ring", name),
		zap.Uint32("capacity", capacity),
		zap.Uint32("publish_batch", publishBatch),
	)
	return &ringpb.CreateRingResponse{}, nil
}

// ShowRing returns the name, per-worker capacity and publish batch of one
// named ring.
func (m *RingService) ShowRing(
	ctx context.Context,
	req *ringpb.ShowRingRequest,
) (*ringpb.ShowRingResponse, error) {
	if err := req.Validate(); err != nil {
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}

	m.mu.Lock()
	defer m.mu.Unlock()

	entry, ok := m.rings[req.GetName()]
	if !ok {
		return nil, status.Errorf(codes.NotFound, "ring %q not found", req.GetName())
	}

	return &ringpb.ShowRingResponse{Ring: ringInfo(entry)}, nil
}

// ListRings returns every registered ring, sorted by name, with the same
// facts ShowRing returns for each.
func (m *RingService) ListRings(
	ctx context.Context,
	req *ringpb.ListRingsRequest,
) (*ringpb.ListRingsResponse, error) {
	m.mu.Lock()
	defer m.mu.Unlock()

	response := &ringpb.ListRingsResponse{
		Rings: make([]*ringpb.RingInfo, 0, len(m.rings)),
	}
	for _, entry := range m.rings {
		response.Rings = append(response.Rings, ringInfo(entry))
	}
	slices.SortFunc(response.Rings, func(a, b *ringpb.RingInfo) int {
		return strings.Compare(a.GetName(), b.GetName())
	})
	return response, nil
}

// ringInfo builds the proto facts for one registered ring.
func ringInfo(entry *ringEntry) *ringpb.RingInfo {
	return &ringpb.RingInfo{
		Name:         entry.Name,
		Capacity:     uint64(entry.Object.Capacity()),
		PublishBatch: entry.Object.PublishBatch(),
	}
}

// DeleteRing removes a named ring.
//
// Refused while a lease from Acquire pins the ring, or while a published
// module config links it by name: either way the ring stays usable and
// registered, and the caller releases the lease or updates the linking
// module before retrying.
func (m *RingService) DeleteRing(
	ctx context.Context,
	req *ringpb.DeleteRingRequest,
) (*ringpb.DeleteRingResponse, error) {
	if err := req.Validate(); err != nil {
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}
	name := req.GetName()

	m.mu.Lock()
	defer m.mu.Unlock()

	entry, ok := m.rings[name]
	if !ok {
		return nil, status.Errorf(codes.NotFound, "ring %q not found", name)
	}

	if leaseCount := m.leases[entry.Handle]; leaseCount > 0 {
		return nil, status.Errorf(
			codes.FailedPrecondition,
			"ring %q is pinned by %d active lease(s)",
			name, leaseCount,
		)
	}

	if err := cring.DeleteObject(m.agent, name); err != nil {
		switch {
		case errors.Is(err, ffi.ErrBusy):
			m.log.Warn("ring deletion refused while linked",
				zap.String("ring", name), zap.Error(err))
			return nil, status.Errorf(
				codes.FailedPrecondition,
				"ring %q is linked by a published module config; update or delete the linking module first: %v",
				name, err,
			)
		case errors.Is(err, ffi.ErrNotFound) || !cring.Exists(m.agent, name):
			// Nothing is left to unpublish, and dropping the entry
			// below is the only way it ever leaves the registry.
			m.log.Warn("ring already absent from the dataplane; dropping it",
				zap.String("ring", name), zap.Error(err))
		default:
			// Still published: keep the entry so a retry can finish
			// the delete.
			m.log.Error("failed to delete ring", zap.String("ring", name), zap.Error(err))
			return nil, status.Errorf(codes.Internal, "failed to delete ring %q: %v", name, err)
		}
	}

	// The delete retired the generation holding the published object;
	// retry the deferred ones, then retire this one.
	m.reclaimDeferred()
	m.freeOrDefer(entry)
	delete(m.rings, name)
	delete(m.leases, entry.Handle)

	m.log.Info("deleted ring", zap.String("ring", name))
	return &ringpb.DeleteRingResponse{}, nil
}

// LookupHandle returns the handle registered under a name, letting a
// consumer resolve a configured ring name to the handle it binds through.
func (m *RingService) LookupHandle(name string) (Handle, bool) {
	m.mu.Lock()
	defer m.mu.Unlock()

	entry, ok := m.rings[name]
	if !ok {
		return 0, false
	}
	return entry.Handle, true
}

// Acquire pins the named ring against deletion, provided it is still the
// ring behind handle, and returns a lease releasing that pin.
//
// Fails once that ring has been deleted, even if a new ring exists under the
// same name: the new ring has its own handle from its own create.
func (m *RingService) Acquire(name string, handle Handle) (*Lease, error) {
	m.mu.Lock()
	defer m.mu.Unlock()

	if entry, ok := m.rings[name]; !ok || entry.Handle != handle {
		return nil, fmt.Errorf("ring %q handle %d no longer exists", name, handle)
	}

	m.leases[handle]++
	return &Lease{
		handle: handle,
		release: func() {
			m.mu.Lock()
			defer m.mu.Unlock()
			if m.leases[handle] <= 1 {
				delete(m.leases, handle)
			} else {
				m.leases[handle]--
			}
		},
	}, nil
}

// freeOrDefer frees an entry's object once nothing publishes it any more.
//
// A refusal while a live generation still references it defers the entry;
// any other failure is logged and the memory leaked to avoid a double free.
// The caller holds the service lock.
func (m *RingService) freeOrDefer(entry *ringEntry) {
	if err := entry.Object.Free(); err != nil {
		if errors.Is(err, ffi.ErrStillReferenced) {
			m.deferred = append(m.deferred, entry)
			return
		}
		m.log.Error("failed to free ring", zap.String("ring", entry.Name), zap.Error(err))
	}
}

// ReclaimDeferred retries every deferred ring, dropping the ones whose
// generations have drained and keeping the rest.
//
// The service runs it on every delete; anything else may call it at any
// time.
func (m *RingService) ReclaimDeferred() {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.reclaimDeferred()
}

// reclaimDeferred is ReclaimDeferred without the lock; the caller holds the
// service lock.
func (m *RingService) reclaimDeferred() {
	kept := m.deferred[:0]
	for _, entry := range m.deferred {
		if err := entry.Object.Free(); err != nil {
			if errors.Is(err, ffi.ErrStillReferenced) {
				kept = append(kept, entry)
			} else {
				m.log.Error("failed to free deferred ring", zap.String("ring", entry.Name), zap.Error(err))
			}
		}
	}
	clear(m.deferred[len(kept):])
	m.deferred = kept
}
