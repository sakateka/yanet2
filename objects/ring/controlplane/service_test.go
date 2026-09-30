package ring_test

import (
	"net"
	"strings"
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
	ring "github.com/yanet-platform/yanet2/objects/ring/controlplane"
	ringpb "github.com/yanet-platform/yanet2/objects/ring/controlplane/ringpb/v1"
	"github.com/yanet-platform/yanet2/objects/ring/internal/ringlink"
	"github.com/yanet-platform/yanet2/objects/ring/internal/ringref"
)

// newTestAgent builds a throwaway dataplane_ut harness with the ring object
// loaded, plus any extraModules, and one attached agent, both torn down at
// test end.
func newTestAgent(t *testing.T, workerCount uint64, extraModules ...string) *ffi.Agent {
	t.Helper()

	h, err := dataplaneut.NewHarness(dataplaneut.Config{
		CPMemory:      uint64(64 * datasize.MB),
		DPMemory:      uint64(4 * datasize.MB),
		WorkerCount:   workerCount,
		Modules:       extraModules,
		ObjectsToLoad: []string{"ring"},
	})
	require.NoError(t, err)
	t.Cleanup(h.Free)

	agent, err := h.SharedMemory().AgentAttach("ring-service-test", 0, 16*datasize.MB)
	require.NoError(t, err)
	t.Cleanup(func() { _ = agent.CleanUp() })

	return agent
}

// startRingService hosts service behind a real gRPC server chained with
// the validate interceptor the module runner applies to every module, so a
// validation-row test observes InvalidArgument from the real request path.
func startRingService(t *testing.T, service *ring.RingService) ringpb.RingServiceClient {
	t.Helper()

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

	return ringpb.NewRingServiceClient(conn)
}

// newRingService builds a RingService over a throwaway agent and exposes it
// behind a real gRPC client, for a test that needs both the client surface
// and direct access to the owner's handle/lease API.
func newRingService(t *testing.T, workerCount uint64, extraModules ...string) (*ring.RingService, ringpb.RingServiceClient) {
	t.Helper()

	agent := newTestAgent(t, workerCount, extraModules...)
	service := ring.NewRingService(agent, ring.WithLog(zap.NewNop()))
	return service, startRingService(t, service)
}

// Test_RingService_CreateShowList verifies that a created ring is reported
// by both ShowRing and ListRings with the same name, capacity and worker
// count.
func Test_RingService_CreateShowList(t *testing.T) {
	_, client := newRingService(t, 2)
	ctx := t.Context()

	_, err := client.CreateRing(ctx, &ringpb.CreateRingRequest{Name: "ring0", Capacity: 64})
	require.NoError(t, err)

	show, err := client.ShowRing(ctx, &ringpb.ShowRingRequest{Name: "ring0"})
	require.NoError(t, err)
	require.Equal(t, "ring0", show.GetRing().GetName())
	require.Equal(t, uint64(64), show.GetRing().GetCapacity())
	require.Equal(t, uint32(2), show.GetRing().GetWorkerCount())

	list, err := client.ListRings(ctx, &ringpb.ListRingsRequest{})
	require.NoError(t, err)
	require.Len(t, list.GetRings(), 1)
	require.Equal(t, show.GetRing(), list.GetRings()[0])
}

// Test_RingService_ShowRing_UnknownNameNotFound verifies that describing an
// unregistered name reports NotFound.
func Test_RingService_ShowRing_UnknownNameNotFound(t *testing.T) {
	_, client := newRingService(t, 1)

	_, err := client.ShowRing(t.Context(), &ringpb.ShowRingRequest{Name: "missing"})
	require.Equal(t, codes.NotFound, status.Code(err))
}

// Test_RingService_DeleteRing_UnknownNameNotFound verifies that deleting an
// unregistered name reports NotFound.
func Test_RingService_DeleteRing_UnknownNameNotFound(t *testing.T) {
	_, client := newRingService(t, 1)

	_, err := client.DeleteRing(t.Context(), &ringpb.DeleteRingRequest{Name: "missing"})
	require.Equal(t, codes.NotFound, status.Code(err))
}

