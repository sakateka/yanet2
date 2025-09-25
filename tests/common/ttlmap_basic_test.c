/**
 * @file test_ttlmap.c
 * @brief Comprehensive test program for TTLMap implementation
 */

#include "common/memory.h"
#include "common/ttlmap.h"
#include "test_utils.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#define ARENA_SIZE (1 << 20) * 400 // 400MB arena

volatile uint32_t now = 0; // For testing purposes it's fine.
const uint32_t ttl = 50000;

static inline void
init_default_config(
	ttlmap_config_t *config, uint32_t index_size, uint32_t extra_count
) {
	config->key_size = sizeof(int);
	config->value_size = sizeof(int);
	config->hash_seed = 0;
	config->worker_count = 1;
	config->hash_fn_id = TTLMAP_HASH_FNV1A;
	config->key_equal_fn_id = TTLMAP_KEY_EQUAL_DEFAULT;
	config->rand_fn_id = TTLMAP_RAND_DEFAULT;
	config->index_size = index_size;
	config->extra_bucket_count = extra_count;
}

void
test_constants() {
	printf("L%d: Bucket size const\n", __LINE__);
	assert(TTLMAP_BUCKET_SIZE == sizeof(ttlmap_bucket_t));

	printf("L%d: Chunk index max size is power of 2\n", __LINE__);
	assert(align_up_pow2(TTLMAP_CHUNK_INDEX_MAX_SIZE) ==
	       TTLMAP_CHUNK_INDEX_MAX_SIZE);

	printf("L%d: Chunk mask is power of 2\n", __LINE__);
	assert(align_up_pow2(TTLMAP_CHUNK_INDEX_MASK) - 1 ==
	       TTLMAP_CHUNK_INDEX_MASK);
}

void
test_basic_operations(void *arena) {
	printf("Testing basic operations...\n");
	uint16_t worker_idx = 0;

	// Create fresh memory context from common arena.
	struct memory_context *ctx =
		init_context_from_arena(arena, ARENA_SIZE, "basic_ops");

	ttlmap_config_t config = {0};
	init_default_config(&config, 128, 8);
	config.hash_seed = 0x12345678;

	printf("L%d: TTLMap new()\n", __LINE__);
	ttlmap_t *map = ttlmap_new(&config, ctx);
	assert(map != NULL);

	printf("L%d: TTLMap empty()\n", __LINE__);
	bool is_empty = ttlmap_empty(map);
	assert(is_empty);

	printf("L%d: TTLMap size()\n", __LINE__);
	size_t size = ttlmap_size(map);
	assert(size == 0);

	// Test insertion.
	int key1 = 777, value1 = 100;
	printf("L%d: TTLMap put()\n", __LINE__);
	int ret = ttlmap_put(map, worker_idx, now, ttl, &key1, &value1, NULL);
	assert(ret >= 0);

	printf("L%d: TTLMap size()\n", __LINE__);
	size = ttlmap_size(map);
	assert(size == 1);

	printf("L%d: TTLMap empty()\n", __LINE__);
	is_empty = ttlmap_empty(map);
	assert(!is_empty);

	// Test retrieval.
	int *found_value = NULL;
	printf("L%d: TTLMap get()\n", __LINE__);
	int get_ok = ttlmap_get(
		map, worker_idx, now, &key1, (void **)&found_value, NULL
	);
	assert(get_ok >= 0);

	assert(*found_value == 100);

	// Test update.
	int value2 = 200;
	printf("L%d: TTLMap put()\n", __LINE__);
	ret = ttlmap_put(map, worker_idx, now, ttl, &key1, &value2, NULL);
	assert(ret >= 0);

	printf("L%d: TTLMap size()\n", __LINE__);
	size = ttlmap_size(map);
	assert(size == 1); // Size shouldn't change.

	printf("L%d: TTLMap get()\n", __LINE__);
	get_ok = ttlmap_get(
		map, worker_idx, now, &key1, (void **)&found_value, NULL
	);
	assert(get_ok >= 0);
	assert(*found_value == 200);

	// Test multiple insertions.
	printf("L%d: TTLMap put() +100 values\n", __LINE__);
	for (int i = 0; i < 100; i++) {
		int key = i;
		int value = i * 10;
		int ret = ttlmap_put(
			map, worker_idx, now, ttl, &key, &value, NULL
		);
		assert(ret >= 0);
		size = ttlmap_size(map);
		assert(size == (size_t)(i + 2)); // + 1 existing (key1).
	}
	printf("L%d: TTLMap size()\n", __LINE__);
	size = ttlmap_size(map);
	assert(size == 101); // 100 new + 1 existing (key1).
	printf("L%d: Complete inserting +100 values\n", __LINE__);

	printf("L%d: Going to read 100 values\n", __LINE__);
	// Test retrieval of multiple values.
	for (int i = 0; i < 100; i++) {
		int key = i;
		get_ok = ttlmap_get(
			map, worker_idx, now, &key, (void **)&found_value, NULL
		);
		if (get_ok < 0) {
			printf("L%d: failed to get key for %d\n", __LINE__, i);
			assert(false);
			exit(1);
		}
		if (i == 42) {
			assert(*found_value != 200); // This was updated.
		} else {
			assert(*found_value == i * 10);
		}
	}

	// Clean up.
	printf("L%d: Going to destroy the map\n", __LINE__);
	ttlmap_destroy(map, ctx);

	// Verify memory leaks.
	verify_memory_leaks(ctx, "basic_operations");
	printf("L%d: Basic operations test PASSED\n", __LINE__);
}

