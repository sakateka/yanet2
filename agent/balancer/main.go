package main

//#cgo CFLAGS: -I../../ -I../../lib
//#cgo LDFLAGS: -L../../build/modules/balancer/ -lbalancer_cp
//#cgo LDFLAGS: -L../../build/lib/controlplane/agent -lagent
//#cgo LDFLAGS: -L../../build/lib/controlplane/config -lconfig_cp
//#cgo LDFLAGS: -L../../build/lib/dataplane/config -lconfig_dp
//#include "api/agent.h"
//#include "modules/balancer/controlplane.h"
import "C"
import "unsafe"

func main() {
	cPath := C.CString("/dev/hugepages/yanet")
	defer C.free(unsafe.Pointer(cPath))

	shm, err := C.yanet_shm_attach(cPath)
	if err != nil {
		panic(err)
	}
	defer C.yanet_shm_detach(shm)

	agent := C.agent_attach(
		shm,
		0,
		C.CString("balancer"),
		16*1024*1024,
	)

	bmc := C.balancer_module_config_init(agent, C.CString("balancer0"))

	srv := C.balancer_service_config_create(
		0x010002,
		&([]C.uint8_t{
			0x2a, 0x02, 0x06, 0xb8,
			0x00, 0x00, 0x34, 0x00,
			0x00, 0x00, 0x85, 0x3a,
			0x00, 0x00, 0x00, 0x03,
		})[0],
		6,
	)
	defer C.balancer_service_config_free(srv)

	src_addr := &([]C.uint8_t{
		0x2a, 0x02, 0x06, 0xb8,
		0x66, 0x66, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
	})[0]
	src_mask := &([]C.uint8_t{
		0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff,
		0x00, 0x00, 0x00, 0x00,
	})[0]

	C.balancer_service_config_set_real(
		srv,
		0,
		0x02,
		&([]C.uint8_t{
			0x2a, 0x02, 0x06, 0xb8,
			0x0c, 0x0e, 0x10, 0x03,
			0x00, 0x00, 0x06, 0x75,
			0xa1, 0x5a, 0x33, 0x14,
		})[0],
		src_addr,
		src_mask,
	)

	C.balancer_service_config_set_real(
		srv,
		0,
		0x02,
		&([]C.uint8_t{
			0x2a, 0x02, 0x06, 0xb8,
			0x0c, 0x0e, 0x10, 0x03,
			0x00, 0x00, 0x06, 0x75,
			0xa1, 0x5a, 0x3c, 0xa0,
		})[0],
		src_addr,
		src_mask,
	)

	C.balancer_service_config_set_real(
		srv,
		0,
		0x02,
		&([]C.uint8_t{
			0x2a, 0x02, 0x06, 0xb8,
			0x0c, 0x0e, 0x10, 0x03,
			0x00, 0x00, 0x06, 0x75,
			0xa1, 0x5a, 0x41, 0x74,
		})[0],
		src_addr,
		src_mask,
	)

	C.balancer_service_config_set_real(
		srv,
		0,
		0x02,
		&([]C.uint8_t{
			0x2a, 0x02, 0x06, 0xb8,
			0x0c, 0x0e, 0x10, 0x03,
			0x00, 0x00, 0x06, 0x75,
			0xa1, 0x5a, 0x4b, 0xb8,
		})[0],
		src_addr,
		src_mask,
	)

	C.balancer_service_config_set_real(
		srv,
		0,
		0x02,
		&([]C.uint8_t{
			0x2a, 0x02, 0x06, 0xb8,
			0x0c, 0x0e, 0x10, 0x03,
			0x00, 0x00, 0x06, 0x75,
			0xa1, 0x5a, 0x4d, 0x6c,
		})[0],
		src_addr,
		src_mask,
	)

	C.balancer_service_config_set_real(
		srv,
		0,
		0x02,
		&([]C.uint8_t{
			0x2a, 0x02, 0x06, 0xb8,
			0x0c, 0x0e, 0x10, 0x03,
			0x00, 0x00, 0x06, 0x75,
			0xa1, 0x5a, 0x0e, 0x98,
		})[0],
		src_addr,
		src_mask,
	)

	C.balancer_module_config_add_service(
		bmc,
		srv,
	)

	configs := [1]*C.struct_cp_module{bmc}

	C.agent_update_modules(
		agent,
		1,
		&configs[0],
	)

}