// Test_RingService_CreateRing_ValidationRejectsInvalidRequests verifies
// that a request the proto's own Validate rejects reaches the handler as
// InvalidArgument through the real gRPC and interceptor path, and creates
// nothing.
func Test_RingService_CreateRing_ValidationRejectsInvalidRequests(t *testing.T) {
	_, client := newRingService(t, 1)
	ctx := t.Context()

	cases := []struct {
		name    string
		request *ringpb.CreateRingRequest
	}{
		{name: "empty name", request: &ringpb.CreateRingRequest{Capacity: 64}},
		{
			name: "overlong name",
			request: &ringpb.CreateRingRequest{
				Name:     strings.Repeat("a", ringpb.MaxRingNameLen),
				Capacity: 64,
			},
		},
		{
			name:    "capacity not a power of two",
			request: &ringpb.CreateRingRequest{Name: "bad", Capacity: 24},
		},
		{
			name:    "capacity below the record frame size",
			request: &ringpb.CreateRingRequest{Name: "bad", Capacity: 4},
		},
		{
			name:    "capacity beyond the uint32 range",
			request: &ringpb.CreateRingRequest{Name: "bad", Capacity: 1 << 32},
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := client.CreateRing(ctx, tc.request)
			require.Equal(t, codes.InvalidArgument, status.Code(err))

			list, err := client.ListRings(ctx, &ringpb.ListRingsRequest{})
			require.NoError(t, err)
			require.Empty(t, list.GetRings(), "a rejected create must mutate nothing")
		})
	}
}

// Test_RingService_CreateRing_AboveMaxCapacityRejected verifies that a
// capacity the proto's Validate lets through — a power of two within the
// uint32 range — but the C allocator refuses as too large is reported as
// InvalidArgument from the real create path, and creates nothing.
func Test_RingService_CreateRing_AboveMaxCapacityRejected(t *testing.T) {
	_, client := newRingService(t, 1)
	ctx := t.Context()

	// 1<<27 exceeds MEMORY_BLOCK_ALLOCATOR_MAX_SIZE (1<<26, less an ASan
	// red zone) regardless of build type, while itself staying a valid
	// power of two well inside the uint32 range.
	_, err := client.CreateRing(ctx, &ringpb.CreateRingRequest{Name: "too-big", Capacity: 1 << 27})
	require.Equal(t, codes.InvalidArgument, status.Code(err))

	list, err := client.ListRings(ctx, &ringpb.ListRingsRequest{})
	require.NoError(t, err)
	require.Empty(t, list.GetRings())
}

// Test_RingService_CreateRing_DuplicateNameRejected verifies that creating
// a second ring under a name already registered reports AlreadyExists and
// leaves the original the only entry.
func Test_RingService_CreateRing_DuplicateNameRejected(t *testing.T) {
	_, client := newRingService(t, 1)
	ctx := t.Context()

	_, err := client.CreateRing(ctx, &ringpb.CreateRingRequest{Name: "dup", Capacity: 64})
	require.NoError(t, err)

	_, err = client.CreateRing(ctx, &ringpb.CreateRingRequest{Name: "dup", Capacity: 128})
	require.Equal(t, codes.AlreadyExists, status.Code(err))

	list, err := client.ListRings(ctx, &ringpb.ListRingsRequest{})
	require.NoError(t, err)
	require.Len(t, list.GetRings(), 1, "the rejected duplicate must not add a second entry")
	require.Equal(t, uint64(64), list.GetRings()[0].GetCapacity(), "the original ring must be unchanged")
}

