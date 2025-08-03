#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/memory.h"
#include "common/memory_block.h"
#include "common/swissmap.h"

#define ARENA_SIZE (1 << 20) // 1MB arena

// Fuzzing parameters
struct swissmap_internals_fuzzing_params {
	void *arena;
	struct block_allocator ba;
	struct memory_context mctx;
	swiss_map_config_t config;
};

static struct swissmap_internals_fuzzing_params fuzz_params = {0};

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
		&fuzz_params.mctx, "swissmap_internals_fuzzing", &fuzz_params.ba
	);

	// Configure map
	fuzz_params.config.key_size = sizeof(uint32_t);
	fuzz_params.config.value_size = sizeof(uint32_t);
	fuzz_params.config.hash_fn_id = SWISS_HASH_FNV1A;
	fuzz_params.config.key_equal_fn_id = SWISS_KEY_EQUAL_DEFAULT;
	fuzz_params.config.alloc_fn_id = SWISS_ALLOC_SHARED;
	fuzz_params.config.free_fn_id = SWISS_FREE_SHARED;
	fuzz_params.config.rand_fn_id = SWISS_RAND_DEFAULT;
	fuzz_params.config.mem_ctx = &fuzz_params.mctx;

	return 0;
}

// Test control group operations with fuzzer input
static void
test_control_group_operations(const uint8_t *data, size_t size) {
	if (size < 8)
		return;

	swiss_ctrl_group_t ctrl = 0;

	// Initialize control group from fuzzer data
	for (size_t i = 0; i < SWISS_GROUP_SLOTS && i < size; i++) {
		uint8_t ctrl_byte = data[i];

		// Test setting various control byte values
		swiss_ctrl_set(&ctrl, i, ctrl_byte);

		// Verify the value was set correctly
		uint8_t retrieved = swiss_ctrl_get(ctrl, i);
		assert(retrieved == ctrl_byte);
	}

	// Test control byte matching functions
	for (size_t i = 0; i < size && i < SWISS_GROUP_SLOTS; i++) {
		uint8_t h2 = data[i] & 0x7F; // Valid H2 range

		// Test H2 matching
		swiss_bitset_t h2_match = swiss_ctrl_match_h2(ctrl, h2);

		// Verify bitset operations
		if (h2_match != 0) {
			size_t first_match = swiss_bitset_first(h2_match);
			assert(first_match < SWISS_GROUP_SLOTS);

			// Test bitset manipulation
			swiss_bitset_t removed_first =
				swiss_bitset_remove_first(h2_match);
			assert(removed_first != h2_match || h2_match == 0);

			// Verify that operations do not lead to crashes.
			swiss_bitset_t removed_below =
				swiss_bitset_remove_below(
					h2_match, first_match
				);
			(void)removed_below; // Suppress unused variable warning
		}
	}

	// Verify that operations do not lead to crashes.
	// Test empty slot matching
	swiss_bitset_t empty_match = swiss_ctrl_match_empty(ctrl);
	(void)empty_match; // Suppress unused variable warning

	// Test empty or deleted slot matching
	swiss_bitset_t empty_or_deleted =
		swiss_ctrl_match_empty_or_deleted(ctrl);
	(void)empty_or_deleted; // Suppress unused variable warning

	// Test full slot matching
	swiss_bitset_t full_match = swiss_ctrl_match_full(ctrl);
	(void)full_match; // Suppress unused variable warning
}

