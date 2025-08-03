#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/memory.h"
#include "common/memory_block.h"
#include "common/swissmap.h"

#define ARENA_SIZE (1 << 20) // 1MB arena
#define MAX_KEY_SIZE 256
#define MAX_VALUE_SIZE 256
#define MAX_OPERATIONS 1000

// Operation types for fuzzing
// NOLINTBEGIN(readability-identifier-naming)
typedef enum {
	OP_PUT = 0,
	OP_GET = 1,
	OP_DELETE = 2,
	OP_CLEAR = 3,
	OP_COUNT = 4
} fuzz_operation_t;
// NOLINTEND(readability-identifier-naming)

// Fuzzing parameters
struct swissmap_operations_fuzzing_params {
	swiss_map_t *map;
	void *arena;
	struct block_allocator ba;
	struct memory_context mctx;
	swiss_map_config_t config;
};

static struct swissmap_operations_fuzzing_params fuzz_params = {
	.map = NULL,
};

// Hash function that creates controlled collisions for testing
static uint64_t
collision_hash(const void *key, size_t key_size, uint64_t seed) {
	(void)seed;
	if (key_size == 0)
		return 0;

	const uint8_t *k = (const uint8_t *)key;
	// Force keys to same group but different H2 values
	// This creates hash collisions to stress the collision handling
	uint64_t hash = k[0];
	for (size_t i = 1; i < key_size; i++) {
		hash = (hash * 31) + k[i];
	}
	// Force collisions by limiting H1 range
	return ((hash >> 7) % 16) << 7 | (hash & 0x7F);
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
		&fuzz_params.mctx,
		"swissmap_operations_fuzzing",
		&fuzz_params.ba
	);

	// Configure map with default settings
	fuzz_params.config.key_size = sizeof(uint32_t);
	fuzz_params.config.value_size = sizeof(uint32_t);
	fuzz_params.config.hash_fn_id = SWISS_HASH_FNV1A;
	fuzz_params.config.key_equal_fn_id = SWISS_KEY_EQUAL_DEFAULT;
	fuzz_params.config.alloc_fn_id = SWISS_ALLOC_SHARED;
	fuzz_params.config.free_fn_id = SWISS_FREE_SHARED;
	fuzz_params.config.rand_fn_id = SWISS_RAND_DEFAULT;
	fuzz_params.config.mem_ctx = &fuzz_params.mctx;

	fuzz_params.map = swiss_map_new(&fuzz_params.config, 8);
	return fuzz_params.map ? 0 : -1;
}

// Parse fuzzer input to extract operation and key/value data
static int
parse_fuzz_input(
	const uint8_t *data,
	size_t size,
	fuzz_operation_t *op,
	void *key,
	size_t *key_size,
	void *value,
	size_t *value_size
) {
	if (size < 1)
		return -1;

	*op = data[0] % OP_COUNT;
	size_t offset = 1;

	// Extract key size and data
	if (offset >= size)
		return -1;
	*key_size = (data[offset] % MAX_KEY_SIZE) + 1;
	offset++;

	if (offset + *key_size > size) {
		*key_size = size - offset;
	}
	if (*key_size > 0) {
		memcpy(key, data + offset, *key_size);
		offset += *key_size;
	}

	// Extract value size and data
	if (offset < size) {
		*value_size = (data[offset] % MAX_VALUE_SIZE) + 1;
		offset++;

		if (offset + *value_size > size) {
			*value_size = size - offset;
		}
		if (*value_size > 0) {
			memcpy(value, data + offset, *value_size);
		}
	} else {
		*value_size = sizeof(uint32_t);
		*(uint32_t *)value = 0xDEADBEEF;
	}

	return 0;
}

