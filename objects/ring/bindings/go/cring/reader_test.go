package cring_test

import (
	"encoding/binary"
	"os"
	"strconv"
	"testing"

	"github.com/stretchr/testify/require"

	"github.com/yanet-platform/yanet2/controlplane/ffi"
	"github.com/yanet-platform/yanet2/objects/ring/bindings/go/cring"
	ringpb "github.com/yanet-platform/yanet2/objects/ring/controlplane/ringpb/v1"
	"github.com/yanet-platform/yanet2/objects/ring/internal/ringwriter"
)

// repeatedFrameSizePayload returns n bytes built from repeated
// little-endian encodings of the record frame size.
//
// An evicting write filled with this payload leaves only frame-size-shaped
// words behind wherever it lands: if a defect let stale or torn bytes leak
// into a result, they parse as a plausible small record (frame size
// exactly, empty payload) instead of tripping the out-of-range guard by
// accident, so a discriminating test can tell the two failure modes apart.
func repeatedFrameSizePayload(n int) []byte {
	payload := make([]byte, n)
	for idx := 0; idx+4 <= n; idx += 4 {
		binary.LittleEndian.PutUint32(payload[idx:idx+4], cring.RecordFrameSize)
	}
	return payload
}

// newRingObject creates and publishes a ring object, freeing it at test
// end.
func newRingObject(t *testing.T, agent *ffi.Agent, name string, capacity uint32) *cring.Object {
	t.Helper()

	object, err := cring.NewObject(agent, name, capacity)
	require.NoError(t, err)
	t.Cleanup(func() { _ = object.Free() })

	require.NoError(t, object.Publish())
	return object
}

// source returns the real record source of one worker's ring.
func source(t *testing.T, object *cring.Object, workerIdx uint64) cring.RecordSource {
	t.Helper()

	sources, err := object.Sources()
	require.NoError(t, err)
	require.Less(t, workerIdx, uint64(len(sources)))
	return sources[workerIdx]
}

// openReader opens a fresh reader over one worker's ring.
func openReader(t *testing.T, object *cring.Object, workerIdx uint64) *cring.Reader {
	t.Helper()

	readers, err := object.OpenReaders()
	require.NoError(t, err)
	require.Less(t, workerIdx, uint64(len(readers)))
	return readers[workerIdx]
}

// newWriter resolves the raw C writer primitives for one worker, to drive
// records directly as the dataplane would.
func newWriter(t *testing.T, object *cring.Object, workerIdx uint64) *ringwriter.Writer {
	t.Helper()

	writer, err := ringwriter.NewWriter(object.AsRawPtr(), workerIdx)
	require.NoError(t, err)
	return writer
}

// Test_Reader_Read_RoundTripAcrossPhysicalWrap verifies that a record
// straddling the ring's physical end round-trips its bytes unchanged.
func Test_Reader_Read_RoundTripAcrossPhysicalWrap(t *testing.T) {
	agent := newTestAgent(t, 1)
	object := newRingObject(t, agent, "wrap", 32)
	writer := newWriter(t, object, 0)

	// The frame lands just inside the boundary and the payload straddles
	// it: bytes [28,32) then [0,4), as in the C wrap fixture of this size.
	writer.SetIndices(20, 20)

	payload := []byte{1, 2, 3, 4, 5, 6, 7, 8}
	seqno, err := writer.WriteRecord(payload)
	require.NoError(t, err)
	require.Equal(t, uint32(0), seqno)

	reader := openReader(t, object, 0)

	records := reader.Read(1024)
	require.Len(t, records, 1)
	require.Equal(t, uint64(0), records[0].Worker)
	require.Equal(t, uint32(0), records[0].Seqno)
	require.Equal(t, payload, records[0].Bytes)
}

