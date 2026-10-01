// Package ringabi holds the record rounding the Go reader shares with the C
// writer and probes comparing the archive's worker layout with cgo's view.
//
// Every Go package that includes the ring header compiles it with the same
// global cgo flags, so this package's cgo view stands for the reader's.
package ringabi

//#cgo CFLAGS: -I../../../../../../../
//#cgo LDFLAGS: -L../../../../../../../build/objects/ring/api -lring_objects
//
//#include "objects/ring/api/ring_layout.h"
//#include "objects/ring/dataplane/ring.h"
//
//static inline uint64_t cgo_ring_worker_align(void) {
//	return _Alignof(struct ring_worker);
//}
//
//static inline uint64_t cgo_cache_line_size(void) {
//	return YANET_CACHE_LINE_SIZE;
//}
import "C"

import "unsafe"

// Align4 rounds a record length up to the 4-byte boundary every record
// starts at, wrapping like the C writer's 32-bit arithmetic.
func Align4(totalLen uint32) uint32 {
	return (totalLen + 3) &^ 3
}

// WorkerLayout is the layout of the per-worker ring metadata as one
// compilation unit sees it.
type WorkerLayout struct {
	Size              uint64
	Align             uint64
	CacheLineSize     uint64
	WriteIdxOffset    uint64
	ReadableIdxOffset uint64
	SizeOffset        uint64
	MaskOffset        uint64
}

// ArchiveWorkerLayout returns the layout the linked C archive was compiled
// with.
func ArchiveWorkerLayout() WorkerLayout {
	layout := C.ring_layout_worker()
	return WorkerLayout{
		Size:              uint64(layout.size),
		Align:             uint64(layout.align),
		CacheLineSize:     uint64(layout.cache_line_size),
		WriteIdxOffset:    uint64(layout.write_idx_offset),
		ReadableIdxOffset: uint64(layout.readable_idx_offset),
		SizeOffset:        uint64(layout.size_offset),
		MaskOffset:        uint64(layout.mask_offset),
	}
}

// CgoWorkerLayout returns the layout cgo uses when Go code reads worker
// fields out of shared memory.
func CgoWorkerLayout() WorkerLayout {
	var worker C.struct_ring_worker
	return WorkerLayout{
		Size:              uint64(C.sizeof_struct_ring_worker),
		Align:             uint64(C.cgo_ring_worker_align()),
		CacheLineSize:     uint64(C.cgo_cache_line_size()),
		WriteIdxOffset:    uint64(unsafe.Offsetof(worker.write_idx)),
		ReadableIdxOffset: uint64(unsafe.Offsetof(worker.readable_idx)),
		SizeOffset:        uint64(unsafe.Offsetof(worker.size)),
		MaskOffset:        uint64(unsafe.Offsetof(worker.mask)),
	}
}

// CgoWorkerGoSize returns the size of the Go type cgo generates for the
// worker metadata.
//
// A pointer cast from shared memory relies on it matching the C size.
func CgoWorkerGoSize() uint64 {
	return uint64(unsafe.Sizeof(C.struct_ring_worker{}))
}
