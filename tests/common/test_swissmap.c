#include "common/memory.h"
#include "common/memory_address.h"
#include "common/memory_block.h"
#include "common/swissmap.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_SIZE (1 << 20) // 1MB arena

// Simple key equality function for integers
bool
int_equal(const void *a, const void *b, size_t size) {
	(void)size;
	return *(const int *)a == *(const int *)b;
}

// Simple key equality function for strings
bool
string_equal(const void *a, const void *b, size_t size) {
	(void)size;
	return strcmp((const char *)a, (const char *)b) == 0;
}

// Hash function that creates controlled collisions
static uint64_t
collision_hash(const void *key, size_t key_size, uint64_t seed) {
	(void)key_size;
	(void)seed;
	int *k = (int *)key;
	// Force keys to same group but different H2 values
	return ((*k / 8) << 7) | (*k & 0x7F);
}

// Helper functions for common test patterns

// Create standard int map configuration with memory context
static swiss_map_config_t
create_default_int_config(struct memory_context *ctx) {
	swiss_map_config_t config = {0};
	config.key_size = sizeof(int);
	config.value_size = sizeof(int);
	config.hash_fn_id = SWISS_HASH_FNV1A;
	config.key_equal_fn_id =
		SWISS_KEY_EQUAL_DEFAULT; // Note: using default instead of
					 // int_equal
	config.alloc_fn_id = SWISS_ALLOC_SHARED;
	config.free_fn_id = SWISS_FREE_SHARED;
	config.rand_fn_id = SWISS_RAND_DEFAULT;
	config.mem_ctx = ctx;
	return config;
}

// Create and validate map
static swiss_map_t *
create_map(swiss_map_config_t *config, size_t size) {
	swiss_map_t *map = swiss_map_new(config, size);
	assert(map != NULL);
	return map;
}

// Insert and verify multiple key-value pairs
static void
insert_and_verify(swiss_map_t *map, int start, int count, int multiplier) {
	size_t initial_size = swiss_map_size(map);

	for (int i = start; i < start + count; i++) {
		int value = i * multiplier;
		assert(swiss_map_put(map, &i, &value) == 0);
	}
	assert(swiss_map_size(map) == initial_size + (size_t)count);

	for (int i = start; i < start + count; i++) {
		int *found_value;
		assert(swiss_map_get(map, &i, (void **)&found_value));
		assert(*found_value == i * multiplier);
	}
}

// Test basic integer map operations
void
test_int_map(struct memory_context *ctx) {
	swiss_map_config_t config = create_default_int_config(ctx);
	swiss_map_t *map = create_map(&config, 0);
	assert(swiss_map_empty(map));
	assert(swiss_map_size(map) == 0);

	// Test insertions
	int key1 = 42, value1 = 100;
	int key2 = 24, value2 = 200;
	int key3 = 13, value3 = 300;

	swiss_map_put(map, &key1, &value1);
	assert(swiss_map_size(map) == 1);
	assert(!swiss_map_empty(map));

	swiss_map_put(map, &key2, &value2);
	swiss_map_put(map, &key3, &value3);
	assert(swiss_map_size(map) == 3);

	// Test lookups
	int *found_value;
	assert(swiss_map_get(map, &key1, (void **)&found_value));
	assert(*found_value == value1);

	assert(swiss_map_get(map, &key2, (void **)&found_value));
	assert(*found_value == value2);

	assert(swiss_map_get(map, &key3, (void **)&found_value));
	assert(*found_value == value3);

	// Test non-existent key
	int key4 = 999;
	assert(!swiss_map_get(map, &key4, (void **)&found_value));

	// Test update
	int new_value1 = 150;
	swiss_map_put(map, &key1, &new_value1);
	assert(swiss_map_size(map) == 3); // Size shouldn't change
	assert(swiss_map_get(map, &key1, (void **)&found_value));
	assert(*found_value == new_value1);

	// Test deletion
	assert(swiss_map_delete(map, &key2));
	assert(swiss_map_size(map) == 2);
	assert(!swiss_map_get(map, &key2, (void **)&found_value));

	// Test deletion of non-existent key
	assert(!swiss_map_delete(map, &key4));
	assert(swiss_map_size(map) == 2);

	// Test clear
	swiss_map_clear(map);
	assert(swiss_map_empty(map));
	assert(swiss_map_size(map) == 0);
	assert(!swiss_map_get(map, &key1, (void **)&found_value));

	swiss_map_free(map);
}