// Test_Reader_Read_RoundTripAcrossWorkers verifies that OpenReaders returns
// one reader per worker and that each sees only its own worker's records.
func Test_Reader_Read_RoundTripAcrossWorkers(t *testing.T) {
	const workerCount = 3

	agent := newTestAgent(t, workerCount)
	object := newRingObject(t, agent, "workers", 64)

	for workerIdx := range workerCount {
		writer := newWriter(t, object, uint64(workerIdx))
		_, err := writer.WriteRecord([]byte{byte(workerIdx), byte(workerIdx)})
		require.NoError(t, err)
	}

	readers, err := object.OpenReaders()
	require.NoError(t, err)
	require.Len(t, readers, workerCount)

	for workerIdx, reader := range readers {
		records := reader.Read(1024)
		require.Len(t, records, 1)
		require.Equal(t, uint64(workerIdx), records[0].Worker)
		require.Equal(t, []byte{byte(workerIdx), byte(workerIdx)}, records[0].Bytes)
	}
}

// Test_Reader_Read_RoundTripSequentialWrites verifies that records committed
// back to back return in order, unchanged, with contiguous sequence numbers.
func Test_Reader_Read_RoundTripSequentialWrites(t *testing.T) {
	agent := newTestAgent(t, 1)
	object := newRingObject(t, agent, "sequential", 128)
	writer := newWriter(t, object, 0)

	first := []byte("first-record")
	second := []byte("second-record-is-longer")

	seqnoFirst, err := writer.WriteRecord(first)
	require.NoError(t, err)
	seqnoSecond, err := writer.WriteRecord(second)
	require.NoError(t, err)
	require.Equal(t, seqnoFirst+1, seqnoSecond)

	reader := openReader(t, object, 0)

	records := reader.Read(1024)
	require.Len(t, records, 2)
	require.Equal(t, seqnoFirst, records[0].Seqno)
	require.Equal(t, first, records[0].Bytes)
	require.Equal(t, seqnoSecond, records[1].Seqno)
	require.Equal(t, second, records[1].Bytes)
}

// Test_Reader_Read_SeqnoWrapsContiguously verifies that the sequence
// number wraps from 0xffffffff to 0 without a gap.
func Test_Reader_Read_SeqnoWrapsContiguously(t *testing.T) {
	agent := newTestAgent(t, 1)
	object := newRingObject(t, agent, "seqno-wrap", 64)
	writer := newWriter(t, object, 0)
	writer.SetNextSeqno(0xffffffff)

	seqnoBeforeWrap, err := writer.WriteRecord([]byte("a"))
	require.NoError(t, err)
	require.Equal(t, uint32(0xffffffff), seqnoBeforeWrap)

	seqnoAfterWrap, err := writer.WriteRecord([]byte("b"))
	require.NoError(t, err)
	require.Equal(t, uint32(0), seqnoAfterWrap)

	reader := openReader(t, object, 0)

	records := reader.Read(1024)
	require.Len(t, records, 2)
	require.Equal(t, uint32(0xffffffff), records[0].Seqno)
	require.Equal(t, uint32(0), records[1].Seqno)
}

// Test_Reader_WriteRecord_OversizedRejected verifies that an oversized record
// is refused before anything is written or a sequence number is consumed.
func Test_Reader_WriteRecord_OversizedRejected(t *testing.T) {
	agent := newTestAgent(t, 1)
	object := newRingObject(t, agent, "oversized", 32)
	writer := newWriter(t, object, 0)

	before := writer.NextSeqno()

	_, err := writer.WriteRecord(make([]byte, 64))
	require.Error(t, err)
	require.Equal(t, before, writer.NextSeqno())

	reader := openReader(t, object, 0)
	require.Empty(t, reader.Read(1024))
}