// Test_RingService_DeleteRing_LeasedRingStaysUsable verifies that deleting
// a ring pinned by an active lease is refused with FailedPrecondition,
// that the ring stays fully usable meanwhile, and that releasing the lease
// lets the delete succeed.
func Test_RingService_DeleteRing_LeasedRingStaysUsable(t *testing.T) {
	service, client := newRingService(t, 1)
	ctx := t.Context()

	_, err := client.CreateRing(ctx, &ringpb.CreateRingRequest{Name: "leased", Capacity: 64})
	require.NoError(t, err)

	handle, ok := service.LookupHandle("leased")
	require.True(t, ok)
	lease, err := service.Acquire(handle)
	require.NoError(t, err)

	_, err = client.DeleteRing(ctx, &ringpb.DeleteRingRequest{Name: "leased"})
	require.Equal(t, codes.FailedPrecondition, status.Code(err))

	show, err := client.ShowRing(ctx, &ringpb.ShowRingRequest{Name: "leased"})
	require.NoError(t, err)
	require.Equal(t, "leased", show.GetRing().GetName())

	lease.Release()

	_, err = client.DeleteRing(ctx, &ringpb.DeleteRingRequest{Name: "leased"})
	require.NoError(t, err)
}

// Test_RingService_DeleteRing_RefusedWhileLinked verifies that a published
// module's link to a ring refuses its deletion with FailedPrecondition,
// mapping the dataplane's link refusal end to end, and that the ring stays
// registered afterward.
func Test_RingService_DeleteRing_RefusedWhileLinked(t *testing.T) {
	agent := newTestAgent(t, 1, "forward")
	service := ring.NewRingService(agent, ring.WithLog(zap.NewNop()))
	client := startRingService(t, service)
	ctx := t.Context()

	_, err := client.CreateRing(ctx, &ringpb.CreateRingRequest{Name: "linked", Capacity: 64})
	require.NoError(t, err)

	moduleConfig, err := cforward.NewModuleConfig(agent, "ring-linker")
	require.NoError(t, err)
	require.NoError(t, ringlink.LinkRing(moduleConfig.AsFFIModule().AsRawPtr(), "linked"))
	require.NoError(t, agent.UpdateModules([]ffi.ModuleConfig{moduleConfig.AsFFIModule()}))

	_, err = client.DeleteRing(ctx, &ringpb.DeleteRingRequest{Name: "linked"})
	require.Equal(t, codes.FailedPrecondition, status.Code(err))

	show, err := client.ShowRing(ctx, &ringpb.ShowRingRequest{Name: "linked"})
	require.NoError(t, err)
	require.Equal(t, "linked", show.GetRing().GetName())
}

// Test_RingService_DeleteRing_RefusedFreeRetried verifies that a delete
// whose typed free is refused by a live reference still reports success
// and unregisters the name, that the ring's memory stays held while the
// reference remains, and that a later mutation retries and completes the
// deferred free once the reference releases.
func Test_RingService_DeleteRing_RefusedFreeRetried(t *testing.T) {
	agent := newTestAgent(t, 1)
	service := ring.NewRingService(agent, ring.WithLog(zap.NewNop()))
	client := startRingService(t, service)
	ctx := t.Context()

	baseline := agent.BlockAllocatorFreeSize()

	_, err := client.CreateRing(ctx, &ringpb.CreateRingRequest{Name: "deferred", Capacity: 64})
	require.NoError(t, err)

	ref, err := ringref.Hold(agent, "deferred")
	require.NoError(t, err)

	_, err = client.DeleteRing(ctx, &ringpb.DeleteRingRequest{Name: "deferred"})
	require.NoError(t, err)

	_, err = client.ShowRing(ctx, &ringpb.ShowRingRequest{Name: "deferred"})
	require.Equal(t, codes.NotFound, status.Code(err), "a deleted ring is unregistered even while its free is deferred")

	require.Less(t, agent.BlockAllocatorFreeSize(), baseline,
		"the deferred ring's memory must still be held while a live reference remains")

	ref.Release()

	// A later mutation retries every deferred free; creating and deleting
	// something else is enough to trigger and observe it.
	_, err = client.CreateRing(ctx, &ringpb.CreateRingRequest{Name: "other", Capacity: 64})
	require.NoError(t, err)
	_, err = client.DeleteRing(ctx, &ringpb.DeleteRingRequest{Name: "other"})
	require.NoError(t, err)

	require.Equal(t, baseline, agent.BlockAllocatorFreeSize(),
		"the deferred ring's memory must be reclaimed once its last reference releases")
}