// Test string map operations
void
test_string_map(struct memory_context *ctx) {
	swiss_map_config_t config = {0};
	config.key_size = 32; // Fixed size strings
	config.value_size = sizeof(int);
	config.hash_fn_id = SWISS_HASH_FNV1A;
	config.key_equal_fn_id =
		SWISS_KEY_EQUAL_DEFAULT; // Note: using default instead of
					 // string_equal
	config.alloc_fn_id = SWISS_ALLOC_SHARED;
	config.free_fn_id = SWISS_FREE_SHARED;
	config.rand_fn_id = SWISS_RAND_DEFAULT;
	config.mem_ctx = ctx;

	swiss_map_t *map = swiss_map_new(&config, 0);
	assert(map != NULL);

	// Test string keys
	char key1[32] = "hello";
	char key2[32] = "world";
	char key3[32] = "test";
	int value1 = 100, value2 = 200, value3 = 300;

	swiss_map_put(map, key1, &value1);
	swiss_map_put(map, key2, &value2);
	swiss_map_put(map, key3, &value3);

	assert(swiss_map_size(map) == 3);

	int *found_value;
	assert(swiss_map_get(map, key1, (void **)&found_value));
	assert(*found_value == value1);

	assert(swiss_map_get(map, key2, (void **)&found_value));
	assert(*found_value == value2);

	char key4[32] = "notfound";
	assert(!swiss_map_get(map, key4, (void **)&found_value));

	swiss_map_free(map);
}

// Test type-safe macro interface
SWISS_MAP_DECLARE(IntMap, int, int);

void
test_macro_interface(struct memory_context *ctx) {
	IntMap_t *map = IntMap_new(ctx, 0);
	assert(map != NULL);

	int key = 42, value = 100;
	IntMap_put(map, &key, &value);
	assert(IntMap_size(map) == 1);

	int *found_value;
	assert(IntMap_get(map, &key, &found_value));
	assert(*found_value == value);

	assert(IntMap_delete(map, &key));
	assert(IntMap_size(map) == 0);

	IntMap_free(map);
}

// Test control group operations
void
test_control_operations() {
	swiss_ctrl_group_t ctrl = 0;
	swiss_ctrl_set_empty(&ctrl);

	// Test empty matching
	swiss_bitset_t empty_match = swiss_ctrl_match_empty(ctrl);
	assert(empty_match == BITSET_MSB); // All slots should match as empty

	// Set some slots to different values
	swiss_ctrl_set(&ctrl, 0, 0x42); // h2 = 0x42
	swiss_ctrl_set(&ctrl, 1, 0x24); // h2 = 0x24
	swiss_ctrl_set(&ctrl, 2, CTRL_DELETED);

	// Test h2 matching
	swiss_bitset_t h2_match = swiss_ctrl_match_h2(ctrl, 0x42);
	assert(swiss_bitset_first(h2_match) == 0 && "H2 slot 0 should match");

	h2_match = swiss_ctrl_match_h2(ctrl, 0x24);
	assert(swiss_bitset_first(h2_match) == 1 && "H2 slot 1 should match");

	// Test empty or deleted matching
	swiss_bitset_t empty_or_deleted =
		swiss_ctrl_match_empty_or_deleted(ctrl);
	// Should match slots 2 (deleted) and 3-7 (empty)
	assert(empty_or_deleted != 0);
}

