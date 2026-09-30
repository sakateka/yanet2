// Package ringref places an artificial extra reference on a published ring,
// so service tests can reproduce a refused free without generation timing.
//
// Production code never does this: a ring's only real reference comes from
// a published configuration generation. The package sits directly under
// objects/ring so both the bindings and the service tests can reach it.
package ringref

//#cgo CFLAGS: -I../../../../
//#cgo LDFLAGS: -L../../../../build/lib/controlplane/config -lconfig_cp
//
//#include <stdlib.h>
//
//#include "common/memory_address.h"
//#include "lib/controlplane/agent/agent.h"
//#include "lib/controlplane/config/cp_object.h"
//#include "lib/controlplane/config/zone.h"
//
//// ringref_config resolves agent's config pointer, the same identity
//// cp_config_lock records as the calling thread's holder and every
//// registry mutation's assert-locked check compares against.
//static inline struct cp_config *
//ringref_config(struct agent *agent) {
//	return ADDR_OF(&agent->cp_config);
//}
//
//// ringref_lookup_object resolves the currently published cp_object for
//// (object_type, object_name) in agent's live generation, mirroring the
//// resolution ring_object_exists performs internally.
//static inline struct cp_object *
//ringref_lookup_object(
//	struct agent *agent, const char *object_type, const char *object_name
//) {
//	struct cp_config *cp_config = ADDR_OF(&agent->cp_config);
//	cp_config_lock(cp_config);
//	struct cp_config_gen *gen = ADDR_OF(&cp_config->cp_config_gen);
//	struct cp_object *object =
//		cp_config_gen_lookup_object(gen, object_type, object_name);
//	cp_config_unlock(cp_config);
//	return object;
//}
//
//// ringref_locked_upsert runs cp_object_registry_upsert under agent's
//// config lock, the same lock every production registry mutation
//// (cp_config_gen_install, cp_object_try_destroy) runs under, so this
//// artificial reference cannot race a concurrent publish or delete that
//// touches the same object's reference count.
//static inline int
//ringref_locked_upsert(
//	struct agent *agent,
//	struct cp_object_registry *registry,
//	const char *object_type,
//	const char *object_name,
//	struct cp_object *object,
//	yanet_error **err
//) {
//	struct cp_config *cp_config = ADDR_OF(&agent->cp_config);
//	cp_config_lock(cp_config);
//	int rc = cp_object_registry_upsert(
//		registry, object_type, object_name, object, err
//	);
//	cp_config_unlock(cp_config);
//	return rc;
//}
//
//// ringref_locked_fini runs cp_object_registry_fini under agent's config
//// lock, for the same reason ringref_locked_upsert does.
//static inline void
//ringref_locked_fini(
//	struct agent *agent, struct cp_object_registry *registry
//) {
//	struct cp_config *cp_config = ADDR_OF(&agent->cp_config);
//	cp_config_lock(cp_config);
//	cp_object_registry_fini(registry);
//	cp_config_unlock(cp_config);
//}
import "C"

import (
	"fmt"
	"unsafe"

	"github.com/yanet-platform/yanet2/bindings/go/cerrors"
	"github.com/yanet-platform/yanet2/controlplane/ffi"
	"github.com/yanet-platform/yanet2/objects/ring/bindings/go/cring"
)

// Reference is an artificial extra reference to a published ring, standing
// in for a live generation that has not yet retired.
//
// Its backing registry lives in C memory: the C registry calls may keep
// pointers into it beyond the call, which needs a stable C address.
type Reference struct {
	agent    *C.struct_agent
	registry *C.struct_cp_object_registry
}

// Hold registers an extra reference to the named ring from the agent's
// published generation, so the ring's free is refused until Release.
func Hold(agent *ffi.Agent, name string) (*Reference, error) {
	cAgent := (*C.struct_agent)(agent.AsRawPtr())

	cType := C.CString(cring.ObjectType)
	defer C.free(unsafe.Pointer(cType))
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))

	object := C.ringref_lookup_object(cAgent, cType, cName)
	if object == nil {
		return nil, fmt.Errorf("ring %q is not published", name)
	}

	registry := (*C.struct_cp_object_registry)(C.malloc(C.sizeof_struct_cp_object_registry))
	if registry == nil {
		return nil, fmt.Errorf("failed to allocate reference registry")
	}

	// The registry's owner must be the real config: the registry calls
	// assert the lock they run under against it.
	//
	// A NULL owner would match only a thread holding no lock, tripping
	// that assertion once this reference takes the agent's lock.
	cpConfig := C.ringref_config(cAgent)

	var cErr *C.yanet_error
	if rc := C.cp_object_registry_init(&cAgent.memory_context, cpConfig, registry, &cErr); rc != 0 {
		C.free(unsafe.Pointer(registry))
		return nil, fmt.Errorf("failed to init reference registry: %w", cerrors.FromC(unsafe.Pointer(cErr)))
	}
	if rc := C.ringref_locked_upsert(cAgent, registry, cType, cName, object, &cErr); rc != 0 {
		C.ringref_locked_fini(cAgent, registry)
		C.free(unsafe.Pointer(registry))
		return nil, fmt.Errorf("failed to hold ring %q: %w", name, cerrors.FromC(unsafe.Pointer(cErr)))
	}

	return &Reference{agent: cAgent, registry: registry}, nil
}

// Release drops the artificial reference and frees the registry; later
// calls do nothing.
func (m *Reference) Release() {
	if m.registry == nil {
		return
	}
	C.ringref_locked_fini(m.agent, m.registry)
	C.free(unsafe.Pointer(m.registry))
	m.registry = nil
}
