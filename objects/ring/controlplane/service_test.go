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
	"github.com/yanet-platform/yanet2/objects/ring/bindings/go/cring"
	ring "github.com/yanet-platform/yanet2/objects/ring/controlplane"
	ringpb "github.com/yanet-platform/yanet2/objects/ring/controlplane/ringpb/v1"
	"github.com/yanet-platform/yanet2/objects/ring/internal/ringlink"
	"github.com/yanet-platform/yanet2/objects/ring/internal/ringref"
	"github.com/yanet-platform/yanet2/objects/ring/internal/ringwriter"
)

// newTestAgent builds a throwaway harness with the ring object and any extra
// modules loaded and one attached agent, both torn down at test end.
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

// startRingService hosts the service behind a real gRPC server with the
// validate interceptor every module gets, so validation runs on the real path.
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

// newRingService builds a RingService over a throwaway agent, returning it
// with a real gRPC client for tests that also use the handle and lease API.
func newRingService(t *testing.T, workerCount uint64, extraModules ...string) (*ring.RingService, ringpb.RingServiceClient) {
	t.Helper()

	agent := newTestAgent(t, workerCount, extraModules...)
	service := ring.NewRingService(agent, ring.WithLog(zap.NewNop()))
	return service, startRingService(t, service)
}

// requireRingUsable asserts that the named ring still carries a record from
// the C writer on its first worker to a reader, bytes and seqno intact.
//
// It assumes nothing was written to the ring before.
func requireRingUsable(t *testing.T, agent *ffi.Agent, name string) {
	t.Helper()

	writer, err := ringwriter.NewPublishedWriter(agent, name, 0)
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

// Test_RingService_CreateShowList verifies that a created ring is reported by
// both show and list with the same name, capacity and worker count.
func Test_RingService_CreateShowList(t *testing.T) {
	_, client := newRingService(t, 2)
	ctx := t.Context()

	_, err := client.CreateRing(ctx, &ringpb.CreateRingRequest{Name: "ring0", Capacity: 64})
	require.NoError(t, err)

	show, err := client.ShowRing(ctx, &ringpb.ShowRingRequest{Name: "ring0"})
	require.NoError(t, err)
	require.Equal(t, "ring0", show.GetRing().GetName())
	require.Equal(t, uint64(64), show.GetRing().GetCapacity())
	require.Equal(t, uint64(2), show.GetRing().GetWorkerCount())

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

// Test_RingService_CreateRing_ValidationRejectsInvalidRequests verifies that
// an invalid request fails as InvalidArgument through the real gRPC path.
//
// A rejected create registers nothing.
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

// Test_RingService_CreateRing_AboveMaxCapacityRejected verifies that a valid
// power of two above the allocator's maximum is refused as InvalidArgument.
//
// The rejected create registers nothing.
func Test_RingService_CreateRing_AboveMaxCapacityRejected(t *testing.T) {
	_, client := newRingService(t, 1)
	ctx := t.Context()

	// 1<<27 exceeds the allocator's maximum block (64 MiB, less two ASan
	// red zones) in any build while staying a valid power of two.
	_, err := client.CreateRing(ctx, &ringpb.CreateRingRequest{Name: "too-big", Capacity: 1 << 27})
	require.Equal(t, codes.InvalidArgument, status.Code(err))

	list, err := client.ListRings(ctx, &ringpb.ListRingsRequest{})
	require.NoError(t, err)
	require.Empty(t, list.GetRings())
}

// Test_RingService_CreateRing_DuplicateNameRejected verifies that a duplicate
// name reports AlreadyExists and leaves the original ring the only entry.
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

// Test_RingService_ListRings_SortedByName verifies that list reports every
// registered ring exactly once, sorted by name, with its own capacity.
func Test_RingService_ListRings_SortedByName(t *testing.T) {
	_, client := newRingService(t, 1)
	ctx := t.Context()

	created := []*ringpb.RingInfo{
		{Name: "gamma", Capacity: 4096},
		{Name: "alpha", Capacity: 64},
		{Name: "beta", Capacity: 128},
	}
	for _, info := range created {
		_, err := client.CreateRing(ctx, &ringpb.CreateRingRequest{Name: info.GetName(), Capacity: info.GetCapacity()})
		require.NoError(t, err)
	}

	list, err := client.ListRings(ctx, &ringpb.ListRingsRequest{})
	require.NoError(t, err)
	var names []string
	capacities := map[string]uint64{}
	for _, info := range list.GetRings() {
		names = append(names, info.GetName())
		capacities[info.GetName()] = info.GetCapacity()
	}
	require.Equal(t, []string{"alpha", "beta", "gamma"}, names)
	require.Equal(t, map[string]uint64{"alpha": 64, "beta": 128, "gamma": 4096}, capacities)
}

// Test_RingService_CreateRing_InProcessValidation verifies that an in-process
// call skipping the interceptor is still validated.
//
// A capacity beyond 32 bits is therefore rejected instead of truncated.
func Test_RingService_CreateRing_InProcessValidation(t *testing.T) {
	service, client := newRingService(t, 1)
	ctx := t.Context()

	_, err := service.CreateRing(ctx, &ringpb.CreateRingRequest{Name: "truncated", Capacity: 1<<32 | 64})
	require.Equal(t, codes.InvalidArgument, status.Code(err))

	_, err = service.ShowRing(ctx, &ringpb.ShowRingRequest{})
	require.Equal(t, codes.InvalidArgument, status.Code(err))
	_, err = service.DeleteRing(ctx, &ringpb.DeleteRingRequest{})
	require.Equal(t, codes.InvalidArgument, status.Code(err))

	list, err := client.ListRings(ctx, &ringpb.ListRingsRequest{})
	require.NoError(t, err)
	require.Empty(t, list.GetRings())
}

// Test_RingService_CreateRing_ExternallyPublishedNameRejected verifies that a
// name published outside the service is refused and that ring left as is.
func Test_RingService_CreateRing_ExternallyPublishedNameRejected(t *testing.T) {
	agent := newTestAgent(t, 1)
	service := ring.NewRingService(agent, ring.WithLog(zap.NewNop()))
	client := startRingService(t, service)
	ctx := t.Context()

	object, err := cring.NewObject(agent, "taken", 64)
	require.NoError(t, err)
	require.NoError(t, object.Publish())
	t.Cleanup(func() {
		_ = cring.DeleteObject(agent, "taken")
		_ = object.Free()
	})

	_, err = client.CreateRing(ctx, &ringpb.CreateRingRequest{Name: "taken", Capacity: 128})
	require.Equal(t, codes.AlreadyExists, status.Code(err))

	writer, err := ringwriter.NewPublishedWriter(agent, "taken", 0)
	require.NoError(t, err)
	require.Equal(t, uint32(64), writer.Capacity(), "the published ring must keep its own capacity")
	require.Equal(t, object.AsRawPtr(), writer.Object(), "the published ring must still be the original object")
}

// Test_RingService_CreateRing_PublishFailureReleasesObject verifies that a
// create whose publish runs out of controlplane memory reports Internal.
//
// Nothing is registered or published, and the object's memory returns to
// the agent arena.
func Test_RingService_CreateRing_PublishFailureReleasesObject(t *testing.T) {
	const cpMemory = 8 * datasize.MB

	h, err := dataplaneut.NewHarness(dataplaneut.Config{
		CPMemory:      uint64(cpMemory),
		DPMemory:      uint64(4 * datasize.MB),
		WorkerCount:   1,
		ObjectsToLoad: []string{"ring"},
	})
	require.NoError(t, err)
	t.Cleanup(h.Free)

	agent, err := h.SharedMemory().AgentAttach("ring-service-test", 0, datasize.MB)
	require.NoError(t, err)
	service := ring.NewRingService(agent, ring.WithLog(zap.NewNop()))
	client := startRingService(t, service)
	ctx := t.Context()

	// Drain the rest of the controlplane pool into a filler agent, largest
	// blocks first, so a publish finds no block for its new generation.
	//
	// The service's own arena stays intact.
	filler, err := h.SharedMemory().AgentAttach("ring-cp-filler", 0, datasize.B)
	require.NoError(t, err)
	for size := cpMemory; size > 0; {
		if filler.Extend(size) != nil {
			size /= 2
		}
	}

	baseline := agent.BlockAllocatorFreeSize()

	_, err = client.CreateRing(ctx, &ringpb.CreateRingRequest{Name: "unpublished", Capacity: 64})
	require.Equal(t, codes.Internal, status.Code(err))
	require.ErrorContains(t, err, "failed to publish ring")

	list, err := client.ListRings(ctx, &ringpb.ListRingsRequest{})
	require.NoError(t, err)
	require.Empty(t, list.GetRings())
	require.False(t, cring.Exists(agent, "unpublished"))
	require.Equal(t, baseline, agent.BlockAllocatorFreeSize())
}

// Test_RingService_DeleteRing_AlreadyUnpublishedDropsEntry verifies that a
// ring the dataplane no longer publishes is still unregistered and freed.
func Test_RingService_DeleteRing_AlreadyUnpublishedDropsEntry(t *testing.T) {
	agent := newTestAgent(t, 1)
	service := ring.NewRingService(agent, ring.WithLog(zap.NewNop()))
	client := startRingService(t, service)
	ctx := t.Context()

	baseline := agent.BlockAllocatorFreeSize()

	_, err := client.CreateRing(ctx, &ringpb.CreateRingRequest{Name: "vanished", Capacity: 64})
	require.NoError(t, err)
	require.NoError(t, cring.DeleteObject(agent, "vanished"))

	_, err = client.DeleteRing(ctx, &ringpb.DeleteRingRequest{Name: "vanished"})
	require.NoError(t, err)

	_, err = client.ShowRing(ctx, &ringpb.ShowRingRequest{Name: "vanished"})
	require.Equal(t, codes.NotFound, status.Code(err))
	require.Equal(t, baseline, agent.BlockAllocatorFreeSize())
}

// Test_RingService_DeleteRing_LeasedRingStaysUsable verifies that a leased
// ring refuses deletion with FailedPrecondition yet stays usable.
//
// Releasing the lease lets the delete succeed.
func Test_RingService_DeleteRing_LeasedRingStaysUsable(t *testing.T) {
	agent := newTestAgent(t, 1)
	service := ring.NewRingService(agent, ring.WithLog(zap.NewNop()))
	client := startRingService(t, service)
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
	requireRingUsable(t, agent, "leased")

	lease.Release()

	_, err = client.DeleteRing(ctx, &ringpb.DeleteRingRequest{Name: "leased"})
	require.NoError(t, err)
}

// Test_RingService_DeleteRing_RefusedWhileLinked verifies that a published
// module's link refuses the ring's deletion with FailedPrecondition.
//
// The ring stays registered and usable, and removing the linking module
// lets the delete succeed.
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
	requireRingUsable(t, agent, "linked")

	// Once the linking module is gone, the same delete goes through.
	require.NoError(t, agent.DeleteModuleConfig("forward", "ring-linker"))
	_, err = client.DeleteRing(ctx, &ringpb.DeleteRingRequest{Name: "linked"})
	require.NoError(t, err)
	require.False(t, cring.Exists(agent, "linked"))
}

// Test_RingService_DeleteRing_RefusedFreeRetried verifies that a delete whose
// free a live reference refuses still succeeds and unregisters the name.
//
// The name can be recreated under a fresh handle, the memory stays held
// while the reference remains, and a later mutation completes the deferred
// free once the reference is released.
func Test_RingService_DeleteRing_RefusedFreeRetried(t *testing.T) {
	agent := newTestAgent(t, 1)
	service := ring.NewRingService(agent, ring.WithLog(zap.NewNop()))
	client := startRingService(t, service)
	ctx := t.Context()

	baseline := agent.BlockAllocatorFreeSize()

	_, err := client.CreateRing(ctx, &ringpb.CreateRingRequest{Name: "deferred", Capacity: 64})
	require.NoError(t, err)
	oldHandle, ok := service.LookupHandle("deferred")
	require.True(t, ok)

	ref, err := ringref.Hold(agent, "deferred")
	require.NoError(t, err)

	_, err = client.DeleteRing(ctx, &ringpb.DeleteRingRequest{Name: "deferred"})
	require.NoError(t, err)

	_, err = client.ShowRing(ctx, &ringpb.ShowRingRequest{Name: "deferred"})
	require.Equal(t, codes.NotFound, status.Code(err), "a deleted ring is unregistered even while its free is deferred")
	list, err := client.ListRings(ctx, &ringpb.ListRingsRequest{})
	require.NoError(t, err)
	require.Empty(t, list.GetRings(), "a deleted ring is not listed even while its free is deferred")

	require.Less(t, agent.BlockAllocatorFreeSize(), baseline,
		"the deferred ring's memory must still be held while a live reference remains")

	// The name is free again while the old object is referenced: a
	// recreate gets a fresh handle, and deleting it keeps the old ring held.
	_, err = client.CreateRing(ctx, &ringpb.CreateRingRequest{Name: "deferred", Capacity: 64})
	require.NoError(t, err)
	newHandle, ok := service.LookupHandle("deferred")
	require.True(t, ok)
	require.NotEqual(t, oldHandle, newHandle)
	_, err = client.DeleteRing(ctx, &ringpb.DeleteRingRequest{Name: "deferred"})
	require.NoError(t, err)

	require.Less(t, agent.BlockAllocatorFreeSize(), baseline,
		"the deferred ring's memory must stay held until its live reference releases")

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

// Test_RingService_DeleteThenRecreate_NewHandle verifies that a ring
// recreated under a deleted name gets a fresh handle.
//
// The old handle is never acquired again, and a stale release from before
// the delete does not unpin the new ring.
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

// Test_RingService_LeaseVsDeleteRace verifies under concurrent load that a
// delete never succeeds while a lease is admitted.
//
// Whichever way the race falls the ring ends up deleted, and its handle
// never admits a lease again.
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
		// The delete won before any lease was admitted: under the single
		// service lock a delete and a later lease on one handle never coexist.
		require.Zero(t, admitted, "a delete must never succeed while a lease is admitted")

		_, err = client.DeleteRing(ctx, &ringpb.DeleteRingRequest{Name: "race"})
		require.Equal(t, codes.NotFound, status.Code(err))
	} else {
		// A lease was admitted before the delete checked and refused it;
		// releasing every admitted lease must let a retry succeed.
		require.Equal(t, codes.FailedPrecondition, status.Code(raceErr))

		_, err = client.DeleteRing(ctx, &ringpb.DeleteRingRequest{Name: "race"})
		require.NoError(t, err, "delete must succeed once every admitted lease has released")
	}

	_, err = service.Acquire(handle)
	require.Error(t, err, "a handle whose delete succeeded must never admit a new lease")
}
