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

// Owns the per-worker ring metadata and data areas of one named ring, as an
// independent shared-memory cp_object registered under ("ring", name).
// Module configs reference it by name and resolve it to per-worker ring
// pointers at ectx build time.
struct ring_object {
	struct cp_object cp_object;

	uint64_t worker_count;
	// Per-worker data area size in bytes: a power of two fixed at
	// creation, from the frame size up to the allocator's maximum block.
	uint32_t capacity;

	// The metadata array's raw allocation and its byte count, kept for
	// freeing: the checked over-allocation that rounds the array up to
	// a cache-line boundary means the aligned array start below is not
	// itself the allocated pointer.
	void *workers_raw;
	uint64_t workers_raw_size;
	struct ring_worker *workers;
};

// RAII lifecycle for struct ring_object.
//
// new allocates ONLY the struct in the agent shared memory. init zeroes
// the enclosing struct and calls cp_object_init; on error callers must
// call free. fini releases field memory (per-worker data blocks, the
// metadata array, cp_object_fini) and is idempotent. free deallocates
// ONLY the struct and is NULL-safe.
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

// Registration convenience: allocate + init + create and return the
// cp_object pointer for agent_update_objects. On failure the object is
// fully cleaned up and NULL is returned.
struct cp_object *
ring_object_config_new(
	struct agent *agent,
	const char *name,
	uint32_t capacity,
	yanet_error **err
);

// Destroy the object when it is dangling, per cp_object_try_destroy.
//
// Returns -1 with errno EAGAIN while a live generation still references
// the object; the caller must keep its handle and retry later.
int
ring_object_config_free(struct cp_object *cp_object, yanet_error **err);

// Allocate the per-worker metadata array and data blocks.
//
// The worker count comes from the agent's dp_config. Called once, before
// the object is published. Returns 0 on success or -1 with errno set:
// EINVAL for a capacity below the frame size or not a power of two, E2BIG
// above the allocator's maximum block, EEXIST when the object was already
// created, ENOMEM when an allocation fails. A failure leaves the object
// without storage and the agent's arena unchanged.
int
ring_object_create(
	struct ring_object *self, uint32_t capacity, yanet_error **err
);

// Number of per-worker rings behind this object, fixed at creation.
uint64_t
ring_object_worker_count(const struct cp_object *cp_object);

// Per-worker data area size in bytes, fixed at creation.
uint32_t
ring_object_capacity(const struct cp_object *cp_object);

// Return worker_idx's metadata, computed from the object's aligned array so
// a caller never does its own stride arithmetic across the shared-memory
// boundary. Returns NULL when worker_idx is outside the object's workers.
struct ring_worker *
ring_object_worker(const struct cp_object *cp_object, uint64_t worker_idx);

// Return worker_idx's data area as a process-local absolute pointer, or
// NULL when worker_idx is outside the object's workers.
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

// Allocate count entries of stride bytes each from ctx as one checked
// over-allocation, rounding both the array's base address and each entry's
// stride up to alignment.
//
// The block allocator's own alignment guarantee tops out at 64 bytes, and
// an ASan red zone can displace even that, so the raw allocation asks for
// one extra alignment unit of slack and this rounds the returned base up
// itself rather than trust the allocator. On success the whole raw
// allocation is zeroed and *raw/*raw_size receive the values a matching
// memory_bfree needs; a caller with no matching free leaks. Returns NULL
// with errno EOVERFLOW on an arithmetic overflow or ENOMEM on an
// allocation failure, in which case *raw/*raw_size are left untouched.
void *
ring_object_align_alloc(
	struct memory_context *ctx,
	uint64_t stride,
	uint64_t count,
	uint64_t alignment,
	void **raw,
	uint64_t *raw_size
);