// Test_Reader_Read_CorruptFrameResyncsToWriteBoundary verifies that after a
// corrupt frame the reader resumes at that call's snapshot write position.
//
// A later read therefore never parses a record's payload bytes as a frame.
func Test_Reader_Read_CorruptFrameResyncsToWriteBoundary(t *testing.T) {
	agent := newTestAgent(t, 1)
	object := newRingObject(t, agent, "corrupt-frame", 64)
	writer := newWriter(t, object, 0)

	// The first payload is two frame-size words, so a misaligned reader
	// would parse it as valid empty records instead of hitting the guard.
	corruptOffset := writer.WriteIdx()
	_, err := writer.WriteRecord([]byte{0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00})
	require.NoError(t, err)
	writer.CorruptTotalLen(corruptOffset, 0xffffffff)

	_, err = writer.WriteRecord([]byte("BBBBBBBB"))
	require.NoError(t, err)

	reader := openReader(t, object, 0)

	// A small budget stops the copy inside the first payload, well short
	// of the write position, so the cursor could land mid-record.
	require.Empty(t, reader.Read(12))

	_, err = writer.WriteRecord([]byte("CCCCCCCC"))
	require.NoError(t, err)

	records := reader.Read(1024)
	require.Len(t, records, 1)
	require.Equal(t, []byte("CCCCCCCC"), records[0].Bytes)
}

// Test_Reader_Read_CorruptFrameReturnsEarlierRecords verifies that records
// parsed before a corrupt frame are returned and the rest is dropped.
func Test_Reader_Read_CorruptFrameReturnsEarlierRecords(t *testing.T) {
	agent := newTestAgent(t, 1)
	object := newRingObject(t, agent, "corrupt-after-good", 128)
	writer := newWriter(t, object, 0)

	_, err := writer.WriteRecord([]byte("good-one"))
	require.NoError(t, err)
	corruptOffset := writer.WriteIdx()
	_, err = writer.WriteRecord([]byte("bad-frame"))
	require.NoError(t, err)
	writer.CorruptTotalLen(corruptOffset, 0xffffffff)
	_, err = writer.WriteRecord([]byte("dropped"))
	require.NoError(t, err)

	reader := openReader(t, object, 0)

	records := reader.Read(1024)
	require.Len(t, records, 1)
	require.Equal(t, []byte("good-one"), records[0].Bytes)
	require.False(t, reader.HasMore(), "the cursor must resume at the snapshot write position")

	_, err = writer.WriteRecord([]byte("after"))
	require.NoError(t, err)

	records = reader.Read(1024)
	require.Len(t, records, 1)
	require.Equal(t, []byte("after"), records[0].Bytes)
}

// Test_Reader_Read_TwoIndependentReadersSeeSameStream verifies that two
// readers on one worker each see every record through their own cursor.
func Test_Reader_Read_TwoIndependentReadersSeeSameStream(t *testing.T) {
	agent := newTestAgent(t, 1)
	object := newRingObject(t, agent, "two-readers", 128)
	writer := newWriter(t, object, 0)

	_, err := writer.WriteRecord([]byte("record-a"))
	require.NoError(t, err)

	readerOne := openReader(t, object, 0)
	readerTwo := openReader(t, object, 0)

	recordsOne := readerOne.Read(1024)
	require.Len(t, recordsOne, 1)
	require.Equal(t, []byte("record-a"), recordsOne[0].Bytes)

	// A second reader's cursor starts independently, so it still sees the
	// already-consumed record too.
	recordsTwo := readerTwo.Read(1024)
	require.Len(t, recordsTwo, 1)
	require.Equal(t, []byte("record-a"), recordsTwo[0].Bytes)

	_, err = writer.WriteRecord([]byte("record-b"))
	require.NoError(t, err)

	recordsOne = readerOne.Read(1024)
	require.Len(t, recordsOne, 1)
	require.Equal(t, []byte("record-b"), recordsOne[0].Bytes)

	recordsTwo = readerTwo.Read(1024)
	require.Len(t, recordsTwo, 1)
	require.Equal(t, []byte("record-b"), recordsTwo[0].Bytes)
}

// hookedSource wraps a real RecordSource and runs injected actions at chosen
// points of the read protocol.
//
// One hook fires right before the delegated copy, one before the first
// read's recheck (its second index snapshot), and a general one on any
// 1-based index snapshot across every read, for a later call's recheck.
type hookedSource struct {
	real            cring.RecordSource
	indicesCalls    int
	onCopy          func()
	onSecondIndices func()
	onIndicesCall   func(call int)
}

