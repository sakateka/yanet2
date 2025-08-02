/**
 * @file test_multiprocess_swissmap.c
 * @brief Comprehensive multi-process test for Swiss map with shared memory
 *
 * This test implements a complex scenario with clean process separation:
 * 1. Main process forks and execs child process #0:
 *    - Child sets up shared memory arena and memory context
 *    - Creates map with hint size 1 and fills it with 10k entries
 * 2. Main process forks and execs child process #1:
 *    - Child attaches to shared memory and looks up entries
 *    - Removes 2k entries from the map
 * 3. Main process forks and execs child process #2:
 *    - Child attaches to shared memory and verifies remaining 8k entries
 * 4. Main process forks and execs child process #3:
 *    - Child attaches to shared memory and copies entire content to anonymous
 * memory
 *    - Child verifies that the map works correctly from the anonymous memory
 * copy
 * 5. Main process performs final verification by attaching to shared memory
 *    - Validates final map state and cleans up shared memory
 *
 * All processes use named shared memory for cross-process communication,
 * with proper offset-based pointer handling for address space independence.
 */

#include "common/memory.h"
#include "common/memory_block.h"
#include "common/swissmap.h"
#include <assert.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define ARENA_SIZE (16 << 20) // 16MB arena for large test
#define NUM_ENTRIES 10000     // 10k entries to insert
#define NUM_TO_REMOVE 2000    // 2k entries to remove in child 1
#define NUM_REMAINING 8000    // 8k entries remaining for child 2

// Test modes
// NOLINTBEGIN(readability-identifier-naming)
typedef enum {
	MODE_MAIN = 0,
	MODE_CHILD0 = 1,
	MODE_CHILD1 = 2,
	MODE_CHILD2 = 3,
	MODE_CHILD3 = 4
} test_mode_t;
// NOLINTEND(readability-identifier-naming)

static const char *
test_mode_to_string(test_mode_t mode) {
	switch (mode) {
	case MODE_MAIN:
		return "MAIN";
	case MODE_CHILD0:
		return "CHILD0";
	case MODE_CHILD1:
		return "CHILD1";
	case MODE_CHILD2:
		return "CHILD2";
	case MODE_CHILD3:
		return "CHILD3";
	default:
		return "INVALID";
	}
}

// Shared memory structure to pass between processes
struct shared_memory_info {
	size_t arena_size;
	struct block_allocator ba;
	struct memory_context mctx;
	swiss_map_t *map;
	size_t entries_inserted;
	size_t entries_removed;
};

// Shared memory name
static const char *SHM_NAME = "/swissmap_test_shm"; // NOLINT

/**
 * @brief Create standard int map configuration with memory context
 */
static swiss_map_config_t
create_int_config(struct memory_context *ctx) {
	swiss_map_config_t config = {0};
	config.key_size = sizeof(int);
	config.value_size = sizeof(int);
	config.hash_fn_id = SWISS_HASH_FNV1A;
	config.key_equal_fn_id = SWISS_KEY_EQUAL_DEFAULT;
	config.alloc_fn_id = SWISS_ALLOC_SHARED;
	config.free_fn_id = SWISS_FREE_SHARED;
	config.rand_fn_id = SWISS_RAND_DEFAULT;
	config.mem_ctx = ctx;
	return config;
}

/**
 * @brief Setup shared memory arena and initialize memory context
 */
