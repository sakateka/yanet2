package ring

// This service holds one internal lock across every request that reads or
// changes its registry — including admitting or dropping a lease — for
// that request's whole duration. Without it, recreating a ring under a
// name whose deletion is in progress, or admitting a lease against a
// handle whose deletion is in progress, could let a consumer bind by
// handle to a ring the dataplane no longer has.

import (
	"context"
	"errors"
	"fmt"
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

// ringEntry is one registered ring: read-only owner data set once at
// create and never mutated. RingService alone decides, under its own
// lock, when an entry is replaced or removed from its registry.
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

// RingService implements the gRPC service for standalone named ring
// management, and is the owner of every ring it creates: it is the only
// caller of the typed Free, and it hands out the leases a consumer uses to
// bind to a ring by handle.
type RingService struct {
	ringpb.UnimplementedRingServiceServer

	mu       sync.Mutex
	agent    *ffi.Agent
	rings    map[string]*ringEntry
	byHandle map[Handle]*ringEntry
	// leases counts active leases per handle, independent of ringEntry,
	// and only ever holds a positive count: a missing key means zero.
	// Each lease's own release guard runs its decrement at most once, and
	// a delete never removes a handle whose count is still positive, so
	// every decrement always lands on a count a matching increment
	// already raised.
	leases     map[Handle]int
	nextHandle Handle
	// deferred holds superseded ring entries whose free was refused
	// because a live configuration generation still referenced them. This
	// service is their owner: it retries them on its next mutation and
	// nothing else remembers them.
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
		agent:    agent,
		rings:    map[string]*ringEntry{},
		byHandle: map[Handle]*ringEntry{},
		leases:   map[Handle]int{},
		log:      o.Log,
	}
}

// CreateRing creates a new named ring and publishes it to the dataplane.
func (m *RingService) CreateRing(
	ctx context.Context,
	req *ringpb.CreateRingRequest,
) (*ringpb.CreateRingResponse, error) {
	name := req.GetName()
	// Validate already rejected a capacity too large for the uint32 the C
	// API takes.
	capacity := uint32(req.GetCapacity())

	m.mu.Lock()
	defer m.mu.Unlock()

	m.reclaimDeferred()

	if _, exists := m.rings[name]; exists {
		return nil, status.Errorf(codes.AlreadyExists, "ring %q already exists", name)
	}
	if cring.Exists(m.agent, name) {
		return nil, status.Errorf(codes.AlreadyExists, "ring %q already exists", name)
	}

	object, err := cring.NewObject(m.agent, name, capacity)
	if err != nil {
		if errors.Is(err, cerrors.InvalidArgument) {
			return nil, status.Errorf(codes.InvalidArgument, "failed to create ring %q: %v", name, err)
		}
		m.log.Error("failed to create ring object", zap.String("ring", name), zap.Error(err))
		return nil, status.Errorf(codes.Internal, "failed to create ring %q: %v", name, err)
	}

	if err := object.Publish(); err != nil {
		if freeErr := object.Free(); freeErr != nil {
			m.log.Error("failed to free unpublished ring",
				zap.String("ring", name), zap.Error(freeErr))
		}
		m.log.Error("failed to publish ring", zap.String("ring", name), zap.Error(err))
		return nil, status.Errorf(codes.Internal, "failed to publish ring %q: %v", name, err)
	}

	m.nextHandle++
	entry := &ringEntry{Handle: m.nextHandle, Name: name, Object: object}
	m.rings[name] = entry
	m.byHandle[entry.Handle] = entry

	m.log.Info("created ring", zap.String("ring", name), zap.Uint32("capacity", capacity))
	return &ringpb.CreateRingResponse{}, nil
}

// ShowRing returns the name, capacity and worker count of one named ring.
func (m *RingService) ShowRing(
	ctx context.Context,
	req *ringpb.ShowRingRequest,
) (*ringpb.ShowRingResponse, error) {
	m.mu.Lock()
	defer m.mu.Unlock()

	entry, ok := m.rings[req.GetName()]
	if !ok {
		return nil, status.Errorf(codes.NotFound, "ring %q not found", req.GetName())
	}

	return &ringpb.ShowRingResponse{Ring: ringInfo(entry)}, nil
}

// ListRings returns every registered ring with the same facts ShowRing
// would return for each.
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
	return response, nil
}

// ringInfo builds the proto facts for one registered ring.
func ringInfo(entry *ringEntry) *ringpb.RingInfo {
	return &ringpb.RingInfo{
		Name:        entry.Name,
		Capacity:    uint64(entry.Object.Capacity()),
		WorkerCount: uint32(entry.Object.WorkerCount()),
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
	name := req.GetName()

	m.mu.Lock()
	defer m.mu.Unlock()

	m.reclaimDeferred()

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
		if errors.Is(err, ffi.ErrBusy) {
			m.log.Warn("ring deletion refused while linked",
				zap.String("ring", name), zap.Error(err))
			return nil, status.Errorf(
				codes.FailedPrecondition,
				"ring %q is linked by a published module config; update or delete the linking module first: %v",
				name, err,
			)
		}
		m.log.Error("failed to delete ring", zap.String("ring", name), zap.Error(err))
		return nil, status.Errorf(codes.Internal, "failed to delete ring %q: %v", name, err)
	}

	// The delete retired the generation holding the published object; a
	// refusal here joins the deferred list, any other failure is logged
	// and the entry's memory is leaked rather than risk a double free.
	if err := entry.Object.Free(); err != nil {
		if errors.Is(err, ffi.ErrStillReferenced) {
			m.deferred = append(m.deferred, entry)
		} else {
			m.log.Error("failed to free deleted ring", zap.String("ring", name), zap.Error(err))
		}
	}
	delete(m.rings, name)
	delete(m.byHandle, entry.Handle)
	delete(m.leases, entry.Handle)

	m.log.Info("deleted ring", zap.String("ring", name))
	return &ringpb.DeleteRingResponse{}, nil
}

// LookupHandle returns the handle currently registered under name, letting
// a consumer resolve a configured ring name to the handle it binds
// through.
func (m *RingService) LookupHandle(name string) (Handle, bool) {
	m.mu.Lock()
	defer m.mu.Unlock()

	entry, ok := m.rings[name]
	if !ok {
		return 0, false
	}
	return entry.Handle, true
}

// Acquire pins the ring behind handle against deletion and returns a lease
// releasing that pin.
//
// Fails once that handle has been deleted, even if a new ring exists under
// the name the handle used to name: the new ring has its own handle from
// its own CreateRing call, and this call never matches it.
func (m *RingService) Acquire(handle Handle) (*Lease, error) {
	m.mu.Lock()
	defer m.mu.Unlock()

	if _, ok := m.byHandle[handle]; !ok {
		return nil, fmt.Errorf("ring handle %d no longer exists", handle)
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

// ReclaimDeferred retries every deferred ring entry, dropping the ones
// whose generations have drained and keeping the rest deferred. It is the
// reclamation handler for this service's superseded objects; the service
// itself runs it at the start of every mutating RPC, and anything else may
// call it at any time.
func (m *RingService) ReclaimDeferred() {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.reclaimDeferred()
}

// reclaimDeferred is ReclaimDeferred without the lock. The caller must hold
// m.mu.
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
