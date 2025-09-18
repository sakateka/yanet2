/**
 * @file test_ttlmap.c
 * @brief Comprehensive test program for TTLMap implementation
 */

#include "hugepages.h"
#include "memory.h"
#include "memory_block.h"
#include "ttlmap.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define ARENA_SIZE (1 << 20) * 400 // MB arena

// ANSI color codes for output
#define COLOR_RESET "\033[0m"
#define COLOR_RED "\033[31m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BLUE "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN "\033[36m"
#define COLOR_WHITE "\033[37m"
#define COLOR_BOLD "\033[1m"

// Multi-threaded test configuration
#define NUM_REPETITIONS 10
#define NUM_THREADS 10
#define L3_CACHE_SIZE (32ULL * 1024 * 1024) // 32MB typical L3 cache
#define VALUE_SIZE 64			    // B per value

#define MT_ARENA_SIZE (1 << 20) * 1024ULL * 1	      // MB arena for MT test
#define TOTAL_VALUES (L3_CACHE_SIZE / VALUE_SIZE * 8) // Nx L3 cache size
#define TOTAL_OPS (TOTAL_VALUES * NUM_THREADS * NUM_REPETITIONS)

volatile uint32_t now = 0; // for testing purposes it's fine

static double
get_time(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

/**
 * @brief Allocate memory using hugepages for better performance
 */
static void *
allocate_locked_memory(size_t size) {
	char *storage_path = "/dev/hugepages/arena";
	int mem_fd =
		open(storage_path, O_CREAT | O_TRUNC | O_RDWR, S_IRUSR | S_IWUSR
		);
	if (mem_fd < 0) {
		printf("L%d: failed to open storage path\n", __LINE__);
		return NULL;
	}

	if (ftruncate(mem_fd, size)) {
		printf("L%d: failed to truncate storage path\n", __LINE__);
		close(mem_fd);
		return NULL;
	}

	void *storage =
		mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, 0);

	if (storage == MAP_FAILED) {
		int err = errno;
		printf("L%d: failed to create memory-mapped storage %s: %s\n",
		       __LINE__,
		       storage_path,
		       strerror(errno));

		if (err == ENOMEM && is_file_on_hugepages_fs(mem_fd) == 1) {
			printf("L%d: "
			       "the storage %s is meant to be allocated on "
			       "HUGETLBFS, but there is no memory. Maybe "
			       "because "
			       "either there are no preallocated pages or "
			       "another "
			       "process have consumed the memory\n",
			       __LINE__,
			       storage_path);
		}

		close(mem_fd);
		return NULL;
	}
	close(mem_fd);
	return storage;
}

/**
 * @brief Free memory allocated with allocate_locked_memory
 */
static void
free_memory(void *ptr, size_t size) {
	if (ptr && ptr != MAP_FAILED) {
		munmap(ptr, size);
	}
}

/**
 * @brief Format a number in human-readable form with appropriate units
 * @param num The number to format
 * @return Pointer to the formatted string
 */
static inline char *
numfmt(size_t num) {
#define BUF_SIZE 32
	static int offset = 0;
	static char buf_data[BUF_SIZE * 8];
	const char *units[] = {"", "K", "M", "G", "T"};

	int unit_index = 0;
	double value = (double)num;

	while (value >= 1000.0 && unit_index < 4) {
		value /= 1000.0;
		unit_index++;
	}

	char *buf = &buf_data[offset];
	if (unit_index == 0) {
		snprintf(buf, BUF_SIZE, "%zu", num);
	} else if (value == (int)value) {
		snprintf(buf, BUF_SIZE, "%d%s", (int)value, units[unit_index]);
	} else {
		snprintf(buf, BUF_SIZE, "%.1f%s", value, units[unit_index]);
	}

	buf[offset + BUF_SIZE - 1] = '\0';
	offset = (offset + BUF_SIZE) % sizeof(buf_data);
	return buf;
}

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

/**
 * @brief Helper function to initialize memory context from arena
 * @param ctx Memory context to initialize
 * @param arena Arena memory
 * @param name Context name
 */