// Test bitset operations with fuzzer input
static void
test_bitset_operations(const uint8_t *data, size_t size) {
	if (size < 8)
		return;

	// Create bitset from fuzzer data
	swiss_bitset_t bitset = *(uint64_t *)data;

	// Test bitset_first operation
	if (bitset != 0) {
		size_t first = swiss_bitset_first(bitset);
		assert(first < 8); // Should be valid slot index

		// Test remove_first - this removes the lowest set bit using b &
		// (b - 1)
		swiss_bitset_t after_remove = swiss_bitset_remove_first(bitset);

		// Verify that after_remove has exactly one fewer bit set than
		// original
		assert(__builtin_popcountll(after_remove) ==
		       __builtin_popcountll(bitset) - 1);

		// Verify that after_remove is a subset of original (all bits in
		// after_remove are also in original)
		assert((after_remove & bitset) == after_remove);

		// Verify that the removed bit was indeed the lowest set bit
		uint64_t lowest_bit =
			bitset & (~bitset + 1); // isolate lowest set bit
		assert((bitset & ~lowest_bit) == after_remove);

		// Additional verification: the lowest set bit should correspond
		// to the first slot index
		uint64_t expected_lowest_bit_pos = __builtin_ctzll(bitset);
		uint64_t actual_lowest_bit_pos = __builtin_ctzll(lowest_bit);
		assert(expected_lowest_bit_pos == actual_lowest_bit_pos);
	}

	// Test remove_below with different indices
	for (size_t i = 0; i < SWISS_GROUP_SLOTS; i++) {
		swiss_bitset_t removed_below =
			swiss_bitset_remove_below(bitset, i);

		// Verify bits below index i are cleared
		for (size_t j = 0; j < i; j++) {
			uint64_t bit_mask = 0xFFULL << (8 * j);
			assert((removed_below & bit_mask) == 0);
		}
	}
}

// Test probe sequence generation and advancement
static void
test_probe_sequence(const uint8_t *data, size_t size) {
	if (size < 8)
		return;

	uint64_t hash = *(uint64_t *)data;

	// Test with different mask sizes (powers of 2)
	uint64_t masks[] = {0x1, 0x3, 0x7, 0xF, 0x1F, 0x3F, 0x7F, 0xFF};

	for (size_t i = 0; i < sizeof(masks) / sizeof(masks[0]); i++) {
		uint64_t mask = masks[i];

		// Create initial probe sequence
		swiss_probe_seq_t seq = swiss_make_probe_seq(hash, mask);

		// Verify initial state
		assert(seq.mask == mask);
		assert(seq.index == 0);
		assert(seq.offset <= mask);
		assert(seq.offset == (swiss_h1(hash) & mask));

		// Test probe sequence advancement
		swiss_probe_seq_t prev_seq = seq;
		for (size_t j = 0; j < mask + 1; j++) {
			seq = swiss_probe_seq_next(seq);

			// Verify advancement
			assert(seq.index == prev_seq.index + 1);
			assert(seq.mask == mask);
			assert(seq.offset <= mask);

			prev_seq = seq;
		}
	}
}

// Test hash extraction functions
static void
test_hash_extraction(const uint8_t *data, size_t size) {
	if (size < 8)
		return;

	uint64_t hash = *(uint64_t *)data;

	// Test H1 extraction
	uint64_t h1 = swiss_h1(hash);
	assert(h1 == (hash >> 7));

	// Test H2 extraction
	uint8_t h2 = swiss_h2(hash);
	assert(h2 == (hash & 0x7F));
	assert(h2 <= 0x7F); // H2 should be 7 bits

	// Test reconstruction
	uint64_t reconstructed = (h1 << 7) | h2;
	assert(reconstructed == hash);

	// Test with edge case values
	uint64_t edge_cases[] = {
		0x0000000000000000ULL,
		0xFFFFFFFFFFFFFFFFULL,
		0x8000000000000000ULL,
		0x7FFFFFFFFFFFFFFFULL,
		0x0000000000000080ULL,
		0x000000000000007FULL
	};

	for (size_t i = 0; i < sizeof(edge_cases) / sizeof(edge_cases[0]);
	     i++) {
		uint64_t test_hash = edge_cases[i];
		uint64_t test_h1 = swiss_h1(test_hash);
		uint8_t test_h2 = swiss_h2(test_hash);

		assert(test_h1 == (test_hash >> 7));
		assert(test_h2 == (test_hash & 0x7F));
		assert(((test_h1 << 7) | test_h2) == test_hash);
	}
}

