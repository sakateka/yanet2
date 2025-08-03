#include "common/swissmap.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ARENA_SIZE (1 << 30) // 1GB arena for large test
#define NUM_ARENAS 3	     // Use 3 arenas for 3GB total

// Five-tuple structure for network flows
typedef struct {
	uint8_t transport;    // Protocol (TCP=6, UDP=17, etc.)
	uint32_t source_ip;   // Source IP address
	uint16_t source_port; // Source port
	uint32_t dest_ip;     // Destination IP address
	uint16_t dest_port;   // Destination port
} __attribute__((packed)) five_tuple_t;

// Test configuration
#define TEST_SIZE (33 * 1024 * 1024)
#define COMMON_DEST_IP_RATIO 0.8  // 80% of entries use same dest IP
#define COMMON_DEST_IP 0xC0A80001 // 192.168.0.1 in network byte order

// Statistics structure
typedef struct {
	size_t insertions;
	size_t lookups_found;
	size_t lookups_not_found;
	size_t deletions;
	size_t duplicate_keys; // Track duplicate keys generated
	double insert_time;
	double lookup_time;
	double delete_time;
	size_t memory_usage_estimate;
} test_stats_t;

/**
 * @brief Custom hash function optimized for five-tuples
 *
 * This hash function is designed to handle the case where many entries
 * have the same destination IP (80% in our test case). It combines
 * all fields with different weights to ensure good distribution.
 */
uint64_t
five_tuple_hash(const void *key, size_t key_size, uint64_t seed) {
	(void)key_size; // We know the size is sizeof(five_tuple_t)

	const five_tuple_t *tuple = (const five_tuple_t *)key;

	// Use FNV-1a hash with field-specific mixing
	uint64_t hash = 14695981039346656037ULL ^ seed;

	// Hash transport protocol
	hash ^= tuple->transport;
	hash *= 1099511628211ULL;

	// Hash source IP with more weight since it varies more
	hash ^= tuple->source_ip;
	hash *= 1099511628211ULL;
	hash ^= tuple->source_ip >> 16;
	hash *= 1099511628211ULL;

	// Hash source port
	hash ^= tuple->source_port;
	hash *= 1099511628211ULL;

	// Hash destination IP
	hash ^= tuple->dest_ip;
	hash *= 1099511628211ULL;
	hash ^= tuple->dest_ip >> 16;
	hash *= 1099511628211ULL;

	// Hash destination port with extra mixing for better distribution
	hash ^= tuple->dest_port;
	hash *= 1099511628211ULL;

	return hash;
}

/**
 * @brief Custom key comparison function for five-tuples
 */
bool
five_tuple_equal(const void *a, const void *b, size_t size) {
	(void)size; // We know the size
	return memcmp(a, b, sizeof(five_tuple_t)) == 0;
}

/**
 * @brief Generate a random five-tuple with controlled destination IP
 * distribution Fixed to ensure unique keys by incorporating index more directly
 */
five_tuple_t
generate_five_tuple(uint32_t index, uint32_t *rand_state) {
	five_tuple_t tuple;

	// Simple LCG for reproducible randomness
	*rand_state = *rand_state * 1103515245 + 12345;

	// Transport protocol (mostly TCP and UDP) - but ensure uniqueness
	tuple.transport = (index % 10 < 8) ? 6 : 17; // 80% TCP, 20% UDP

	// Source IP - use full index to ensure uniqueness across all 32M
	// entries Distribute index across all 32 bits for better uniqueness
	tuple.source_ip = index ^ ((*rand_state & 0xFF) << 24);

	// Source port - combine index with randomness for uniqueness
	*rand_state = *rand_state * 1103515245 + 12345;
	tuple.source_port =
		(uint16_t)((index & 0xFFFF) ^ (*rand_state & 0xFFFF));

	// Destination IP - 80% use common IP, but add index variation for
	// uniqueness
	*rand_state = *rand_state * 1103515245 + 12345;
	if ((*rand_state % 100) < (COMMON_DEST_IP_RATIO * 100)) {
		// Use common IP but add index-based variation to ensure
		// uniqueness
		tuple.dest_ip = COMMON_DEST_IP ^
				(index >> 16); // XOR with upper bits of index
	} else {
		tuple.dest_ip =
			*rand_state ^ index; // Ensure randomness includes index
	}

	// Destination port - combine index with randomness for guaranteed
	// uniqueness
	*rand_state = *rand_state * 1103515245 + 12345;
	tuple.dest_port = (uint16_t)((index >> 8) ^ (*rand_state & 0xFFFF));

	return tuple;
}