static inline struct memory_context *
init_context_from_arena(void *arena, size_t arena_size, const char *name) {
	struct memory_context *ctx = (struct memory_context *)arena;
	memset(ctx, 0, sizeof(struct memory_context));

	struct block_allocator *ba = (struct block_allocator *)(ctx + 1);
	memset(ba, 0, sizeof(struct block_allocator));
	block_allocator_init(ba);

	arena = (uint8_t *)(ba + 1);
	block_allocator_put_arena(
		ba,
		arena,
		arena_size - sizeof(struct memory_context) -
			sizeof(struct block_allocator)
	);
	memory_context_init(ctx, name, ba);
	return ctx;
}

/**
 * @brief Helper function to verify memory leaks
 * @param ctx Memory context state
 * @param test_name Name of the test for error reporting
 */
static inline void
verify_memory_leaks(const struct memory_context *ctx, const char *test_name) {
	size_t net_alloc_count = ctx->balloc_count - ctx->bfree_count;

	if (ctx->balloc_count != ctx->bfree_count) {
		fprintf(stderr,
			"[%s] Memory leak detected by count: %zu (allocs: %zu, "
			"frees: %zu)\n",
			test_name,
			net_alloc_count,
			ctx->balloc_count,
			ctx->bfree_count);
		assert(false && "Memory leak detected by count");
	}

	if (ctx->balloc_size != ctx->bfree_size) {
		fprintf(stderr,
			"[%s] Memory leak detected by size: allocated %zu, "
			"freed %zu\n",
			test_name,
			ctx->balloc_size,
			ctx->bfree_size);
		assert(false && "Memory leak detected by size");
	}
}

void
test_basic_operations(void *arena) {
	printf("Testing basic operations...\n");
	uint16_t worker_idx = 0;

	// Create fresh memory context from common arena
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

	// Test insertion
	int key1 = 777, value1 = 100;
	printf("L%d: TTLMap put()\n", __LINE__);
	int ret = ttlmap_put(map, worker_idx, now, &key1, &value1, NULL);
	assert(ret >= 0);

	printf("L%d: TTLMap size()\n", __LINE__);
	size = ttlmap_size(map);
	assert(size == 1);

	printf("L%d: TTLMap empty()\n", __LINE__);
	is_empty = ttlmap_empty(map);
	assert(!is_empty);

	// Test retrieval
	int *found_value = NULL;
	printf("L%d: TTLMap get()\n", __LINE__);
	int get_ok = ttlmap_get(
		map, worker_idx, now, &key1, (void **)&found_value, NULL
	);
	assert(get_ok >= 0);

	assert(*found_value == 100);

	// Test update
	int value2 = 200;
	printf("L%d: TTLMap put()\n", __LINE__);
	ret = ttlmap_put(map, worker_idx, now, &key1, &value2, NULL);
	assert(ret >= 0);

	printf("L%d: TTLMap size()\n", __LINE__);
	size = ttlmap_size(map);
	assert(size == 1); // Size shouldn't change

	printf("L%d: TTLMap get()\n", __LINE__);
	get_ok = ttlmap_get(
		map, worker_idx, now, &key1, (void **)&found_value, NULL
	);
	assert(get_ok >= 0);
	assert(*found_value == 200);

	// Test multiple insertions
	printf("L%d: TTLMap put() +100 values\n", __LINE__);
	for (int i = 0; i < 100; i++) {
		int key = i;
		int value = i * 10;
		int ret = ttlmap_put(map, worker_idx, now, &key, &value, NULL);
		assert(ret >= 0);
		size = ttlmap_size(map);
		assert(size == (size_t)(i + 2)); // + 1 existing (key1)
	}
	printf("L%d: TTLMap size()\n", __LINE__);
	size = ttlmap_size(map);
	assert(size == 101); // 100 new + 1 existing (key1)
	printf("L%d: Complete inserting +100 values\n", __LINE__);

	printf("L%d: Going to read 100 values\n", __LINE__);
	// Test retrieval of multiple values
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
			assert(*found_value != 200); // This was updated
		} else {
			assert(*found_value == i * 10);
		}
	}

	// Clean up
	printf("L%d: Going to destroy the map\n", __LINE__);
	ttlmap_destroy(map, ctx);

	// Verify memory leaks
	verify_memory_leaks(ctx, "basic_operations");
	printf("L%d: Basic operations test PASSED\n", __LINE__);
}

/**
 * @brief Custom hash function that always returns the same value
 * This is used to force collisions for testing purposes
 */
static uint64_t
ttlmap_hash_collision_test(const void *key, size_t key_size, uint32_t seed) {
	(void)key;	// Unused parameter
	(void)key_size; // Unused parameter
	(void)seed;	// Unused parameter

	// Always return the same hash value to force all keys to collide
	// Using a non-zero value to avoid any special handling of zero
	return 0x12345678;
}

