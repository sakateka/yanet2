package cring_test

import (
	"os"
	"strconv"
	"testing"

	"github.com/stretchr/testify/require"

	"github.com/yanet-platform/yanet2/objects/ring/bindings/go/cring"
)

// Test_Reader_Stress_ConcurrentWriterNeverTears verifies that the Go reader
// never returns a torn record while the C writer overwrites the ring at
// full speed from another OS thread.
//
// Opt-in: set RING_STRESS_RECORDS (records per run); RING_STRESS_CAPACITY
// sets the ring size (default 4096, so almost every write evicts).
func Test_Reader_Stress_ConcurrentWriterNeverTears(t *testing.T) {
	records := envUint(t, "RING_STRESS_RECORDS", 0)
	if records == 0 {
		t.Skip("set RING_STRESS_RECORDS to run the concurrent stress")
	}
	capacity := uint32(envUint(t, "RING_STRESS_CAPACITY", 4096))

	agent := newTestAgent(t, 1)
	object := newRingObject(t, agent, "stress", capacity)
	writer := newWriter(t, object, 0)
	src, err := object.Source(0)
	require.NoError(t, err)
	reader := cring.NewReader(0, object.Capacity(), src)

	stress, err := writer.StartStress(records)
	require.NoError(t, err)

	var returned, torn uint64
	for {
		done := stress.Done()
		for _, rec := range reader.Read(capacity) {
			returned++
			if !ringwriterValid(rec) {
				torn++
				if torn <= 10 {
					t.Logf("torn record: seqno=%d len=%d", rec.Seqno, len(rec.Bytes))
				}
			}
		}
		if done && !reader.HasMore() {
			break
		}
	}
	written := stress.Written()
	stress.Wait()

	t.Logf("written=%d returned=%d torn=%d", written, returned, torn)
	require.Equal(t, records, written, "the writer must commit every record")
	require.NotZero(t, returned, "the reader must keep up with some records")
	require.Zero(t, torn, "no torn record may ever be returned")
}

func envUint(t *testing.T, name string, def uint64) uint64 {
	t.Helper()
	raw := os.Getenv(name)
	if raw == "" {
		return def
	}
	val, err := strconv.ParseUint(raw, 10, 64)
	require.NoError(t, err, name)
	return val
}
