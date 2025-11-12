package fwstate

//#cgo CFLAGS: -I../../../
//#cgo CFLAGS: -I../../../lib
//#cgo LDFLAGS: -L../../../build/modules/fwstate/api -lfwstate_cp
//
//#include "modules/fwstate/api/controlplane.h"
//#include "modules/fwstate/dataplane/config.h"
//#include "fwstate/config.h"
import "C"

import (
	"encoding/binary"
	"fmt"
	"unsafe"

	"github.com/yanet-platform/yanet2/controlplane/ffi"
)

// ModuleConfig wraps the C fwstate module configuration
type ModuleConfig struct {
	ptr ffi.ModuleConfig
}

// NewModuleConfig creates a new fwstate module configuration
func NewModuleConfig(agent *ffi.Agent, name string) (*ModuleConfig, error) {
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))

	ptr, err := C.fwstate_module_config_init((*C.struct_agent)(agent.AsRawPtr()), cName)
	if err != nil {
		return nil, fmt.Errorf("failed to initialize fwstate module config: %w", err)
	}
	if ptr == nil {
		return nil, fmt.Errorf("failed to initialize fwstate module config: module %q not found", name)
	}

	return &ModuleConfig{
		ptr: ffi.NewModuleConfig(unsafe.Pointer(ptr)),
	}, nil
}

// AsFFIModule returns the FFI module config
func (m *ModuleConfig) AsFFIModule() ffi.ModuleConfig {
	return m.ptr
}

// GetFwStateConfig returns the fwstate config pointer from the fwstate module.
// Returns a pointer in the current process's address space.
func (m *ModuleConfig) GetFwStateConfig() *C.struct_fwstate_config {
	cpModule := (*C.struct_cp_module)(m.ptr.AsRawPtr())
	return C.fwstate_module_get_fwstate_config(cpModule)
}

// GetFwStateConfigWithGlobalOffset returns the fwstate config with map offsets
// converted to global offsets from the shared memory base. Safe to pass to other
// processes with the same shared memory mapped.
func (m *ModuleConfig) GetFwStateConfigWithGlobalOffset(shm unsafe.Pointer) (*C.struct_fwstate_config, error) {
	cpModule := (*C.struct_cp_module)(m.ptr.AsRawPtr())

	var outConfig C.struct_fwstate_config
	rc := C.fwstate_get_config_with_global_offset(cpModule, shm, &outConfig)
	if rc != 0 {
		return nil, fmt.Errorf("failed to get fwstate config with global offset: error code=%d", rc)
	}

	return &outConfig, nil
}

// CreateMaps creates firewall state maps for the fwstate module
func (m *ModuleConfig) CreateMaps(indexSize uint32, extraBucketCount uint32, workerCount uint16) error {
	fwstateCfg := m.GetFwStateConfig()
	if fwstateCfg == nil {
		return fmt.Errorf("fwstate config not available")
	}

	cpModule := (*C.struct_cp_module)(m.ptr.AsRawPtr())

	if rc, err := C.fwstate_config_create_maps(
		&cpModule.memory_context,
		fwstateCfg,
		C.uint32_t(indexSize),
		C.uint32_t(extraBucketCount),
		C.uint16_t(workerCount),
	); rc != 0 || err != nil {
		return fmt.Errorf("failed to create maps: error code=%d, cErr=%v", rc, err)
	}

	return nil
}

// copyCBytes copies bytes from Go byte slice to C uint8_t array element by element
func copyCBytes(dst []C.uint8_t, src []byte) {
	for i := range src {
		if i >= len(dst) {
			break
		}
		dst[i] = C.uint8_t(src[i])
	}
}

// SetSyncConfig configures state synchronization settings for the fwstate module
func (m *ModuleConfig) SetSyncConfig(syncConfig *SyncConfig) error {
	fwstateCfg := m.GetFwStateConfig()
	if fwstateCfg == nil {
		return fmt.Errorf("fwstate config not available")
	}

	return SetSync(fwstateCfg, syncConfig)
}

// TransferMaps transfers maps from old config to new config during reconfiguration
func (m *ModuleConfig) TransferMaps(oldConfig *ModuleConfig) {
	newCfg := m.GetFwStateConfig()
	oldCfg := oldConfig.GetFwStateConfig()

	C.fwstate_config_transfer_maps(newCfg, oldCfg)
}

// SetSync configures state synchronization settings
func SetSync(cfg *C.struct_fwstate_config, syncConfig *SyncConfig) error {

	var cSyncConfig C.struct_fw_state_sync_config

	// Copy source address
	copyCBytes(cSyncConfig.src_addr[:], syncConfig.SrcAddr[:])

	// Copy destination ethernet address
	copyCBytes(cSyncConfig.dst_ether.addr[:], syncConfig.DstEther[:])

	// Copy multicast addresses and ports
	copyCBytes(cSyncConfig.dst_addr_multicast[:], syncConfig.DstAddrMulticast[:])
	// Store port in network byte order (big-endian) for direct comparison with UDP headers in dataplane
	binary.BigEndian.PutUint16((*[2]byte)(unsafe.Pointer(&cSyncConfig.port_multicast))[:], uint16(syncConfig.PortMulticast))

	// Copy unicast addresses and ports
	copyCBytes(cSyncConfig.dst_addr_unicast[:], syncConfig.DstAddrUnicast[:])
	// Store port in network byte order (big-endian) for direct comparison with UDP headers in dataplane
	binary.BigEndian.PutUint16((*[2]byte)(unsafe.Pointer(&cSyncConfig.port_unicast))[:], uint16(syncConfig.PortUnicast))

	// Copy timeouts
	cSyncConfig.timeouts.tcp_syn_ack = C.uint64_t(syncConfig.Timeouts.TCPSynAck)
	cSyncConfig.timeouts.tcp_syn = C.uint64_t(syncConfig.Timeouts.TCPSyn)
	cSyncConfig.timeouts.tcp_fin = C.uint64_t(syncConfig.Timeouts.TCPFin)
	cSyncConfig.timeouts.tcp = C.uint64_t(syncConfig.Timeouts.TCP)
	cSyncConfig.timeouts.udp = C.uint64_t(syncConfig.Timeouts.UDP)
	cSyncConfig.timeouts.default_ = C.uint64_t(syncConfig.Timeouts.Default)

	if rc, cErr := C.fwstate_config_set_sync(cfg, &cSyncConfig); rc != 0 || cErr != nil {
		return fmt.Errorf("failed to set sync config: error code=%d, cErr=%v", rc, cErr)
	}

	return nil
}

// GetMapSize returns the number of entries in the specified firewall state map
func (m *ModuleConfig) GetMapSize(isIPv6 bool) uint64 {
	fwstateCfg := m.GetFwStateConfig()
	if fwstateCfg == nil {
		return 0
	}

	size := C.fwstate_config_get_map_size(fwstateCfg, C.bool(isIPv6))
	return uint64(size)
}