static struct shared_memory_info *
setup_shared_memory(void) {
	// Clean up any existing shared memory
	shm_unlink(SHM_NAME);

	// Create ONE shared memory region that contains everything
	// Structure: [shared_memory_info][arena_data...]
	size_t total_size = sizeof(struct shared_memory_info) + ARENA_SIZE;

	int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
	if (shm_fd == -1) {
		perror("shm_open failed");
		return NULL;
	}

	if (ftruncate(shm_fd, total_size) == -1) {
		perror("ftruncate failed");
		close(shm_fd);
		shm_unlink(SHM_NAME);
		return NULL;
	}

	void *shm_ptr = mmap(
		NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0
	);
	close(shm_fd);

	if (shm_ptr == MAP_FAILED) {
		perror("mmap failed for shared memory");
		shm_unlink(SHM_NAME);
		return NULL;
	}

	struct shared_memory_info *shm_info =
		(struct shared_memory_info *)shm_ptr;
	memset(shm_info, 0, sizeof(struct shared_memory_info));

	// Arena starts right after the shared_memory_info structure
	void *arena = (char *)shm_ptr + sizeof(struct shared_memory_info);
	shm_info->arena_size = ARENA_SIZE;

	// Initialize block allocator in shared memory
	block_allocator_init(&shm_info->ba);
	block_allocator_put_arena(&shm_info->ba, arena, ARENA_SIZE);

	// Initialize memory context
	memory_context_init(
		&shm_info->mctx, "multiprocess_test", &shm_info->ba
	);

	printf("✓ Shared memory setup complete: shm=%p, arena=%p, size=%zu\n",
	       shm_ptr,
	       arena,
	       (size_t)ARENA_SIZE);

	return shm_info;
}

/**
 * @brief Attach to existing shared memory using named shared memory
 */
static struct shared_memory_info *
attach_shared_memory(void) {
	size_t total_size = sizeof(struct shared_memory_info) + ARENA_SIZE;

	int shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
	if (shm_fd == -1) {
		perror("shm_open failed in child");
		return NULL;
	}

	void *shm_ptr = mmap(
		NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0
	);
	close(shm_fd);

	if (shm_ptr == MAP_FAILED) {
		perror("mmap failed for shared memory in child");
		return NULL;
	}

	struct shared_memory_info *shm_info =
		(struct shared_memory_info *)shm_ptr;

	printf("✓ Attached to shm at %p\n", shm_ptr);
	return shm_info;
}

/**
 * @brief Child process 0: create map and fill with entries
 */
static int
run_child0_process(struct shared_memory_info *shm_info) {
	printf("=== CHILD PROCESS 0: Creating map and inserting %d entries "
	       "===\n",
	       NUM_ENTRIES);

	// Create map with hint size 1 as requested
	swiss_map_config_t config = create_int_config(&shm_info->mctx);
	swiss_map_t *map = swiss_map_new(&config, 1); // hint size = 1
	if (!map) {
		fprintf(stderr, "Failed to create Swiss map\n");
		return -1;
	}

	SET_OFFSET_OF(&shm_info->map, map);
	printf("✓ Map created with hint size 1\n");

	// Record initial directory state
	uint8_t initial_global_depth = map->global_depth;
	int initial_dir_len = map->dir_len;
	printf("Initial directory state: global_depth=%d, dir_len=%d\n",
	       initial_global_depth,
	       initial_dir_len);

	// Insert 10k entries
	printf("Inserting %d entries...\n", NUM_ENTRIES);
	for (int i = 0; i < NUM_ENTRIES; i++) {
		int key = i;
		int value = i * 100; // Simple value pattern

		if (swiss_map_put(map, &key, &value) != 0) {
			fprintf(stderr, "Failed to insert key %d\n", i);
			return -1;
		}

		if ((i + 1) % 1000 == 0) {
			printf("  Inserted %d entries...\n", i + 1);
		}
	}

	// Verify directory expansion occurred
	uint8_t final_global_depth = map->global_depth;
	int final_dir_len = map->dir_len;
	printf("Final directory state: global_depth=%d, dir_len=%d\n",
	       final_global_depth,
	       final_dir_len);

	if (final_global_depth <= initial_global_depth ||
	    final_dir_len <= initial_dir_len) {
		fprintf(stderr,
			"ERROR: Directory expansion did not occur as "
			"expected!\n");
		fprintf(stderr,
			"  Initial: depth=%d, len=%d\n",
			initial_global_depth,
			initial_dir_len);
		fprintf(stderr,
			"  Final: depth=%d, len=%d\n",
			final_global_depth,
			final_dir_len);
		return -1;
	}

	printf("✓ Directory expansion verified: depth %d->%d, len %d->%d\n",
	       initial_global_depth,
	       final_global_depth,
	       initial_dir_len,
	       final_dir_len);

	shm_info->entries_inserted = NUM_ENTRIES;
	size_t map_size = swiss_map_size(map);
	printf("✓ Successfully inserted %d entries, map size: %zu\n",
	       NUM_ENTRIES,
	       map_size);

	// Verify some entries
	printf("Verifying sample entries...\n");
	for (int i = 0; i < 10; i++) {
		int key = i * 1000; // Sample every 1000th entry
		if (key >= NUM_ENTRIES)
			break;

		int *found_value;
		if (!swiss_map_get(map, &key, (void **)&found_value)) {
			fprintf(stderr, "Failed to find key %d\n", key);
			return -1;
		}
		if (*found_value != key * 100) {
			fprintf(stderr,
				"Wrong value for key %d: got %d, expected %d\n",
				key,
				*found_value,
				key * 100);
			return -1;
		}
	}
	printf("✓ Sample entries verified\n");

	return 0;
}

