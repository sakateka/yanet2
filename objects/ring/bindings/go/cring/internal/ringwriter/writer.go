// Package ringwriter drives the C ring writer primitives directly against
// one worker's ring, for cring tests that need to control eviction and
// commit timing precisely against a concurrent Go reader.
//
// Production code never writes ring records from Go: the dataplane is the
// sole writer of a ring's data area.
package ringwriter

//#cgo CFLAGS: -I../../../../../../../
//#cgo LDFLAGS: -L../../../../../../../build/objects/ring/api -lring_objects
//
//#include "objects/ring/api/ring_object.h"
import "C"

import (
	"encoding/binary"
	"fmt"
	"sync/atomic"
	"unsafe"
)

// Writer drives the writer primitives for one worker of a ring object
// resolved from a raw cp_object pointer, such as cring.Object.AsRawPtr.
type Writer struct {
	worker *C.struct_ring_worker
	data   *C.uint8_t
}

// NewWriter resolves the writer primitives for one worker of the ring
// object at objPtr.
func NewWriter(objPtr unsafe.Pointer, workerIdx uint64) (*Writer, error) {
	cpObject := (*C.struct_cp_object)(objPtr)

	worker := C.ring_object_worker(cpObject, C.uint64_t(workerIdx))
	if worker == nil {
		return nil, fmt.Errorf("worker index %d has no ring", workerIdx)
	}
	data := C.ring_object_worker_data(cpObject, C.uint64_t(workerIdx))
	if data == nil {
		return nil, fmt.Errorf("worker index %d has no data area", workerIdx)
	}

	return &Writer{worker: worker, data: data}, nil
}

// NextSeqno peeks the sequence number the next commit would stamp, without
// consuming it.
func (m *Writer) NextSeqno() uint32 {
	return uint32(m.worker.next_seqno)
}

// SetNextSeqno overwrites the sequence counter, letting a test position it
// at the wrap boundary without first writing billions of records.
func (m *Writer) SetNextSeqno(seqno uint32) {
	m.worker.next_seqno = C.uint32_t(seqno)
}

// SetIndices forces the shared write and readable positions, letting a
// test engineer a physical wrap or a preset backlog without writing enough
// records to reach it naturally.
func (m *Writer) SetIndices(write, readable uint64) {
	atomic.StoreUint64((*uint64)(unsafe.Pointer(&m.worker.write_idx)), write)
	atomic.StoreUint64((*uint64)(unsafe.Pointer(&m.worker.readable_idx)), readable)
}

// WriteIdx returns the current shared write position: the logical offset
// the next committed record will start at.
func (m *Writer) WriteIdx() uint64 {
	return atomic.LoadUint64((*uint64)(unsafe.Pointer(&m.worker.write_idx)))
}

// CorruptTotalLen overwrites the total_len field of the frame at the given
// logical offset, simulating a corrupted or torn record header without a
// real writer race.
func (m *Writer) CorruptTotalLen(logicalOffset uint64, totalLen uint32) {
	var frame [4]byte
	binary.LittleEndian.PutUint32(frame[:], totalLen)

	mask := uint64(m.worker.mask)
	data := unsafe.Slice((*byte)(unsafe.Pointer(m.data)), uint32(m.worker.size))
	for idx, b := range frame {
		data[(logicalOffset+uint64(idx))&mask] = b
	}
}

// WriteRecord prepares, writes and commits one opaque record in a single
// call, returning the seqno it was stamped with.
//
// Reports an error without writing anything when the record does not fit
// the ring, matching ring_worker_prepare's contract.
func (m *Writer) WriteRecord(payload []byte) (uint32, error) {
	totalLen := uint32(C.RING_RECORD_FRAME_SIZE) + uint32(len(payload))

	if rc, errno := C.ring_worker_prepare(m.worker, m.data, C.uint32_t(totalLen)); rc != 0 {
		return 0, fmt.Errorf("ring_worker_prepare: %w", errno)
	}

	if len(payload) > 0 {
		C.ring_worker_write(
			m.worker,
			m.data,
			C.uint64_t(C.RING_RECORD_FRAME_SIZE),
			(*C.uint8_t)(unsafe.Pointer(&payload[0])),
			C.uint64_t(len(payload)),
		)
	}

	seqno := C.ring_worker_commit(m.worker, m.data, C.uint32_t(totalLen))
	return uint32(seqno), nil
}
