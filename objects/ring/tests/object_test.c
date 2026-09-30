/*
 * Pins the ring cp_object's lifecycle: layout, checked allocation,
 * validation, reference-based free/delete refusal, and existence.
 *
 * The per-worker metadata layout is cache-line aligned and sized, the
 * checked over-allocation helper rounds both the array base and its
 * stride to the requested alignment even when the allocator did not, bad
 * capacities are rejected before anything is allocated, the worker count
 * tracks the dataplane's configured workers, a generation reference or a
 * linking module refuses the free/delete path exactly like every other
 * cp_object, and the existence query reflects only the currently
 * published generation.
 */

#include "api/agent.h"

#include "common/asan.h"
#include "common/memory.h"
#include "common/memory_block.h"
#include "common/test_assert.h"

#include "lib/controlplane/agent/agent.h"
#include "lib/controlplane/config/cp_object.h"
#include "lib/controlplane/config/zone.h"
#include "lib/dataplane/config/zone.h"

#include "modules/forward/api/controlplane.h"

#include "objects/ring/api/ring_object.h"

#include "lib/dataplane_ut/dataplane_ut.h"
#include "lib/errors/errors.h"

#include "lib/logging/log.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RING_OBJECT_TEST_MEMORY_LIMIT (2u * 1024u * 1024u)

// The metadata struct is cache-line aligned and its size is a whole number
// of cache lines, so an array of them never lets two workers share a line.
static int
run_ring_worker_layout_test(struct yanet_shm *shm) {
	(void)shm;

	TEST_ASSERT_EQUAL(
		sizeof(struct ring_worker) % YANET_CACHE_LINE_SIZE,
		0,
		"ring_worker size must be a whole number of cache lines"
	);
	TEST_ASSERT_EQUAL(
		_Alignof(struct ring_worker),
		YANET_CACHE_LINE_SIZE,
		"ring_worker must be aligned to exactly one cache line"
	);

	return TEST_SUCCESS;
}

// The checked over-allocation helper rounds the array base and each
// entry's stride up to the requested alignment, at both 64 and 128 without
// recompiling, and a matching free restores the arena.
//
// The block allocator's own buddy invariant guarantees a returned block is
// always aligned to at least its own size, so a fresh (non-ASan) build can
// never observe a misaligned raw block from this call; only ASan's fixed
// red-zone offset can shift it below the requested alignment. The
// postconditions below hold either way, and the extra check under ASan
// confirms the rounding is genuinely correcting something, not a no-op.
static int
run_ring_object_align_alloc_test(struct yanet_shm *shm) {
	yanet_error *err = NULL;

	struct agent *agent = agent_attach(
		shm, 0, "ring-align-alloc", RING_OBJECT_TEST_MEMORY_LIMIT, &err
	);
	TEST_ASSERT_NOT_NULL(agent, "agent_attach failed");

	uint64_t alignments[] = {64, 128};
	for (size_t i = 0; i < sizeof(alignments) / sizeof(alignments[0]);
	     ++i) {
		uint64_t alignment = alignments[i];
		size_t baseline =
			block_allocator_free_size(&agent->block_allocator);

		uint64_t stride = 100;
		uint64_t count = 3;
		void *raw;
		uint64_t raw_size;
		void *result = ring_object_align_alloc(
			&agent->memory_context,
			stride,
			count,
			alignment,
			&raw,
			&raw_size
		);
		TEST_ASSERT_NOT_NULL(
			result,
			"align_alloc must succeed at alignment %lu",
			alignment
		);
		TEST_ASSERT_EQUAL(
			(uintptr_t)result % alignment,
			0,
			"the array base must be aligned to %lu",
			alignment
		);

		uint64_t expected_stride =
			(stride + alignment - 1) & ~(alignment - 1);
		uint64_t expected_body = expected_stride * count;
		TEST_ASSERT(
			raw_size >= expected_body &&
				raw_size < expected_body + alignment,
			"raw_size must cover exactly the aligned-stride body "
			"plus "
			"at most one alignment unit of slack"
		);
		TEST_ASSERT(
			(uintptr_t)result - (uintptr_t)raw < alignment,
			"the base must round up by less than one alignment unit"
		);

#ifdef HAVE_ASAN
		if (alignment > MEMORY_BLOCK_MAX_ALIGN) {
			TEST_ASSERT(
				(uintptr_t)raw % alignment != 0,
				"ASan's red zone must displace the raw block "
				"away from %lu-alignment, exercising the "
				"rounding this helper performs",
				alignment
			);
		}
#endif

		memory_bfree(&agent->memory_context, raw, raw_size);
		TEST_ASSERT_EQUAL(
			(long)block_allocator_free_size(&agent->block_allocator
			),
			(long)baseline,
			"freeing the raw allocation must restore the arena"
		);
	}

	agent_detach(agent);
	return TEST_SUCCESS;
}

