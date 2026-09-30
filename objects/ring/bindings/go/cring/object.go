// Package cring provides Go bindings for the standalone ring object: a
// named, per-worker overwrite-oldest record buffer with no dataplane module.
package cring

//#cgo CFLAGS: -I../../../../../
//#cgo LDFLAGS: -L../../../../../build/objects/ring/api -lring_objects
//
//#include "api/agent.h"
//#include "objects/ring/api/ring_object.h"
import "C"

import (
	"errors"
	"fmt"
	"unsafe"

	"github.com/yanet-platform/yanet2/bindings/go/cerrors"
	"github.com/yanet-platform/yanet2/controlplane/ffi"
)

// ObjectType is the registered shared-memory object type for a ring.
const ObjectType = C.RING_OBJECT_TYPE

// MaxNameLen is the C object-name buffer size, including the terminating
// NUL. The longest accepted name is one byte shorter than this bound.
const MaxNameLen = C.CP_OBJECT_NAME_LEN

// Object is an opaque handle to a standalone named ring object in shared
// memory, owned by the control plane until it is freed.
type Object struct {
	ptr   ffi.ObjectConfig
	agent *ffi.Agent
}

// NewObject creates a new ring with the given fixed per-worker capacity.
//
// The returned handle is not yet published to the dataplane; call Publish.
// A capacity the C layer rejects — not a power of two, below the record
// frame size, or above the allocator's maximum block — is reported through
// the returned error, distinguishable with errors.Is against
// cerrors.InvalidArgument. It does not check the name against what is
// already published; a caller that must reject a duplicate name does that
// check itself.
func NewObject(agent *ffi.Agent, name string, capacity uint32) (*Object, error) {
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))

	var cErr *C.yanet_error
	ptr := C.ring_object_config_new(
		(*C.struct_agent)(agent.AsRawPtr()), cName, C.uint32_t(capacity), &cErr,
	)
	if ptr == nil {
		return nil, fmt.Errorf("failed to create ring object: %w", cerrors.FromC(unsafe.Pointer(cErr)))
	}

	return &Object{
		ptr:   ffi.NewObjectConfig(unsafe.Pointer(ptr)),
		agent: agent,
	}, nil
}

// AsRawPtr returns the underlying C cp_object pointer as unsafe.Pointer,
// for a sibling CGo package that needs to reach the object directly.
func (m *Object) AsRawPtr() unsafe.Pointer {
	return m.ptr.AsRawPtr()
}

func (m *Object) asRawPtr() *C.struct_cp_object {
	return (*C.struct_cp_object)(m.ptr.AsRawPtr())
}

// errFreed reports a method call on a handle whose object Free destroyed.
var errFreed = errors.New("ring object already freed")

// Publish upserts the object into a new configuration generation. A module
// linking it by name follows it from then on.
func (m *Object) Publish() error {
	return m.agent.UpdateObjects([]ffi.ObjectConfig{m.ptr})
}

// Free destroys the object, or reports ffi.ErrStillReferenced while a live
// generation still holds it; the handle then stays usable for a retry.
//
// Safe to call multiple times. After a successful free the handle is inert:
// its size accessors report 0, and opening a source or reader and
// publishing are refused.
func (m *Object) Free() error {
	return m.ptr.Free(func(ptr unsafe.Pointer) (int, unsafe.Pointer, error) {
		var cErr *C.yanet_error
		rc, errno := C.ring_object_config_free((*C.struct_cp_object)(ptr), &cErr)
		return int(rc), unsafe.Pointer(cErr), errno
	})
}

// Exists reports whether a ring of the given name is in the agent's
// currently published configuration generation.
func Exists(agent *ffi.Agent, name string) bool {
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))

	return bool(C.ring_object_exists((*C.struct_agent)(agent.AsRawPtr()), cName))
}

// DeleteObject removes the named ring from the dataplane.
//
// Refused while a module config still links the ring, distinguishable with
// errors.Is against ffi.ErrBusy.
func DeleteObject(agent *ffi.Agent, name string) error {
	return agent.DeleteObject(ObjectType, name)
}

// WorkerCount reports the number of per-worker rings behind this object,
// fixed at creation from the dataplane's configured worker count.
func (m *Object) WorkerCount() uint64 {
	ptr := m.asRawPtr()
	if ptr == nil {
		return 0
	}
	return uint64(C.ring_object_worker_count(ptr))
}

// Capacity reports the per-worker data area size in bytes, fixed at
// creation.
func (m *Object) Capacity() uint32 {
	ptr := m.asRawPtr()
	if ptr == nil {
		return 0
	}
	return uint32(C.ring_object_capacity(ptr))
}

// Source resolves the RecordSource for one worker's ring through the C
// accessors, so no caller does stride arithmetic across shared memory.
//
// OpenReader is the usual entry point; Source lets a caller wrap the real
// source, for instance to drive the read protocol against externally paced
// writer state.
func (m *Object) Source(workerIdx uint64) (RecordSource, error) {
	return SourceFromRaw(m.AsRawPtr(), workerIdx)
}

// SourceFromRaw is Source for a raw ring object pointer, such as one a
// sibling cgo package resolved from a published generation.
func SourceFromRaw(objPtr unsafe.Pointer, workerIdx uint64) (RecordSource, error) {
	if objPtr == nil {
		return nil, errFreed
	}
	ptr := (*C.struct_cp_object)(objPtr)

	count := uint64(C.ring_object_worker_count(ptr))
	if workerIdx >= count {
		return nil, fmt.Errorf("worker index %d exceeds worker count %d", workerIdx, count)
	}

	worker := C.ring_object_worker(ptr, C.uint64_t(workerIdx))
	if worker == nil {
		return nil, fmt.Errorf("worker index %d has no ring", workerIdx)
	}
	data := C.ring_object_worker_data(ptr, C.uint64_t(workerIdx))
	if data == nil {
		return nil, fmt.Errorf("worker index %d has no data area", workerIdx)
	}

	return &shmSource{
		writeIdx:    (*uint64)(unsafe.Pointer(&worker.write_idx)),
		readableIdx: (*uint64)(unsafe.Pointer(&worker.readable_idx)),
		data:        unsafe.Slice((*byte)(unsafe.Pointer(data)), uint32(worker.size)),
		mask:        uint64(worker.mask),
	}, nil
}

// OpenReader opens an independent reader over one worker's ring, starting
// at the ring's current oldest readable record.
//
// Multiple readers may be opened over the same worker; each keeps its own
// read cursor and does not affect the others.
func (m *Object) OpenReader(workerIdx uint64) (*Reader, error) {
	src, err := m.Source(workerIdx)
	if err != nil {
		return nil, err
	}
	return NewReader(workerIdx, m.Capacity(), src)
}