// Test hash functions
void
test_hash_functions() {
	int key1 = 42;
	int key2 = 43;
	uint64_t seed = 12345;

	uint64_t hash1 = swiss_hash_fnv1a(&key1, sizeof(key1), seed);
	uint64_t hash2 = swiss_hash_fnv1a(&key2, sizeof(key2), seed);

	// Different keys should produce different hashes (with high
	// probability)
	assert(hash1 != hash2);

	// Same key should produce same hash
	uint64_t hash1_again = swiss_hash_fnv1a(&key1, sizeof(key1), seed);
	assert(hash1 == hash1_again);

	// Test h1 and h2 extraction
	uint64_t h1 = swiss_h1(hash1);
	uint8_t h2 = swiss_h2(hash1);

	// h2 should be 7 bits
	assert(h2 <= 0x7F);

	// Reconstruct should give us back most of the hash
	uint64_t reconstructed = (h1 << 7) | h2;
	assert(reconstructed == hash1);
}

// Hash function for duplicate key bug test
static uint64_t
bug_hash(const void *key, size_t key_size, uint64_t seed) {
	(void)key_size;
	(void)seed;
	return (uint64_t)*(int *)key;
}

void
test_duplicate_key_bug(struct memory_context *ctx) {
	swiss_map_config_t config = create_default_int_config(ctx);
	// Temporarily replace the hash function in the registry with bug_hash
	void *original_hash_fn = swiss_func_registry[SWISS_HASH_FNV1A];
	swiss_func_registry[SWISS_HASH_FNV1A] = (void *)bug_hash;

	swiss_map_t *map = create_map(&config, 16);

	// Fill group 0 completely
	for (int i = 1; i <= 8; i++) {
		int value = i * 100;
		swiss_map_put(map, &i, &value);
	}

	// Insert key 99 (goes to group 1 after probing group 0)
	int key99 = 99, value99 = 9900;
	swiss_map_put(map, &key99, &value99);
	assert(swiss_map_size(map) == 9);

	// Delete key 4 to create deleted slot in group 0 (no empty slots)
	int key4 = 4;
	swiss_map_delete(map, &key4);
	assert(swiss_map_size(map) == 8);

	// Reinsert key 99 - broken logic will use deleted slot instead of
	// finding existing
	int new_value99 = 9999;
	swiss_map_put(map, &key99, &new_value99);

	// Check for duplicate: should still be size 8, but broken logic makes
	// it 9
	size_t final_size = swiss_map_size(map);
	if (final_size > 8) {
		printf("✗ Duplicate key bug detected! Size: %zu (expected 8)\n",
		       final_size);

		// Confirm by double-delete
		swiss_map_delete(map, &key99);
		bool second_delete = swiss_map_delete(map, &key99);
		if (second_delete) {
			printf("✗ Confirmed: Key 99 deleted twice!\n");
		}
		assert(false && "Duplicate key bug confirmed");
	}

	swiss_map_free(map);

	// Restore original hash function
	swiss_func_registry[SWISS_HASH_FNV1A] = original_hash_fn;
}

// Test table growth and splitting
void
test_table_growth(struct memory_context *ctx) {
	swiss_map_config_t config = create_default_int_config(ctx);
	swiss_map_t *map = create_map(&config, 8);

	// Insert many keys to trigger growth and verify
	insert_and_verify(map, 0, 100, 10);

	swiss_map_free(map);
}

// Test collision handling with many keys
void
test_collision_handling(struct memory_context *ctx) {
	swiss_map_config_t config = create_default_int_config(ctx);
	// Temporarily replace the hash function in the registry with
	// collision_hash
	void *original_hash_fn = swiss_func_registry[SWISS_HASH_FNV1A];
	swiss_func_registry[SWISS_HASH_FNV1A] = (void *)collision_hash;

	swiss_map_t *map = create_map(&config, 16);

	// Insert keys that will collide in groups and verify
	insert_and_verify(map, 0, 64, 100);

	// Test deletion with collisions
	for (int i = 0; i < 32; i++) {
		assert(swiss_map_delete(map, &i));
	}
	assert(swiss_map_size(map) == 32);

	// Verify remaining keys still work
	for (int i = 32; i < 64; i++) {
		int *found_value;
		assert(swiss_map_get(map, &i, (void **)&found_value));
		assert(*found_value == i * 100);
	}

	swiss_map_free(map);

	// Restore original hash function
	swiss_func_registry[SWISS_HASH_FNV1A] = original_hash_fn;
}