void
test_collision_handling(void *arena) {
	printf("Testing collision handling...\n");

	size_t worker_idx = 0;

	// Create fresh memory context from common arena
	struct memory_context *ctx =
		init_context_from_arena(arena, ARENA_SIZE, "collision");

	ttlmap_config_t config = {0};
	init_default_config(&config, 1000, 1000);

	// Register our custom collision hash function
	void *original_func = ttlmap_func_registry[TTLMAP_HASH_FNV1A];
	ttlmap_func_registry[TTLMAP_HASH_FNV1A] =
		(void *)ttlmap_hash_collision_test;

	// Small index size to force collisions
	ttlmap_t *map = ttlmap_new(&config, ctx);
	assert(map != NULL);

	// Insert many items to force collisions and chain growth
	for (int i = 0; i < 1000; i++) {
		int key = i;
		int value = i * 2;
		int ret = ttlmap_put(map, worker_idx, now, &key, &value, NULL);
		assert(ret >= 0);
	}

	size_t size = ttlmap_size(map);
	assert(size == 1000);

	// Verify all values
	for (int i = 0; i < 1000; i++) {
		int key = i;
		int *found_value = NULL;
		int get_ok = ttlmap_get(
			map, worker_idx, now, &key, (void **)&found_value, NULL
		);
		assert(get_ok >= 0);
		assert(*found_value == i * 2);
	}

	// Check chain length statistics
	printf("  Max chain length: %zu\n", ttlmap_max_chain_length(map));

	ttlmap_stats_t stats;
	ttlmap_get_stats(map, &stats);
	printf("  Memory used: %zu bytes\n", stats.memory_used);

	ttlmap_destroy(map, ctx);

	// Restore the original function in the registry
	ttlmap_func_registry[TTLMAP_HASH_FNV1A] = original_func;

	// Verify memory leaks
	verify_memory_leaks(ctx, "collision_handling");
	printf("Collision handling test PASSED\n");
}

void
benchmark_performance(void *arena) {
	printf("\nPerformance benchmark:\n");
	size_t worker_idx = 0;

	// Create fresh memory context from common arena
	struct memory_context *ctx =
		init_context_from_arena(arena, ARENA_SIZE, "benchmark");

	ttlmap_config_t config = {0};
	const int index_size = L3_CACHE_SIZE / 4 / 2;
	init_default_config(&config, index_size, index_size >> 8);

	ttlmap_t *map = ttlmap_new(&config, ctx);
	assert(map != NULL);

	// Benchmark insertions
	double start = get_time();
	for (int j = 0; j < NUM_REPETITIONS; j++) {
		for (int i = 0; i < index_size; i++) {
			int key = i;
			int value = i;
			int ret = ttlmap_put(
				map, worker_idx, now, &key, &value, NULL
			);
			if (ret < 0) {
				printf("L%d: failed to insert key %d on "
				       "repetition %d\n",
				       __LINE__,
				       i,
				       j + 1);
				assert(false);
				exit(1);
			}
		}
	}
	double end = get_time();
	size_t total_elements = map->counters[0].total_elements;
	assert(total_elements == (size_t)index_size);
	double insert_time = (end - start) / (double)NUM_REPETITIONS;
	double insert_throughput = index_size / insert_time;
	printf("  Inserted %s items in %.3f seconds " COLOR_CYAN
	       "(%s ops/sec)" COLOR_RESET "\n",
	       numfmt(index_size),
	       insert_time,
	       numfmt((size_t)insert_throughput));

	// Benchmark lookups
	start = get_time();
	volatile int checksum = 0; // Prevent compiler optimization
	for (int j = 0; j < NUM_REPETITIONS; j++) {
		for (int i = 0; i < index_size; i++) {
			int key = i;
			int *value;
			int get_ok = ttlmap_get(
				map,
				worker_idx,
				now,
				&key,
				(void **)&value,
				NULL
			);
			if (get_ok < 0) {
				printf("failed to get key: %d at L%d",
				       key,
				       __LINE__);
				assert(false);
				exit(1);
			}
			if (j == 0) {

				checksum +=
					*value; // Force compiler to actually
						// use the read value
			}
		}
	}
	end = get_time();
	// Use checksum to prevent dead code elimination
	if (checksum == 0) {
		printf("Unexpected checksum\n");
		assert(false);
		exit(1);
	}
	double lookup_time = ((double)(end - start)) / (double)NUM_REPETITIONS;
	double lookup_throughput = index_size / lookup_time;
	printf("  Looked up %s items in %.3f seconds " COLOR_CYAN
	       "(%s ops/sec)" COLOR_RESET "\n",
	       numfmt(index_size),
	       lookup_time,
	       numfmt((size_t)lookup_throughput));

	// Print statistics
	ttlmap_stats_t stats;
	ttlmap_get_stats(map, &stats);
	printf("  Final statistics:\n");
	printf("    Total elements: %s\n", numfmt(stats.total_elements));
	printf("    Index size: %zu\n", stats.index_size);
	printf("    Max chain length: %zu\n", stats.max_chain_length);
	printf("    Memory used: %zu KB\n", stats.memory_used / 1024);

	ttlmap_destroy(map, ctx);

	// Verify memory leaks
	verify_memory_leaks(ctx, "benchmark_performance");
}

