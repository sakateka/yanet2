// Package ringlink links a module config to a named ring object, for
// service tests that must reproduce the dataplane's refusal to delete a
// ring a published module still links.
//
// Production code never links a module to a ring itself: linking is the
// dataplane module's own concern, exercised here only to pin the delete
// refusal end to end. Rooted directly under objects/ring so both the
// cring bindings tests and the controlplane service tests can reach it.
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

// LinkRing links the module config at moduleConfigPtr (an
// ffi.ModuleConfig.AsRawPtr value) to the named ring object, mapping 1:1 to
// cp_module_link_object.
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
