// Package ringlink links a module config to a named ring object, so service
// tests can reproduce the refusal to delete a linked ring.
//
// Production code never links a module to a ring here: linking belongs to
// the dataplane module itself. The package sits directly under objects/ring
// so both the bindings and the service tests can reach it.
package ringlink

//#cgo CFLAGS: -I../../../../
//#cgo LDFLAGS: -L../../../../build/lib/controlplane/config -lconfig_cp
//
//#include "lib/controlplane/config/cp_module.h"
import "C"

import (
	"fmt"
	"unsafe"

	"github.com/yanet-platform/yanet2/bindings/go/cerrors"
	"github.com/yanet-platform/yanet2/objects/ring/bindings/go/cring"
)

// LinkRing links a raw module config pointer to the named ring object, a
// direct wrapper of the C module link call.
func LinkRing(moduleConfigPtr unsafe.Pointer, name string) error {
	cpModule := (*C.struct_cp_module)(moduleConfigPtr)

	cType := C.CString(cring.ObjectType)
	defer C.free(unsafe.Pointer(cType))
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))

	var index C.uint64_t
	var cErr *C.yanet_error
	rc := C.cp_module_link_object(cpModule, cType, cName, &index, &cErr)
	if rc != 0 {
		return fmt.Errorf("failed to link ring %q: %w", name, cerrors.FromC(unsafe.Pointer(cErr)))
	}
	return nil
}
