package cring

//#include "objects/ring/dataplane/ring.h"
import "C"

import (
	"encoding/binary"
	"fmt"
	"slices"
	"sync/atomic"

	"github.com/yanet-platform/yanet2/objects/ring/bindings/go/cring/internal/ringabi"
)

// RecordFrameSize is the wire size of the frame preceding every record's
// payload: the smallest declared record length a ring accepts.
const RecordFrameSize = uint32(C.RING_RECORD_FRAME_SIZE)

// Record is one payload parsed out of a worker's ring, tagged with its
// worker and the sequence number the writer stamped at commit.
//
// The payload is owned by the returned records alone (the records of one
// read share one allocation), so it stays unchanged across later reads. Its
// capacity is capped to its length, so appending always allocates instead
// of reaching into a neighbouring record.
type Record struct {
	Worker uint64
	Seqno  uint32
	Bytes  []byte
}

// RecordSource supplies the raw primitives a Reader parses records from:
// the shared index pair and a bounded copy of the ring's data area.
//
// A ring object provides the real shared-memory source; a substitute lets a
// caller drive the read protocol against externally paced writer state.
type RecordSource interface {
	// Indices returns the current write and readable logical positions, in
	// that order.
	Indices() (write, readable uint64)
	// CopyRange fills a destination of exactly the requested byte count
	// from a logical offset of the data area, wrapping at its physical end.
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

// Reader parses committed records out of one worker's ring, keeping a read
// cursor independent of any other reader over the same worker.
//
// Read must not run concurrently with itself; HasMore may be polled from
// another goroutine while Read runs.
type Reader struct {
	worker   uint64
	capacity uint32
	src      RecordSource

	readIdx atomic.Uint64
	// buf is scratch reused across calls and never handed to a caller.
	//
	// Between calls it holds only the carried prefix of a record not yet
	// fully copied, compacted to its front.
	buf []byte
}

// NewReader builds a Reader that tags every parsed record with its worker
// and bounds each declared record length by the ring's capacity.
//
// Fails for a capacity below the frame size: no record fits such a ring,
// and every declared length would be rejected as corruption.
func NewReader(worker uint64, capacity uint32, src RecordSource) (*Reader, error) {
	if capacity < RecordFrameSize {
		return nil, fmt.Errorf("ring capacity %d is below the record frame size %d", capacity, RecordFrameSize)
	}
	return &Reader{worker: worker, capacity: capacity, src: src}, nil
}

// HasMore reports whether the ring holds data this Reader has not yet
// consumed.
func (m *Reader) HasMore() bool {
	write, _ := m.src.Indices()
	return write > m.readIdx.Load()
}

// Read copies up to the given byte budget of newly readable data and parses
// the whole records it completes.
//
// After copying, the cursor advances and a recheck of the readable position
// drops any prefix the writer invalidated mid-copy, so no caller sees a
// trampled record. A declared length outside [frame size, capacity] is
// corruption: the records parsed before it are returned, the rest of the
// buffer is dropped, and the cursor resumes at the write position of this
// call's snapshot. A steady-state call allocates only for the records it
// returns: one slice and one payload block, none when none completed.
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
	m.buf = slices.Grow(m.buf, int(size))[:after]
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
		m.dropPrefix(int(diff))
	}

	// First pass: count whole records and their payload bytes, so the
	// second pass allocates exactly once for each.
	parsed := 0
	count := 0
	payloadBytes := 0
	corrupt := false
	for len(m.buf)-parsed >= int(RecordFrameSize) {
		totalLen := binary.LittleEndian.Uint32(m.buf[parsed : parsed+4])
		if totalLen < RecordFrameSize || totalLen > m.capacity {
			corrupt = true
			break
		}
		skip := int(ringabi.Align4(totalLen))
		if skip > len(m.buf)-parsed {
			break
		}
		count++
		payloadBytes += int(totalLen - RecordFrameSize)
		parsed += skip
	}

	var records []Record
	if count > 0 {
		records = make([]Record, 0, count)
		payloads := make([]byte, payloadBytes)
		for offset := 0; offset < parsed; {
			totalLen := binary.LittleEndian.Uint32(m.buf[offset : offset+4])
			seqno := binary.LittleEndian.Uint32(m.buf[offset+4 : offset+8])

			n := copy(payloads, m.buf[offset+int(RecordFrameSize):offset+int(totalLen)])
			records = append(records, Record{
				Worker: m.worker,
				Seqno:  seqno,
				Bytes:  payloads[:n:n],
			})
			payloads = payloads[n:]
			offset += int(ringabi.Align4(totalLen))
		}
	}

	if corrupt {
		// Drop the untrusted remainder and resume at the snapshot's write
		// position.
		//
		// The writer advances that position only by whole committed
		// records, so the next read starts at a real frame instead of
		// parsing payload bytes as a header.
		m.buf = m.buf[:0]
		m.readIdx.Store(write)
		return records
	}

	m.dropPrefix(parsed)
	return records
}

// dropPrefix discards a buffered prefix, compacting the rest to the front so
// the capacity keeps being reused.
//
// Safe because no returned record aliases the buffer.
func (m *Reader) dropPrefix(n int) {
	m.buf = m.buf[:copy(m.buf, m.buf[n:])]
}
