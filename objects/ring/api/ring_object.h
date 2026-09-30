#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/controlplane/config/cp_object.h"

#include "lib/errors/errors.h"

#include "objects/ring/dataplane/ring.h"

#define RING_OBJECT_TYPE "ring"

struct agent;
struct cp_object;
struct memory_context;

// A named ring: the per-worker metadata and data areas published as a
// standalone shared-memory object of type "ring".
//
// A module config can link a ring by name and resolve it to per-worker ring
// pointers when its execution context is built.
struct ring_object {
	struct cp_object cp_object;

	uint64_t worker_count;
	// Per-worker data area size in bytes: a power of two fixed at
	// creation, from the frame size up to the allocator's maximum block.
	uint32_t capacity;

	// Raw allocation of the metadata array and its byte count, kept for
	// freeing.
	//
	// Rounding the array up to a cache-line boundary means the aligned
	// start below is not itself the allocated pointer.
	void *workers_raw;
	uint64_t workers_raw_size;
	struct ring_worker *workers;
};

// Lifecycle of a ring object: new, init, fini, free.
//
// New allocates only the struct in the agent's shared memory. Init zeroes
// the struct and initializes its shared-object header; on error the caller
// must free it. Fini releases the per-worker data blocks, the metadata array
// and the header, clearing what it freed so a second call is a no-op. Free
// deallocates only the struct and accepts NULL.
struct ring_object *
ring_object_new(struct agent *agent);

int
ring_object_init(
	struct ring_object *self,
	struct agent *agent,
	const char *name,
	yanet_error **err
);

void
ring_object_fini(struct ring_object *self);

void
ring_object_free(struct ring_object *self, struct agent *agent);

// Allocate, initialize and create a ring, returning the shared-object handle
// to register with the agent.
//
// On failure the object is fully cleaned up and NULL is returned with errno
// kept from the failing step: ENOMEM when the struct cannot be allocated,
// otherwise the errno of creation. Initialization sets no errno of its own,
// so errno is unspecified when it fails.
struct cp_object *
ring_object_config_new(
	struct agent *agent,
	const char *name,
	uint32_t capacity,
	yanet_error **err
);

// Destroy the object once no live configuration generation references it.
//
// Returns -1 with errno EAGAIN while a live generation still references
// the object; the caller must keep its handle and retry later.
int
ring_object_config_free(struct cp_object *cp_object, yanet_error **err);

// Allocate the per-worker metadata array and data blocks.
//
// The worker count follows the dataplane's configured worker count. Called
// once, before the object is published. Returns 0 on success or -1 with errno
// set: EINVAL for a capacity below the frame size or not a power of two, or for
// a dataplane reporting zero workers; E2BIG above the allocator's maximum
// block; EEXIST when the object was already created; ENOMEM when an allocation
// fails. A failure leaves the object without storage and the agent's arena
// unchanged.
int
ring_object_create(
	struct ring_object *self, uint32_t capacity, yanet_error **err
);

// Per-worker data area size in bytes, fixed at creation.
uint32_t
ring_object_capacity(const struct cp_object *cp_object);

// Metadata of one worker, located in the object's aligned array so a caller
// never does stride arithmetic across the shared-memory boundary.
//
// Returns NULL for a worker index outside the object's workers, so a
// caller enumerates every ring by walking indices up to the first NULL.
struct ring_worker *
ring_object_worker(const struct cp_object *cp_object, uint64_t worker_idx);

// Data area of one worker as a process-local pointer, or NULL for a worker
// index outside the object's workers.
uint8_t *
ring_object_worker_data(const struct cp_object *cp_object, uint64_t worker_idx);

// Whether ("ring", name) exists in the agent's currently published
// configuration generation, checked under the config lock.
//
// Lets a creator reject a create that would otherwise silently replace a
// published ring by name; it says nothing about objects still being
// assembled by a concurrent, not-yet-published update.
bool
ring_object_exists(struct agent *agent, const char *name);

// Allocate an array of fixed-stride entries as one checked over-allocation,
// aligning both the array's base address and each entry's stride.
//
// An ASan red zone can displace any alignment above 64 bytes, so the raw
// allocation carries one extra alignment unit of slack and the base is
// rounded up here. On success the
// whole raw allocation is zeroed and the raw pointer and size outputs hold
// what the matching free needs. Returns NULL with errno EOVERFLOW on an
// arithmetic overflow or ENOMEM on an allocation failure, leaving the raw
// outputs untouched.
void *
ring_object_align_alloc(
	struct memory_context *ctx,
	uint64_t stride,
	uint64_t count,
	uint64_t alignment,
	void **raw,
	uint64_t *raw_size
);
