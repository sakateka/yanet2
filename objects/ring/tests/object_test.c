/*
 * Lifecycle tests for the ring shared object: layout, checked allocation,
 * validation, reference-based refusal of free and delete, and existence.
 *
 * Bad capacities are rejected with the arena left unchanged, the worker
 * count tracks the dataplane's workers, and a generation reference or a
 * linking module refuses destruction exactly as for every other shared
 * object.
 */

#include "api/agent.h"

#include "common/asan.h"
#include "common/memory.h"
#include "common/memory_block.h"
#include "common/test_assert.h"

#include "lib/controlplane/agent/agent.h"
#include "lib/controlplane/config/cp_module.h"
#include "lib/controlplane/config/cp_object.h"
#include "lib/controlplane/config/zone.h"
#include "lib/dataplane/config/zone.h"
#include "lib/dataplane/module/module.h"

#include "objects/ring/api/ring_object.h"

#include "lib/dataplane_ut/dataplane_ut.h"
#include "lib/errors/errors.h"

#include "lib/logging/log.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RING_OBJECT_TEST_MEMORY_LIMIT (2u * 1024u * 1024u)

// Module type of the test-local stub that links a ring.
#define RING_TEST_LINKER_MODULE "ring_test_linker"

struct module *
new_module_ring_test_linker(void);

// Dataplane constructor of the stub linker module, resolved by the harness
// through dlsym from this binary.
//
// The stub only gives module config init a registered module type: the harness
// never runs packets, so the module needs no packet or commit handler.
struct module *
new_module_ring_test_linker(void) {
	struct module *module = (struct module *)calloc(1, sizeof(*module));
	if (module == NULL) {
		return NULL;
	}
	snprintf(
		module->name,
		sizeof(module->name),
		"%s",
		RING_TEST_LINKER_MODULE
	);
	return module;
}

// Creates a bare stub module config, owned by the caller until freed with
// ring_test_linker_free.
static struct cp_module *
ring_test_linker_new(struct agent *agent, const char *name, yanet_error **err) {
	struct cp_module *module = (struct cp_module *)memory_balloc(
		&agent->memory_context, sizeof(*module)
	);
	if (module == NULL) {
		yanet_error_add(err, "failed to allocate the linker module");
		return NULL;
	}
	if (cp_module_init(module, agent, RING_TEST_LINKER_MODULE, name, err)) {
		memory_bfree(&agent->memory_context, module, sizeof(*module));
		return NULL;
	}
	return module;
}

// Destroys a stub module config once no generation references it.
static int
ring_test_linker_free(struct cp_module *module, yanet_error **err) {
	if (cp_module_try_destroy(module, err)) {
		return -1;
	}
	struct agent *agent = ADDR_OF(&module->agent);
	cp_module_fini(module);
	memory_bfree(&agent->memory_context, module, sizeof(*module));
	return 0;
}

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

// The checked over-allocation rounds the array base and each entry's stride
// up to the requested alignment, and a matching free restores the arena.
//
// A buddy block is always aligned to at least its own size, so only ASan's
// red-zone offset can misalign the raw block. The postconditions hold
// either way; the extra check under ASan confirms the rounding corrects a
// real offset.
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

// Smallest power of two strictly greater than the given value.
//
// Builds an oversize capacity that is a power of two yet exceeds the
// allocator's maximum whether or not ASan red zones lowered it.
static uint32_t
pow2_above(uint32_t val) {
	uint32_t pow2 = 1;
	while (pow2 <= val) {
		pow2 <<= 1;
	}
	return pow2;
}

// A capacity of zero, below the frame, not a power of two or above the
// allocator's maximum block is refused with the arena left unchanged.
//
// The errno names the failed check.
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

// A dataplane worker count of zero or above UINT16_MAX is refused before any
// allocation, so the 16-bit worker count never truncates.
//
// The harness runs two workers; the test overrides the published count
// for each probe and restores it before returning.
static int
run_ring_object_bad_worker_count_test(struct yanet_shm *shm) {
	yanet_error *err = NULL;

	struct agent *agent = agent_attach(
		shm, 0, "ring-bad-workers", RING_OBJECT_TEST_MEMORY_LIMIT, &err
	);
	TEST_ASSERT_NOT_NULL(agent, "agent_attach failed");

	struct dp_config *dp_config = agent_dp_config(agent);
	uint64_t saved_worker_count = dp_config->worker_count;

	struct {
		uint64_t worker_count;
		int expected_errno;
	} bad_counts[] = {
		{0, EINVAL},
		{(uint64_t)UINT16_MAX + 1, E2BIG},
	};
	int res = TEST_SUCCESS;
	for (size_t i = 0; i < sizeof(bad_counts) / sizeof(bad_counts[0]);
	     ++i) {
		uint64_t worker_count = bad_counts[i].worker_count;
		size_t baseline =
			block_allocator_free_size(&agent->block_allocator);

		dp_config->worker_count = worker_count;
		yanet_error *create_err = NULL;
		struct cp_object *object = ring_object_config_new(
			agent, "bad-workers", 64, &create_err
		);
		int create_errno = errno;
		dp_config->worker_count = saved_worker_count;
		yanet_error_free(create_err);

		if (object != NULL) {
			LOG(ERROR,
			    "worker count %lu must be refused",
			    (unsigned long)worker_count);
			yanet_error *free_err = NULL;
			ring_object_config_free(object, &free_err);
			yanet_error_free(free_err);
			res = TEST_FAILED;
			break;
		}
		if (create_errno != bad_counts[i].expected_errno) {
			LOG(ERROR,
			    "worker count %lu: errno %d, expected %d",
			    (unsigned long)worker_count,
			    create_errno,
			    bad_counts[i].expected_errno);
			res = TEST_FAILED;
			break;
		}
		if (block_allocator_free_size(&agent->block_allocator) !=
		    baseline) {
			LOG(ERROR,
			    "worker count %lu: a refused create must leave "
			    "the arena unchanged",
			    (unsigned long)worker_count);
			res = TEST_FAILED;
			break;
		}
	}

	agent_detach(agent);
	return res;
}