/**
 * @brief Print test statistics
 */
void
print_stats(const test_stats_t *stats) {
	printf("\n=== Test Statistics ===\n");
	printf("Insertions: %zu\n", stats->insertions);
	printf("Lookups (found): %zu\n", stats->lookups_found);
	printf("Lookups (not found): %zu\n", stats->lookups_not_found);
	printf("Deletions: %zu\n", stats->deletions);
	printf("\n=== Performance ===\n");
	printf("Insert time: %.2f seconds (%.0f ops/sec)\n",
	       stats->insert_time,
	       stats->insertions / stats->insert_time);
	printf("Lookup time: %.2f seconds (%.0f ops/sec)\n",
	       stats->lookup_time,
	       (stats->lookups_found + stats->lookups_not_found) /
		       stats->lookup_time);
	if (stats->delete_time > 0) {
		printf("Delete time: %.2f seconds (%.0f ops/sec)\n",
		       stats->delete_time,
		       stats->deletions / stats->delete_time);
	}
	printf("\n=== Memory Usage ===\n");
	printf("Estimated memory usage: %.2f MB\n",
	       stats->memory_usage_estimate / (1024.0 * 1024.0));
	printf("Bytes per entry: %.1f\n",
	       (double)stats->memory_usage_estimate / stats->insertions);
}

/**
 * @brief Estimate memory usage of the map
 */
size_t
estimate_memory_usage(swiss_map_t *map) {
	// Calculate actual memory usage based on Swiss Table structure
	size_t total_entries = swiss_map_size(map);

	// Base map structure
	size_t base_size = sizeof(swiss_map_t);

	// Directory memory - grows with global_depth
	size_t directory_size = map->dir_len * sizeof(swiss_table_t *);

	// Per-table overhead
	size_t table_overhead = 0;
	size_t calculated_entries = 0;

	// Count unique tables and calculate their overhead
	swiss_table_t **directory = (swiss_table_t **)ADDR_OF(&map->dir_ptr);
	swiss_table_t *last_table = NULL;

	for (int i = 0; i < map->dir_len; i++) {
		swiss_table_t *table = (swiss_table_t *)ADDR_OF(&directory[i]);
		if (table != last_table) {
			// Table structure overhead
			table_overhead += sizeof(swiss_table_t);

			// Groups array overhead
			size_t group_count = table->groups.length_mask + 1;
			size_t slot_size =
				sizeof(five_tuple_t) + sizeof(uint32_t);
			size_t group_size = sizeof(swiss_ctrl_group_t) +
					    SWISS_GROUP_SLOTS * slot_size;
			table_overhead += group_count * group_size;

			// Track entries for validation
			calculated_entries += table->used;

			last_table = table;
		}
	}

	// Assert that our calculated entries match the map size
	assert(calculated_entries == total_entries);

	// Total actual memory usage
	size_t total_memory = base_size + directory_size + table_overhead;

	return total_memory;
}

/**
 * @brief Run the large Swiss Table test
 */
