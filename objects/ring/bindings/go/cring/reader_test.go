package cring_test

import (
	"encoding/binary"
	"testing"

	"github.com/stretchr/testify/require"

	"github.com/yanet-platform/yanet2/controlplane/ffi"
	"github.com/yanet-platform/yanet2/objects/ring/bindings/go/cring"
	"github.com/yanet-platform/yanet2/objects/ring/bindings/go/cring/internal/ringwriter"
	ringpb "github.com/yanet-platform/yanet2/objects/ring/controlplane/ringpb/v1"
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

// newWriter resolves the raw C writer primitives for one worker of object,
// for driving records directly the way the dataplane would.
func newWriter(t *testing.T, object *cring.Object, workerIdx uint64) *ringwriter.Writer {
	t.Helper()

	writer, err := ringwriter.NewWriter(object.AsRawPtr(), workerIdx)
	require.NoError(t, err)
	return writer
}

// Test_Reader_Read_RoundTripAcrossPhysicalWrap verifies that a record whose
// frame and payload straddle the ring's physical boundary round-trips its
// opaque bytes unchanged.
func Test_Reader_Read_RoundTripAcrossPhysicalWrap(t *testing.T) {
	agent := newTestAgent(t, 1)
	object := newRingObject(t, agent, "wrap", 32)
	writer := newWriter(t, object, 0)

	// The frame lands just inside the boundary and the payload straddles
	// it: physical bytes [28,32) then [0,4), matching the C ring_test.c
	// wrap fixture at the same ring size.
	writer.SetIndices(20, 20)

	payload := []byte{1, 2, 3, 4, 5, 6, 7, 8}
	seqno, err := writer.WriteRecord(payload)
	require.NoError(t, err)
	require.Equal(t, uint32(0), seqno)

	reader, err := object.OpenReader(0)
	require.NoError(t, err)

	records := reader.Read(1024)
	require.Len(t, records, 1)
	require.Equal(t, uint64(0), records[0].Worker)
	require.Equal(t, uint32(0), records[0].Seqno)
	require.Equal(t, payload, records[0].Bytes)
}

// Test_Reader_Read_RoundTripAcrossWorkers verifies that each worker's ring
// is read in isolation: a reader opened on one worker never sees another
// worker's records.
func Test_Reader_Read_RoundTripAcrossWorkers(t *testing.T) {
	const workerCount = 3

	agent := newTestAgent(t, workerCount)
	object := newRingObject(t, agent, "workers", 64)

	for workerIdx := range workerCount {
		writer := newWriter(t, object, uint64(workerIdx))
		_, err := writer.WriteRecord([]byte{byte(workerIdx), byte(workerIdx)})
		require.NoError(t, err)
	}

	for workerIdx := range workerCount {
		reader, err := object.OpenReader(uint64(workerIdx))
		require.NoError(t, err)

		records := reader.Read(1024)
		require.Len(t, records, 1)
		require.Equal(t, uint64(workerIdx), records[0].Worker)
		require.Equal(t, []byte{byte(workerIdx), byte(workerIdx)}, records[0].Bytes)
	}
}

// Test_Reader_Read_RoundTripSequentialWrites verifies that two records
// committed one after another on the same worker, before any read, both
// come back in commit order with contiguous seqnos and unchanged bytes.
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

	reader, err := object.OpenReader(0)
	require.NoError(t, err)

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

	reader, err := object.OpenReader(0)
	require.NoError(t, err)

	records := reader.Read(1024)
	require.Len(t, records, 2)
	require.Equal(t, uint32(0xffffffff), records[0].Seqno)
	require.Equal(t, uint32(0), records[1].Seqno)
}

// Test_Reader_WriteRecord_OversizedRejected verifies that a record too
// large for the ring is refused before anything is written: the sequence
// counter does not advance and a reader finds nothing.
func Test_Reader_WriteRecord_OversizedRejected(t *testing.T) {
	agent := newTestAgent(t, 1)
	object := newRingObject(t, agent, "oversized", 32)
	writer := newWriter(t, object, 0)

	before := writer.NextSeqno()

	_, err := writer.WriteRecord(make([]byte, 64))
	require.Error(t, err)
	require.Equal(t, before, writer.NextSeqno())

	reader, err := object.OpenReader(0)
	require.NoError(t, err)
	require.Empty(t, reader.Read(1024))
}