// The created object holds one ring per dataplane worker, each with its own
// metadata and data area, and resolves no ring past the last worker.
static int
run_ring_object_worker_rings_test(struct yanet_shm *shm) {
	yanet_error *err = NULL;

	struct agent *agent = agent_attach(
		shm, 0, "ring-worker-rings", RING_OBJECT_TEST_MEMORY_LIMIT, &err
	);
	TEST_ASSERT_NOT_NULL(agent, "agent_attach failed");

	uint64_t worker_count = agent_dp_config(agent)->worker_count;
	TEST_ASSERT_EQUAL(
		(long)worker_count, 2L, "the harness must run two workers"
	);

	struct cp_object *object =
		ring_object_config_new(agent, "worker-rings", 64, &err);
	TEST_ASSERT_NOT_NULL(
		object,
		"ring_object_config_new failed: %s",
		err ? yanet_error_message(err) : "?"
	);
	TEST_ASSERT_EQUAL(
		ring_object_capacity(object),
		64,
		"the object's capacity must stick"
	);

	for (uint64_t idx = 0; idx < worker_count; ++idx) {
		struct ring_worker *worker = ring_object_worker(object, idx);
		uint8_t *data = ring_object_worker_data(object, idx);
		TEST_ASSERT_NOT_NULL(
			worker, "worker %lu has no ring", (unsigned long)idx
		);
		TEST_ASSERT_NOT_NULL(
			data, "worker %lu has no data", (unsigned long)idx
		);

		for (uint64_t prev = 0; prev < idx; ++prev) {
			TEST_ASSERT(
				ring_object_worker(object, prev) != worker,
				"workers %lu and %lu share metadata",
				(unsigned long)prev,
				(unsigned long)idx
			);
			TEST_ASSERT(
				ring_object_worker_data(object, prev) != data,
				"workers %lu and %lu share a data area",
				(unsigned long)prev,
				(unsigned long)idx
			);
		}
	}
	TEST_ASSERT_NULL(
		ring_object_worker(object, worker_count),
		"no ring may resolve past the last worker"
	);
	TEST_ASSERT_NULL(
		ring_object_worker_data(object, worker_count),
		"no data area may resolve past the last worker"
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
// through the same guard every shared object gets.
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
		ring_test_linker_new(agent, "ring-linker", &err);
	TEST_ASSERT_NOT_NULL(
		module,
		"ring_test_linker_new failed: %s",
		err ? yanet_error_message(err) : "?"
	);
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

	// Once the linking module is gone, the same delete goes through.
	TEST_ASSERT_SUCCESS(
		agent_delete_module(
			agent, RING_TEST_LINKER_MODULE, "ring-linker", &err
		),
		"agent_delete_module failed: %s",
		err ? yanet_error_message(err) : "?"
	);
	TEST_ASSERT_SUCCESS(
		ring_test_linker_free(module, &err),
		"freeing the unpublished linker module failed: %s",
		err ? yanet_error_message(err) : "?"
	);
	TEST_ASSERT_SUCCESS(
		agent_delete_object(agent, RING_OBJECT_TYPE, "linked", &err),
		"deleting the ring after its linking module is removed must "
		"succeed: %s",
		err ? yanet_error_message(err) : "?"
	);
	TEST_ASSERT(
		!ring_object_exists(agent, "linked"),
		"a deleted ring must no longer be published"
	);

	agent_detach(agent);
	return TEST_SUCCESS;
}

// A second fini after a full create is a no-op: the first one clears the
// fields it freed, so nothing is returned to the arena twice.
static int
run_ring_object_fini_idempotent_test(struct yanet_shm *shm) {
	yanet_error *err = NULL;

	struct agent *agent = agent_attach(
		shm, 0, "ring-fini-twice", RING_OBJECT_TEST_MEMORY_LIMIT, &err
	);
	TEST_ASSERT_NOT_NULL(agent, "agent_attach failed");

	size_t baseline = block_allocator_free_size(&agent->block_allocator);

	struct ring_object *self = ring_object_new(agent);
	TEST_ASSERT_NOT_NULL(self, "ring_object_new failed");
	TEST_ASSERT_SUCCESS(
		ring_object_init(self, agent, "fini-twice", &err),
		"ring_object_init failed"
	);
	TEST_ASSERT_SUCCESS(
		ring_object_create(self, 64, &err), "ring_object_create failed"
	);

	ring_object_fini(self);
	ring_object_fini(self);
	TEST_ASSERT_NULL(
		ADDR_OF(&self->workers), "fini must forget the workers array"
	);
	TEST_ASSERT_EQUAL(
		(long)self->worker_count, 0L, "fini must reset worker_count"
	);
	ring_object_free(self, agent);

	TEST_ASSERT_EQUAL(
		(long)block_allocator_free_size(&agent->block_allocator),
		(long)baseline,
		"a double fini must return the arena to exactly its baseline"
	);

	agent_detach(agent);
	return TEST_SUCCESS;
}

// Running out of memory midway through the per-worker data areas rolls all
// of them back: creation reports ENOMEM and the arena returns to baseline.
//
// The capacity at which worker 0 still fits but worker 1 does not depends
// on the arena's layout, so the test probes powers of two downward and
// picks the first whose failure names worker 1.
static int
run_ring_object_enomem_rollback_test(struct yanet_shm *shm) {
	yanet_error *err = NULL;

	struct agent *agent = agent_attach(
		shm, 0, "ring-enomem", RING_OBJECT_TEST_MEMORY_LIMIT, &err
	);
	TEST_ASSERT_NOT_NULL(agent, "agent_attach failed");
	TEST_ASSERT_EQUAL(
		(long)agent_dp_config(agent)->worker_count,
		2L,
		"the harness must run two workers"
	);

	size_t baseline = block_allocator_free_size(&agent->block_allocator);

	bool rolled_back_mid_allocation = false;
	for (uint32_t capacity =
		     pow2_above(MEMORY_BLOCK_ALLOCATOR_MAX_SIZE) >> 1;
	     capacity >= RING_RECORD_FRAME_SIZE;
	     capacity >>= 1) {
		yanet_error *create_err = NULL;
		struct cp_object *object = ring_object_config_new(
			agent, "enomem", capacity, &create_err
		);
		if (object != NULL) {
			// Both workers fit, and every smaller capacity will
			// too: nothing left to probe.
			yanet_error *free_err = NULL;
			TEST_ASSERT_SUCCESS(
				ring_object_config_free(object, &free_err),
				"freeing a dangling ring object must succeed"
			);
			break;
		}
		int create_errno = errno;
		TEST_ASSERT_EQUAL(
			create_errno,
			ENOMEM,
			"capacity %u must fail only for lack of memory",
			capacity
		);
		TEST_ASSERT_EQUAL(
			(long)block_allocator_free_size(&agent->block_allocator
			),
			(long)baseline,
			"a failed create must restore the arena: capacity=%u",
			capacity
		);
		for (const yanet_error *cause = create_err; cause != NULL;
		     cause = yanet_error_cause(cause)) {
			const char *message = yanet_error_message(cause);
			if (message != NULL &&
			    strstr(message, "ring data for worker 1") != NULL) {
				rolled_back_mid_allocation = true;
			}
		}
		yanet_error_free(create_err);
		if (rolled_back_mid_allocation) {
			break;
		}
	}
	TEST_ASSERT(
		rolled_back_mid_allocation,
		"some capacity must fit worker 0 but not worker 1"
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

	const char *mods_to_load[] = {RING_TEST_LINKER_MODULE};
	const char *objs_to_load[] = {RING_OBJECT_TYPE};

	struct dataplane_ut_config cfg = {
		.cp_memory = 1u << 26,
		.dp_memory = 1u << 20,
		.worker_count = 2,
		.modules = mods_to_load,
		.module_count = 1,
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

	struct test_case {
		const char *name;
		int (*func)(struct yanet_shm *shm);
	};

	struct test_case cases[] = {
		{"worker_layout", run_ring_worker_layout_test},
		{"align_alloc", run_ring_object_align_alloc_test},
		{"bad_capacity", run_ring_object_bad_capacity_test},
		{"bad_worker_count", run_ring_object_bad_worker_count_test},
		{"worker_rings", run_ring_object_worker_rings_test},
		{"free_refused_while_referenced",
		 run_ring_object_free_refused_while_referenced_test},
		{"delete_refused_while_linked",
		 run_ring_object_delete_refused_while_linked_test},
		{"exists", run_ring_object_exists_test},
		{"fini_idempotent", run_ring_object_fini_idempotent_test},
		{"enomem_rollback", run_ring_object_enomem_rollback_test},
	};

	// The cases share one harness, so a failure stops the run rather
	// than letting later cases observe the state it left behind.
	int res = TEST_SUCCESS;
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		LOG(INFO, "%s running", cases[i].name);
		res = cases[i].func(shm);
		if (res != TEST_SUCCESS) {
			LOG(ERROR, "%s failed", cases[i].name);
			break;
		}
		LOG(INFO, "%s passed", cases[i].name);
	}

	dataplane_ut_free(ut);

	return (res == TEST_SUCCESS) ? 0 : 1;
}
