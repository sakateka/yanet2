package pdump

//#cgo CFLAGS: -I../../../ -I../dataplane -I../../../subprojects/libpcap
//#cgo LDFLAGS: -L../../../build/modules/pdump/api -lpdump_cp
//#cgo LDFLAGS: -L../../../build/subprojects/libpcap -l:libpcap_static.a
//
//#include <stddef.h>
//#include <stdlib.h>
//#include "modules/pdump/api/controlplane.h"
//
//// Exposes struct pdump_record_hdr's field offsets to Go, so a test can
//// assert the Go decoder reads each field where the C struct actually
//// puts it, with no separate C source file.
//enum {
//	pdump_record_hdr_magic_offset = offsetof(struct pdump_record_hdr, magic),
//	pdump_record_hdr_packet_len_offset = offsetof(struct pdump_record_hdr, packet_len),
//	pdump_record_hdr_timestamp_offset = offsetof(struct pdump_record_hdr, timestamp),
//	pdump_record_hdr_worker_idx_offset = offsetof(struct pdump_record_hdr, worker_idx),
//	pdump_record_hdr_pipeline_idx_offset = offsetof(struct pdump_record_hdr, pipeline_idx),
//	pdump_record_hdr_rx_device_id_offset = offsetof(struct pdump_record_hdr, rx_device_id),
//	pdump_record_hdr_tx_device_id_offset = offsetof(struct pdump_record_hdr, tx_device_id),
//	pdump_record_hdr_queue_offset = offsetof(struct pdump_record_hdr, queue),
//};
import "C"

import (
	"bytes"
	"errors"
	"fmt"
	"runtime/cgo"
	"strings"
	"unsafe"

	"go.uber.org/zap"

	"github.com/yanet-platform/yanet2/bindings/go/cerrors"
	"github.com/yanet-platform/yanet2/controlplane/ffi"
)

var (
	logger    *zap.Logger
	debugEBPF bool

	defaultSnaplen = uint32(C.default_snaplen)
	replacer       = strings.NewReplacer("\n", "\\n")

	defaultMode uint32 = C.PDUMP_INPUT
	maxMode     uint32 = C.PDUMP_ALL
)

// pdumpRecordHdrSize is the size of the fixed metadata block a record
// carries behind the ring's own frame.
//
// It mirrors struct pdump_record_hdr (modules/pdump/dataplane/record.h).
const pdumpRecordHdrSize = uint32(C.sizeof_struct_pdump_record_hdr)

// pdumpRecordMagic identifies a pdump record's metadata block. A reader
// checks it before trusting the rest of the block.
//
// It mirrors PDUMP_RECORD_MAGIC (modules/pdump/dataplane/record.h).
var pdumpRecordMagic = uint32(C.pdump_record_magic)

// pdumpRecordHdr*Offset are struct pdump_record_hdr's field offsets
// (modules/pdump/dataplane/record.h), used only to assert in a test that
// the Go decoder reads each field where the C struct actually puts it.
const (
	pdumpRecordHdrMagicOffset       = uintptr(C.pdump_record_hdr_magic_offset)
	pdumpRecordHdrPacketLenOffset   = uintptr(C.pdump_record_hdr_packet_len_offset)
	pdumpRecordHdrTimestampOffset   = uintptr(C.pdump_record_hdr_timestamp_offset)
	pdumpRecordHdrWorkerIdxOffset   = uintptr(C.pdump_record_hdr_worker_idx_offset)
	pdumpRecordHdrPipelineIdxOffset = uintptr(C.pdump_record_hdr_pipeline_idx_offset)
	pdumpRecordHdrRxDeviceIDOffset  = uintptr(C.pdump_record_hdr_rx_device_id_offset)
	pdumpRecordHdrTxDeviceIDOffset  = uintptr(C.pdump_record_hdr_tx_device_id_offset)
	pdumpRecordHdrQueueOffset       = uintptr(C.pdump_record_hdr_queue_offset)
)

//export pdumpGoControlplaneLog
func pdumpGoControlplaneLog(level C.uint32_t, msg *C.char) {
	if logger == nil {
		return
	}
	goMsg := C.GoString(msg)
	switch level {
	case C.log_emerg, C.log_alert, C.log_crit:
		logger.Sugar().Errorf("CRIT: %s", replacer.Replace(goMsg))
	case C.log_error:
		logger.Sugar().Errorf("%s", goMsg) // format for suppressing trace

	case C.log_warn:
		logger.Warn(replacer.Replace(goMsg))
	case C.log_notice, C.log_info:
		logger.Info(replacer.Replace(goMsg))
	case C.log_debug:
		if strings.HasPrefix(goMsg, "BPF: ") && !debugEBPF {
			return
		}
		logger.Debug(replacer.Replace(goMsg))
	}
}

//export goErrorCallback
func goErrorCallback(h C.uintptr_t, msg *C.char) {
	fn := cgo.Handle(h).Value().(func(*C.char))
	fn(msg)
}

// errorCallbackContext manages the lifecycle of a CGO error callback handle.
// It captures error messages from C code and provides them as a string.
type errorCallbackContext struct {
	buf    bytes.Buffer
	handle cgo.Handle
}