int
test_large_swissmap(struct memory_context *ctx) {
	printf("Starting large Swiss Table test...\n");
	printf("Test size: %d entries (%.1f million)\n",
	       TEST_SIZE,
	       TEST_SIZE / 1e6);
	printf("Common destination IP ratio: %.0f%%\n",
	       COMMON_DEST_IP_RATIO * 100);

	test_stats_t stats = {0};

	// Configure the map for five-tuples
	swiss_map_config_t config = {0};
	config.key_size = sizeof(five_tuple_t);
	config.value_size = sizeof(uint32_t);
	config.hash_fn_id = SWISS_HASH_FNV1A; // Note: using FNV1A instead of
					      // five_tuple_hash
	config.key_equal_fn_id =
		SWISS_KEY_EQUAL_DEFAULT; // Note: using default instead of
					 // five_tuple_equal
	config.alloc_fn_id = SWISS_ALLOC_SHARED;
	config.free_fn_id = SWISS_FREE_SHARED;
	config.rand_fn_id = SWISS_RAND_DEFAULT;
	config.mem_ctx = ctx;

	// Create map with size hint
	size_t hint = TEST_SIZE / 10;
	printf("Creating Swiss map with hint %.0f%% of %d entries...\n",
	       (float)TEST_SIZE / (float)hint,
	       TEST_SIZE);
	swiss_map_t *map = swiss_map_new(&config, hint);
	if (!map) {
		printf("Failed to create Swiss map!\n");
		return 1;
	}

	// Phase 1: Insert all entries
	printf("\nPhase 1: Inserting %d entries...\n", TEST_SIZE);
	clock_t start_time = clock();

	uint32_t rand_state = 12345; // Seed for reproducible results
	for (uint32_t i = 0; i < TEST_SIZE; i++) {
		five_tuple_t key = generate_five_tuple(i, &rand_state);
		uint32_t value = i + 1000000; // Unique value for each entry

		// Check if key already exists (duplicate)
		uint32_t *existing_value;
		if (swiss_map_get(map, &key, (void **)&existing_value)) {
			stats.duplicate_keys++;
		}

		int put_result = swiss_map_put(map, &key, &value);
		if (put_result != 0) {
			printf("ERROR: Failed to insert entry at index %u (map "
			       "size: %zu)\n",
			       i,
			       swiss_map_size(map));
			printf("ERROR: swiss_map_put() returned %d, errno: "
			       "%d\n",
			       put_result,
			       errno);
			assert(put_result == 0);
		}
		stats.insertions++;

		// Progress indicator
		if (i % (TEST_SIZE / 20) == 0) {
			printf("  Progress: %.1f%% (%u entries)\n",
			       (double)i / TEST_SIZE * 100,
			       i);
		}
	}
	printf("Duplicate keys detected: %zu\n", stats.duplicate_keys);

	clock_t end_time = clock();
	stats.insert_time = (double)(end_time - start_time) / CLOCKS_PER_SEC;

	printf("Insertion complete! Map size: %zu\n", swiss_map_size(map));
	stats.memory_usage_estimate = estimate_memory_usage(map);

	// Compare our estimation with actual block allocator usage
	printf("=== Memory Usage Comparison ===\n");
	printf("Our estimation: %zu bytes (%.2f MB)\n",
	       stats.memory_usage_estimate,
	       stats.memory_usage_estimate / (1024.0 * 1024.0));
	printf("Block allocator allocated: %zu bytes (%.2f MB)\n",
	       ctx->balloc_size,
	       ctx->balloc_size / (1024.0 * 1024.0));
	printf("Block allocator freed: %zu bytes (%.2f MB)\n",
	       ctx->bfree_size,
	       ctx->bfree_size / (1024.0 * 1024.0));
	printf("Net memory usage: %zu bytes (%.2f MB)\n",
	       ctx->balloc_size - ctx->bfree_size,
	       (ctx->balloc_size - ctx->bfree_size) / (1024.0 * 1024.0));

	// Calculate accuracy
	double accuracy = (double)stats.memory_usage_estimate /
			  (ctx->balloc_size - ctx->bfree_size);
	printf("Estimation accuracy: %.2f%%\n", accuracy * 100);

	// Assert that our estimation is reasonably accurate (within 20%)
	assert(accuracy >= 0.8 && accuracy <= 1.2);

	// Phase 2: Lookup all entries (should find all)
	printf("\nPhase 2: Looking up all inserted entries...\n");
	start_time = clock();

	rand_state = 12345; // Reset to same seed
	for (uint32_t i = 0; i < TEST_SIZE; i++) {
		five_tuple_t key = generate_five_tuple(i, &rand_state);
		uint32_t *found_value;

		if (swiss_map_get(map, &key, (void **)&found_value)) {
			if (*found_value == i + 1000000) {
				stats.lookups_found++;
			} else {
				printf("ERROR: Value mismatch at index %u! "
				       "Expected %u, got %u\n",
				       i,
				       i + 1000000,
				       *found_value);
				return 1;
			}
		} else {
			printf("ERROR: Failed to find entry at index %u!\n", i);
			return 1;
		}

		// Progress indicator
		if (i % (TEST_SIZE / 10) == 0) {
			printf("  Lookup progress: %.1f%%\n",
			       (double)i / TEST_SIZE * 100);
		}
	}

	end_time = clock();
	stats.lookup_time = (double)(end_time - start_time) / CLOCKS_PER_SEC;

	// Phase 3: Lookup non-existent entries
	printf("\nPhase 3: Looking up non-existent entries...\n");
	start_time = clock();

	for (uint32_t i = 0; i < TEST_SIZE / 100; i++) { // Test 1% of entries
		five_tuple_t key =
			generate_five_tuple(TEST_SIZE + i, &rand_state);
		uint32_t *found_value;

		if (!swiss_map_get(map, &key, (void **)&found_value)) {
			stats.lookups_not_found++;
		} else {
			printf("ERROR: Found non-existent entry at index %u!\n",
			       TEST_SIZE + i);
		}
	}

	end_time = clock();
	stats.lookup_time += (double)(end_time - start_time) / CLOCKS_PER_SEC;

	// Phase 4: Delete some entries (10% of total)
	printf("\nPhase 4: Deleting 10%% of entries...\n");
	start_time = clock();

	rand_state = 12345; // Reset seed
	for (uint32_t i = 0; i < TEST_SIZE / 10; i++) {
		five_tuple_t key = generate_five_tuple(i * 10, &rand_state);
		// Skip ahead in random state to match the key we want to delete
		for (int j = 0; j < 9; j++) {
			generate_five_tuple(i * 10 + j + 1, &rand_state);
		}

		if (swiss_map_delete(map, &key)) {
			stats.deletions++;
		} else {
			printf("ERROR: Failed to delete entry at index %u!\n",
			       i * 10);
		}

		if (i % (TEST_SIZE / 100) == 0) {
			printf("  Delete progress: %.1f%%\n",
			       (double)i / (TEST_SIZE / 10) * 100);
		}
	}

	end_time = clock();
	stats.delete_time = (double)(end_time - start_time) / CLOCKS_PER_SEC;

	printf("Deletion complete! Map size: %zu\n", swiss_map_size(map));

	// Print final statistics
	print_stats(&stats);

	// Verify final state
	printf("\n=== Final Verification ===\n");
	size_t expected_size = TEST_SIZE - stats.deletions;
	size_t actual_size = swiss_map_size(map);
	printf("Expected final size: %zu\n", expected_size);
	printf("Actual final size: %zu\n", actual_size);

	if (actual_size == expected_size) {
		printf("✓ Size verification passed!\n");
	} else {
		printf("✗ Size verification failed!\n");
		swiss_map_free(map);
		return 1;
	}

	// Clean up
	swiss_map_free(map);
	printf("\n✓ Large Swiss Table test completed successfully!\n");

	return 0;
}

