package cring

//#include "objects/ring/dataplane/ring.h"
import "C"

import (
	"encoding/binary"
	"sync/atomic"

	"github.com/yanet-platform/yanet2/objects/ring/bindings/go/cring/internal/ringabi"
)

// RecordFrameSize is the wire size of the frame preceding every record's
// payload: the smallest declared record length a ring accepts.
const RecordFrameSize = uint32(C.RING_RECORD_FRAME_SIZE)

// Record is one payload a Reader parsed out of a worker's ring, tagged with
// the worker it came from and the seqno the writer stamped it with at
// commit.
//
// Bytes aliases the Reader's own buffer and stays valid and unchanged
// across later Read calls on the same Reader: that buffer only ever grows
// at the end or drops from the front, never overwrites a record already
// handed out. Its capacity is capped to its length, so appending to it
// always allocates instead of reaching into the reader's buffer.
type Record struct {
	Worker uint64
	Seqno  uint32
	Bytes  []byte
}

// RecordSource supplies the raw primitives a Reader parses records from:
// the shared write/readable index pair and a bounded copy of the ring's
// data area. Object.Source resolves the real source through the C
// accessors; a substitute lets a caller drive the read protocol against
// externally paced writer state.
type RecordSource interface {
	// Indices returns the current write and readable logical positions, in
	// that order.
	Indices() (write, readable uint64)
	// CopyRange copies size bytes of the ring's data area starting at the
	// logical offset start into dst, wrapping at the ring's physical
	// boundary. dst must have length size.
	CopyRange(dst []byte, start, size uint64)
}

// shmSource is the RecordSource backed by one worker's real shared-memory
// ring.
type shmSource struct {
	writeIdx    *uint64
	readableIdx *uint64
	data        []byte
	mask        uint64
}

func (m *shmSource) Indices() (write, readable uint64) {
	return atomic.LoadUint64(m.writeIdx), atomic.LoadUint64(m.readableIdx)
}

func (m *shmSource) CopyRange(dst []byte, start, size uint64) {
	if size == 0 {
		return
	}

	startPos := start & m.mask
	endPos := (startPos + size) & m.mask
	if endPos > startPos {
		copy(dst, m.data[startPos:startPos+size])
		return
	}

	n := copy(dst, m.data[startPos:])
	copy(dst[n:], m.data[:endPos])
}

// Reader parses committed records out of one worker's ring through a
// RecordSource, keeping a read cursor independent of any other reader over
// the same worker.
//
// Read must not be called concurrently with itself; HasMore may be polled
// from another goroutine while Read runs, mirroring a capture loop's
// waker.
type Reader struct {
	worker   uint64
	capacity uint32
	src      RecordSource

	readIdx atomic.Uint64
	buf     []byte
}

// NewReader builds a Reader over the given RecordSource, tagging every
// parsed record with worker and bounding a record's declared length
// against capacity.
func NewReader(worker uint64, capacity uint32, src RecordSource) *Reader {
	return &Reader{worker: worker, capacity: capacity, src: src}
}

// HasMore reports whether the ring holds data this Reader has not yet
// consumed.
func (m *Reader) HasMore() bool {
	write, _ := m.src.Indices()
	return write > m.readIdx.Load()
}

// Read copies up to maxBytes of newly readable bytes from the source and
// parses whatever whole records that yields.
//
// The protocol is snapshot the shared indices, copy the readable range
// into a private buffer, advance this reader's cursor, then recheck the
// shared readable index: if the writer invalidated part of what was just
// copied, the corresponding prefix is dropped before parsing, so no caller
// ever sees a record the writer trampled mid-copy. A declared record
// length outside [frame size, capacity] is treated as corruption and the
// whole buffer is discarded rather than trusted as a bound.
func (m *Reader) Read(maxBytes uint32) []Record {
	write, readable := m.src.Indices()

	if readable > m.readIdx.Load() {
		// The writer evicted data this reader had not reached yet: any
		// partial record buffered from a previous call is now stale.
		m.buf = m.buf[:0]
		m.readIdx.Store(readable)
	} else {
		readable = m.readIdx.Load()
	}

	if write <= readable {
		return nil
	}

	size := min(write-readable, uint64(maxBytes))

	before := len(m.buf)
	after := before + int(size)
	if cap(m.buf) < after {
		grown := make([]byte, after)
		copy(grown, m.buf)
		m.buf = grown
	} else {
		m.buf = m.buf[:after]
	}
	m.src.CopyRange(m.buf[before:after], readable, size)

	// Both this atomic add and the atomic reload below are load-bearing on
	// arm64: never make either a plain access or move the recheck above it.
	//
	// Besides advancing the cursor another goroutine polls, the add's
	// release half pairs with the acquire reload (RCsc release then
	// acquire) so the copy above completes before the recheck reads the
	// readable position; otherwise the recheck could miss an eviction
	// whose overwrite the copy already saw. On x86-64 the ordering comes
	// from TSO, but the same code must stay correct on both.
	m.readIdx.Add(size)

	_, latest := m.src.Indices()
	if latest > readable {
		diff := latest - readable + uint64(before)
		if diff > uint64(len(m.buf)) {
			m.buf = m.buf[:0]
			m.readIdx.Store(latest)
			return nil
		}
		m.buf = m.buf[diff:]
	}

	var records []Record
	for len(m.buf) >= int(RecordFrameSize) {
		totalLen := binary.LittleEndian.Uint32(m.buf[0:4])
		seqno := binary.LittleEndian.Uint32(m.buf[4:8])

		if totalLen < RecordFrameSize || totalLen > m.capacity {
			// The rest of this call's buffer cannot be trusted, but the
			// write index captured at the top of this call is a genuine
			// record boundary: the writer only ever advances it by whole
			// committed records. Resuming there next time, rather than at
			// wherever this call's copy happened to stop, keeps the next
			// read aligned to a real frame instead of parsing a record's
			// payload bytes as if they were a header.
			m.buf = m.buf[:0]
			m.readIdx.Store(write)
			return records
		}

		skip := ringabi.Align4(totalLen)
		if int(skip) > len(m.buf) {
			return records
		}

		records = append(records, Record{
			Worker: m.worker,
			Seqno:  seqno,
			Bytes:  m.buf[RecordFrameSize:totalLen:totalLen],
		})

		m.buf = m.buf[skip:]
	}

	return records
}