/**
 * @brief Child process 1: lookup entries and remove 2k
 */
static int
run_child1_process(struct shared_memory_info *shm_info) {
	printf("=== CHILD PROCESS 1: Looking up entries and removing %d ===\n",
	       NUM_TO_REMOVE);

	swiss_map_t *map = (swiss_map_t *)ADDR_OF(&shm_info->map);
	if (!map) {
		fprintf(stderr, "Map not found in shared memory\n");
		return -1;
	}

	size_t initial_size = swiss_map_size(map);
	printf("✓ Found map in shared memory, initial size: %zu\n",
	       initial_size);

	// Verify we can lookup entries
	printf("Verifying lookup functionality...\n");
	int lookup_count = 0;
	for (int i = 0; i < NUM_ENTRIES; i += 100) { // Sample every 100th entry
		int key = i;
		int *found_value;
		if (swiss_map_get(map, &key, (void **)&found_value)) {
			if (*found_value == key * 100) {
				lookup_count++;
			} else {
				fprintf(stderr,
					"Wrong value for key %d: got %d, "
					"expected %d\n",
					key,
					*found_value,
					key * 100);
				return -1;
			}
		} else {
			fprintf(stderr, "Failed to find key %d\n", key);
			return -1;
		}
	}
	printf("✓ Successfully looked up %d sample entries\n", lookup_count);

	// Remove 2k entries (remove every 5th entry: 0, 5, 10, 15, ...)
	printf("Removing %d entries...\n", NUM_TO_REMOVE);
	int removed_count = 0;
	for (int i = 0; i < NUM_ENTRIES && removed_count < NUM_TO_REMOVE;
	     i += 5) {
		int key = i;
		if (swiss_map_delete(map, &key)) {
			removed_count++;
			if (removed_count % 500 == 0) {
				printf("  Removed %d entries...\n",
				       removed_count);
			}
		}
	}

	shm_info->entries_removed = removed_count;
	size_t final_size = swiss_map_size(map);
	printf("✓ Removed %d entries, final map size: %zu\n",
	       removed_count,
	       final_size);

	if (final_size != initial_size - removed_count) {
		fprintf(stderr,
			"Size mismatch: expected %zu, got %zu\n",
			initial_size - removed_count,
			final_size);
		return -1;
	}

	// Verify removed entries are gone
	printf("Verifying removed entries are gone...\n");
	int verified_removed = 0;
	for (int i = 0; i < NUM_ENTRIES && verified_removed < 100; i += 5) {
		int key = i;
		int *found_value;
		if (!swiss_map_get(map, &key, (void **)&found_value)) {
			verified_removed++;
		} else {
			fprintf(stderr,
				"Key %d should have been removed but was "
				"found\n",
				key);
			return -1;
		}
	}
	printf("✓ Verified %d removed entries are gone\n", verified_removed);

	return 0;
}