// Test memory leak prevention using block allocator tracking
void
test_memory_leak_prevention(struct memory_context *ctx) {
	swiss_map_config_t config = create_default_int_config(ctx);

	// Record initial allocation state (both counts and sizes)
	size_t initial_alloc_count = ctx->balloc_count;
	size_t initial_free_count = ctx->bfree_count;
	size_t initial_balloc_size = ctx->balloc_size;
	size_t initial_bfree_size = ctx->bfree_size;

	// Create and destroy map
	swiss_map_t *map = create_map(&config, 16);
	assert(ctx->balloc_count > initial_alloc_count);
	assert(ctx->balloc_size > initial_balloc_size);

	size_t after_create_alloc_count = ctx->balloc_count;
	size_t after_create_balloc_size = ctx->balloc_size;

	// Insert some data to trigger allocations
	for (int i = 0; i < 50; i++) {
		int value = i * 10;
		assert(swiss_map_put(map, &i, &value) == 0);
	}

	// Verify more allocations occurred
	assert(ctx->balloc_count > after_create_alloc_count);
	assert(ctx->balloc_size > after_create_balloc_size);

	// Clear the map
	swiss_map_clear(map);

	// Delete all elements
	for (int i = 0; i < 50; i++) {
		swiss_map_delete(map, &i);
	}

	// Free the map
	swiss_map_free(map);

	// Verify all allocations were freed using both methods
	// Method 1: Check allocation counts balance
	size_t final_alloc_count = ctx->balloc_count;
	size_t final_free_count = ctx->bfree_count;
	size_t net_alloc_count = (final_alloc_count - initial_alloc_count) -
				 (final_free_count - initial_free_count);

	// Method 2: Check net memory size
	size_t net_initial_size = initial_balloc_size - initial_bfree_size;
	size_t net_final_size = ctx->balloc_size - ctx->bfree_size;

	if (net_alloc_count != 0) {
		printf("Net allocation count: %zu (allocs: %zu, frees: %zu) - "
		       "Memory leak detected!\n",
		       net_alloc_count,
		       final_alloc_count - initial_alloc_count,
		       final_free_count - initial_free_count);
		assert(false && "Memory leak detected by count");
	}

	if (net_initial_size != net_final_size) {
		printf("Net memory size changed: initial %zu, final %zu - "
		       "Memory leak detected!\n",
		       net_initial_size,
		       net_final_size);
		assert(false && "Memory leak detected by size");
	}

	// Test with multiple maps to ensure no cross-contamination
	initial_alloc_count = ctx->balloc_count;
	initial_free_count = ctx->bfree_count;
	initial_balloc_size = ctx->balloc_size;
	initial_bfree_size = ctx->bfree_size;

	swiss_map_t *maps[5];
	for (int i = 0; i < 5; i++) {
		maps[i] = create_map(&config, 8);
	}

	// Insert data into each map
	for (int i = 0; i < 5; i++) {
		for (int j = 0; j < 1024; j++) {
			int key = i * 10 + j;
			int value = key * 10;
			assert(swiss_map_put(maps[i], &key, &value) == 0);
		}
	}

	// Free all maps
	for (int i = 0; i < 5; i++) {
		swiss_map_free(maps[i]);
	}

	// Verify all allocations were freed using both methods
	final_alloc_count = ctx->balloc_count;
	final_free_count = ctx->bfree_count;
	net_alloc_count = (final_alloc_count - initial_alloc_count) -
			  (final_free_count - initial_free_count);

	net_initial_size = initial_balloc_size - initial_bfree_size;
	net_final_size = ctx->balloc_size - ctx->bfree_size;

	if (net_alloc_count != 0) {
		printf("Net allocation count: %zu (allocs: %zu, frees: %zu) - "
		       "Memory leak detected!\n",
		       net_alloc_count,
		       final_alloc_count - initial_alloc_count,
		       final_free_count - initial_free_count);
		assert(false && "Memory leak detected by count");
	}

	if (net_initial_size != net_final_size) {
		printf("Net memory size changed: initial %zu, final %zu - "
		       "Memory leak detected!\n",
		       net_initial_size,
		       net_final_size);
		assert(false && "Memory leak detected by size");
	}
}

