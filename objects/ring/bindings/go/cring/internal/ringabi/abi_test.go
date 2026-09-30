package ringabi_test

import (
	"math"
	"testing"

	"github.com/stretchr/testify/require"

	"github.com/yanet-platform/yanet2/objects/ring/bindings/go/cring/internal/ringabi"
)

// Test_Parity_RingWorkerLayout verifies that cgo's view of the per-worker
// metadata layout matches the one the C archive was compiled with.
func Test_Parity_RingWorkerLayout(t *testing.T) {
	require.Equal(t, ringabi.ArchiveWorkerLayout(), ringabi.CgoWorkerLayout())
}

// Test_Parity_RingWorkerGoSize verifies that the Go type cgo generates spans
// exactly the C struct, cache-line padding included.
func Test_Parity_RingWorkerGoSize(t *testing.T) {
	require.Equal(t, ringabi.ArchiveWorkerLayout().Size, ringabi.CgoWorkerGoSize())
}

// Test_Parity_Align4 verifies that the reader's record rounding agrees with
// the C writer's at the edges of the 32-bit range, wraparound included.
func Test_Parity_Align4(t *testing.T) {
	cases := []struct {
		name string
		val  uint32
	}{
		{name: "zero", val: 0},
		{name: "one", val: 1},
		{name: "one below alignment", val: 3},
		{name: "exact alignment", val: 4},
		{name: "one above alignment", val: 5},
		{name: "two to the 31st", val: 1 << 31},
		{name: "largest aligned length", val: math.MaxUint32 - 3},
		{name: "wraps past the range", val: math.MaxUint32},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			require.Equal(t, ringabi.ArchiveAlign4(tc.val), ringabi.Align4(tc.val))
		})
	}
}
