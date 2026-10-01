package ring_test

import (
	"fmt"
	"net"
	"sync"
	"testing"

	"github.com/c2h5oh/datasize"
	"github.com/stretchr/testify/require"
	"go.uber.org/zap"
	"golang.org/x/sync/errgroup"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"

	dataplaneut "github.com/yanet-platform/yanet2/bindings/go/dataplane_ut"
	"github.com/yanet-platform/yanet2/common/go/xgrpc"
	"github.com/yanet-platform/yanet2/controlplane/ffi"
	"github.com/yanet-platform/yanet2/modules/forward/bindings/go/cforward"
	"github.com/yanet-platform/yanet2/objects/ring/bindings/go/cring"
	ring "github.com/yanet-platform/yanet2/objects/ring/controlplane"
	ringpb "github.com/yanet-platform/yanet2/objects/ring/controlplane/ringpb/v1"
	"github.com/yanet-platform/yanet2/objects/ring/tests/ringtest"
)

// ringFixture is a service over a fresh single-worker harness, reached
// through a real gRPC server that runs the validate interceptor.
type ringFixture struct {
	service *ring.RingService
	client  ringpb.RingServiceClient
	agent   *ffi.Agent
	shm     *ffi.SharedMemory
}

// newRingFixture builds a fixture with the ring object and any extra modules
// loaded; everything is torn down at test end.
func newRingFixture(t *testing.T, extraModules ...string) *ringFixture {
	t.Helper()

	h, err := dataplaneut.NewHarness(dataplaneut.Config{
		CPMemory:      uint64(64 * datasize.MB),
		DPMemory:      uint64(4 * datasize.MB),
		WorkerCount:   1,
		Modules:       extraModules,
		ObjectsToLoad: []string{"ring"},
	})
	require.NoError(t, err)
	t.Cleanup(h.Free)

	agent, err := h.SharedMemory().AgentAttach("ring-service-test", 0, 16*datasize.MB)
	require.NoError(t, err)
	t.Cleanup(func() { _ = agent.CleanUp() })

	service := ring.NewRingService(agent, ring.WithLog(zap.NewNop()))

	listener, err := net.Listen("tcp", "127.0.0.1:0")
	require.NoError(t, err)
	server := grpc.NewServer(grpc.ChainUnaryInterceptor(xgrpc.ValidateUnaryInterceptor()))
	service.Register(server)
	var group errgroup.Group
	group.Go(func() error { return server.Serve(listener) })
	t.Cleanup(func() {
		server.Stop()
		_ = group.Wait()
	})

	conn, err := grpc.NewClient(listener.Addr().String(), grpc.WithTransportCredentials(insecure.NewCredentials()))
	require.NoError(t, err)
	t.Cleanup(func() { _ = conn.Close() })

	return &ringFixture{
		service: service,
		client:  ringpb.NewRingServiceClient(conn),
		agent:   agent,
		shm:     h.SharedMemory(),
	}
}

// create registers a ring and fails the test if the service refuses it.
func (m *ringFixture) create(t *testing.T, name string, capacity uint64) {
	t.Helper()

	_, err := m.client.CreateRing(t.Context(), &ringpb.CreateRingRequest{Name: name, Capacity: capacity})
	require.NoError(t, err)
}

// delete removes a ring and returns the service's error.
func (m *ringFixture) delete(t *testing.T, name string) error {
	_, err := m.client.DeleteRing(t.Context(), &ringpb.DeleteRingRequest{Name: name})
	return err
}

// list returns the listed rings as "name/capacity" strings, in list order.
func (m *ringFixture) list(t *testing.T) []string {
	t.Helper()

	response, err := m.client.ListRings(t.Context(), &ringpb.ListRingsRequest{})
	require.NoError(t, err)
	var rings []string
	for _, info := range response.GetRings() {
		rings = append(rings, fmt.Sprintf("%s/%d", info.GetName(), info.GetCapacity()))
	}
	return rings
}

// requireRingUsable asserts that a record written into the named ring is read
// back intact; it assumes nothing was written to the ring before.
func requireRingUsable(t *testing.T, agent *ffi.Agent, name string) {
	t.Helper()

	writer, err := ringtest.NewPublishedWriter(agent, name, 0)
	require.NoError(t, err)
	payload := []byte("still-usable")
	seqno, err := writer.WriteRecord(payload)
	require.NoError(t, err)

	src, err := writer.Source()
	require.NoError(t, err)
	reader, err := cring.NewReader(0, writer.Capacity(), src)
	require.NoError(t, err)
	records := reader.Read(1024)
	require.Len(t, records, 1)
	require.Equal(t, seqno, records[0].Seqno)
	require.Equal(t, payload, records[0].Bytes)
}

