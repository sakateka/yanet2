package acl

//#cgo CFLAGS: -I../../../
//#cgo CFLAGS: -I../../../lib
//#cgo LDFLAGS: -L../../../build/modules/acl/api -lacl_cp
//#cgo LDFLAGS: -L../../../build/modules/fwstate/api -lfwstate_cp
//#cgo LDFLAGS: -L../../../build/filter -lfilter
//
//#include <stdlib.h>
//#include "modules/acl/api/controlplane.h"
//#include "fwstate/config.h"
import "C"

import (
	"encoding/binary"
	"fmt"
	"unsafe"

	"github.com/yanet-platform/yanet2/controlplane/ffi"
	"github.com/yanet-platform/yanet2/modules/fwstate/controlplane/fwstatepb"
)

// ModuleConfig wraps the C ACL module configuration
type ModuleConfig struct {
	ptr ffi.ModuleConfig
}

// NewModuleConfig creates a new ACL module configuration
func NewModuleConfig(agent *ffi.Agent, name string) (*ModuleConfig, error) {
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))

	// Create a new module config using the C API
	ptr, err := C.acl_module_config_init((*C.struct_agent)(agent.AsRawPtr()), cName)
	if err != nil {
		return nil, fmt.Errorf("failed to initialize ACL module config: %w", err)
	}
	if ptr == nil {
		return nil, fmt.Errorf("failed to initialize ACL module config: module %q not found", name)
	}

	return &ModuleConfig{
		ptr: ffi.NewModuleConfig(unsafe.Pointer(ptr)),
	}, nil
}

// copyCBytes copies bytes from Go byte slice to C uint8_t array element by element.
func copyCBytes(dst []C.uint8_t, src []byte) {
	for i := range src {
		if i >= len(dst) {
			break
		}
		dst[i] = C.uint8_t(src[i])
	}
}

// asRawPtr returns the raw C pointer
func (m *ModuleConfig) asRawPtr() *C.struct_cp_module {
	return (*C.struct_cp_module)(m.ptr.AsRawPtr())
}

// AsFFIModule returns the FFI module config
func (m *ModuleConfig) AsFFIModule() ffi.ModuleConfig {
	return m.ptr
}

func (m *ModuleConfig) SetFwStateConfig(shm unsafe.Pointer, fwstateResp *fwstatepb.GetFwStateConfigResponse) error {
	cpModule := m.asRawPtr()

	var fwstateConfig C.struct_fwstate_config

	// Set map offsets - converting uint64 offsets to pointers
	// This is safe because these are offsets from shm base, not actual pointers
	// The C function will convert them back to local offsets.
	fwstateConfig.fw4state = (*C.fwmap_t)(unsafe.Pointer(uintptr(fwstateResp.Fw4StateOffset)))
	fwstateConfig.fw6state = (*C.fwmap_t)(unsafe.Pointer(uintptr(fwstateResp.Fw6StateOffset)))

	// Copy sync config from protobuf to C struct
	if syncConfig := fwstateResp.SyncConfig; syncConfig != nil {
		// Copy addresses
		copyCBytes(fwstateConfig.sync_config.src_addr[:], syncConfig.SrcAddr)
		copyCBytes(fwstateConfig.sync_config.dst_ether.addr[:], syncConfig.DstEther)
		copyCBytes(fwstateConfig.sync_config.dst_addr_multicast[:], syncConfig.DstAddrMulticast)
		copyCBytes(fwstateConfig.sync_config.dst_addr_unicast[:], syncConfig.DstAddrUnicast)

		// Copy ports
		// Store port in network byte order (big-endian) for direct assignment at runtime
		binary.BigEndian.PutUint16(
			(*[2]byte)(unsafe.Pointer(&fwstateConfig.sync_config.port_multicast))[:],
			uint16(syncConfig.PortMulticast),
		)
		// Store port in network byte order (big-endian) for direct assignment at runtime
		binary.BigEndian.PutUint16(
			(*[2]byte)(unsafe.Pointer(&fwstateConfig.sync_config.port_unicast))[:],
			uint16(syncConfig.PortUnicast),
		)

		// Copy timeouts
		fwstateConfig.sync_config.timeouts.tcp_syn_ack = C.uint64_t(syncConfig.TcpSynAck)
		fwstateConfig.sync_config.timeouts.tcp_syn = C.uint64_t(syncConfig.TcpSyn)
		fwstateConfig.sync_config.timeouts.tcp_fin = C.uint64_t(syncConfig.TcpFin)
		fwstateConfig.sync_config.timeouts.tcp = C.uint64_t(syncConfig.Tcp)
		fwstateConfig.sync_config.timeouts.udp = C.uint64_t(syncConfig.Udp)
		fwstateConfig.sync_config.timeouts.default_ = C.uint64_t(syncConfig.Default)
	}

	// Call C API to set the fwstate config (converts pointers to offsets)
	if rc := C.acl_module_set_fwstate_config(cpModule, shm, fwstateConfig); rc != 0 {
		return fmt.Errorf("failed to set fwstate config: error code=%d", rc)
	}

	return nil
}

// Rule represents an ACL rule (stub)
type Rule struct {
	ID     uint32
	Action string
}
