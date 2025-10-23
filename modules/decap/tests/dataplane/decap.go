package decap_test

//#cgo CFLAGS: -I../../../../ -I../../../../lib
//#cgo LDFLAGS: -L../../../../build/modules/decap/dataplane -ldecap_dp
//#cgo LDFLAGS: -L../../../../build/lib/dataplane/packet -lpacket
//#cgo LDFLAGS: -L../../../../build/lib/logging -llogging
/*
#include "common/memory.h"
#include "common/lpm.h"
#include "modules/decap/dataplane/config.h"
#include "dataplane/module/module.h"

void
decap_handle_packets(
	struct dp_worker *dp_worker,
	struct module_ectx *module_ectx,
	struct packet_front *packet_front
);

void
test_decap_handle_packets(
	struct dp_worker *dp_worker,
	struct cp_module *cp_module,
	struct packet_front *packet_front
) {
	struct module_ectx module_ectx = {};
	SET_OFFSET_OF(&module_ectx.cp_module, cp_module);
	decap_handle_packets(dp_worker, &module_ectx, packet_front);
}

*/
import "C"
import (
	"net/netip"
	"unsafe"

	"github.com/gopacket/gopacket"

	"github.com/yanet-platform/yanet2/common/go/xnetip"
	"github.com/yanet-platform/yanet2/tests/go/common"
)

func memCtxCreate() *C.struct_memory_context {
	blockAlloc := C.struct_block_allocator{}
	arena := C.malloc(1 << 20)
	C.block_allocator_put_arena(&blockAlloc, arena, 1<<20)
	memCtx := C.struct_memory_context{}
	C.memory_context_init(&memCtx, C.CString("test"), &blockAlloc)
	return &memCtx
}

func buildLPMs(
	prefixes []netip.Prefix,
	memCtx *C.struct_memory_context,
	lpm4 *C.struct_lpm,
	lpm6 *C.struct_lpm,
) {
	C.lpm_init(lpm4, memCtx)
	C.lpm_init(lpm6, memCtx)

	for _, prefix := range prefixes {
		if prefix.Addr().Is4() {
			ipv4 := prefix.Addr().As4()
			mask := xnetip.LastAddr(prefix).As4()
			from := (*C.uint8_t)(&ipv4[0])
			to := (*C.uint8_t)(&mask[0])
			C.lpm_insert(lpm4, 4, from, to, 1)
		} else {
			ipv6 := prefix.Addr().As16()
			mask := xnetip.LastAddr(prefix).As16()
			from := (*C.uint8_t)(&ipv6[0])
			to := (*C.uint8_t)(&mask[0])
			C.lpm_insert(lpm6, 16, from, to, 1)
		}
	}
}

func decapModuleConfig(prefixes []netip.Prefix, memCtx *C.struct_memory_context) *C.struct_decap_module_config {
	m := &C.struct_decap_module_config{
		cp_module: C.struct_cp_module{},
	}
	buildLPMs(prefixes, memCtx, &m.prefixes4, &m.prefixes6)

	return m
}

func decapHandlePackets(mc *C.struct_decap_module_config, packets ...gopacket.Packet) (common.PacketFrontResult, error) {
	payload := common.PacketsToPaylod(packets)
	pf := common.PacketFrontFromPayload(payload)
	err := common.ParsePackets(pf)
	if err != nil {
		return common.PacketFrontResult{}, err
	}

	return cDecapHandlePackets(mc, pf), nil
}

func cDecapHandlePackets(mc *C.struct_decap_module_config, pf *common.CPacketFront) common.PacketFrontResult {
	C.test_decap_handle_packets(nil, &mc.cp_module, (*C.struct_packet_front)(unsafe.Pointer(pf)))
	return common.PacketFrontToPayload(pf)
}