/**
 * @brief Child process 2: lookup remaining 8k entries
 */
static int
run_child2_process(struct shared_memory_info *shm_info) {
	printf("=== CHILD PROCESS 2: Looking up remaining entries ===\n");

	swiss_map_t *map = (swiss_map_t *)ADDR_OF(&shm_info->map);
	if (!map) {
		fprintf(stderr, "Map not found in shared memory\n");
		return -1;
	}

	size_t map_size = swiss_map_size(map);
	size_t expected_remaining = NUM_ENTRIES - shm_info->entries_removed;
	printf("✓ Found map in shared memory, size: %zu, expected remaining: "
	       "%zu\n",
	       map_size,
	       expected_remaining);

	if (map_size != expected_remaining) {
		fprintf(stderr,
			"Unexpected map size: got %zu, expected %zu\n",
			map_size,
			expected_remaining);
		return -1;
	}

	// Lookup remaining entries (should be all entries except those
	// divisible by 5)
	printf("Looking up remaining entries...\n");
	int found_count = 0;
	int expected_found = 0;

	for (int i = 0; i < NUM_ENTRIES; i++) {
		int key = i;
		int *found_value;

		if (i % 5 == 0) {
			// This key should have been removed
			if (swiss_map_get(map, &key, (void **)&found_value)) {
				fprintf(stderr,
					"Key %d should have been removed but "
					"was found\n",
					key);
				return -1;
			}
		} else {
			// This key should still exist
			expected_found++;
			if (swiss_map_get(map, &key, (void **)&found_value)) {
				if (*found_value == key * 100) {
					found_count++;
				} else {
					fprintf(stderr,
						"Wrong value for key %d: got "
						"%d, expected %d\n",
						key,
						*found_value,
						key * 100);
					return -1;
				}
			} else {
				fprintf(stderr,
					"Key %d should exist but was not "
					"found\n",
					key);
				return -1;
			}
		}

		if ((i + 1) % 2000 == 0) {
			printf("  Processed %d keys, found %d valid "
			       "entries...\n",
			       i + 1,
			       found_count);
		}
	}

	printf("✓ Successfully found %d entries (expected %d)\n",
	       found_count,
	       expected_found);

	if (found_count != expected_found) {
		fprintf(stderr,
			"Found count mismatch: got %d, expected %d\n",
			found_count,
			expected_found);
		return -1;
	}

	if (found_count != NUM_REMAINING) {
		fprintf(stderr,
			"Found count doesn't match expected remaining: got %d, "
			"expected %d\n",
			found_count,
			NUM_REMAINING);
		return -1;
	}

	return 0;
}

/**
 * @brief Child process 3: copy shared memory to anonymous memory and verify
 */