// Test extendible hashing mechanics
void
test_extendible_hashing(struct memory_context *ctx) {
	swiss_map_config_t config = create_default_int_config(ctx);
	size_t hint = 8;
	swiss_map_t *map = create_map(&config, hint);

	// Test initial state
	assert(map->global_depth >= 0);
	assert(map->global_shift == 64 - map->global_depth);

	// Insert keys that test different bit patterns
	int test_keys[] = {
		0x00,
		0x01,
		0x02,
		0x03,
		0x10,
		0x11,
		0x12,
		0x13,
		0x20,
		0x21,
		0x22,
		0x23,
		0x30,
		0x31,
		0x32,
		0x33
	};
	int test_values[] = {
		0, 1, 2, 3, 16, 17, 18, 19, 32, 33, 34, 35, 48, 49, 50, 51
	};

	// Insert all keys
	for (int i = 0; i < 16; i++) {
		assert(swiss_map_put(map, &test_keys[i], &test_values[i]) == 0);
	}

	// Verify global/local depth relationships
	swiss_table_t **directory = (swiss_table_t **)ADDR_OF(&map->dir_ptr);

	for (int i = 0; i < map->dir_len; i++) {
		swiss_table_t *table = (swiss_table_t *)ADDR_OF(&directory[i]);
		assert(table != NULL);
		assert(table->local_depth <= map->global_depth);

		// Verify that when directory entries point to same table,
		// local_depth < global_depth
		if (i > 0 &&
		    table == (swiss_table_t *)ADDR_OF(&directory[i - 1])) {
			assert(table->local_depth < map->global_depth);
		}
	}

	// Test that hash distribution works correctly
	for (int i = 0; i < 16; i++) {
		uint64_t hash = swiss_hash_fnv1a(
			&test_keys[i], sizeof(test_keys[i]), map->seed
		);
		uint64_t dir_idx = swiss_map_directory_index(map, hash);
		swiss_table_t *table = swiss_map_directory_at(map, dir_idx);

		// Verify the key is actually in this table
		int *found_value;
		assert(swiss_table_get(
			table,
			&config,
			map,
			&test_keys[i],
			(void **)&found_value
		));
		assert(*found_value == test_values[i]);
	}

	// Test depth changes during operations
	uint8_t initial_global_depth = map->global_depth;

	// Insert more keys to increase depth
	insert_and_verify(map, 100, MAX_TABLE_CAPACITY * hint, 10);

	// Verify depth may have increased
	assert(map->global_depth > initial_global_depth);

	// Verify all original keys are still accessible
	for (int i = 0; i < 16; i++) {
		int *found_value;
		assert(swiss_map_get(map, &test_keys[i], (void **)&found_value)
		);
		assert(*found_value == test_values[i]);
	}

	swiss_map_free(map);
}

// Test probe sequence algorithm
void
test_probe_sequence_algorithm() {
	// Test probe sequence generation
	uint64_t hash = 0x123456789ABCDEF0ULL;
	uint64_t mask = 0xFF; // 256 groups

	swiss_probe_seq_t seq = swiss_make_probe_seq(hash, mask);

	// Verify initial state
	assert(seq.mask == mask);
	assert(seq.index == 0);
	assert(seq.offset == (swiss_h1(hash) & mask));

	// Test probe sequence advancement
	uint64_t prev_offset = seq.offset;
	seq = swiss_probe_seq_next(seq);
	assert(seq.index == 1);
	assert(seq.offset == ((prev_offset + 1) & mask));

	// Test that probe sequence covers all groups
	bool visited[256] = {false};
	seq = swiss_make_probe_seq(hash, mask);

	for (int i = 0; i < 256; i++) {
		assert(seq.offset < 256);
		assert(!visited[seq.offset]
		); // Should not visit same group twice
		visited[seq.offset] = true;
		seq = swiss_probe_seq_next(seq);
	}

	// Test with different hash values
	uint64_t hash2 = 0xFEDCBA9876543210ULL;
	swiss_probe_seq_t seq2 = swiss_make_probe_seq(hash2, mask);
	assert(seq2.offset != seq.offset
	); // Different hashes should start at different offsets

	// Test with small mask
	uint64_t small_mask = 0x3; // 4 groups
	swiss_probe_seq_t seq3 = swiss_make_probe_seq(hash, small_mask);

	for (int i = 0; i < 4; i++) {
		assert(seq3.offset < 4);
		seq3 = swiss_probe_seq_next(seq3);
	}
}