int
main(void) {
	printf("Swiss Table Large-Scale Test\n");
	printf("============================\n");
	printf("Testing with 32M five-tuples (transport, src_ip, src_port, "
	       "dst_ip, dst_port)\n");
	printf("Key size: %zu bytes\n", sizeof(five_tuple_t));
	printf("Value size: %zu bytes\n", sizeof(uint32_t));
	printf("80%% of entries will have the same destination IP\n\n");

	// Set up multiple arenas for large test
	void *arenas[NUM_ARENAS];
	for (int i = 0; i < NUM_ARENAS; i++) {
		arenas[i] = malloc(ARENA_SIZE);
		if (arenas[i] == NULL) {
			fprintf(stdout, "could not allocate arena%d\n", i);
			// Free previously allocated arenas
			for (int j = 0; j < i; j++) {
				free(arenas[j]);
			}
			return -1;
		}
	}

	struct block_allocator ba;
	block_allocator_init(&ba);

	// Add all arenas to the block allocator
	for (int i = 0; i < NUM_ARENAS; i++) {
		block_allocator_put_arena(&ba, arenas[i], ARENA_SIZE);
	}

	struct memory_context mctx;
	memory_context_init(&mctx, "large_swissmap", &ba);

	printf("Allocated %d arenas of %dMB each (total: %luMB)\n",
	       NUM_ARENAS,
	       ARENA_SIZE >> 20,
	       ((uint64_t)NUM_ARENAS * ARENA_SIZE) >> 20);

	int result = test_large_swissmap(&mctx);

	// Verify no memory leaks
	if (mctx.balloc_size != mctx.bfree_size) {
		fprintf(stdout,
			"alloc and free sizes should be equal %lu != %lu\n",
			mctx.balloc_size,
			mctx.bfree_size);
		result = -1;
	}

	// Free all arenas
	for (int i = 0; i < NUM_ARENAS; i++) {
		free(arenas[i]);
	}

	if (result == 0) {
		printf("\n🎉 All tests passed!\n");
	} else {
		printf("\n❌ Test failed!\n");
	}

	return result;
}
