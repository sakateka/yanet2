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
func newTestAgent(t testing.TB, workerCount uint64) *ffi.Agent {
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

// Test_Object_NewObject_RejectsBadCapacity verifies that a malformed or
// oversized capacity is reported as an invalid argument.
//
// A service built on this binding maps that kind to a gRPC status.
func Test_Object_NewObject_RejectsBadCapacity(t *testing.T) {
	agent := newTestAgent(t, 1)

	cases := []struct {
		name     string
		capacity uint32
	}{
		{name: "not a power of two", capacity: 24},
		{name: "above allocator maximum", capacity: 1 << 27},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := cring.NewObject(agent, "bad-capacity-"+tc.name, tc.capacity)
			require.Error(t, err)
			require.ErrorIs(t, err, cerrors.InvalidArgument)
		})
	}
}

// Test_Object_Free_RefusedWhileReferenced verifies that freeing a published
// object is refused as still referenced while its generation is live.
func Test_Object_Free_RefusedWhileReferenced(t *testing.T) {
	agent := newTestAgent(t, 1)

	object, err := cring.NewObject(agent, "referenced", 64)
	require.NoError(t, err)
	require.NoError(t, object.Publish())

	require.ErrorIs(t, object.Free(), ffi.ErrStillReferenced)
}

// Test_Object_Exists_TracksPublishAndDelete verifies that Exists is true
// only between publishing and deleting the ring.
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

// Test_Parity_MaxNameLen verifies that the C object-name bound agrees with
// the name bound the proto package enforces independently.
func Test_Parity_MaxNameLen(t *testing.T) {
	require.Equal(t, ringpb.MaxRingNameLen, cring.MaxNameLen)
}

// Test_Object_Free_LeavesHandleInert verifies that every accessor on a freed
// handle reports zero or an error instead of touching released memory.
func Test_Object_Free_LeavesHandleInert(t *testing.T) {
	agent := newTestAgent(t, 1)

	object, err := cring.NewObject(agent, "freed", 64)
	require.NoError(t, err)
	require.NoError(t, object.Free())
	require.NoError(t, object.Free(), "a second Free must be a no-op")

	require.Zero(t, object.Capacity())
	_, err = object.Sources()
	require.Error(t, err)
	_, err = object.OpenReaders()
	require.Error(t, err)
	require.Error(t, object.Publish())
}