func (m *hookedSource) Indices() (uint64, uint64) {
	m.indicesCalls++
	if m.indicesCalls == 2 && m.onSecondIndices != nil {
		m.onSecondIndices()
	}
	if m.onIndicesCall != nil {
		m.onIndicesCall(m.indicesCalls)
	}
	return m.real.Indices()
}

func (m *hookedSource) CopyRange(dst []byte, start, size uint64) {
	if m.onCopy != nil {
		m.onCopy()
	}
	m.real.CopyRange(dst, start, size)
}

// Test_Reader_Read_DeterministicOverwrite verifies that a record invalidated
// during a read is never returned and the reader recovers afterwards.
//
// The invalidation lands either between the snapshot and the copy or
// between the copy and the recheck.
func Test_Reader_Read_DeterministicOverwrite(t *testing.T) {
	cases := []struct {
		name string
		arm  func(src *hookedSource, evict func())
	}{
		{
			name: "invalidates between snapshot and copy",
			arm:  func(src *hookedSource, evict func()) { src.onCopy = evict },
		},
		{
			name: "invalidates between copy and recheck",
			arm:  func(src *hookedSource, evict func()) { src.onSecondIndices = evict },
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			agent := newTestAgent(t, 1)
			object := newRingObject(t, agent, "overwrite", 64)
			writer := newWriter(t, object, 0)

			_, err := writer.WriteRecord([]byte("stale"))
			require.NoError(t, err)

			real := source(t, object, 0)

			fired := false
			evict := func() {
				if fired {
					return
				}
				fired = true
				// Big enough to evict the "stale" record entirely, moving
				// the readable position past everything this read saw.
				//
				// The payload is frame-size words throughout, so a torn or
				// stale reinterpretation never trips the range guard: only
				// the recheck can reject it, the property under test.
				_, evictErr := writer.WriteRecord(repeatedFrameSizePayload(48))
				require.NoError(t, evictErr)
			}

			hooked := &hookedSource{real: real}
			tc.arm(hooked, evict)

			reader, err := cring.NewReader(0, object.Capacity(), hooked)
			require.NoError(t, err)
			records := reader.Read(1024)
			require.Empty(t, records, "an invalidated record must never be returned")

			_, err = writer.WriteRecord([]byte("fresh"))
			require.NoError(t, err)

			records = reader.Read(1024)
			require.Len(t, records, 1)
			require.Equal(t, []byte("fresh"), records[0].Bytes)
		})
	}
}

// Test_Reader_Read_PartialPrefixDropKeepsSurvivingRecord verifies that
// evicting the older of two buffered records keeps the younger one intact.
func Test_Reader_Read_PartialPrefixDropKeepsSurvivingRecord(t *testing.T) {
	agent := newTestAgent(t, 1)
	object := newRingObject(t, agent, "partial-drop", 128)
	writer := newWriter(t, object, 0)

	_, err := writer.WriteRecord([]byte("AAAAAAAA")) // stale: occupies [0,16)
	require.NoError(t, err)
	_, err = writer.WriteRecord([]byte("survive-me!!")) // occupies [16,36)
	require.NoError(t, err)

	real := source(t, object, 0)

	fired := false
	hooked := &hookedSource{real: real}
	hooked.onSecondIndices = func() {
		if fired {
			return
		}
		fired = true
		// 96 bytes exceed the 92 free, so the write evicts exactly the
		// stale record; the survivor is untouched, physically and by the drop.
		_, evictErr := writer.WriteRecord(repeatedFrameSizePayload(88))
		require.NoError(t, evictErr)
	}

	reader, err := cring.NewReader(0, object.Capacity(), hooked)
	require.NoError(t, err)
	records := reader.Read(1024)
	require.Len(t, records, 1, "only the invalidated prefix must be dropped")
	require.Equal(t, []byte("survive-me!!"), records[0].Bytes)
}