// Test_RingService_DeleteThenRecreate_NewHandle verifies that recreating a
// ring under a deleted name yields a fresh handle: the old handle can
// never be acquired again, and a stale release from before the delete does
// not unpin the new ring.
func Test_RingService_DeleteThenRecreate_NewHandle(t *testing.T) {
	service, client := newRingService(t, 1)
	ctx := t.Context()

	_, err := client.CreateRing(ctx, &ringpb.CreateRingRequest{Name: "recreate", Capacity: 64})
	require.NoError(t, err)

	oldHandle, ok := service.LookupHandle("recreate")
	require.True(t, ok)

	// Acquire and release before deleting, so the stale lease below has
	// something to release once the handle it names is long gone.
	oldLease, err := service.Acquire(oldHandle)
	require.NoError(t, err)
	oldLease.Release()

	_, err = client.DeleteRing(ctx, &ringpb.DeleteRingRequest{Name: "recreate"})
	require.NoError(t, err)

	_, err = client.CreateRing(ctx, &ringpb.CreateRingRequest{Name: "recreate", Capacity: 128})
	require.NoError(t, err)

	newHandle, ok := service.LookupHandle("recreate")
	require.True(t, ok)
	require.NotEqual(t, oldHandle, newHandle)

	_, err = service.Acquire(oldHandle)
	require.Error(t, err, "the old handle must not resolve to the new ring")

	newLease, err := service.Acquire(newHandle)
	require.NoError(t, err)

	// A stale second release of the old, already-released lease must
	// never touch the new ring's lease count.
	oldLease.Release()

	_, err = client.DeleteRing(ctx, &ringpb.DeleteRingRequest{Name: "recreate"})
	require.Equal(t, codes.FailedPrecondition, status.Code(err),
		"the stale release must not have unpinned the new ring's lease")

	newLease.Release()
	_, err = client.DeleteRing(ctx, &ringpb.DeleteRingRequest{Name: "recreate"})
	require.NoError(t, err)
}

// Test_RingService_LeaseVsDeleteRace verifies, under concurrent load, that
// a delete never succeeds while any lease is admitted, that whichever way
// the race falls the ring ends up deleted, and that the handle never
// admits a lease again once it is.
func Test_RingService_LeaseVsDeleteRace(t *testing.T) {
	service, client := newRingService(t, 1)
	ctx := t.Context()

	_, err := client.CreateRing(ctx, &ringpb.CreateRingRequest{Name: "race", Capacity: 64})
	require.NoError(t, err)

	handle, ok := service.LookupHandle("race")
	require.True(t, ok)

	const attempts = 64
	var mu sync.Mutex
	var leases []*ring.Lease

	var group errgroup.Group
	for range attempts {
		group.Go(func() error {
			lease, err := service.Acquire(handle)
			if err != nil {
				return nil
			}
			mu.Lock()
			leases = append(leases, lease)
			mu.Unlock()
			return nil
		})
	}

	var raceErr error
	group.Go(func() error {
		_, raceErr = client.DeleteRing(ctx, &ringpb.DeleteRingRequest{Name: "race"})
		return nil
	})
	require.NoError(t, group.Wait())

	mu.Lock()
	admitted := len(leases)
	for _, lease := range leases {
		lease.Release()
	}
	mu.Unlock()

	if raceErr == nil {
		// The race's own delete won before any Acquire could be admitted:
		// under the service's single lock, a delete and a later-admitted
		// lease on the same handle can never both exist, so admitted must
		// be exactly zero here, and the ring is already gone.
		require.Zero(t, admitted, "a delete must never succeed while a lease is admitted")

		_, err = client.DeleteRing(ctx, &ringpb.DeleteRingRequest{Name: "race"})
		require.Equal(t, codes.NotFound, status.Code(err))
	} else {
		// Some Acquire won an admission before the race's delete checked,
		// refusing it; releasing every admitted lease must let a retry
		// succeed.
		require.Equal(t, codes.FailedPrecondition, status.Code(raceErr))

		_, err = client.DeleteRing(ctx, &ringpb.DeleteRingRequest{Name: "race"})
		require.NoError(t, err, "delete must succeed once every admitted lease has released")
	}

	_, err = service.Acquire(handle)
	require.Error(t, err, "a handle whose delete succeeded must never admit a new lease")
}