// Test_Reader_Read_CorruptFrameResyncsToWriteBoundary verifies that once a
// corrupted frame forces a read to discard its buffer, the reader resumes
// at the write index snapshotted for that call rather than wherever the
// copy happened to stop, so a later read never reinterprets a record's
// payload bytes as a new frame.
func Test_Reader_Read_CorruptFrameResyncsToWriteBoundary(t *testing.T) {
	agent := newTestAgent(t, 1)
	object := newRingObject(t, agent, "corrupt-frame", 64)
	writer := newWriter(t, object, 0)

	// recordA's payload is two repeated little-endian encodings of the
	// frame size: if a reader ever landed misaligned inside it, every
	// 4-byte word would misparse as a deceptively "valid" empty record
	// instead of a bounds violation, so any resulting garbage is
	// unambiguous rather than an accidental out-of-range reject.
	corruptOffset := writer.WriteIdx()
	_, err := writer.WriteRecord([]byte{0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00})
	require.NoError(t, err)
	writer.CorruptTotalLen(corruptOffset, 0xffffffff)

	_, err = writer.WriteRecord([]byte("BBBBBBBB"))
	require.NoError(t, err)

	reader, err := object.OpenReader(0)
	require.NoError(t, err)

	// A small maxBytes stops the copy inside recordA's own payload, well
	// short of the write index, so a naive cursor would resume mid-record.
	require.Empty(t, reader.Read(12))

	_, err = writer.WriteRecord([]byte("CCCCCCCC"))
	require.NoError(t, err)

	records := reader.Read(1024)
	require.Len(t, records, 1)
	require.Equal(t, []byte("CCCCCCCC"), records[0].Bytes)
}