// Test group operations with fuzzer input
static void
test_group_operations(const uint8_t *data, size_t size) {
	if (size < 32)
		return; // Need enough data for keys and values

	swiss_map_t *map = swiss_map_new(&fuzz_params.config, 8);
	if (!map)
		return;

	// Insert some data to create groups
	for (size_t i = 0; i < size / 8 && i < 16; i++) {
		uint32_t key = *(uint32_t *)(data + i * 4);
		uint32_t value = key ^ 0xDEADBEEF;
		swiss_map_put(map, &key, &value);
	}

	// Test directory operations
	for (size_t i = 0; i < size / 4 && i < 8; i++) {
		uint32_t test_key = *(uint32_t *)(data + i * 4);
		uint64_t hash = swiss_hash_fnv1a(
			&test_key, sizeof(test_key), map->seed
		);

		// Test directory index calculation
		uint64_t dir_idx = swiss_map_directory_index(map, hash);
		assert(dir_idx < (uint64_t)map->dir_len);

		// Test directory access
		swiss_table_t *table = swiss_map_directory_at(map, dir_idx);
		assert(table != NULL);

		// Verify table properties
		assert(table->local_depth <= map->global_depth);
		assert(table->used <= table->capacity);
		assert(table->capacity > 0);
		assert(table->index >= 0);
		assert(table->index < map->dir_len);
	}

	swiss_map_free(map);
}

// Test utility functions
static void
test_utility_functions(const uint8_t *data, size_t size) {
	if (size < 8)
		return;

	// Test alignment functions
	for (size_t i = 0; i < size && i < 16; i++) {
		uint64_t n = data[i];
		if (n == 0)
			continue;

		uint64_t aligned = align_up_pow2(n);
		if (aligned) {
			// Should be power of 2
			assert((aligned & (aligned - 1)) == 0);
			// Should be >= original value
			assert(aligned >= n);
			// Should be the smallest power of 2 >= n
			if (aligned > 1) {
				if ((aligned >> 1) >= n) {
					printf("Alignment assertion failed: "
					       "aligned=%lu, n=%lu, (aligned "
					       ">> 1)=%lu\n",
					       aligned,
					       n,
					       aligned >> 1);
				}
				assert((aligned >> 1) < n);
			}
		}
	}
}

// Test control byte state transitions
static void
test_control_byte_transitions(const uint8_t *data, size_t size) {
	if (size < 8)
		return;

	swiss_ctrl_group_t ctrl = 0;

	// Initialize to empty
	swiss_ctrl_set_empty(&ctrl);

	// Verify all slots are empty
	for (size_t i = 0; i < SWISS_GROUP_SLOTS; i++) {
		assert(swiss_ctrl_get(ctrl, i) == CTRL_EMPTY);
	}

	// Test transitions based on fuzzer input
	for (size_t i = 0; i < SWISS_GROUP_SLOTS && i < size; i++) {
		uint8_t new_state = data[i];

		// Set new state
		swiss_ctrl_set(&ctrl, i, new_state);
		assert(swiss_ctrl_get(ctrl, i) == new_state);

		// Test state-specific matching
		if (new_state == CTRL_EMPTY) {
			swiss_bitset_t empty_match =
				swiss_ctrl_match_empty(ctrl);
			assert((empty_match & (0xFFULL << (8 * i))) != 0);
		} else if (new_state == CTRL_DELETED) {
			swiss_bitset_t deleted_match =
				swiss_ctrl_match_empty_or_deleted(ctrl);
			assert((deleted_match & (0xFFULL << (8 * i))) != 0);
		} else if ((new_state & 0x80) == 0) {
			// Full slot
			swiss_bitset_t full_match = swiss_ctrl_match_full(ctrl);
			assert((full_match & (0xFFULL << (8 * i))) != 0);

			// Should match H2
			uint8_t h2 = new_state & 0x7F;
			swiss_bitset_t h2_match = swiss_ctrl_match_h2(ctrl, h2);
			assert((h2_match & (0xFFULL << (8 * i))) != 0);
		}
	}
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

	// Test different internal operations based on fuzzer input
	switch (data[0] % 7) {
	case 0:
		test_control_group_operations(data + 1, size - 1);
		break;
	case 1:
		test_bitset_operations(data + 1, size - 1);
		break;
	case 2:
		test_probe_sequence(data + 1, size - 1);
		break;
	case 3:
		test_hash_extraction(data + 1, size - 1);
		break;
	case 4:
		test_group_operations(data + 1, size - 1);
		break;
	case 5:
		test_utility_functions(data + 1, size - 1);
		break;
	case 6:
		test_control_byte_transitions(data + 1, size - 1);
		break;
	}

	return 0;
}
