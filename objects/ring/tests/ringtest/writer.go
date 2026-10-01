package ringtest

//#cgo CFLAGS: -I../../../../
//#cgo LDFLAGS: -L../../../../build/objects/ring/api -lring_objects
//#cgo LDFLAGS: -L../../../../build/lib/controlplane/config -lconfig_cp
//
//#include <stdlib.h>
//
//#include "common/memory_address.h"
//#include "lib/controlplane/agent/agent.h"
//#include "lib/controlplane/config/zone.h"
//#include "objects/ring/api/ring_object.h"
//
//// ringtest_writer_lookup_object resolves the ring published under name in
//// agent's live generation, the same resolution ring_object_exists
//// performs.
//static inline struct cp_object *
//ringtest_writer_lookup_object(struct agent *agent, const char *name) {
//	struct cp_config *cp_config = ADDR_OF(&agent->cp_config);
//	cp_config_lock(cp_config);
//	struct cp_config_gen *gen = ADDR_OF(&cp_config->cp_config_gen);
//	struct cp_object *object =
//		cp_config_gen_lookup_object(gen, RING_OBJECT_TYPE, name);
//	cp_config_unlock(cp_config);
//	return object;
//}
import "C"

import (
	"encoding/binary"
	"fmt"
	"unsafe"

	"github.com/yanet-platform/yanet2/controlplane/ffi"
	"github.com/yanet-platform/yanet2/objects/ring/bindings/go/cring"
)

// Writer drives the writer primitives for one worker of a ring object
// resolved from a raw object pointer.
type Writer struct {
	object    unsafe.Pointer
	workerIdx uint16
	worker    *C.struct_ring_worker
	data      *C.uint8_t
}

// NewWriter resolves the writer primitives for one worker of a raw ring
// object pointer.
func NewWriter(objPtr unsafe.Pointer, workerIdx uint16) (*Writer, error) {
	cpObject := (*C.struct_cp_object)(objPtr)

	worker := C.ring_object_worker(cpObject, C.uint64_t(workerIdx))
	if worker == nil {
		return nil, fmt.Errorf("worker index %d has no ring", workerIdx)
	}
	data := C.ring_object_worker_data(cpObject, C.uint64_t(workerIdx))
	if data == nil {
		return nil, fmt.Errorf("worker index %d has no data area", workerIdx)
	}

	return &Writer{object: objPtr, workerIdx: workerIdx, worker: worker, data: data}, nil
}

// NewPublishedWriter resolves the writer primitives for one worker of the
// ring published under a name, as its owner service exposes it.
func NewPublishedWriter(agent *ffi.Agent, name string, workerIdx uint16) (*Writer, error) {
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))

	object := C.ringtest_writer_lookup_object((*C.struct_agent)(agent.AsRawPtr()), cName)
	if object == nil {
		return nil, fmt.Errorf("ring %q is not published", name)
	}
	return NewWriter(unsafe.Pointer(object), workerIdx)
}

// Object returns the raw ring object pointer this writer resolved its
// worker from.
func (m *Writer) Object() unsafe.Pointer {
	return m.object
}

// Capacity reports the worker's data area size in bytes.
func (m *Writer) Capacity() uint32 {
	return uint32(m.worker.local.size)
}

// Source returns the production record source for this writer's worker, so
// a test reads back through the same path a real reader uses.
func (m *Writer) Source() (cring.RecordSource, error) {
	sources, err := cring.SourcesFromRaw(m.object)
	if err != nil {
		return nil, err
	}
	if int(m.workerIdx) >= len(sources) {
		return nil, fmt.Errorf("worker index %d has no ring source", m.workerIdx)
	}
	return sources[m.workerIdx], nil
}

// SetIndices forces the write and readable positions, the writer's own and
// the published ones alike, letting a test set up a physical wrap or
// backlog without writing records to reach it.
func (m *Writer) SetIndices(write, readable uint64) {
	C.ring_worker_set_positions(m.worker, C.uint64_t(write), C.uint64_t(readable))
}

// WriteIdx returns the writer's own write position: the logical offset the
// next committed record will start at, published or not.
func (m *Writer) WriteIdx() uint64 {
	return uint64(m.worker.local.write_idx)
}

// CorruptTotalLen overwrites the length of the frame at a logical offset,
// simulating a corrupt record header without a real writer race.
func (m *Writer) CorruptTotalLen(logicalOffset uint64, totalLen uint32) {
	var frame [4]byte
	binary.LittleEndian.PutUint32(frame[:], totalLen)

	mask := uint64(m.worker.local.mask)
	data := unsafe.Slice((*byte)(unsafe.Pointer(m.data)), uint32(m.worker.local.size))
	for idx, b := range frame {
		data[(logicalOffset+uint64(idx))&mask] = b
	}
}

// WriteRecord commits one opaque record and publishes it in a single call,
// returning the seqno it was stamped with.
//
// Reports an error without writing anything when the record does not fit
// the ring, matching the C writer's contract.
func (m *Writer) WriteRecord(payload []byte) (uint32, error) {
	seqno, err := m.CommitRecord(payload)
	if err != nil {
		return 0, err
	}
	m.Publish()
	return seqno, nil
}

// CommitRecord prepares, writes and commits one opaque record into the
// unpublished batch, returning the seqno it was stamped with; readers see
// it after the next Publish, or once the ring publishes the batch on its
// own: when the record fills the publish batch, or when a later record
// would take the batch past BatchRoom.
//
// Reports an error without writing anything when the record does not fit
// the ring, matching the C writer's contract.
func (m *Writer) CommitRecord(payload []byte) (uint32, error) {
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

// Publish makes every record committed since the last publication visible
// to readers.
func (m *Writer) Publish() {
	C.ring_worker_publish(m.worker)
}

// BatchRoom reports how many more bytes, in aligned record lengths, the
// unpublished batch may grow by.
func (m *Writer) BatchRoom() uint64 {
	return uint64(C.ring_worker_batch_room(m.worker))
}