// Test_Reader_Read_InvalidatesCarriedPartialRecord verifies that the recheck
// also drops the bytes of a record partly buffered by an earlier read.
func Test_Reader_Read_InvalidatesCarriedPartialRecord(t *testing.T) {
	agent := newTestAgent(t, 1)
	object := newRingObject(t, agent, "carried-partial", 64)
	writer := newWriter(t, object, 0)

	// A 24-byte record of frame-size words, so a misaligned remainder still
	// parses as a plausible record instead of hitting the range guard.
	_, err := writer.WriteRecord(repeatedFrameSizePayload(16))
	require.NoError(t, err)

	real := source(t, object, 0)

	fired := false
	hooked := &hookedSource{real: real}
	hooked.onIndicesCall = func(call int) {
		// Call 4 is the second Read's recheck: calls 1 and 2 belong to
		// the first Read's snapshot and (no-op) recheck below.
		if call != 4 || fired {
			return
		}
		fired = true
		// 48 bytes exceed the 40 free while the 24-byte record is still
		// held, so the write evicts it entirely.
		_, evictErr := writer.WriteRecord(repeatedFrameSizePayload(40))
		require.NoError(t, evictErr)
	}

	reader, err := cring.NewReader(0, object.Capacity(), hooked)
	require.NoError(t, err)

	// Captures only the frame and the first payload word (12 of the 24
	// bytes), leaving the rest buffered for the next call.
	require.Empty(t, reader.Read(12))

	records := reader.Read(1024)
	require.Empty(t, records, "the carried prefix plus the newly copied bytes must both be dropped")

	_, err = writer.WriteRecord([]byte("fresh"))
	require.NoError(t, err)

	// The evicting write committed a genuine record after the dropped one,
	// readable ahead of "fresh"; only the buffered record must be gone.
	records = reader.Read(1024)
	require.Len(t, records, 2)
	require.Equal(t, repeatedFrameSizePayload(40), records[0].Bytes)
	require.Equal(t, []byte("fresh"), records[1].Bytes)
}

// Test_Reader_Read_DropExceedsBufferDiscardsEverything verifies that an
// eviction reaching past the copied range clears the whole buffer.
func Test_Reader_Read_DropExceedsBufferDiscardsEverything(t *testing.T) {
	agent := newTestAgent(t, 1)
	object := newRingObject(t, agent, "exceeds-buffer", 64)
	writer := newWriter(t, object, 0)

	_, err := writer.WriteRecord([]byte("AAAAAAAA")) // occupies [0,16)
	require.NoError(t, err)
	_, err = writer.WriteRecord([]byte("BBBBBBBB")) // occupies [16,32)
	require.NoError(t, err)

	real := source(t, object, 0)

	fired := false
	hooked := &hookedSource{real: real}
	hooked.onCopy = func() {
		if fired {
			return
		}
		fired = true
		// 40 bytes exceed the 32 free, so the write evicts exactly the
		// first record, moving the readable position past the 10 copied.
		_, evictErr := writer.WriteRecord(repeatedFrameSizePayload(32))
		require.NoError(t, evictErr)
	}

	reader, err := cring.NewReader(0, object.Capacity(), hooked)
	require.NoError(t, err)
	records := reader.Read(10)
	require.Empty(t, records, "a drop past the copied range must clear the whole buffer, not slice past it")
}

// Test_Reader_Read_RecordBytesAppendDoesNotCorruptLaterRecords verifies
// that appending to one payload never overwrites a later record's payload.
func Test_Reader_Read_RecordBytesAppendDoesNotCorruptLaterRecords(t *testing.T) {
	agent := newTestAgent(t, 1)
	object := newRingObject(t, agent, "bytes-append", 128)
	writer := newWriter(t, object, 0)

	_, err := writer.WriteRecord([]byte("first"))
	require.NoError(t, err)
	_, err = writer.WriteRecord([]byte("second"))
	require.NoError(t, err)

	reader := openReader(t, object, 0)

	records := reader.Read(1024)
	require.Len(t, records, 2)

	// Long enough to reach into the second payload, were payloads not
	// capped to their own length.
	_ = append(records[0].Bytes, []byte("XXXXXXXXXXXXXXX")...)

	require.Equal(t, []byte("second"), records[1].Bytes)
}