// Test_RingService_CreateRing_ShowAndListSortedByName verifies that every
// created ring is listed once, sorted by name, and shown with its capacity.
//
// Sixteen rings created in reverse order cannot match sorted order by chance.
func Test_RingService_CreateRing_ShowAndListSortedByName(t *testing.T) {
	f := newRingFixture(t)

	const ringCount = 16
	want := make([]string, ringCount)
	for idx := ringCount - 1; idx >= 0; idx-- {
		name := fmt.Sprintf("ring-%02d", idx)
		capacity := uint64(64) << (idx % 4)
		f.create(t, name, capacity)
		want[idx] = fmt.Sprintf("%s/%d", name, capacity)
	}

	require.Equal(t, want, f.list(t))

	show, err := f.client.ShowRing(t.Context(), &ringpb.ShowRingRequest{Name: "ring-03"})
	require.NoError(t, err)
	require.Equal(t, uint64(512), show.GetRing().GetCapacity())
}

// Test_RingService_UnknownName_NotFound verifies that show and delete of an
// unregistered name report NotFound.
func Test_RingService_UnknownName_NotFound(t *testing.T) {
	f := newRingFixture(t)

	_, err := f.client.ShowRing(t.Context(), &ringpb.ShowRingRequest{Name: "missing"})
	require.Equal(t, codes.NotFound, status.Code(err))
	require.Equal(t, codes.NotFound, status.Code(f.delete(t, "missing")))
}

// Test_RingService_CreateRing_RejectedCreateMutatesNothing verifies that a
// refused create reports its code and leaves the registry and dataplane as is.
//
// Requests go to the service in process, so validation must not rely on the
// gRPC interceptor.
func Test_RingService_CreateRing_RejectedCreateMutatesNothing(t *testing.T) {
	f := newRingFixture(t)
	f.create(t, "dup", 64)

	external, err := cring.NewObject(f.agent, "taken", 64)
	require.NoError(t, err)
	require.NoError(t, external.Publish())
	t.Cleanup(func() {
		_ = cring.DeleteObject(f.agent, "taken")
		_ = external.Free()
	})

	cases := []struct {
		name     string
		ringName string
		capacity uint64
		code     codes.Code
	}{
		{name: "invalid request", ringName: "truncated", capacity: 1<<32 | 64, code: codes.InvalidArgument},
		// Valid, but beyond the allocator's largest block in any build.
		{name: "above allocator maximum", ringName: "too-big", capacity: 1 << 27, code: codes.InvalidArgument},
		{name: "registered name", ringName: "dup", capacity: 128, code: codes.AlreadyExists},
		{name: "externally published name", ringName: "taken", capacity: 128, code: codes.AlreadyExists},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := f.service.CreateRing(t.Context(), &ringpb.CreateRingRequest{Name: tc.ringName, Capacity: tc.capacity})
			require.Equal(t, tc.code, status.Code(err))
			require.Equal(t, []string{"dup/64"}, f.list(t))
		})
	}

	writer, err := ringtest.NewPublishedWriter(f.agent, "taken", 0)
	require.NoError(t, err)
	require.Equal(t, external.AsRawPtr(), writer.Object(), "the externally published ring must not be replaced")
}

// Test_RingService_CreateRing_PublishFailureReleasesObject verifies that a
// create whose publish finds no controlplane memory reports Internal,
// registers nothing and returns the object's memory to the agent arena.
func Test_RingService_CreateRing_PublishFailureReleasesObject(t *testing.T) {
	f := newRingFixture(t)

	// Drain the controlplane pool, largest blocks first, so a publish finds
	// no block for its new generation; the service's own arena stays intact.
	filler, err := f.shm.AgentAttach("ring-cp-filler", 0, datasize.B)
	require.NoError(t, err)
	for size := 64 * datasize.MB; size > 0; {
		if filler.Extend(size) != nil {
			size /= 2
		}
	}
	baseline := f.agent.BlockAllocatorFreeSize()

	_, err = f.client.CreateRing(t.Context(), &ringpb.CreateRingRequest{Name: "unpublished", Capacity: 64})
	require.Equal(t, codes.Internal, status.Code(err))
	require.ErrorContains(t, err, "failed to publish ring")
	require.Empty(t, f.list(t))
	require.False(t, cring.Exists(f.agent, "unpublished"))
	require.Equal(t, baseline, f.agent.BlockAllocatorFreeSize())
}

// Test_RingService_DeleteRing_PinnedRingStaysUsable verifies that a ring
// pinned by a lease or a published module link refuses deletion with
// FailedPrecondition, stays registered and usable, and deletes once unpinned.
func Test_RingService_DeleteRing_PinnedRingStaysUsable(t *testing.T) {
	cases := []struct {
		name string
		// pin pins the named ring and returns the function that unpins it.
		pin func(t *testing.T, f *ringFixture, name string) func()
	}{
		{
			name: "lease",
			pin: func(t *testing.T, f *ringFixture, name string) func() {
				handle, ok := f.service.LookupHandle(name)
				require.True(t, ok)
				lease, err := f.service.Acquire(name, handle)
				require.NoError(t, err)
				return lease.Release
			},
		},
		{
			name: "module link",
			pin: func(t *testing.T, f *ringFixture, name string) func() {
				config, err := cforward.NewModuleConfig(f.agent, "ring-linker")
				require.NoError(t, err)
				require.NoError(t, ringtest.LinkRing(config.AsFFIModule().AsRawPtr(), name))
				require.NoError(t, f.agent.UpdateModules([]ffi.ModuleConfig{config.AsFFIModule()}))
				return func() { require.NoError(t, f.agent.DeleteModuleConfig("forward", "ring-linker")) }
			},
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			f := newRingFixture(t, "forward")
			f.create(t, "pinned", 64)
			unpin := tc.pin(t, f, "pinned")

			require.Equal(t, codes.FailedPrecondition, status.Code(f.delete(t, "pinned")))
			require.Equal(t, []string{"pinned/64"}, f.list(t))
			requireRingUsable(t, f.agent, "pinned")

			unpin()
			require.NoError(t, f.delete(t, "pinned"))
			require.False(t, cring.Exists(f.agent, "pinned"))
		})
	}
}