// ============================================================================
// Multi-threaded Test Functions
// ============================================================================

// Thread data structure for MT tests
typedef struct {
	ttlmap_t *map;
	uint16_t thread_id;
	int value_seed;
	double elapsed_time;
	// Pointers to per-thread counters
	uint64_t write_checksum;
	uint64_t read_checksum;
	int successful_writes;
	int successful_reads;
} mt_thread_data_t;

/**
 * Thread function for concurrent writes
 */
static void *
writer_thread(void *arg) {
	mt_thread_data_t *data = (mt_thread_data_t *)arg;

	uint8_t value_buffer[VALUE_SIZE];
	memset(value_buffer, data->value_seed, VALUE_SIZE);

	data->write_checksum = 0;
	double start_time = get_time();
	int successful = 0;

	size_t j = 0;
	for (; j < NUM_REPETITIONS; j++) {
		for (size_t i = 0; i < TOTAL_VALUES; i++) {
			int key = (int)i;

			// Use thread-safe put function
			size_t id = key % NUM_THREADS;
			value_buffer[id] = (uint8_t)id;

			int ret = ttlmap_put_safe(
				data->map,
				data->thread_id,
				now,
				&key,
				value_buffer
			);

			if (ret >= 0) {
				successful++;
				if (j == 0) {
					if (id == data->thread_id) {
						data->write_checksum +=
							key + id +
							data->value_seed;
					}
				}
			} else {
				printf("L%d ERROR: failed to write value for "
				       "%d\n",
				       __LINE__,
				       key);
				if (errno) {
					perror("Reason: ");
				}
				assert(false);
				exit(1);
			}
		}
	}

	double end_time = get_time();
	data->elapsed_time = end_time - start_time;

	// Write to per-thread counters (no contention)
	data->successful_writes = successful;

	return NULL;
}

/**
 * Thread function for concurrent reads in benchmark
 */
static void *
reader_thread_benchmark(void *arg) {
	mt_thread_data_t *data = (mt_thread_data_t *)arg;

	data->read_checksum = 0;
	double start_time = get_time();
	int successful = 0;

	int j = 0;
	for (; j < NUM_REPETITIONS; j++) {
		for (size_t i = 0; i < TOTAL_VALUES; i++) {
			int key = i;

			ttlmap_rwlock_t *lock = NULL;
			uint8_t *value;
			int ret = ttlmap_get(
				data->map,
				data->thread_id,
				now,
				&key,
				(void **)&value,
				&lock
			);
			if (ret >= 0) {
				if (j == 0) {
					size_t id = key % NUM_THREADS;
					if (id == data->thread_id) {
						data->read_checksum +=
							key +
							value[data->thread_id] +
							data->value_seed;
					}
				}
				ttlmap_rwlock_read_unlock(lock);
				successful++;
			} else {
				printf("L%d ERROR: value with key=%d is not "
				       "found\n",
				       __LINE__,
				       key);
				assert(false);
				exit(1);
			}
		}
	}

	double end_time = get_time();
	data->elapsed_time = end_time - start_time;
	data->successful_reads = successful;
	return NULL;
}