// Smallest power of two strictly greater than val. Used to build an
// oversize capacity that is always a power of two but always exceeds the
// allocator's maximum, whether or not ASan's red zones lowered that
// maximum for this build.
static uint32_t
pow2_above(uint32_t val) {
	uint32_t pow2 = 1;
	while (pow2 <= val) {
		pow2 <<= 1;
	}
	return pow2;
}

// A capacity of zero, below the frame size, not a power of two, or above
// the allocator's maximum block is refused before any allocation, with
// the errno naming which check failed, leaving the arena unchanged.
static int
run_ring_object_bad_capacity_test(struct yanet_shm *shm) {
	yanet_error *err = NULL;

	struct agent *agent = agent_attach(
		shm, 0, "ring-bad-capacity", RING_OBJECT_TEST_MEMORY_LIMIT, &err
	);
	TEST_ASSERT_NOT_NULL(agent, "agent_attach failed");

	struct {
		uint32_t capacity;
		int expected_errno;
	} bad_capacities[] = {
		{0, EINVAL},
		{4, EINVAL},
		{24, EINVAL},
		{pow2_above(MEMORY_BLOCK_ALLOCATOR_MAX_SIZE), E2BIG},
	};
	for (size_t i = 0;
	     i < sizeof(bad_capacities) / sizeof(bad_capacities[0]);
	     ++i) {
		uint32_t capacity = bad_capacities[i].capacity;
		size_t baseline =
			block_allocator_free_size(&agent->block_allocator);

		yanet_error *create_err = NULL;
		struct cp_object *object = ring_object_config_new(
			agent, "bad-capacity", capacity, &create_err
		);
		TEST_ASSERT_NULL(
			object, "capacity %u must be refused", capacity
		);
		TEST_ASSERT_EQUAL(
			errno,
			bad_capacities[i].expected_errno,
			"capacity %u must set the expected errno",
			capacity
		);
		TEST_ASSERT(
			create_err != NULL,
			"a refused capacity must report an error"
		);
		yanet_error_free(create_err);

		TEST_ASSERT_EQUAL(
			(long)block_allocator_free_size(&agent->block_allocator
			),
			(long)baseline,
			"a refused capacity must leave the arena unchanged: "
			"capacity=%u",
			capacity
		);
	}

	agent_detach(agent);
	return TEST_SUCCESS;
}

// The created object's worker count follows the dataplane's configured
// worker count, not a value the caller passes in.
static int
run_ring_object_worker_count_test(struct yanet_shm *shm) {
	yanet_error *err = NULL;

	struct agent *agent = agent_attach(
		shm, 0, "ring-worker-count", RING_OBJECT_TEST_MEMORY_LIMIT, &err
	);
	TEST_ASSERT_NOT_NULL(agent, "agent_attach failed");

	struct dp_config *dp_config = agent_dp_config(agent);

	struct cp_object *object =
		ring_object_config_new(agent, "worker-count", 64, &err);
	TEST_ASSERT_NOT_NULL(
		object,
		"ring_object_config_new failed: %s",
		err ? yanet_error_message(err) : "?"
	);
	TEST_ASSERT_EQUAL(
		ring_object_worker_count(object),
		dp_config->worker_count,
		"the object's worker count must match dp_config"
	);
	TEST_ASSERT_EQUAL(
		ring_object_capacity(object),
		64,
		"the object's capacity must stick"
	);

	yanet_error *free_err = NULL;
	TEST_ASSERT_SUCCESS(
		ring_object_config_free(object, &free_err),
		"freeing a dangling ring object must succeed"
	);

	agent_detach(agent);
	return TEST_SUCCESS;
}