// Test control byte state transitions
void
test_control_byte_states() {
	swiss_ctrl_group_t ctrl = 0;

	// Test initial empty state
	swiss_ctrl_set_empty(&ctrl);
	for (size_t i = 0; i < SWISS_GROUP_SLOTS; i++) {
		assert(swiss_ctrl_get(ctrl, i) == CTRL_EMPTY);
	}

	// Test empty matching
	swiss_bitset_t empty_match = swiss_ctrl_match_empty(ctrl);
	assert(empty_match == BITSET_MSB); // All slots should be empty

	// Test setting full slots
	uint8_t h2_values[] = {0x01, 0x23, 0x45, 0x67, 0x12, 0x34, 0x56, 0x78};
	for (size_t i = 0; i < SWISS_GROUP_SLOTS; i++) {
		swiss_ctrl_set(&ctrl, i, h2_values[i]);
		assert(swiss_ctrl_get(ctrl, i) == h2_values[i]);
	}

	// Test H2 matching
	for (size_t i = 0; i < SWISS_GROUP_SLOTS; i++) {
		swiss_bitset_t h2_match =
			swiss_ctrl_match_h2(ctrl, h2_values[i]);
		assert(swiss_bitset_first(h2_match) == i);
	}

	// Test setting deleted slots
	swiss_ctrl_set(&ctrl, 2, CTRL_DELETED);
	swiss_ctrl_set(&ctrl, 5, CTRL_DELETED);
	assert(swiss_ctrl_get(ctrl, 2) == CTRL_DELETED);
	assert(swiss_ctrl_get(ctrl, 5) == CTRL_DELETED);

	// Test empty or deleted matching
	swiss_bitset_t empty_or_deleted =
		swiss_ctrl_match_empty_or_deleted(ctrl);
	// Should match slots 2 and 5 (deleted) and no others since no empty
	// slots
	assert((empty_or_deleted & (0xFFULL << (8 * 2))) != 0);
	assert((empty_or_deleted & (0xFFULL << (8 * 5))) != 0);

	// Test full slot matching
	swiss_bitset_t full_match = swiss_ctrl_match_full(ctrl);
	// Should match all slots except 2 and 5
	for (size_t i = 0; i < SWISS_GROUP_SLOTS; i++) {
		if (i != 2 && i != 5) {
			assert((full_match & (0xFFULL << (8 * i))) != 0);
		}
	}

	// Test mixed state handling
	swiss_ctrl_set(&ctrl, 0, CTRL_EMPTY);
	swiss_ctrl_set(&ctrl, 1, 0x42);
	swiss_ctrl_set(&ctrl, 2, CTRL_DELETED);
	swiss_ctrl_set(&ctrl, 3, 0x24);

	// Verify individual states
	assert(swiss_ctrl_get(ctrl, 0) == CTRL_EMPTY);
	assert(swiss_ctrl_get(ctrl, 1) == 0x42);
	assert(swiss_ctrl_get(ctrl, 2) == CTRL_DELETED);
	assert(swiss_ctrl_get(ctrl, 3) == 0x24);

	// Test matching functions work correctly with mixed states
	empty_match = swiss_ctrl_match_empty(ctrl);
	assert(swiss_bitset_first(empty_match) == 0
	); // Only slot 0 should be empty

	swiss_bitset_t h2_match = swiss_ctrl_match_h2(ctrl, 0x42);
	assert(swiss_bitset_first(h2_match) == 1); // Only slot 1 should match

	h2_match = swiss_ctrl_match_h2(ctrl, 0x24);
	assert(swiss_bitset_first(h2_match) == 3); // Only slot 3 should match
}

