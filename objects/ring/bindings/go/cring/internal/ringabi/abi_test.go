package ringabi_test

import (
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