/**
 * @brief Custom hash function that always returns the same value.
 * This is used to force collisions for testing purposes.
 */
static uint64_t
ttlmap_hash_collision_test(const void *key, size_t key_size, uint32_t seed) {
	(void)key;	// Unused parameter.
	(void)key_size; // Unused parameter.
	(void)seed;	// Unused parameter.

	// Always return the same hash value to force all keys to collide.
	// Using a non-zero value to avoid any special handling of zero.
	return 0x12345678;
}

void
test_collision_handling(void *arena) {
	printf("Testing collision handling...\n");

	size_t worker_idx = 0;

	// Create fresh memory context from common arena.
	struct memory_context *ctx =
		init_context_from_arena(arena, ARENA_SIZE, "collision");

	ttlmap_config_t config = {0};
	init_default_config(&config, 1000, 1000);

	// Register our custom collision hash function.
	void *original_func = ttlmap_func_registry[TTLMAP_HASH_FNV1A];
	ttlmap_func_registry[TTLMAP_HASH_FNV1A] =
		(void *)ttlmap_hash_collision_test;

	// Small index size to force collisions.
	ttlmap_t *map = ttlmap_new(&config, ctx);
	assert(map != NULL);

	// Insert many items to force collisions and chain growth.
	for (int i = 0; i < 1000; i++) {
		int key = i;
		int value = i * 2;
		int ret = ttlmap_put(
			map, worker_idx, now, ttl, &key, &value, NULL
		);
		assert(ret >= 0);
	}

	size_t size = ttlmap_size(map);
	assert(size == 1000);

	// Verify all values.
	for (int i = 0; i < 1000; i++) {
		int key = i;
		int *found_value = NULL;
		int get_ok = ttlmap_get(
			map, worker_idx, now, &key, (void **)&found_value, NULL
		);
		assert(get_ok >= 0);
		assert(*found_value == i * 2);
	}

	// Check chain length statistics.
	printf("  Max chain length: %zu\n", ttlmap_max_chain_length(map));

	ttlmap_stats_t stats;
	ttlmap_get_stats(map, &stats);
	printf("  Memory used: %zu bytes\n", stats.memory_used);

	ttlmap_destroy(map, ctx);

	// Restore the original function in the registry.
	ttlmap_func_registry[TTLMAP_HASH_FNV1A] = original_func;

	// Verify memory leaks.
	verify_memory_leaks(ctx, "collision_handling");
	printf("Collision handling test PASSED\n");
}

int
main() {
	printf("%s%s=== TTLMap Test Suite ===%s\n\n", C_BOLD, C_WHITE, C_RESET);

	// Create common arena for all tests
	void *arena = allocate_locked_memory(ARENA_SIZE);
	if (arena == NULL) {
		printf("could not allocate arena\n");
		return -1;
	}

	printf("%s%s=== Single-threaded Tests ===%s\n", C_BOLD, C_BLUE, C_RESET
	);
	test_constants();
	test_basic_operations(arena);
	test_collision_handling(arena);

	free_arena(arena, ARENA_SIZE);
	printf("\n%s%s=== All tests PASSED ===%s\n", C_BOLD, C_GREEN, C_RESET);
	return 0;
}
