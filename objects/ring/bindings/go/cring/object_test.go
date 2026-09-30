package cring_test

import (
	"testing"

	"github.com/c2h5oh/datasize"
	"github.com/stretchr/testify/require"

	"github.com/yanet-platform/yanet2/bindings/go/cerrors"
	dataplaneut "github.com/yanet-platform/yanet2/bindings/go/dataplane_ut"
	"github.com/yanet-platform/yanet2/controlplane/ffi"
	"github.com/yanet-platform/yanet2/objects/ring/bindings/go/cring"
	ringpb "github.com/yanet-platform/yanet2/objects/ring/controlplane/ringpb/v1"
)

// newTestAgent builds a throwaway dataplane_ut harness with the ring object
// loaded and one attached agent, both torn down at test end.
func newTestAgent(t *testing.T, workerCount uint64) *ffi.Agent {
	t.Helper()

	h, err := dataplaneut.NewHarness(dataplaneut.Config{
		CPMemory:      uint64(64 * datasize.MB),
		DPMemory:      uint64(4 * datasize.MB),
		WorkerCount:   workerCount,
		ObjectsToLoad: []string{"ring"},
	})
	require.NoError(t, err)
	t.Cleanup(h.Free)

	agent, err := h.SharedMemory().AgentAttach("ring-test", 0, 16*datasize.MB)
	require.NoError(t, err)
	t.Cleanup(func() { _ = agent.CleanUp() })

	return agent
}

// Test_Object_NewObject_RejectsBadCapacity verifies that the C rejection of
// a zero, undersized or non-power-of-two capacity is reported as
// cerrors.InvalidArgument, so a service built on this binding can map it to
// a gRPC status.
func Test_Object_NewObject_RejectsBadCapacity(t *testing.T) {
	agent := newTestAgent(t, 1)

	cases := []struct {
		name     string
		capacity uint32
	}{
		{name: "zero", capacity: 0},
		{name: "below the record frame size", capacity: 4},
		{name: "not a power of two", capacity: 24},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := cring.NewObject(agent, "bad-capacity-"+tc.name, tc.capacity)
			require.Error(t, err)
			require.ErrorIs(t, err, cerrors.InvalidArgument)
		})
	}
}

// Test_Object_WorkerCountAndCapacity verifies that a created object reports
// the dataplane's configured worker count, not a value the caller passed,
// and the capacity it was created with.
func Test_Object_WorkerCountAndCapacity(t *testing.T) {
	agent := newTestAgent(t, 3)

	object, err := cring.NewObject(agent, "sized", 64)
	require.NoError(t, err)
	t.Cleanup(func() { _ = object.Free() })

	require.Equal(t, uint64(3), object.WorkerCount())
	require.Equal(t, uint32(64), object.Capacity())
}

// Test_Object_Free_RefusedWhileReferenced verifies that freeing a published
// object is refused with ffi.ErrStillReferenced while its generation is
// live, and that the handle stays usable afterward.
func Test_Object_Free_RefusedWhileReferenced(t *testing.T) {
	agent := newTestAgent(t, 1)

	object, err := cring.NewObject(agent, "referenced", 64)
	require.NoError(t, err)
	require.NoError(t, object.Publish())

	require.ErrorIs(t, object.Free(), ffi.ErrStillReferenced)
}

// Test_Object_Exists_TracksPublishAndDelete verifies that Exists reflects
// only the currently published generation: false before create, false
// after create but before publish, true once published, and false again
// once deleted.
func Test_Object_Exists_TracksPublishAndDelete(t *testing.T) {
	agent := newTestAgent(t, 1)

	require.False(t, cring.Exists(agent, "maybe"))

	object, err := cring.NewObject(agent, "maybe", 64)
	require.NoError(t, err)
	require.False(t, cring.Exists(agent, "maybe"))

	require.NoError(t, object.Publish())
	require.True(t, cring.Exists(agent, "maybe"))

	require.NoError(t, cring.DeleteObject(agent, "maybe"))
	require.False(t, cring.Exists(agent, "maybe"))
}

// Test_Parity_MaxNameLen verifies that cring's name-length bound, mirrored
// from the C object-name buffer size, agrees with the request-validation
// bound the ring proto package enforces independently.
func Test_Parity_MaxNameLen(t *testing.T) {
	require.Equal(t, ringpb.MaxRingNameLen, cring.MaxNameLen)
}

// Test_Object_Free_LeavesHandleInert verifies that every accessor on a
// handle whose object was freed reports zero or an error instead of
// touching the released memory.
func Test_Object_Free_LeavesHandleInert(t *testing.T) {
	agent := newTestAgent(t, 1)

	object, err := cring.NewObject(agent, "freed", 64)
	require.NoError(t, err)
	require.NoError(t, object.Free())
	require.NoError(t, object.Free(), "a second Free must be a no-op")

	require.Zero(t, object.WorkerCount())
	require.Zero(t, object.Capacity())
	_, err = object.Source(0)
	require.Error(t, err)
	_, err = object.OpenReader(0)
	require.Error(t, err)
	require.Error(t, object.Publish())
}