// A generation reference refuses the owner's free with EAGAIN and keeps
// the object's memory; once the reference drops, the same free succeeds.
static int
run_ring_object_free_refused_while_referenced_test(struct yanet_shm *shm) {
	yanet_error *err = NULL;

	struct agent *agent = agent_attach(
		shm, 0, "ring-free-refused", RING_OBJECT_TEST_MEMORY_LIMIT, &err
	);
	TEST_ASSERT_NOT_NULL(agent, "agent_attach failed");

	size_t baseline = block_allocator_free_size(&agent->block_allocator);

	struct cp_object *object =
		ring_object_config_new(agent, "referenced", 64, &err);
	TEST_ASSERT_NOT_NULL(
		object,
		"ring_object_config_new failed: %s",
		err ? yanet_error_message(err) : "?"
	);

	struct cp_object_registry reg;
	TEST_ASSERT_SUCCESS(
		cp_object_registry_init(
			&agent->memory_context, NULL, &reg, &err
		),
		"cp_object_registry_init failed"
	);
	TEST_ASSERT_SUCCESS(
		cp_object_registry_upsert(
			&reg, RING_OBJECT_TYPE, "referenced", object, &err
		),
		"cp_object_registry_upsert failed"
	);

	yanet_error *free_err = NULL;
	TEST_ASSERT(
		ring_object_config_free(object, &free_err) == -1 &&
			errno == EAGAIN,
		"freeing a generation-referenced ring object must fail with "
		"EAGAIN"
	);
	yanet_error_free(free_err);
	TEST_ASSERT(
		block_allocator_free_size(&agent->block_allocator) < baseline,
		"a refused free must leave the object's memory in place"
	);

	cp_object_registry_fini(&reg);

	free_err = NULL;
	TEST_ASSERT_SUCCESS(
		ring_object_config_free(object, &free_err),
		"freeing a dangling ring object after its last reference "
		"retired must destroy it"
	);
	TEST_ASSERT_EQUAL(
		(long)block_allocator_free_size(&agent->block_allocator),
		(long)baseline,
		"destroying the object must return the arena to its baseline"
	);

	agent_detach(agent);
	return TEST_SUCCESS;
}

// A published module that links the ring by name refuses its deletion,
// mirroring cp_config_delete_object's generic guard for every cp_object.
static int
run_ring_object_delete_refused_while_linked_test(struct yanet_shm *shm) {
	yanet_error *err = NULL;

	struct agent *agent = agent_attach(
		shm,
		0,
		"ring-delete-linked",
		RING_OBJECT_TEST_MEMORY_LIMIT,
		&err
	);
	TEST_ASSERT_NOT_NULL(agent, "agent_attach failed");

	struct dp_config *dp_config = agent_dp_config(agent);
	struct cp_config *cp_config = ADDR_OF(&agent->cp_config);

	struct cp_object *object =
		ring_object_config_new(agent, "linked", 64, &err);
	TEST_ASSERT_NOT_NULL(
		object,
		"ring_object_config_new failed: %s",
		err ? yanet_error_message(err) : "?"
	);

	struct cp_object *objects[] = {object};
	TEST_ASSERT_SUCCESS(
		agent_update_objects(agent, 1, objects, &err),
		"agent_update_objects failed: %s",
		err ? yanet_error_message(err) : "?"
	);

	struct cp_module *module =
		forward_module_config_init(agent, "ring-linker", &err);
	TEST_ASSERT_NOT_NULL(module, "forward_module_config_init failed");
	uint64_t link_idx;
	TEST_ASSERT_SUCCESS(
		cp_module_link_object(
			module, RING_OBJECT_TYPE, "linked", &link_idx, &err
		),
		"cp_module_link_object failed"
	);
	struct cp_module *modules[] = {module};
	TEST_ASSERT_SUCCESS(
		cp_config_update_modules(
			dp_config, cp_config, 1, modules, &err
		),
		"update_modules failed: %s",
		err ? yanet_error_message(err) : "?"
	);

	int rc = agent_delete_object(agent, RING_OBJECT_TYPE, "linked", &err);
	TEST_ASSERT(
		rc != 0,
		"deleting a ring linked by a published module must fail"
	);
	TEST_ASSERT_STR_CONTAINS(
		yanet_error_message(err),
		"is linked by module",
		"the failure must name the linking module"
	);
	yanet_error_free(err);
	err = NULL;

	struct cp_config_gen *gen = ADDR_OF(&cp_config->cp_config_gen);
	TEST_ASSERT(
		cp_config_gen_lookup_object(gen, RING_OBJECT_TYPE, "linked") !=
			NULL,
		"a refused delete must leave the ring published"
	);

	agent_detach(agent);
	return TEST_SUCCESS;
}