// Test_Parity_RecordFrameSize verifies that the C record frame size agrees
// with the minimum capacity the proto package enforces independently.
func Test_Parity_RecordFrameSize(t *testing.T) {
	require.Equal(t, uint32(ringpb.MinRingCapacity), cring.RecordFrameSize)
}

// Test_Reader_Read_EvictionBetweenReadsDropsCarriedPartial verifies that
// an eviction between two reads discards the partial record carried over.
func Test_Reader_Read_EvictionBetweenReadsDropsCarriedPartial(t *testing.T) {
	agent := newTestAgent(t, 1)
	object := newRingObject(t, agent, "evict-between", 64)
	writer := newWriter(t, object, 0)

	// A 24-byte record of frame-size-shaped words, so stale bytes would
	// parse as plausible records rather than trip the range guard.
	_, err := writer.WriteRecord(repeatedFrameSizePayload(16))
	require.NoError(t, err)

	reader := openReader(t, object, 0)
	require.Empty(t, reader.Read(12), "a partial record must stay buffered")

	// 48 bytes exceed the 40 free, so this write evicts the whole
	// buffered record before the next Read runs.
	payload := repeatedFrameSizePayload(40)
	seqno, err := writer.WriteRecord(payload)
	require.NoError(t, err)

	records := reader.Read(1024)
	require.Len(t, records, 1, "only the record written after the eviction may be returned")
	require.Equal(t, seqno, records[0].Seqno)
	require.Equal(t, payload, records[0].Bytes)
}

// Test_Reader_Read_SteadyStateAllocatesOnlyReturnedRecords verifies that a
// warmed-up read allocates only for returned records, otherwise nothing.
func Test_Reader_Read_SteadyStateAllocatesOnlyReturnedRecords(t *testing.T) {
	agent := newTestAgent(t, 1)
	object := newRingObject(t, agent, "allocs", 4096)
	writer := newWriter(t, object, 0)

	reader := openReader(t, object, 0)

	// 56 payload bytes make a 64-byte record, read in four 16-byte calls
	// of which only the last completes it.
	payload := make([]byte, 56)
	cycle := func() int {
		_, _ = writer.WriteRecord(payload)
		returned := 0
		for range 4 {
			returned += len(reader.Read(16))
		}
		return returned
	}
	require.Equal(t, 1, cycle(), "warm-up must return the record")

	returned := 0
	allocs := testing.AllocsPerRun(100, func() {
		returned += cycle()
	})
	require.Equal(t, 101, returned, "every cycle must return exactly one record")
	// One record slice and one payload block for the completing call.
	require.LessOrEqual(t, allocs, 2.0)

	_, err := writer.WriteRecord(payload)
	require.NoError(t, err)
	partial := testing.AllocsPerRun(1, func() {
		require.Empty(t, reader.Read(8))
	})
	require.Zero(t, partial, "a call completing no record must not allocate")
}

// Test_Reader_NewReader_RejectsCapacityBelowFrame verifies that a reader
// over a capacity too small to hold even one record frame is refused.
func Test_Reader_NewReader_RejectsCapacityBelowFrame(t *testing.T) {
	_, err := cring.NewReader(0, cring.RecordFrameSize-1, nil)
	require.Error(t, err)

	_, err = cring.NewReader(0, cring.RecordFrameSize, nil)
	require.NoError(t, err)
}

// Test_Reader_Stress_ConcurrentWriterNeverTears verifies that the reader
// never returns a torn record while the C writer overwrites at full speed.
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
	reader := openReader(t, object, 0)

	stress, err := writer.StartStress(records)
	require.NoError(t, err)

	var returned, torn uint64
	for {
		done := stress.Done()
		for _, rec := range reader.Read(capacity) {
			returned++
			if !ringwriter.StressRecordValid(rec.Seqno, rec.Bytes) {
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

// envUint reads an unsigned integer from the named environment variable, or
// returns the default when it is unset.
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