static int
run_child3_process(struct shared_memory_info *shm_info) {
	printf("=== CHILD PROCESS 3: Copying to anonymous memory and verifying "
	       "===\n");

	// First, attach to shared memory to get the map
	swiss_map_t *map = (swiss_map_t *)ADDR_OF(&shm_info->map);
	if (!map) {
		fprintf(stderr, "Map not found in shared memory\n");
		return -1;
	}

	size_t map_size = swiss_map_size(map);
	printf("✓ Found map in shared memory, size: %zu\n", map_size);

	// Calculate total size needed for the copy
	size_t total_size = sizeof(struct shared_memory_info) + ARENA_SIZE;
	printf("Copying %zu bytes from shared memory to anonymous memory...\n",
	       total_size);

	// Create anonymous memory region
	void *anon_mem =
		mmap(NULL,
		     total_size,
		     PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS,
		     -1,
		     0);
	if (anon_mem == MAP_FAILED) {
		perror("mmap failed for anonymous memory");
		return -1;
	}

	printf("✓ Anonymous memory allocated at %p\n", anon_mem);

	// Copy the entire shared memory to anonymous memory
	memcpy(anon_mem, shm_info, total_size);
	printf("✓ Shared memory copied to anonymous memory\n");

	// Get the shared memory info pointer from anonymous memory
	struct shared_memory_info *anon_shm_info =
		(struct shared_memory_info *)anon_mem;

	swiss_map_t *anon_map = (swiss_map_t *)ADDR_OF(&anon_shm_info->map);
	// Now verify that we can use the map from anonymous memory
	printf("Verifying map functionality from anonymous memory...\n");

	// Test lookup of remaining entries (should be all entries except those
	// divisible by 5)
	int found_count = 0;
	int expected_found = 0;

	for (int i = 0; i < NUM_ENTRIES; i++) {
		int key = i;
		int *found_value;

		if (i % 5 == 0) {
			// This key should have been removed by child 1
			if (swiss_map_get(
				    anon_map, &key, (void **)&found_value
			    )) {
				fprintf(stderr,
					"Key %d should have been removed but "
					"was found in anonymous memory\n",
					key);
				munmap(anon_mem, total_size);
				return -1;
			}
		} else {
			// This key should still exist
			expected_found++;
			if (swiss_map_get(
				    anon_map, &key, (void **)&found_value
			    )) {
				if (*found_value == key * 100) {
					found_count++;
				} else {
					fprintf(stderr,
						"Wrong value for key %d in "
						"anonymous memory: got %d, "
						"expected %d\n",
						key,
						*found_value,
						key * 100);
					munmap(anon_mem, total_size);
					return -1;
				}
			} else {
				fprintf(stderr,
					"Key %d should exist but was not found "
					"in anonymous memory\n",
					key);
				munmap(anon_mem, total_size);
				return -1;
			}
		}

		if ((i + 1) % 2000 == 0) {
			printf("  Processed %d keys, found %d valid entries in "
			       "anonymous memory...\n",
			       i + 1,
			       found_count);
		}
	}

	printf("✓ Successfully found %d entries in anonymous memory (expected "
	       "%d)\n",
	       found_count,
	       expected_found);

	if (found_count != expected_found) {
		fprintf(stderr,
			"Found count mismatch in anonymous memory: got %d, "
			"expected %d\n",
			found_count,
			expected_found);
		munmap(anon_mem, total_size);
		return -1;
	}

	if (found_count != NUM_REMAINING) {
		fprintf(stderr,
			"Found count doesn't match expected remaining in "
			"anonymous memory: got %d, expected %d\n",
			found_count,
			NUM_REMAINING);
		munmap(anon_mem, total_size);
		return -1;
	}

	// Clean up anonymous memory
	munmap(anon_mem, total_size);
	printf("✓ All verifications passed in anonymous memory!\n");
	return 0;
}

/**
 * @brief Fork and exec child process with shared memory info
 */
static int
fork_and_exec_child(test_mode_t mode) {

	pid_t pid = fork();
	if (pid == -1) {
		perror("fork failed");
		return -1;
	}

	if (pid == 0) {
		// Child process
		char mode_str[8];
		snprintf(mode_str, sizeof(mode_str), "%d", mode);

		// Exec self with mode argument
		execl("/proc/self/exe",
		      "test_multiprocess_swissmap",
		      mode_str,
		      NULL);
		perror("execl failed");
		exit(1);
	}

	// Parent process - wait for child
	int status;
	if (waitpid(pid, &status, 0) == -1) {
		perror("waitpid failed");
		return -1;
	}

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "Child process failed with status %d\n", status
		);
		return -1;
	}

	printf("✓ Child process %s completed successfully\n",
	       test_mode_to_string(mode));
	return 0;
}