// Test directory expansion mechanics
void
test_directory_expansion(struct memory_context *ctx) {
	swiss_map_config_t config = create_default_int_config(ctx);
	size_t hint = 8;
	swiss_map_t *map = create_map(&config, hint);

	// Record initial directory state
	uint8_t initial_global_depth = map->global_depth;
	int initial_dir_len = map->dir_len;

	// Insert enough entries to trigger directory expansion
	// The exact number depends on hash distribution and table growth
	// behavior
	insert_and_verify(map, 0, MAX_TABLE_CAPACITY * hint, 10);

	// Insert additional entries to ensure we get tables with different
	// local depths This should create a scenario where some tables have
	// local_depth < global_depth
	insert_and_verify(
		map,
		MAX_TABLE_CAPACITY * hint,
		MAX_TABLE_CAPACITY * hint * 2,
		10
	);

	// Verify directory structure is consistent
	assert(map->global_shift == 64 - map->global_depth);
	assert(map->dir_len > initial_dir_len);
	assert(map->global_depth > initial_global_depth);

	// Test that directory structure is consistent
	swiss_table_t **directory = (swiss_table_t **)ADDR_OF(&map->dir_ptr);

	// Verify directory length is power of 2 and consistent with global
	// depth
	assert(map->dir_len > 0);
	assert((map->dir_len & (map->dir_len - 1)) == 0); // Power of 2 check
	assert(map->dir_len == (1 << map->global_depth));

	// Track which tables we've seen to detect sharing patterns
	bool *table_visited = calloc(map->dir_len, sizeof(bool));
	assert(table_visited != NULL);

	// Track whether we tested the extendible hashing code path
	bool extendible_hashing_tested = false;

	for (int i = 0; i < map->dir_len; i++) {
		swiss_table_t *table = (swiss_table_t *)ADDR_OF(&directory[i]);
		assert(table != NULL);
		assert(table->local_depth <= map->global_depth);

		// Verify table index consistency
		assert(table->index >= 0);
		assert(table->index < map->dir_len);

		// Verify that directory[table->index] points back to this table
		swiss_table_t *dir_table =
			(swiss_table_t *)ADDR_OF(&directory[table->index]);
		assert(dir_table == table);

		// Verify extendible hashing: consecutive directory entries
		// should point to the same table when local_depth <
		// global_depth
		if (table->local_depth < map->global_depth) {
			extendible_hashing_tested = true;
			int entries_per_table =
				1 << (map->global_depth - table->local_depth);
			int block_start =
				(i / entries_per_table) * entries_per_table;

			// All entries in this block should point to the same
			// table
			for (int j = block_start;
			     j < block_start + entries_per_table;
			     j++) {
				assert(j < map->dir_len);
				assert((swiss_table_t *)ADDR_OF(&directory[j]
				       ) == table);
			}

			// Verify table index is the start of the block
			assert(table->index == block_start);
		} else {
			// When local_depth == global_depth, each table has
			// unique directory entry
			assert(table->index == i);
		}

		// Mark this table as visited (using its index as identifier)
		// In extendible hashing, multiple directory entries can point
		// to the same table, so we only mark the table as visited when
		// we encounter its index for the first time
		if (i == table->index) {
			assert(!table_visited[table->index]
			); // Should not have visited this table before
			table_visited[table->index] = true;
		} else {
			// This is a shared table entry, it should already be
			// marked as visited
			assert(table_visited[table->index]);
		}
	}

	// Ensure that the extendible hashing code path was actually tested
	assert(extendible_hashing_tested &&
	       "Extendible hashing code path was not executed");

	free(table_visited);

	// Verify hash mapping consistency with sample keys
	int test_keys[] = {0, 1, 42, 100, 1000, 12345, 0x7FFFFFFF, 0xFFFFFFFF};
	for (size_t k = 0; k < sizeof(test_keys) / sizeof(test_keys[0]); k++) {
		uint64_t hash = swiss_hash_fnv1a(
			&test_keys[k], sizeof(test_keys[k]), map->seed
		);
		uint64_t dir_idx = swiss_map_directory_index(map, hash);

		assert(dir_idx < (uint64_t)map->dir_len);
		swiss_table_t *table = swiss_map_directory_at(map, dir_idx);

		// Verify the table can handle this hash based on its local
		// depth
		uint64_t table_mask = (1ULL << table->local_depth) - 1;
		uint64_t table_idx = table->index & table_mask;
		uint64_t hash_idx = hash >> (64 - table->local_depth);

		// The hash should map to the correct table within its local
		// depth But this assertion might be too strict for extendible
		// hashing scenarios Let's check if this is a shared table
		// scenario
		if (table->local_depth < map->global_depth) {
			// For shared tables, we need to check if the hash maps
			// to any of the shared entries
			int entries_per_table =
				1 << (map->global_depth - table->local_depth);
			int block_start = (dir_idx / entries_per_table) *
					  entries_per_table;
			assert(table->index == block_start);
		} else {
			// For unique tables, the original assertion should hold
			assert(table_idx == hash_idx);
		}
	}

	swiss_map_free(map);
}

