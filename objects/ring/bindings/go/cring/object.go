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

// Capacity reports the per-worker data area size in bytes, fixed at
// creation.
func (m *Object) Capacity() uint32 {
	ptr := m.asRawPtr()
	if ptr == nil {
		return 0
	}
	return uint32(C.ring_object_capacity(ptr))
}

// Sources resolves the RecordSource of every worker's ring through the C
// accessors, so no caller does stride arithmetic across shared memory. The
// slice is indexed by worker.
//
// OpenReaders is the usual entry point; Sources lets a caller wrap a real
// source, for instance to drive the read protocol against externally paced
// writer state.
func (m *Object) Sources() ([]RecordSource, error) {
	return SourcesFromRaw(m.AsRawPtr())
}

// SourcesFromRaw is Sources for a raw ring object pointer, such as one a
// sibling cgo package resolved from a published generation.
func SourcesFromRaw(objPtr unsafe.Pointer) ([]RecordSource, error) {
	if objPtr == nil {
		return nil, errFreed
	}
	ptr := (*C.struct_cp_object)(objPtr)

	// The C accessor resolves no ring past the last worker, which ends the
	// walk without exposing the worker count. The count fits 16 bits, so
	// the walk ends before the index could wrap.
	var sources []RecordSource
	for idx := uint16(0); ; idx++ {
		worker := C.ring_object_worker(ptr, C.uint64_t(idx))
		if worker == nil {
			break
		}
		data := C.ring_object_worker_data(ptr, C.uint64_t(idx))
		if data == nil {
			return nil, fmt.Errorf("worker %d has no data area", idx)
		}

		sources = append(sources, &shmSource{
			writeIdx:    (*uint64)(unsafe.Pointer(&worker.write_idx)),
			readableIdx: (*uint64)(unsafe.Pointer(&worker.readable_idx)),
			data:        unsafe.Slice((*byte)(unsafe.Pointer(data)), uint32(worker.size)),
			mask:        uint64(worker.mask),
		})
	}
	if len(sources) == 0 {
		return nil, errors.New("ring object has no worker rings")
	}
	return sources, nil
}

// OpenReaders opens one independent reader per worker's ring, each starting
// at that ring's current oldest readable record. The slice is indexed by
// worker, and every record a reader returns carries its worker index.
//
// Calling it again opens another set of readers; each keeps its own read
// cursor and does not affect the others.
func (m *Object) OpenReaders() ([]*Reader, error) {
	sources, err := m.Sources()
	if err != nil {
		return nil, err
	}

	capacity := m.Capacity()
	readers := make([]*Reader, 0, len(sources))
	for idx, src := range sources {
		reader, err := NewReader(uint16(idx), capacity, src)
		if err != nil {
			return nil, err
		}
		readers = append(readers, reader)
	}
	return readers, nil
}