int
main(int argc, char *argv[]) {
	test_mode_t mode = MODE_MAIN;
	struct shared_memory_info *shm_info = NULL;

	// Check if we're being called as a child process
	if (argc > 1) {
		mode = (test_mode_t)atoi(argv[1]);
	}

	printf("Starting multiprocess Swiss map test (mode: %s)\n",
	       test_mode_to_string(mode));

	int result = 0;

	switch (mode) {
	case MODE_MAIN: {
		printf("=== MAIN PROCESS START ===\n");

		// Fork and exec child process 0
		printf("\n=== FORKING CHILD PROCESS 0 ===\n");
		if (fork_and_exec_child(MODE_CHILD0) != 0) {
			fprintf(stderr, "Child process 0 failed\n");
			return 1;
		}

		// Fork and exec child process 1
		printf("\n=== FORKING CHILD PROCESS 1 ===\n");
		if (fork_and_exec_child(MODE_CHILD1) != 0) {
			fprintf(stderr, "Child process 1 failed\n");
			return 1;
		}

		// Fork and exec child process 2
		printf("\n=== FORKING CHILD PROCESS 2 ===\n");
		if (fork_and_exec_child(MODE_CHILD2) != 0) {
			fprintf(stderr, "Child process 2 failed\n");
			return 1;
		}

		// Fork and exec child process 3
		printf("\n=== FORKING CHILD PROCESS 3 ===\n");
		if (fork_and_exec_child(MODE_CHILD3) != 0) {
			fprintf(stderr, "Child process 3 failed\n");
			return 1;
		}

		// Final verification - attach to shared memory to check results
		shm_info = attach_shared_memory();
		if (!shm_info) {
			fprintf(stderr,
				"Failed to attach to shared memory for "
				"verification\n");
			return 1;
		}

		printf("\n=== FINAL VERIFICATION ===\n");
		size_t final_size =
			swiss_map_size((swiss_map_t *)ADDR_OF(&shm_info->map));
		printf("Final map size: %zu\n", final_size);
		printf("Entries inserted: %zu\n", shm_info->entries_inserted);
		printf("Entries removed: %zu\n", shm_info->entries_removed);

		if (final_size != NUM_REMAINING) {
			fprintf(stderr,
				"Final size mismatch: got %zu, expected %d\n",
				final_size,
				NUM_REMAINING);
			result = 1;
		} else {
			printf("✓ All tests passed successfully!\n");
		}

		shm_unlink(SHM_NAME);
		break;
	}

	case MODE_CHILD0: {
		// Setup shared memory
		shm_info = setup_shared_memory();
		if (!shm_info) {
			fprintf(stderr, "Failed to setup shared memory\n");
			return 1;
		}

		// Run child 0 logic
		result = run_child0_process(shm_info);
		break;
	}

	case MODE_CHILD1: {
		// Attach to shared memory
		shm_info = attach_shared_memory();
		if (!shm_info) {
			fprintf(stderr, "Failed to attach to shared memory\n");
			return 1;
		}

		// Run child 1 logic
		result = run_child1_process(shm_info);
		break;
	}

	case MODE_CHILD2: {
		// Attach to shared memory
		shm_info = attach_shared_memory();
		if (!shm_info) {
			fprintf(stderr, "Failed to attach to shared memory\n");
			return 1;
		}

		// Run child 2 logic
		result = run_child2_process(shm_info);
		break;
	}

	case MODE_CHILD3: {
		// Attach to shared memory
		shm_info = attach_shared_memory();
		if (!shm_info) {
			fprintf(stderr, "Failed to attach to shared memory\n");
			return 1;
		}

		// Run child 3 logic
		result = run_child3_process(shm_info);
		break;
	}

	default:
		fprintf(stderr, "Invalid mode: %d\n", mode);
		return 1;
	}

	return result;
}