// Test_Reader_Read_TwoIndependentReadersSeeSameStream verifies that two
// readers opened on the same worker each see every record through their
// own cursor, unaffected by the other reader's progress.
func Test_Reader_Read_TwoIndependentReadersSeeSameStream(t *testing.T) {
	agent := newTestAgent(t, 1)
	object := newRingObject(t, agent, "two-readers", 128)
	writer := newWriter(t, object, 0)

	_, err := writer.WriteRecord([]byte("record-a"))
	require.NoError(t, err)

	readerOne, err := object.OpenReader(0)
	require.NoError(t, err)
	readerTwo, err := object.OpenReader(0)
	require.NoError(t, err)

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

// hookedSource wraps a real RecordSource, running an injected action at a
// controlled point of the read protocol: onCopy right before the delegated
// copy, onSecondIndices right before the delegated recheck of the first
// Read call (its second Indices call). onIndicesCall generalizes this to
// any call, 1-based, across every Read the source lives through, for a
// test that must place the hook on a later call's recheck instead.
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

// Test_Reader_Read_DeterministicOverwrite verifies that a record the
// writer invalidates during a Read call is never returned, whether the
// invalidation lands between the snapshot and the copy or between the copy
// and the recheck, and that the reader recovers cleanly for the next
// genuinely new record.
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

			real, err := object.Source(0)
			require.NoError(t, err)

			fired := false
			evict := func() {
				if fired {
					return
				}
				fired = true
				// Big enough that ring_worker_prepare must evict the
				// "stale" record entirely to make room, advancing
				// readable_idx past everything this Read call snapshotted.
				// The payload is frame-size-shaped words throughout, so a
				// torn or stale reinterpretation of it never trips the
				// out-of-range guard on its own: only the recheck can
				// reject it, which is the property this test pins.
				_, evictErr := writer.WriteRecord(repeatedFrameSizePayload(48))
				require.NoError(t, evictErr)
			}

			hooked := &hookedSource{real: real}
			tc.arm(hooked, evict)

			reader := cring.NewReader(0, object.Capacity(), hooked)
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

// Test_Reader_Read_PartialPrefixDropKeepsSurvivingRecord verifies that when
// an eviction invalidates only the older of two buffered records, the
// recheck drops just that prefix and still returns the younger record
// intact, rather than discarding the whole call's buffer.
func Test_Reader_Read_PartialPrefixDropKeepsSurvivingRecord(t *testing.T) {
	agent := newTestAgent(t, 1)
	object := newRingObject(t, agent, "partial-drop", 128)
	writer := newWriter(t, object, 0)

	_, err := writer.WriteRecord([]byte("AAAAAAAA")) // stale: occupies [0,16)
	require.NoError(t, err)
	_, err = writer.WriteRecord([]byte("survive-me!!")) // occupies [16,36)
	require.NoError(t, err)

	real, err := object.Source(0)
	require.NoError(t, err)

	fired := false
	hooked := &hookedSource{real: real}
	hooked.onSecondIndices = func() {
		if fired {
			return
		}
		fired = true
		// 96 bytes exceeds the 92 free at this point, so prepare evicts
		// exactly the 16-byte stale record and stops: the survivor is
		// never touched, physically or by the recheck's drop range.
		_, evictErr := writer.WriteRecord(repeatedFrameSizePayload(88))
		require.NoError(t, evictErr)
	}

	reader := cring.NewReader(0, object.Capacity(), hooked)
	records := reader.Read(1024)
	require.Len(t, records, 1, "only the invalidated prefix must be dropped")
	require.Equal(t, []byte("survive-me!!"), records[0].Bytes)
}

// Test_Reader_Read_InvalidatesCarriedPartialRecord verifies that an
// eviction invalidates a record correctly even when part of it was
// buffered by an earlier Read call: the recheck accounts for the carried
// bytes, not just what the current call copied.
func Test_Reader_Read_InvalidatesCarriedPartialRecord(t *testing.T) {
	agent := newTestAgent(t, 1)
	object := newRingObject(t, agent, "carried-partial", 64)
	writer := newWriter(t, object, 0)

	// A 24-byte record (frame plus four frame-size words of payload) so
	// a misaligned reinterpretation of any 4-byte-aligned remainder still
	// looks like a plausible record rather than an out-of-range reject.
	_, err := writer.WriteRecord(repeatedFrameSizePayload(16))
	require.NoError(t, err)

	real, err := object.Source(0)
	require.NoError(t, err)

	fired := false
	hooked := &hookedSource{real: real}
	hooked.onIndicesCall = func(call int) {
		// Call 4 is the second Read's recheck: calls 1 and 2 belong to
		// the first Read's snapshot and (no-op) recheck below.
		if call != 4 || fired {
			return
		}
		fired = true
		// 48 bytes exceeds the 40 free while the 24-byte record still
		// fully occupies the ring, so prepare evicts it entirely.
		_, evictErr := writer.WriteRecord(repeatedFrameSizePayload(40))
		require.NoError(t, evictErr)
	}

	reader := cring.NewReader(0, object.Capacity(), hooked)

	// Captures only the frame and the first payload word (12 of the 24
	// bytes), leaving the rest buffered for the next call.
	require.Empty(t, reader.Read(12))

	records := reader.Read(1024)
	require.Empty(t, records, "the carried prefix plus the newly copied bytes must both be dropped")

	_, err = writer.WriteRecord([]byte("fresh"))
	require.NoError(t, err)

	// The evicting write itself committed a genuine record right after
	// the dropped one, so it is legitimately readable now too, ahead of
	// "fresh"; only the originally buffered 24-byte record must be gone.
	records = reader.Read(1024)
	require.Len(t, records, 2)
	require.Equal(t, repeatedFrameSizePayload(40), records[0].Bytes)
	require.Equal(t, []byte("fresh"), records[1].Bytes)
}

// Test_Reader_Read_DropExceedsBufferDiscardsEverything verifies that when
// an eviction invalidates more than this call copied — not just up to the
// end of the copied range — the reader discards the whole buffer rather
// than slicing past it.
func Test_Reader_Read_DropExceedsBufferDiscardsEverything(t *testing.T) {
	agent := newTestAgent(t, 1)
	object := newRingObject(t, agent, "exceeds-buffer", 64)
	writer := newWriter(t, object, 0)

	_, err := writer.WriteRecord([]byte("AAAAAAAA")) // occupies [0,16)
	require.NoError(t, err)
	_, err = writer.WriteRecord([]byte("BBBBBBBB")) // occupies [16,32)
	require.NoError(t, err)

	real, err := object.Source(0)
	require.NoError(t, err)

	fired := false
	hooked := &hookedSource{real: real}
	hooked.onCopy = func() {
		if fired {
			return
		}
		fired = true
		// 40 bytes exceeds the 32 free, so prepare evicts exactly the
		// first 16-byte record, advancing readable_idx to 16 — past the
		// 10 bytes this call's small maxBytes will have copied.
		_, evictErr := writer.WriteRecord(repeatedFrameSizePayload(32))
		require.NoError(t, evictErr)
	}

	reader := cring.NewReader(0, object.Capacity(), hooked)
	records := reader.Read(10)
	require.Empty(t, records, "a drop past the copied range must clear the whole buffer, not slice past it")
}

// Test_Reader_Read_RecordBytesAppendDoesNotCorruptLaterRecords verifies
// that appending to one record's Bytes never reaches into a later
// record's bytes in the same Read call: Bytes is capped to its own
// length, so append always allocates instead of writing into the reader's
// buffer.
func Test_Reader_Read_RecordBytesAppendDoesNotCorruptLaterRecords(t *testing.T) {
	agent := newTestAgent(t, 1)
	object := newRingObject(t, agent, "bytes-append", 128)
	writer := newWriter(t, object, 0)

	_, err := writer.WriteRecord([]byte("first"))
	require.NoError(t, err)
	_, err = writer.WriteRecord([]byte("second"))
	require.NoError(t, err)

	reader, err := object.OpenReader(0)
	require.NoError(t, err)

	records := reader.Read(1024)
	require.Len(t, records, 2)

	// Long enough to reach past "first"'s own backing space and into
	// where "second" lives in the reader's buffer, were Bytes not capped
	// to its own length.
	_ = append(records[0].Bytes, []byte("XXXXXXXXXXXXXXX")...)

	require.Equal(t, []byte("second"), records[1].Bytes)
}

// Test_Parity_RecordFrameSize verifies that cring's record frame size,
// mirrored from the C RING_RECORD_FRAME_SIZE, agrees with the minimum
// capacity the ring proto package enforces independently.
func Test_Parity_RecordFrameSize(t *testing.T) {
	require.Equal(t, uint32(ringpb.MinRingCapacity), cring.RecordFrameSize)
}