// Test with different hash functions to stress collision handling
static void
test_with_hash_function(
	swiss_func_id_t hash_fn_id, const uint8_t *data, size_t size
) {
	// Temporarily replace hash function
	void *original_hash_fn = swiss_func_registry[SWISS_HASH_FNV1A];

	if (hash_fn_id == SWISS_HASH_FNV1A) {
		// Use collision hash for stress testing
		swiss_func_registry[SWISS_HASH_FNV1A] = (void *)collision_hash;
	}

	swiss_map_t *test_map = swiss_map_new(&fuzz_params.config, 8);
	if (!test_map) {
		swiss_func_registry[SWISS_HASH_FNV1A] = original_hash_fn;
		return;
	}

	// Perform operations with the test hash function
	uint8_t key_buf[MAX_KEY_SIZE];
	uint8_t value_buf[MAX_VALUE_SIZE];
	size_t key_size, value_size;
	fuzz_operation_t op;

	if (parse_fuzz_input(
		    data, size, &op, key_buf, &key_size, value_buf, &value_size
	    ) == 0) {
		// Update config for variable key/value sizes
		fuzz_params.config.key_size = key_size;
		fuzz_params.config.value_size = value_size;

		switch (op) {
		case OP_PUT:
			swiss_map_put(test_map, key_buf, value_buf);
			break;
		case OP_GET: {
			void *found_value;
			swiss_map_get(test_map, key_buf, &found_value);
			break;
		}
		case OP_DELETE:
			swiss_map_delete(test_map, key_buf);
			break;
		case OP_CLEAR:
			swiss_map_clear(test_map);
			break;
		default:
			break;
		}
	}

	swiss_map_free(test_map);
	swiss_func_registry[SWISS_HASH_FNV1A] = original_hash_fn;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) { // NOLINT
	if (fuzz_params.map == NULL) {
		if (fuzz_setup() != 0) {
			exit(1);
		}
	}

	if (size == 0)
		return 0;

	// Test basic operations with fuzzer input
	uint8_t key_buf[MAX_KEY_SIZE];
	uint8_t value_buf[MAX_VALUE_SIZE];
	size_t key_size, value_size;
	fuzz_operation_t op;

	// Parse multiple operations from input
	size_t offset = 0;
	int operations = 0;

	while (offset < size && operations < MAX_OPERATIONS) {
		size_t remaining = size - offset;

		if (parse_fuzz_input(
			    data + offset,
			    remaining,
			    &op,
			    key_buf,
			    &key_size,
			    value_buf,
			    &value_size
		    ) != 0) {
			break;
		}

		// Update config for current operation
		swiss_map_config_t temp_config = fuzz_params.config;
		temp_config.key_size = key_size;
		temp_config.value_size = value_size;

		// Create temporary map with current key/value sizes
		swiss_map_t *temp_map = swiss_map_new(&temp_config, 8);
		if (!temp_map)
			break;

		switch (op) {
		case OP_PUT: {
			// Test put operation
			int result =
				swiss_map_put(temp_map, key_buf, value_buf);
			(void)result; // Suppress unused variable warning

			// Verify the put worked by trying to get it back
			void *found_value;
			if (swiss_map_get(temp_map, key_buf, &found_value)) {
				// Verify data integrity
				assert(memcmp(found_value, value_buf, value_size
				       ) == 0);
			}
			break;
		}
		case OP_GET: {
			// Insert something first, then try to get it
			swiss_map_put(temp_map, key_buf, value_buf);
			void *found_value;
			bool found =
				swiss_map_get(temp_map, key_buf, &found_value);
			assert(found);
			assert(memcmp(found_value, value_buf, value_size) == 0);
			break;
		}
		case OP_DELETE: {
			// Insert then delete
			swiss_map_put(temp_map, key_buf, value_buf);
			size_t size_before = swiss_map_size(temp_map);
			bool deleted = swiss_map_delete(temp_map, key_buf);
			assert(deleted);
			assert(swiss_map_size(temp_map) == size_before - 1);
			// Verify it's really gone
			void *found_value;
			assert(!swiss_map_get(temp_map, key_buf, &found_value));
			break;
		}
		case OP_CLEAR: {
			// Insert some data then clear
			swiss_map_put(temp_map, key_buf, value_buf);
			swiss_map_clear(temp_map);
			assert(swiss_map_empty(temp_map));
			assert(swiss_map_size(temp_map) == 0);
			break;
		}
		default:
			break;
		}

		swiss_map_free(temp_map);

		// Move to next operation
		offset += 1 + 1 + key_size + 1 + value_size;
		operations++;
	}

	// Test with collision-inducing hash functions
	if (size > 10) {
		test_with_hash_function(SWISS_HASH_FNV1A, data, size);
	}

	// Test edge cases
	if (size >= 4) {
		// Test with zero-length keys (if supported)
		uint32_t zero_key = 0;
		uint32_t test_value = *(uint32_t *)data;

		swiss_map_config_t edge_config = fuzz_params.config;
		edge_config.key_size = sizeof(uint32_t);
		edge_config.value_size = sizeof(uint32_t);

		swiss_map_t *edge_map = swiss_map_new(&edge_config, 8);
		if (edge_map) {
			swiss_map_put(edge_map, &zero_key, &test_value);

			void *found_value;
			if (swiss_map_get(edge_map, &zero_key, &found_value)) {
				assert(*(uint32_t *)found_value == test_value);
			}

			swiss_map_free(edge_map);
		}
	}

	return 0;
}
