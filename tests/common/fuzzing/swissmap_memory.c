#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/memory.h"
#include "common/memory_block.h"
#include "common/swissmap.h"

#define ARENA_SIZE (1 << 20) // 1MB arena
#define MAX_MAPS 10
#define MAX_OPERATIONS 500

// Memory failure injection parameters
struct memory_failure_params {
	size_t fail_after_count;    // Fail after N allocations
	size_t current_alloc_count; // Current allocation count
	bool should_fail;	    // Whether to fail allocations
	size_t fail_size_threshold; // Fail allocations above this size
};

// Fuzzing parameters
struct swissmap_memory_fuzzing_params {
	void *arena;
	struct block_allocator ba;
	struct memory_context mctx;
	swiss_map_config_t config;
	struct memory_failure_params failure_params;
	swiss_map_t *maps[MAX_MAPS];
	size_t map_count;
};

static struct swissmap_memory_fuzzing_params fuzz_params = {
	.map_count = 0,
};

// Custom allocator that can simulate allocation failures
static void *
failing_alloc(void *ctx, size_t size) {
	struct swissmap_memory_fuzzing_params *params =
		(struct swissmap_memory_fuzzing_params
			 *)((char *)ctx -
			    offsetof(
				    struct swissmap_memory_fuzzing_params, mctx
			    ));

	params->failure_params.current_alloc_count++;

	// Fail if we've reached the failure count
	if (params->failure_params.should_fail &&
	    params->failure_params.current_alloc_count >=
		    params->failure_params.fail_after_count) {
		errno = ENOMEM;
		return NULL;
	}

	// Fail if allocation size is above threshold
	if (params->failure_params.should_fail &&
	    size >= params->failure_params.fail_size_threshold) {
		errno = ENOMEM;
		return NULL;
	}

	return memory_balloc((struct memory_context *)ctx, size);
}

// Custom free function
static void
failing_free(void *ctx, void *ptr, size_t size) {
	memory_bfree((struct memory_context *)ctx, ptr, size);
}

static int
fuzz_setup() {
	fuzz_params.arena = malloc(ARENA_SIZE);
	if (fuzz_params.arena == NULL) {
		return -1;
	}

	block_allocator_init(&fuzz_params.ba);
	block_allocator_put_arena(
		&fuzz_params.ba, fuzz_params.arena, ARENA_SIZE
	);
	memory_context_init(
		&fuzz_params.mctx, "swissmap_memory_fuzzing", &fuzz_params.ba
	);

	// Initialize failure parameters
	fuzz_params.failure_params.fail_after_count = 0;
	fuzz_params.failure_params.current_alloc_count = 0;
	fuzz_params.failure_params.should_fail = false;
	fuzz_params.failure_params.fail_size_threshold = SIZE_MAX;

	// Configure map with custom allocator
	fuzz_params.config.key_size = sizeof(uint32_t);
	fuzz_params.config.value_size = sizeof(uint32_t);
	fuzz_params.config.hash_fn_id = SWISS_HASH_FNV1A;
	fuzz_params.config.key_equal_fn_id = SWISS_KEY_EQUAL_DEFAULT;
	fuzz_params.config.alloc_fn_id =
		SWISS_ALLOC_SHARED;			   // Will be overridden
	fuzz_params.config.free_fn_id = SWISS_FREE_SHARED; // Will be overridden
	fuzz_params.config.rand_fn_id = SWISS_RAND_DEFAULT;
	fuzz_params.config.mem_ctx = &fuzz_params.mctx;

	// Override allocator functions in registry for testing
	swiss_func_registry[SWISS_ALLOC_SHARED] = (void *)failing_alloc;
	swiss_func_registry[SWISS_FREE_SHARED] = (void *)failing_free;

	return 0;
}

// Test map creation under memory pressure
static void
test_map_creation_failures(const uint8_t *data, size_t size) {
	if (size < 4)
		return;

	// Configure failure parameters from fuzzer input
	fuzz_params.failure_params.fail_after_count = data[0] + 1;
	fuzz_params.failure_params.should_fail = true;
	fuzz_params.failure_params.current_alloc_count = 0;

	// Try to create a map - should handle allocation failure gracefully
	swiss_map_t *map = swiss_map_new(&fuzz_params.config, data[1]);

	// If creation succeeded, try some operations
	if (map) {
		uint32_t key = *(uint32_t *)data;
		uint32_t value = key ^ 0xDEADBEEF;

		// These operations might fail due to memory pressure
		swiss_map_put(map, &key, &value);

		void *found_value;
		swiss_map_get(map, &key, &found_value);

		swiss_map_delete(map, &key);
		swiss_map_free(map);
	}

	fuzz_params.failure_params.should_fail = false;
}

// Test table growth under memory pressure
static void
test_growth_failures(const uint8_t *data, size_t size) {
	if (size < 8)
		return;

	swiss_map_t *map = swiss_map_new(&fuzz_params.config, 8);
	if (!map)
		return;

	// Fill the map to trigger growth
	for (size_t i = 0; i < size && i < 100; i++) {
		uint32_t key = data[i] | (i << 8);
		uint32_t value = key ^ 0xCAFEBABE;

		// Configure failure at different points during growth
		if (i == size / 2) {
			fuzz_params.failure_params.fail_after_count =
				fuzz_params.failure_params.current_alloc_count +
				2;
			fuzz_params.failure_params.should_fail = true;
		}

		// This might fail during table growth/splitting
		int result = swiss_map_put(map, &key, &value);
		(void)result; // Suppress unused variable warning

		// Verify map is still in consistent state
		assert(swiss_map_size(map) <= i + 1);
	}

	fuzz_params.failure_params.should_fail = false;
	swiss_map_free(map);
}