// The existence query is true only once a ring has been published into the
// current generation and false again once it is deleted.
static int
run_ring_object_exists_test(struct yanet_shm *shm) {
	yanet_error *err = NULL;

	struct agent *agent = agent_attach(
		shm, 0, "ring-exists", RING_OBJECT_TEST_MEMORY_LIMIT, &err
	);
	TEST_ASSERT_NOT_NULL(agent, "agent_attach failed");

	TEST_ASSERT(
		!ring_object_exists(agent, "maybe"),
		"an unpublished name must not exist"
	);

	struct cp_object *object =
		ring_object_config_new(agent, "maybe", 64, &err);
	TEST_ASSERT_NOT_NULL(
		object,
		"ring_object_config_new failed: %s",
		err ? yanet_error_message(err) : "?"
	);
	TEST_ASSERT(
		!ring_object_exists(agent, "maybe"),
		"a created but not yet published ring must not exist"
	);

	struct cp_object *objects[] = {object};
	TEST_ASSERT_SUCCESS(
		agent_update_objects(agent, 1, objects, &err),
		"agent_update_objects failed: %s",
		err ? yanet_error_message(err) : "?"
	);
	TEST_ASSERT(
		ring_object_exists(agent, "maybe"),
		"a published ring must exist"
	);

	TEST_ASSERT_SUCCESS(
		agent_delete_object(agent, RING_OBJECT_TYPE, "maybe", &err),
		"agent_delete_object failed: %s",
		err ? yanet_error_message(err) : "?"
	);
	TEST_ASSERT(
		!ring_object_exists(agent, "maybe"),
		"a deleted ring must no longer exist"
	);

	agent_detach(agent);
	return TEST_SUCCESS;
}

int
main(void) {
	log_enable_name("debug");

	const char *port_names[] = {"01:00.0"};
	const char *mods_to_load[] = {"forward"};
	const char *devs_to_load[] = {"plain"};
	const char *objs_to_load[] = {RING_OBJECT_TYPE};

	struct dataplane_ut_config cfg = {
		.cp_memory = 1u << 26,
		.dp_memory = 1u << 20,
		.worker_count = 2,
		.devices = port_names,
		.device_count = 1,
		.modules = mods_to_load,
		.module_count = 1,
		.devices_to_load = devs_to_load,
		.devices_to_load_count = 1,
		.objects_to_load = objs_to_load,
		.objects_to_load_count = 1,
	};

	struct dataplane_ut *ut = dataplane_ut_new(&cfg);
	if (ut == NULL) {
		fprintf(stderr, "dataplane_ut_new failed\n");
		return 1;
	}

	struct yanet_shm *shm = dataplane_ut_shm(ut);
	if (shm == NULL) {
		fprintf(stderr, "dataplane_ut_shm returned NULL\n");
		dataplane_ut_free(ut);
		return 1;
	}

	int res = run_ring_worker_layout_test(shm);
	if (res == TEST_SUCCESS) {
		res = run_ring_object_align_alloc_test(shm);
	}
	if (res == TEST_SUCCESS) {
		res = run_ring_object_bad_capacity_test(shm);
	}
	if (res == TEST_SUCCESS) {
		res = run_ring_object_worker_count_test(shm);
	}
	if (res == TEST_SUCCESS) {
		res = run_ring_object_free_refused_while_referenced_test(shm);
	}
	if (res == TEST_SUCCESS) {
		res = run_ring_object_delete_refused_while_linked_test(shm);
	}
	if (res == TEST_SUCCESS) {
		res = run_ring_object_exists_test(shm);
	}

	dataplane_ut_free(ut);

	return (res == TEST_SUCCESS) ? 0 : 1;
}