void
test_multithreaded_benchmark(void) {
	printf(COLOR_BOLD COLOR_GREEN
	       "=== Multi-threaded Benchmark Test ===" COLOR_RESET "\n\n");
	size_t index_size = TOTAL_VALUES;

	printf("Configuration:\n");
	printf("  Threads: %d\n", NUM_THREADS);
	printf("  Arena size: %s\n", numfmt(MT_ARENA_SIZE));
	printf("  Total values: %s\n", numfmt(TOTAL_VALUES));
	printf("  Index size: %s\n", numfmt(index_size));
	printf("  Value size: %d bytes\n", VALUE_SIZE);
	printf("  Total data size: %.2f MB (%.1fx L3 cache)\n",
	       (double)(TOTAL_VALUES * VALUE_SIZE) / (1024 * 1024),
	       (double)(TOTAL_VALUES * VALUE_SIZE) / L3_CACHE_SIZE);
	printf("  Map index size (%s bytes): %s\n",
	       numfmt(index_size * 8),
	       numfmt(index_size));
	printf("\n");

	// Create arena for memory allocation (larger for benchmark)
	void *mt_arena = allocate_locked_memory(MT_ARENA_SIZE);
	if (!mt_arena) {
		printf("Failed to allocate MT arena\n");
		assert(false);
		exit(1);
	}
	struct memory_context *ctx =
		init_context_from_arena(mt_arena, MT_ARENA_SIZE, "benchmark");

	ttlmap_config_t config = {0};
	init_default_config(&config, index_size, index_size >> 8);
	config.value_size = VALUE_SIZE;
	config.worker_count = NUM_THREADS;

	// Create map with appropriate size
	ttlmap_t *map = ttlmap_new(&config, ctx);
	if (!map) {
		if (errno != 0) {
			perror("failed to create TTLMap: ");
		} else {

			printf("Failed to create TTLMap (unknown error)\n");
		}
		free_memory(mt_arena, MT_ARENA_SIZE);
		assert(false);
		exit(1);
	}

	uint8_t value_seed = (uint8_t)rand();

	// Create thread data
	pthread_t threads[NUM_THREADS];
	mt_thread_data_t thread_data[NUM_THREADS];

	// Phase 1: Concurrent writes
	double write_start = get_time();

	for (int i = 0; i < NUM_THREADS; i++) {
		thread_data[i].map = map;
		thread_data[i].thread_id = i;
		thread_data[i].value_seed = value_seed;
		if (pthread_create(
			    &threads[i], NULL, writer_thread, &thread_data[i]
		    ) != 0) {
			printf("Failed to create writer thread %d\n", i);
			assert(false);
			exit(1);
		}
	}

	// Wait for all writer threads to complete
	for (int i = 0; i < NUM_THREADS; i++) {
		pthread_join(threads[i], NULL);
	}

	double write_end = get_time();
	double total_write_time = write_end - write_start;
	double total_write_elapsed_time = 0.0;

	// Sum up per-thread write statistics (fast, no atomics)
	uint64_t total_successful_writes = 0;
	for (int i = 0; i < NUM_THREADS; i++) {
		total_successful_writes += thread_data[i].successful_writes;
		total_write_elapsed_time += thread_data[i].elapsed_time;
	}

	printf("\n" COLOR_BOLD COLOR_YELLOW
	       "+ Write Phase Results +" COLOR_RESET "\n");
	printf("Wall write time: %.3f seconds\n", total_write_time);
	printf("Total write time (CPU time): %.3f seconds\n",
	       total_write_elapsed_time);
	printf("Total write operations: %s\n", numfmt(TOTAL_OPS));
	printf("Successful writes: %s\n", numfmt(total_successful_writes));
	printf("Write throughput: " COLOR_CYAN "%s ops/sec" COLOR_RESET "\n",
	       numfmt(TOTAL_OPS / total_write_elapsed_time));
	assert(TOTAL_OPS == total_successful_writes);

	// Get map statistics
	ttlmap_stats_t stats;
	ttlmap_get_stats(map, &stats);
	printf("\nMap statistics after writes:\n");
	printf("  Total elements: %s\n", numfmt(stats.total_elements));
	printf("  Max chain length: %zu\n", stats.max_chain_length);
	printf("  Memory used: %.2f MB\n",
	       stats.memory_used / (1024.0 * 1024.0));

	// Phase 2: Concurrent reads
	double read_start = get_time();
	for (int i = 0; i < NUM_THREADS; i++) {
		thread_data[i].map = map;
		thread_data[i].thread_id = i;
		thread_data[i].value_seed = value_seed;

		if (pthread_create(
			    &threads[i],
			    NULL,
			    reader_thread_benchmark,
			    &thread_data[i]
		    ) != 0) {
			printf("Failed to create reader thread %d\n", i);
			assert(false);
			exit(1);
		}
	}

	// Wait for all reader threads to complete
	for (int i = 0; i < NUM_THREADS; i++) {
		pthread_join(threads[i], NULL);
	}

	double read_end = get_time();
	double total_read_time = read_end - read_start;

	// Sum up per-thread read statistics (fast, no atomics)
	uint64_t total_successful_reads = 0;
	double total_read_elapsed_time = 0.0;
	uint64_t result_read_checksum = 0;
	for (int i = 0; i < NUM_THREADS; i++) {
		total_successful_reads += thread_data[i].successful_reads;
		total_read_elapsed_time += thread_data[i].elapsed_time;
		result_read_checksum += thread_data[i].read_checksum;
	}
	printf("\n" COLOR_BOLD COLOR_YELLOW "+ Read Phase Results +" COLOR_RESET
	       "\n");
	printf("Wall read time: %.3f seconds\n", total_read_time);
	printf("Total read time (CPU time): %.3f seconds\n",
	       total_read_elapsed_time);
	printf("Total read operations: %s\n", numfmt(TOTAL_OPS));
	printf("Successful reads: %s\n", numfmt(total_successful_reads));
	printf("Read checksum: %zu\n", result_read_checksum);
	printf("Read throughput: " COLOR_CYAN "%s ops/sec" COLOR_RESET "\n",
	       numfmt(TOTAL_OPS / total_read_elapsed_time));

	// Overall summary
	printf("\n" COLOR_BOLD COLOR_MAGENTA
	       "=== Overall Summary ===" COLOR_RESET "\n");
	printf("Main arena size %llu MB\n", MT_ARENA_SIZE >> 20);
	printf("Total operations (write + read): %s\n", numfmt(TOTAL_OPS * 2));
	printf("Total successful operations: %s\n",
	       numfmt(total_successful_writes + total_successful_reads));

	// Add assertions to fail the test if success rates are not 100%
	// Compare actual counts instead of percentages to avoid floating point
	// precision issues
	if (total_successful_writes != TOTAL_OPS) {
		printf("L%d ERROR: Write success rate (%lu/%llu) is below "
		       "required threshold\n",
		       __LINE__,
		       total_successful_writes,
		       TOTAL_OPS);
		assert(false);
		exit(1);
	}
	if (total_successful_reads != TOTAL_OPS) {
		printf("L%d ERROR: Read success rate (%lu/%llu) is below "
		       "required "
		       "threshold\n",
		       __LINE__,
		       total_successful_reads,
		       TOTAL_OPS);
		assert(false);
		exit(1);
	}
	for (int i = 0; i < NUM_THREADS; i++) {
		if (thread_data[i].read_checksum !=
		    thread_data[i].write_checksum) {
			printf("L%d: Read checksum mismatch for %d: "
			       "read=%zu "
			       "!= write=%zu\n",
			       __LINE__,
			       i,
			       thread_data[i].read_checksum,
			       thread_data[i].write_checksum);
			assert(false);
			exit(1);
		}
	}

	// Cleanup
	ttlmap_destroy(map, ctx);
	free_memory(mt_arena, MT_ARENA_SIZE);

	printf("\n" COLOR_BOLD COLOR_GREEN
	       "Multi-threaded benchmark test PASSED" COLOR_RESET "\n");
}

int
main() {
	printf(COLOR_BOLD COLOR_WHITE
	       "=== TTLMap Comprehensive Test Suite ===" COLOR_RESET "\n\n");

	// Create common arena for all tests
	void *arena = allocate_locked_memory(ARENA_SIZE);
	if (arena == NULL) {
		printf("could not allocate arena\n");
		return -1;
	}

	printf(COLOR_BOLD COLOR_BLUE "=== Single-threaded Tests ===" COLOR_RESET
				     "\n");
	test_basic_operations(arena);
	test_collision_handling(arena);
	benchmark_performance(arena);
	test_multithreaded_benchmark();

	free_memory(arena, ARENA_SIZE);
	printf("\n" COLOR_BOLD COLOR_GREEN
	       "=== All tests PASSED ===" COLOR_RESET "\n");
	return 0;
}