// Test directory expansion failures
static void
test_directory_expansion_failures(const uint8_t *data, size_t size) {
	if (size < 16)
		return;

	swiss_map_t *map = swiss_map_new(&fuzz_params.config, 8);
	if (!map)
		return;

	// Insert many keys to force directory expansion
	for (size_t i = 0; i < size && i < 200; i++) {
		uint32_t key = (data[i % size] << 24) | (i << 8) |
			       (data[(i + 1) % size]);
		uint32_t value = key ^ 0xDEADC0DE;

		// Inject failure during directory expansion
		if (i == size / 3) {
			fuzz_params.failure_params.fail_size_threshold =
				64; // Small threshold
			fuzz_params.failure_params.should_fail = true;
		}

		swiss_map_put(map, &key, &value);

		// Verify map consistency
		void *found_value;
		if (swiss_map_get(map, &key, &found_value)) {
			assert(*(uint32_t *)found_value == value);
		}
	}

	fuzz_params.failure_params.should_fail = false;
	fuzz_params.failure_params.fail_size_threshold = SIZE_MAX;
	swiss_map_free(map);
}

// Test multiple maps under memory pressure
static void
test_multiple_maps_memory_pressure(const uint8_t *data, size_t size) {
	if (size < 4)
		return;

	size_t num_maps = (data[0] % MAX_MAPS) + 1;
	swiss_map_t *maps[MAX_MAPS] = {NULL};

	// Create multiple maps
	for (size_t i = 0; i < num_maps; i++) {
		// Inject failures randomly
		if (i > 0 && (data[i % size] & 1)) {
			fuzz_params.failure_params.fail_after_count =
				fuzz_params.failure_params.current_alloc_count +
				(data[i % size] % 5) + 1;
			fuzz_params.failure_params.should_fail = true;
		}

		maps[i] = swiss_map_new(
			&fuzz_params.config, data[i % size] % 32 + 8
		);

		if (maps[i]) {
			// Add some data to each map
			for (size_t j = 0; j < 10 && j < size; j++) {
				uint32_t key = (i << 16) | j;
				uint32_t value = data[j % size] | (i << 8);
				swiss_map_put(maps[i], &key, &value);
			}
		}

		fuzz_params.failure_params.should_fail = false;
	}

	// Verify all maps are still consistent
	for (size_t i = 0; i < num_maps; i++) {
		if (maps[i]) {
			for (size_t j = 0; j < 10 && j < size; j++) {
				uint32_t key = (i << 16) | j;
				uint32_t expected_value =
					data[j % size] | (i << 8);

				void *found_value;
				if (swiss_map_get(
					    maps[i], &key, &found_value
				    )) {
					assert(*(uint32_t *)found_value ==
					       expected_value);
				}
			}
		}
	}

	// Clean up
	for (size_t i = 0; i < num_maps; i++) {
		if (maps[i]) {
			swiss_map_free(maps[i]);
		}
	}
}

// Test memory leak detection
static void
test_memory_leak_detection() {
	size_t initial_alloc_size = fuzz_params.mctx.balloc_size;
	size_t initial_free_size = fuzz_params.mctx.bfree_size;

	// Create and destroy a map
	swiss_map_t *map = swiss_map_new(&fuzz_params.config, 16);
	if (map) {
		// Add some data
		for (uint32_t i = 0; i < 50; i++) {
			uint32_t value = i * 2;
			swiss_map_put(map, &i, &value);
		}

		// Clear and free
		swiss_map_clear(map);
		swiss_map_free(map);
	}

	// Check for memory leaks
	size_t final_alloc_size = fuzz_params.mctx.balloc_size;
	size_t final_free_size = fuzz_params.mctx.bfree_size;

	size_t net_initial = initial_alloc_size - initial_free_size;
	size_t net_final = final_alloc_size - final_free_size;

	// Should not have leaked memory
	assert(net_initial == net_final);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) { // NOLINT
	if (fuzz_params.arena == NULL) {
		if (fuzz_setup() != 0) {
			exit(1);
		}
	}

	if (size == 0)
		return 0;

	// Test different memory failure scenarios
	switch (data[0] % 5) {
	case 0:
		test_map_creation_failures(data + 1, size - 1);
		break;
	case 1:
		test_growth_failures(data + 1, size - 1);
		break;
	case 2:
		test_directory_expansion_failures(data + 1, size - 1);
		break;
	case 3:
		test_multiple_maps_memory_pressure(data + 1, size - 1);
		break;
	case 4:
		test_memory_leak_detection();
		break;
	}

	// Reset failure parameters for next iteration
	fuzz_params.failure_params.should_fail = false;
	fuzz_params.failure_params.current_alloc_count = 0;
	fuzz_params.failure_params.fail_after_count = 0;
	fuzz_params.failure_params.fail_size_threshold = SIZE_MAX;

	return 0;
}