// Test_RingService_DeleteRing_AlreadyUnpublishedDropsEntry verifies that a
// ring the dataplane no longer publishes is still unregistered and freed.
func Test_RingService_DeleteRing_AlreadyUnpublishedDropsEntry(t *testing.T) {
	f := newRingFixture(t)
	baseline := f.agent.BlockAllocatorFreeSize()

	f.create(t, "vanished", 64)
	require.NoError(t, cring.DeleteObject(f.agent, "vanished"))

	require.NoError(t, f.delete(t, "vanished"))
	require.Empty(t, f.list(t))
	require.Equal(t, baseline, f.agent.BlockAllocatorFreeSize())
}

// Test_RingService_DeleteRing_RefusedFreeRetried verifies that a delete whose
// free a live generation reference refuses still unregisters the name.
//
// The name is recreated under a fresh handle while the old memory stays held,
// and the next delete frees it once the reference is released.
func Test_RingService_DeleteRing_RefusedFreeRetried(t *testing.T) {
	f := newRingFixture(t)
	baseline := f.agent.BlockAllocatorFreeSize()

	f.create(t, "deferred", 64)
	oldHandle, ok := f.service.LookupHandle("deferred")
	require.True(t, ok)
	ref, err := ringtest.Hold(f.agent, "deferred")
	require.NoError(t, err)

	require.NoError(t, f.delete(t, "deferred"))
	require.Empty(t, f.list(t))
	require.Less(t, f.agent.BlockAllocatorFreeSize(), baseline, "a referenced ring must stay allocated")

	f.create(t, "deferred", 64)
	newHandle, ok := f.service.LookupHandle("deferred")
	require.True(t, ok)
	require.NotEqual(t, oldHandle, newHandle)
	require.NoError(t, f.delete(t, "deferred"))

	ref.Release()
	f.list(t)
	require.Less(t, f.agent.BlockAllocatorFreeSize(), baseline, "only a delete retries deferred frees")

	f.create(t, "other", 64)
	require.NoError(t, f.delete(t, "other"))
	require.Equal(t, baseline, f.agent.BlockAllocatorFreeSize())
}

// Test_RingService_DeleteThenRecreate_NewHandle verifies that a ring
// recreated under a deleted name gets a fresh handle, and that the old handle
// never admits a lease against it.
func Test_RingService_DeleteThenRecreate_NewHandle(t *testing.T) {
	f := newRingFixture(t)

	f.create(t, "recreate", 64)
	oldHandle, ok := f.service.LookupHandle("recreate")
	require.True(t, ok)
	require.NoError(t, f.delete(t, "recreate"))

	f.create(t, "recreate", 64)
	newHandle, ok := f.service.LookupHandle("recreate")
	require.True(t, ok)
	require.NotEqual(t, oldHandle, newHandle)

	_, err := f.service.Acquire("recreate", oldHandle)
	require.Error(t, err)
}

// Test_RingService_LeaseVsDeleteRace verifies under concurrent acquires that
// a delete never succeeds while a lease is admitted, and that a deleted
// handle never admits a lease again.
func Test_RingService_LeaseVsDeleteRace(t *testing.T) {
	f := newRingFixture(t)
	f.create(t, "race", 64)
	handle, ok := f.service.LookupHandle("race")
	require.True(t, ok)

	var mu sync.Mutex
	var leases []*ring.Lease
	var group errgroup.Group
	for range 64 {
		group.Go(func() error {
			if lease, err := f.service.Acquire("race", handle); err == nil {
				mu.Lock()
				leases = append(leases, lease)
				mu.Unlock()
			}
			return nil
		})
	}
	var raceErr error
	group.Go(func() error {
		raceErr = f.delete(t, "race")
		return nil
	})
	require.NoError(t, group.Wait())

	for _, lease := range leases {
		lease.Release()
	}

	if raceErr == nil {
		require.Empty(t, leases, "a delete must never succeed while a lease is admitted")
	} else {
		require.Equal(t, codes.FailedPrecondition, status.Code(raceErr))
		require.NoError(t, f.delete(t, "race"))
	}

	_, err := f.service.Acquire("race", handle)
	require.Error(t, err, "a deleted handle must never admit a lease")
}