// newErrorCallbackContext creates a new error callback context.
// The returned context must be closed with Close() to free the CGO handle.
func newErrorCallbackContext() *errorCallbackContext {
	ctx := &errorCallbackContext{}
	ctx.handle = cgo.NewHandle(func(msg *C.char) {
		goMsg := C.GoString(msg)
		ctx.buf.WriteString(goMsg)
	})
	return ctx
}

// Handle returns the CGO handle that can be passed to C functions.
func (e *errorCallbackContext) Handle() C.uintptr_t {
	return C.uintptr_t(e.handle)
}

// Reason returns the accumulated error messages from C code.
func (e *errorCallbackContext) Reason() string {
	return e.buf.String()
}

// Close releases the CGO handle. Must be called to prevent handle leaks.
func (e *errorCallbackContext) Close() {
	e.handle.Delete()
}

type ModuleConfig struct {
	ptr ffi.ModuleConfig
}

func NewModuleConfig(agent *ffi.Agent, name string) (*ModuleConfig, error) {
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))

	// Create a new module config using the C API
	var cErr *C.yanet_error
	ptr := C.pdump_module_config_new((*C.struct_agent)(agent.AsRawPtr()), cName, &cErr)
	if ptr == nil {
		return nil, fmt.Errorf("failed to create module config: %w", cerrors.FromC(unsafe.Pointer(cErr)))
	}

	return &ModuleConfig{
		ptr: ffi.NewModuleConfig(unsafe.Pointer(ptr)),
	}, nil
}

func (m *ModuleConfig) asRawPtr() *C.struct_cp_module {
	return (*C.struct_cp_module)(m.ptr.AsRawPtr())
}

func (m *ModuleConfig) AsFFIModule() ffi.ModuleConfig {
	return m.ptr
}

// Free destroys the module config, or reports ffi.ErrStillReferenced while a
// live generation still holds it. Safe to call multiple times.
func (m *ModuleConfig) Free() error {
	return m.ptr.Free(func(ptr unsafe.Pointer) (int, unsafe.Pointer, error) {
		var cErr *C.yanet_error
		rc, errno := C.pdump_module_config_free((*C.struct_cp_module)(ptr), &cErr)
		return int(rc), unsafe.Pointer(cErr), errno
	})
}

func (m *ModuleConfig) SetFilter(filter string) error {
	cFilter := C.CString(filter)
	defer C.free(unsafe.Pointer(cFilter))

	errCtx := newErrorCallbackContext()
	defer errCtx.Close()

	rc, err := C.pdump_module_config_set_filter(
		m.asRawPtr(),
		cFilter,
		errCtx.Handle(),
	)
	if rc != 0 {
		if reason := errCtx.Reason(); reason != "" {
			return errors.Join(err, fmt.Errorf("reason=%s", reason))
		}
		return errors.Join(err, fmt.Errorf("error code=%d", rc))
	}
	return nil
}

func (m *ModuleConfig) SetDumpMode(pbMode uint32) error {
	if pbMode > C.PDUMP_ALL {
		return fmt.Errorf("unknown pdump mode %x (max known %x)", pbMode, C.PDUMP_ALL)
	}

	var mode C.enum_pdump_mode
	if pbMode&C.PDUMP_INPUT != 0 {
		mode |= C.PDUMP_INPUT
	}
	if pbMode&C.PDUMP_DROPS != 0 {
		mode |= C.PDUMP_DROPS
	}

	if pbMode != uint32(mode) {
		// This check validates the exhaustiveness of the preceding if
		// statements against the pdump_mode enum.
		// This check will fail if new modes are added.
		return fmt.Errorf("unknown pdump mode %x", pbMode^mode)
	}

	rc, err := C.pdump_module_config_set_mode(
		m.asRawPtr(),
		mode,
	)
	if rc != 0 {
		return errors.Join(fmt.Errorf("error code=%d", rc), err)
	}
	return nil
}

func (m *ModuleConfig) SetSnapLen(snaplen uint32) error {
	errCtx := newErrorCallbackContext()
	defer errCtx.Close()

	rc, err := C.pdump_module_config_set_snaplen(
		m.asRawPtr(),
		C.uint32_t(snaplen),
		errCtx.Handle(),
	)
	if rc != 0 {
		if reason := errCtx.Reason(); reason != "" {
			return errors.Join(err, fmt.Errorf("reason=%s", reason))
		}
		return errors.Join(fmt.Errorf("error code=%d", rc), err)
	}
	return nil
}

// LinkRing records the ring this config captures into, by name.
//
// The dataplane resolves the link to a ring object at the next ectx
// build; whether a ring by this name exists is not checked here. The
// caller establishes that order itself: the ring-service lease is taken,
// and the name is known to resolve, before this link is published.
func (m *ModuleConfig) LinkRing(ringName string) error {
	cName := C.CString(ringName)
	defer C.free(unsafe.Pointer(cName))

	var cErr *C.yanet_error
	rc := C.pdump_module_config_link_ring(m.asRawPtr(), cName, &cErr)
	if rc != 0 {
		return fmt.Errorf("failed to link ring %q: %w", ringName, cerrors.FromC(unsafe.Pointer(cErr)))
	}
	return nil
}