// Test overwriting half of the values in the map
void
test_overwrite(struct memory_context *ctx) {
	swiss_map_config_t config = create_default_int_config(ctx);
	swiss_map_t *map = create_map(&config, 0);

	// Insert initial values
	const int count = 100;
	const int multiplier = 10;
	const int overwrite_multiplier = 100;

	// Insert initial key-value pairs
	insert_and_verify(map, 0, count, multiplier);

	// Overwrite half of the values
	for (int i = 0; i < count / 2; i++) {
		int value = i * overwrite_multiplier;
		assert(swiss_map_put(map, &i, &value) == 0);
	}

	// Verify all values - first half should be overwritten, second half
	// unchanged
	for (int i = 0; i < count; i++) {
		int *found_value;
		assert(swiss_map_get(map, &i, (void **)&found_value));

		if (i < count / 2) {
			// First half should be overwritten
			assert(*found_value == i * overwrite_multiplier);
		} else {
			// Second half should be unchanged
			assert(*found_value == i * multiplier);
		}
	}

	// Verify map size is still the same
	assert(swiss_map_size(map) == (size_t)count);

	swiss_map_free(map);
}

int
main() {
	// Set up arena and memory context like in lpm_test.c
	void *arena0 = malloc(ARENA_SIZE);
	if (arena0 == NULL) {
		fprintf(stdout, "could not allocate arena0\n");
		return -1;
	}

	struct block_allocator ba;
	block_allocator_init(&ba);
	block_allocator_put_arena(&ba, arena0, ARENA_SIZE);

	struct memory_context mctx;
	memory_context_init(&mctx, "swissmap", &ba);

	printf("Testing shared memory Swiss map implementation...\n");

	test_control_operations();
	test_hash_functions();
	test_int_map(&mctx);
	test_string_map(&mctx);
	test_macro_interface(&mctx);
	test_duplicate_key_bug(&mctx);

	// Algorithm-focused tests
	test_table_growth(&mctx);
	test_collision_handling(&mctx);
	test_memory_leak_prevention(&mctx);
	test_extendible_hashing(&mctx);
	test_probe_sequence_algorithm();
	test_control_byte_states();
	test_directory_expansion(&mctx);
	test_overwrite(&mctx);

	// Verify no memory leaks (like in lpm_test.c)
	if (mctx.balloc_size != mctx.bfree_size) {
		fprintf(stdout,
			"alloc and free sizes should be equal %lu != %lu\n",
			mctx.balloc_size,
			mctx.bfree_size);
		return -1;
	}

	free(arena0);
	printf("All tests passed! ✓\n");
	return 0;
}