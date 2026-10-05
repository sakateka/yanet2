package pdump

import (
	"context"
	"errors"
	"sync"

	"go.uber.org/zap"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	"github.com/yanet-platform/yanet2/modules/pdump/controlplane/pdumppb/v1"
)

// recordBufferSize bounds how far a stream's readers run ahead of its
// client.
const recordBufferSize = 16

// errRetired is reported by a capture that was retired before a stream
// started, so the caller can look up the capture that replaced it.
var errRetired = errors.New("capture retired")

// binding owns one lease on a ring together with the gate that admits and
// drains the streams reading it.
//
// Configs sharing a ring get separate bindings and gates: reuse happens
// only across a same-object update of one config, never between two
// configs that merely name the same ring. There, the predecessor's
// binding is shared across the old and new capture entries, so an
// in-flight stream keeps reading the same sources with no reset. Retain
// and release track how many entries still reference it.
type binding struct {
	ringName string
	handle   RingHandle
	lease    RingLease
	readers  *gate

	mu   sync.Mutex
	refs int
	log  *zap.Logger
}

// bindingOptions configures a binding.
type bindingOptions struct {
	Log *zap.Logger
}

func newBindingOptions() *bindingOptions {
	return &bindingOptions{
		Log: zap.NewNop(),
	}
}

// bindingOption configures a binding.
type bindingOption func(*bindingOptions)

// withBindingLog sets the binding's logger.
func withBindingLog(log *zap.Logger) bindingOption {
	return func(o *bindingOptions) {
		o.Log = log
	}
}

// newBinding wraps a freshly acquired lease in a binding with one
// reference.
func newBinding(ringName string, lease RingLease, opts ...bindingOption) *binding {
	o := newBindingOptions()
	for _, opt := range opts {
		opt(o)
	}

	return &binding{
		ringName: ringName,
		handle:   lease.Handle(),
		lease:    lease,
		readers:  newGate(),
		refs:     1,
		log:      o.Log,
	}
}

// Handle returns the handle of the ring this binding pins.
func (m *binding) Handle() RingHandle {
	return m.handle
}

// MaxRecordLen returns the largest record the bound ring accepts, for a
// fit check against a candidate snaplen.
func (m *binding) MaxRecordLen() uint32 {
	return m.lease.MaxRecordLen()
}

// Retain adds one more reference, for a capture entry that reuses this
// binding across a same-object update instead of acquiring its own lease.
func (m *binding) Retain() {
	m.mu.Lock()
	m.refs++
	m.mu.Unlock()
}

// Release drops one reference.
//
// The last reference closes admission to the gate, cancels and drains the
// readers it admitted, then releases the lease. The caller must not hold
// a mutex the ring owner also takes: the drain waits for readers that may
// be mid-read.
func (m *binding) Release() {
	m.mu.Lock()
	m.refs--
	last := m.refs == 0
	m.mu.Unlock()
	if !last {
		return
	}

	m.readers.Close()
	m.lease.Release()
}

// Read runs a read session over the bound ring inside the gate, so a
// retired binding is never read.
//
// It reports whether the session ran at all, and an error finding the
// ring's sources or reading them; the read loop's own cancellation is
// not reported here, since the caller already observes it through ctx.
func (m *binding) Read(ctx context.Context, recordCh chan<- *pdumppb.Record) (ran bool, sourceErr error) {
	ran = m.readers.Run(ctx, func(ctx context.Context) {
		sources, err := m.lease.Sources()
		if err != nil {
			sourceErr = status.Errorf(codes.Internal, "ring %q is not readable: %v", m.ringName, err)
			return
		}

		m.log.Info("start ring readers", zap.Int("count", len(sources)))
		err = runReaders(ctx, sources, m.lease.Capacity(), m.log, recordCh)
		m.log.Info("ring readers stopped", zap.Error(err))
		if err != nil && !errors.Is(err, context.Canceled) && !errors.Is(err, context.DeadlineExceeded) {
			sourceErr = status.Errorf(codes.Internal, "ring %q reader failed: %v", m.ringName, err)
		}
	})
	return ran, sourceErr
}

// capture is the configuration published under one name together with the
// binding its streams read.
//
// A same-object update shares its predecessor's binding, so Free must drop
// this entry's own reference exactly once no matter how many times the
// store retries it while the module free it also runs stays refused.
type capture struct {
	settings Settings
	module   Module
	binding  *binding

	release sync.Once
}

// Settings returns the capture parameters the config was published with.
func (m *capture) Settings() Settings {
	return m.settings
}

// Binding returns the binding this capture's streams read.
func (m *capture) Binding() *binding {
	return m.binding
}

// Read streams the captured records until the stream ends, the client
// cannot take them any more or the capture is retired.
//
// A capture retired before the stream started reports errRetired and reads
// nothing. A capture retired by a same-object update never closes the
// binding's gate, so a stream already running when that happens keeps
// reading the same ring sources, with no reset of positions or sequence
// numbers.
func (m *capture) Read(ctx context.Context, send func(*pdumppb.Record) error) error {
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()

	records := make(chan *pdumppb.Record, recordBufferSize)
	result := make(chan error, 1)
	go func() {
		defer close(records)

		// The ring sources are taken inside the binding's gate, so a
		// retired binding is never read at all.
		ran, readErr := m.binding.Read(ctx, records)
		if !ran {
			readErr = errRetired
		}
		result <- readErr
	}()

	// Sending outside the gate keeps a slow client from delaying a retire.
	for record := range records {
		if err := send(record); err != nil {
			cancel()
			for range records {
			}
			return err
		}
	}

	return <-result
}

// Free drops this entry's reference to its binding exactly once, then
// frees the module.
//
// The module free alone is retryable: the store calls Free again while it
// is refused, and that must not drop the binding reference a second time.
func (m *capture) Free() error {
	m.release.Do(m.binding.Release)
	return m.module.Free()
}

// gate admits work until it closes, and close waits for the work it
// admitted.
type gate struct {
	mu     sync.Mutex
	closed bool
	work   sync.WaitGroup
	ctx    context.Context
	cancel context.CancelFunc
}

func newGate() *gate {
	ctx, cancel := context.WithCancel(context.Background())
	return &gate{ctx: ctx, cancel: cancel}
}

// Run runs work unless the gate is closed, cancelling its context once the
// gate closes.
//
// It reports whether the work ran at all.
func (m *gate) Run(ctx context.Context, work func(context.Context)) bool {
	m.mu.Lock()
	if m.closed {
		m.mu.Unlock()
		return false
	}
	m.work.Add(1)
	m.mu.Unlock()
	defer m.work.Done()

	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	defer context.AfterFunc(m.ctx, cancel)()

	work(ctx)
	return true
}

// Close refuses further work, cancels the running work and waits for it.
//
// Calling it again returns at once.
func (m *gate) Close() {
	m.mu.Lock()
	m.closed = true
	m.mu.Unlock()

	m.cancel()
	m.work.Wait()
}
