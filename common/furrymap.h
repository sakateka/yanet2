/**
 * FurryMap - High-performance hash map implementation
 *
 * Key features:
 * - Fixed index size (no resizing) for predictable performance
 * - Cache-line optimized groups (64 bytes)
 * - SIMD-friendly control bytes for parallel slot matching
 * - Bucket-level locking for thread safety
 * - Shared memory compatible (offset-based pointers)
 */

#ifndef FURRYMAP_H
#define FURRYMAP_H

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

#include "memory.h"
#include "memory_address.h"
#include "memory_block.h"
#include "numutils.h"

// ============================================================================
// Constants and Global Registry
// ============================================================================

#define FURRYMAP_GROUP_SLOTS 8
#define FURRYMAP_CTRL_EMPTY 0x80
#define FURRYMAP_CTRL_DELETED 0xFE
// 0x00-0x7F: Occupied slots with H2 hash bits

// Bitset constants for parallel operations
#define FURRYMAP_BITSET_LSB 0x0101010101010101UL
#define FURRYMAP_BITSET_MSB 0x8080808080808080UL
#define FURRYMAP_BITSET_EMPTY (FURRYMAP_BITSET_LSB * FURRYMAP_CTRL_EMPTY)

// Function registry for cross-process compatibility
// NOLINTBEGIN(readability-identifier-naming)
typedef enum {
	FURRYMAP_HASH_FNV1A,
	FURRYMAP_KEY_EQUAL_DEFAULT,
	FURRYMAP_RAND_DEFAULT,
	FURRYMAP_RAND_SECURE,
	FURRYMAP_FUNC_COUNT
} furrymap_func_id_t;
// NOLINTEND(readability-identifier-naming)

// Global function registry (declared here, defined at bottom)
static void *furrymap_func_registry[FURRYMAP_FUNC_COUNT];

// Spinlock structure (copied from DPDK rte_spinlock_t)
typedef struct {
	volatile int locked;
} furrymap_lock_t;

// Acquire spinlock (copied from DPDK rte_spinlock_lock x86)
static inline void
furrymap_lock_acquire_read(furrymap_lock_t *lock) {
	int lock_val = 1;
	asm volatile("1:\n"
		     "xchg %[locked], %[lv]\n"
		     "test %[lv], %[lv]\n"
		     "jz 3f\n"
		     "2:\n"
		     "pause\n"
		     "cmpl $0, %[locked]\n"
		     "jnz 2b\n"
		     "jmp 1b\n"
		     "3:\n"
		     : [locked] "=m"(lock->locked), [lv] "=q"(lock_val)
		     : "[lv]"(lock_val)
		     : "memory");
}

// Release spinlock (copied from DPDK rte_spinlock_unlock x86)
static inline void
furrymap_lock_release_read(furrymap_lock_t *lock) {
	int unlock_val = 0;
	asm volatile("xchg %[locked], %[ulv]\n"
		     : [locked] "=m"(lock->locked), [ulv] "=q"(unlock_val)
		     : "[ulv]"(unlock_val)
		     : "memory");
}

static inline void
furrymap_lock_acquire_write(furrymap_lock_t *lock) {
	furrymap_lock_acquire_read(lock);
}

static inline void
furrymap_lock_release_write(furrymap_lock_t *lock) {
	furrymap_lock_release_read(lock);
}

// Hash function type
typedef uint64_t (*furrymap_hash_fn_t)(
	const void *key, size_t key_size, uint32_t seed
);

// Key comparison function type
typedef bool (*furrymap_key_equal_fn_t)(
	const void *key1, const void *key2, size_t key_size
);

// Random number generator for hash seed randomization
// Used to prevent hash collision attacks and ensure different distributions
typedef uint64_t (*furrymap_rand_fn_t)(void);

// Map configuration - function pointers resolved at runtime from IDs for
// cross-process compatibility
typedef struct furrymap_config {
	struct memory_context *mem_ctx;
	size_t key_size;
	size_t value_size;
	uint32_t hash_seed;
	size_t worker_count;
	furrymap_func_id_t hash_fn_id;
	furrymap_func_id_t key_equal_fn_id;
	furrymap_func_id_t rand_fn_id;
	bool enable_locks;
} furrymap_config_t;

typedef uint8_t *furrymap_subgroup_t;

// Group structure - 64-byte header (1 cache line) + inline slots
typedef struct furrymap_group {
	uint64_t ctrls[4]; // Control bits for 8 inline slots per group
	furrymap_subgroup_t subgroup1;
	furrymap_subgroup_t subgroup2;
	furrymap_subgroup_t subgroup3;
	struct furrymap_group *next_group;
	uint8_t inline_slots[];
} furrymap_group_t;

typedef struct furrymap_ctx {
	struct memory_context *ctx;
	furrymap_group_t *free_group;
	furrymap_subgroup_t free_subgroup;
} furrymap_ctx_t;

typedef struct furrymap {
	furrymap_config_t config;
	size_t index_mask; // index_size - 1 for fast modulo operation
	size_t total_elements;
	uint32_t max_chain_length;
	uint32_t seed;
	furrymap_group_t **index_array;
	furrymap_lock_t
		*locks_ptr; // Per-bucket locks when enable_locks is true
	furrymap_lock_t main_ctx_lock;
	furrymap_ctx_t *local_ctx;
} furrymap_t;

typedef struct furrymap_stats {
	size_t total_elements;
	size_t index_size;
	size_t max_chain_length;
	size_t total_groups;
	size_t total_subgroups;
	size_t memory_used;
} furrymap_stats_t;

static inline void
furrymap_bucket_lock(furrymap_t *map, size_t bucket) {
	if (!map->config.enable_locks)
		return;
	furrymap_lock_t *locks = ADDR_OF(&map->locks_ptr);
	furrymap_lock_acquire_read(&locks[bucket]);
}

static inline void
furrymap_bucket_unlock(furrymap_t *map, size_t bucket) {
	if (!map->config.enable_locks)
		return;
	furrymap_lock_t *locks = ADDR_OF(&map->locks_ptr);
	furrymap_lock_release_read(&locks[bucket]);
}

// Unlocker structure returned by furrymap_get_safe
// Must be used to unlock the bucket when done with the retrieved value
typedef struct furrymap_unlocker {
	furrymap_t *map;
	size_t bucket;
	bool is_locked;
	void *value; // NULL if not found
} furrymap_unlocker_t;

static inline void
furrymap_unlock(furrymap_unlocker_t *unlocker) {
	if (unlocker && unlocker->is_locked) {
		furrymap_bucket_unlock(unlocker->map, unlocker->bucket);
		unlocker->is_locked = false;
	}
}

// Extract H1 (upper 57 bits) for bucket selection
static inline uint64_t
furrymap_h1(uint64_t hash) {
	return hash >> 7;
}

// Extract H2 (lower 7 bits) for control bytes
static inline uint8_t
furrymap_h2(uint64_t hash) {
	return (uint8_t)(hash & 0x7F);
}

static inline size_t
furrymap_hash_to_bucket(uint64_t hash, size_t index_mask) {
	return furrymap_h1(hash) & index_mask;
}

// Default FNV-1a hash function
static inline uint64_t
furrymap_hash_fnv1a(const void *key, size_t key_size, uint32_t seed) {
	const uint8_t *data = (const uint8_t *)key;
	uint64_t hash = 14695981039346656037ULL ^ (uint64_t)seed;

	for (size_t i = 0; i < key_size; i++) {
		hash ^= data[i];
		hash *= 1099511628211ULL;
	}

	return hash;
}

// ============================================================================
// Control Byte Operations - SIMD-style slot matching
// ============================================================================

static inline uint64_t
furrymap_ctrl_match_h2(uint64_t ctrl, uint8_t h2) {
	uint64_t xor_val = ctrl ^ (FURRYMAP_BITSET_LSB * h2);
	return ((xor_val - FURRYMAP_BITSET_LSB) & ~xor_val) &
	       FURRYMAP_BITSET_MSB;
}

static inline uint64_t
furrymap_ctrl_match_empty(uint64_t ctrl) {
	return (ctrl & ~(ctrl << 6)) & FURRYMAP_BITSET_MSB;
}

static inline uint64_t
furrymap_ctrl_match_empty_or_deleted(uint64_t ctrl) {
	return ctrl & FURRYMAP_BITSET_MSB;
}

static inline uint64_t
furrymap_ctrl_match_full(uint64_t ctrl) {
	return ~ctrl & FURRYMAP_BITSET_MSB;
}

static inline uint8_t
furrymap_ctrl_get(uint64_t ctrl, size_t i) {
	return (uint8_t)((ctrl >> (8 * i)) & 0xFF);
}

static inline void
furrymap_ctrl_set(uint64_t *ctrl, size_t i, uint8_t value) {
	uint64_t mask = 0xFFULL << (8 * i);
	*ctrl = (*ctrl & ~mask) | ((uint64_t)value << (8 * i));
}

static inline void
furrymap_ctrl_set_empty(uint64_t *ctrl) {
	*ctrl = FURRYMAP_BITSET_EMPTY;
}

// ============================================================================
// Bitset Operations - slot iteration
// ============================================================================

static inline size_t
furrymap_bitset_first(uint64_t b) {
	return __builtin_ctzll(b) >> 3;
}

static inline uint64_t
furrymap_bitset_remove_first(uint64_t b) {
	return b & (b - 1);
}

// ============================================================================
// Default Functions - random number generators for hash seeds
// ============================================================================

static uint64_t furrymap_rand_lcg_state = 1;

// Simple LCG for testing and general use
static inline uint64_t
furrymap_rand_default(void) {
	furrymap_rand_lcg_state = furrymap_rand_lcg_state * 1103515245 + 12345;
	return furrymap_rand_lcg_state;
}

// Secure random using system entropy
static inline uint64_t
furrymap_rand_secure(void) {
	uint32_t seed;
	ssize_t result = getrandom(&seed, sizeof(seed), 0);
	(void)result;
	return seed;
}

// Default key comparison using memcmp
static inline bool
furrymap_default_key_equal(const void *a, const void *b, size_t size) {
	return memcmp(a, b, size) == 0;
}

// ============================================================================
// Memory Management
// ============================================================================

static inline size_t
furrymap_subgroup_size(const furrymap_config_t *config) {
	size_t slot_size = config->key_size + config->value_size;
	size_t subgroup_size = (slot_size * FURRYMAP_GROUP_SLOTS);

	return subgroup_size;
}

static inline size_t
furrymap_group_size(const furrymap_config_t *config) {
	size_t payload_size = furrymap_subgroup_size(config);
	size_t group_size = sizeof(furrymap_group_t) + payload_size;

	return group_size;
}

static inline int
furrymap_expand_worker_context_internal(
	furrymap_t *map, struct memory_context *local_ctx
) {
	struct memory_context *ctx = ADDR_OF(&map->config.mem_ctx);
	size_t alloc_size = MEMORY_BLOCK_ALLOCATOR_MAX_SIZE;
	size_t group_size = furrymap_group_size(&map->config);

	void *arena_chunk = NULL;
	while (alloc_size > group_size) {
		// Allocate arena chunk from main context
		arena_chunk = memory_balloc(ctx, alloc_size);
		if (arena_chunk)
			break;
		alloc_size >>= 1;
	}
	if (!arena_chunk) {
		return -1;
	}

	// Add the new chunk to the existing worker-local block allocator
	struct block_allocator *ba = ADDR_OF(&local_ctx->block_allocator);
	block_allocator_put_arena(ba, arena_chunk, alloc_size);

	return 0;
}

// Expand worker context when it runs out of memory
static inline int
furrymap_expand_worker_context(
	furrymap_t *map, struct memory_context *worker_ctx
) {
	furrymap_lock_acquire_write(&map->main_ctx_lock);
	int ret = furrymap_expand_worker_context_internal(map, worker_ctx);
	furrymap_lock_release_write(&map->main_ctx_lock);
	return ret;
}

static inline int
furrymap_init_local_context(furrymap_t *map) {
	struct memory_context *main_ctx = ADDR_OF(&map->config.mem_ctx);

	size_t count = map->config.enable_locks ? map->config.worker_count : 1;

	size_t mem_ctx_size =
		map->config.enable_locks ? sizeof(struct memory_context) : 0;
	size_t ba_size =
		map->config.enable_locks ? sizeof(struct block_allocator) : 0;

	size_t alloc_size =
		(sizeof(furrymap_ctx_t) + mem_ctx_size + ba_size) * count;

	void *alloc = memory_balloc(main_ctx, alloc_size);
	if (!alloc) {
		return -1;
	}

	memset(alloc, 0, alloc_size);
	furrymap_ctx_t *local_ctx = alloc;
	struct memory_context *ctx_array =
		alloc + sizeof(furrymap_ctx_t) * count;
	struct block_allocator *ba_array =
		((void *)ctx_array) + sizeof(struct memory_context) * count;

	if (map->config.enable_locks) {
		size_t idx = 0;
		for (; idx < count; idx++) {
			struct memory_context *ctx = &ctx_array[idx];
			struct block_allocator *ba = &ba_array[idx];
			char buf[80];
			snprintf(buf, 80, "worker_ctx_%lu", idx);
			memory_context_init(ctx, buf, ba);

			furrymap_ctx_t *lctx = &local_ctx[idx];
			SET_OFFSET_OF(&lctx->ctx, ctx);

			if (furrymap_expand_worker_context_internal(map, ctx)) {
				break;
			};
		}
		if (idx != count) {
			// One of the furrymap_expand_worker_context_internal
			// calls failed
			for (size_t i = 0; i <= idx; i++) {
				// FIXME: free block allocators and arenas
			}
			return -1;
		}
	} else {
		// Properly recalculate the offset
		SET_OFFSET_OF(&local_ctx->ctx, main_ctx);
	}

	SET_OFFSET_OF(&map->local_ctx, local_ctx);

	return 0;
}

static inline furrymap_ctx_t *
furrymap_get_worker_context(furrymap_t *map, size_t worker_idx) {
	size_t idx = map->config.enable_locks ? worker_idx : 0;
	return &(ADDR_OF(&map->local_ctx)[idx]);
}

static inline void
furrymap_free_to_map(
	furrymap_t *map, void *ptr, size_t size, size_t worker_idx
) {
	if (!ptr)
		return;

	furrymap_ctx_t *fctx = furrymap_get_worker_context(map, worker_idx);
	struct memory_context *ctx = ADDR_OF(&fctx->ctx);
	memory_bfree(ctx, ptr, size);
}

static inline void
furrymap_destroy_local_context(furrymap_t *map) {
	if (!map->local_ctx)
		return;

	furrymap_ctx_t *local_ctx = ADDR_OF(&map->local_ctx);
	size_t count = map->config.enable_locks ? map->config.worker_count : 1;

	// Free stashed groups and subgroups from each worker context
	for (size_t i = 0; i < count; i++) {
		furrymap_ctx_t *fctx = &local_ctx[i];

		// Free stashed group if any
		if (fctx->free_group) {
			furrymap_group_t *group = ADDR_OF(&fctx->free_group);
			size_t group_size = furrymap_group_size(&map->config);
			furrymap_free_to_map(map, group, group_size, i);
		}

		// Free stashed subgroup if any
		if (fctx->free_subgroup) {
			furrymap_subgroup_t subgroup =
				ADDR_OF(&fctx->free_subgroup);
			size_t subgroup_size =
				furrymap_subgroup_size(&map->config);
			furrymap_free_to_map(map, subgroup, subgroup_size, i);
		}
	}

	// FIXME: if locking is enabled, we should deallocate all arenas that we
	// put into block_allocator inside the expand worker context function.
	// Free the local context allocation itself.
	struct memory_context *main_ctx = ADDR_OF(&map->config.mem_ctx);
	size_t mem_ctx_size =
		map->config.enable_locks ? sizeof(struct memory_context) : 0;
	size_t ba_size =
		map->config.enable_locks ? sizeof(struct block_allocator) : 0;
	size_t alloc_size =
		(sizeof(furrymap_ctx_t) + mem_ctx_size + ba_size) * count;

	memory_bfree(main_ctx, local_ctx, alloc_size);
	map->local_ctx = NULL;
}

// Allocate memory using map with worker-local support
static inline void *
furrymap_alloc_from_map(furrymap_t *map, size_t size, size_t worker_idx) {
	furrymap_ctx_t *fctx = furrymap_get_worker_context(map, worker_idx);
	struct memory_context *ctx = ADDR_OF(&fctx->ctx);
	void *ptr = memory_balloc(ctx, size);

	// Try to expand worker context if allocation failed
	if (!ptr && map->config.enable_locks) {
		if (!furrymap_expand_worker_context(map, ctx)) {
			ptr = memory_balloc(ctx, size);
		}
	}

	return ptr;
}

// ============================================================================
// Group Operations
// ============================================================================

static inline void *
furrymap_slot_key(
	void *group, const furrymap_config_t *config, size_t slot_idx
) {
	assert(slot_idx < FURRYMAP_GROUP_SLOTS);

	size_t slot_size = config->key_size + config->value_size;
	return (uint8_t *)group + (slot_size * slot_idx);
}

static inline void *
furrymap_slot_value(void *slot_key, const furrymap_config_t *config) {
	return (uint8_t *)slot_key + config->key_size;
}

// Allocate a new group with inline slots
static inline furrymap_group_t *
furrymap_allocate_group(furrymap_t *map, size_t worker_idx) {
	// Check worker-local storage first
	furrymap_ctx_t *fctx = furrymap_get_worker_context(map, worker_idx);
	furrymap_group_t *group = ADDR_OF(&fctx->free_group);
	if (group) {
		fctx->free_group = NULL;
	} else {
		size_t group_size = furrymap_group_size(&map->config);
		group = furrymap_alloc_from_map(map, group_size, worker_idx);
		if (!group)
			return NULL;
	}

	// Initialize control bytes and slots
	furrymap_ctrl_set_empty(&group->ctrls[0]);
	furrymap_ctrl_set_empty(&group->ctrls[1]);
	furrymap_ctrl_set_empty(&group->ctrls[2]);
	furrymap_ctrl_set_empty(&group->ctrls[3]);

	// memset(group->inline_slots, 0, furrymap_subgroup_size(&map->config));

	group->subgroup1 = NULL;
	group->subgroup2 = NULL;
	group->subgroup3 = NULL;
	group->next_group = NULL;

	return group;
}

// Allocate a new subgroup
static inline furrymap_subgroup_t
furrymap_allocate_subgroup(furrymap_t *map, size_t worker_idx) {
	furrymap_ctx_t *fctx = furrymap_get_worker_context(map, worker_idx);
	furrymap_subgroup_t subgroup = ADDR_OF(&fctx->free_subgroup);
	if (subgroup) {
		SET_OFFSET_OF(&fctx->free_subgroup, NULL);
	} else {
		size_t subgroup_size = furrymap_subgroup_size(&map->config);
		subgroup =
			furrymap_alloc_from_map(map, subgroup_size, worker_idx);
		if (!subgroup)
			return NULL;
	}
	// memset(subgroup, 0, furrymap_subgroup_size(&map->config));

	return subgroup;
}

static inline void
furrymap_free_subgroup(
	furrymap_t *map, furrymap_subgroup_t subgroup, size_t worker_idx
) {
	if (!subgroup)
		return;

	furrymap_ctx_t *fctx = furrymap_get_worker_context(map, worker_idx);
	if (!fctx->free_subgroup) {
		SET_OFFSET_OF(&fctx->free_subgroup, subgroup);
		return;
	}

	size_t subgroup_size = furrymap_subgroup_size(&map->config);

	furrymap_free_to_map(map, subgroup, subgroup_size, worker_idx);
}
// Free a group and its subgroups
static inline void
furrymap_free_group(
	furrymap_t *map, furrymap_group_t *group, size_t worker_idx
) {
	if (group->subgroup1)
		furrymap_free_subgroup(
			map, ADDR_OF(&group->subgroup1), worker_idx
		);
	if (group->subgroup2)
		furrymap_free_subgroup(
			map, ADDR_OF(&group->subgroup2), worker_idx
		);
	if (group->subgroup3)
		furrymap_free_subgroup(
			map, ADDR_OF(&group->subgroup3), worker_idx
		);

	furrymap_ctx_t *fctx = furrymap_get_worker_context(map, worker_idx);
	if (!fctx->free_group) {
		SET_OFFSET_OF(&fctx->free_group, group);
		return;
	}
	size_t group_size = furrymap_group_size(&map->config);
	furrymap_free_to_map(map, group, group_size, worker_idx);
}

// ============================================================================
// Utility Operations
// ============================================================================

static inline size_t
furrymap_size(const furrymap_t *map) {
	return map ? map->total_elements : 0;
}

static inline bool
furrymap_empty(const furrymap_t *map) {
	return !map || map->total_elements == 0;
}

static inline size_t
furrymap_max_chain_length(const furrymap_t *map) {
	return map ? map->max_chain_length : 0;
}

// Get detailed statistics about the map
static inline void
furrymap_get_stats(const furrymap_t *map, furrymap_stats_t *stats) {
	if (!stats)
		return;

	memset(stats, 0, sizeof(furrymap_stats_t));
	stats->total_elements =
		__atomic_load_n(&map->total_elements, __ATOMIC_RELAXED);
	stats->index_size = map->index_mask + 1;
	stats->max_chain_length = map->max_chain_length;

	// Count groups and subgroups
	furrymap_group_t **index_array = ADDR_OF(&map->index_array);
	for (size_t i = 0; i <= map->index_mask; i++) {
		furrymap_group_t *group = ADDR_OF(&index_array[i]);
		while (group) {
			stats->total_groups++;

			if (ADDR_OF(&group->subgroup1))
				stats->total_subgroups++;
			if (ADDR_OF(&group->subgroup2))
				stats->total_subgroups++;
			if (ADDR_OF(&group->subgroup3))
				stats->total_subgroups++;

			group = (furrymap_group_t *)ADDR_OF(&group->next_group);
		}
	}

	// Calculate memory usage
	size_t group_size = furrymap_group_size(&map->config);
	size_t subgroup_size = furrymap_subgroup_size(&map->config);

	stats->memory_used = sizeof(furrymap_t) +
			     (stats->index_size * sizeof(furrymap_group_t *)) +
			     (stats->total_groups * group_size) +
			     (stats->total_subgroups * subgroup_size);
}

// ============================================================================
// Core Map Operations
// ============================================================================

// Create a new FurryMap (index_size will be rounded to power of 2)
static inline furrymap_t *
furrymap_new(const furrymap_config_t *config, size_t index_size) {
	// Ensure index_size is a power of 2
	if (index_size < 16)
		index_size = 16;
	index_size = align_up_pow2(index_size);
	if (!index_size) {
		errno = EINVAL;
		return NULL;
	}

	// Get function pointers from the registry
	furrymap_rand_fn_t rand_fn =
		(furrymap_rand_fn_t)furrymap_func_registry[config->rand_fn_id];

	struct memory_context *ctx = ADDR_OF(&config->mem_ctx);
	furrymap_t *map = memory_balloc(ctx, sizeof(furrymap_t));
	if (!map) {
		errno = ENOMEM;
		return NULL;
	}
	// Initialize the map
	map->config = *config;
	// Recalculate the ctx offset
	SET_OFFSET_OF(&map->config.mem_ctx, ctx);
	map->index_mask = index_size - 1;
	map->total_elements = 0;
	map->max_chain_length = 0;
	// Generate hash seed using the random function if not provided
	map->seed = config->hash_seed ? config->hash_seed : (uint32_t)rand_fn();

	// Allocate the local memory contexts
	if (furrymap_init_local_context(map)) {
		memory_bfree(ctx, map, sizeof(furrymap_t));
		return NULL;
	}

	size_t index_array_size = sizeof(furrymap_group_t *) * index_size;
	furrymap_group_t **index_array = memory_balloc(ctx, index_array_size);
	if (!index_array) {
		furrymap_destroy_local_context(map);
		memory_bfree(ctx, map, sizeof(furrymap_t));
		errno = ENOMEM;
		return NULL;
	}

	// Initialize all buckets to NULL
	memset(index_array, 0, index_array_size);
	SET_OFFSET_OF(&map->index_array, index_array);

	// Initialize the main context lock
	map->main_ctx_lock.locked = 0;
	if (config->enable_locks) {
		furrymap_lock_t *locks = memory_balloc(
			ctx, index_size * sizeof(furrymap_lock_t)
		);
		if (!locks) {
			memory_bfree(ctx, index_array, index_array_size);
			furrymap_destroy_local_context(map);
			memory_bfree(ctx, map, sizeof(furrymap_t));
			errno = ENOMEM;
			return NULL;
		}
		memset(locks, 0, index_size * sizeof(furrymap_lock_t));
		SET_OFFSET_OF(&map->locks_ptr, locks);
	} else {
		SET_OFFSET_OF(&map->locks_ptr, NULL);
	}

	return map;
}

// Free a FurryMap and all its resources
static inline void
furrymap_destroy(furrymap_t *map) {
	struct memory_context *ctx = ADDR_OF(&map->config.mem_ctx);
	furrymap_group_t **index_array = ADDR_OF(&map->index_array);

	if (!map->config.enable_locks) {
		// Free all groups and their chains when local contexts were not
		// used
		for (size_t i = 0; i <= map->index_mask; i++) {
			furrymap_group_t *group = ADDR_OF(&index_array[i]);
			while (group) {
				furrymap_group_t *next = NULL;
				if (group->next_group) {
					next = ADDR_OF(&group->next_group);
				}
				furrymap_free_group(
					map,
					group,
					0
				); // Free to worker 0
				group = next;
			}
		}
	} // else: If locking was used, we can simply destroy local contexts and
	  // return all arenas to the main context, because all allocations
	  // to the map were completed via local contexts.

	// Free the index array
	memory_bfree(
		ctx,
		index_array,
		(map->index_mask + 1) * sizeof(furrymap_group_t *)
	);

	// Free the locks if present
	furrymap_lock_t *locks = ADDR_OF(&map->locks_ptr);
	if (locks) {
		memory_bfree(
			ctx,
			locks,
			(map->index_mask + 1) * sizeof(furrymap_lock_t)
		);
	}

	furrymap_destroy_local_context(map);
	memory_bfree(ctx, map, sizeof(furrymap_t));
}

// Get value by key from map (without locking)
static inline bool
furrymap_get(furrymap_t *map, const void *key, void **value) {
	furrymap_hash_fn_t hash_fn = (furrymap_hash_fn_t
	)furrymap_func_registry[map->config.hash_fn_id];
	furrymap_key_equal_fn_t key_equal_fn = (furrymap_key_equal_fn_t
	)furrymap_func_registry[map->config.key_equal_fn_id];

	uint64_t hash = hash_fn(key, map->config.key_size, map->seed);
	uint64_t h1 = furrymap_h1(hash);
	uint8_t h2 = furrymap_h2(hash);
	size_t bucket = h1 & map->index_mask;

	furrymap_group_t **index_array = ADDR_OF(&map->index_array);
	furrymap_group_t *group = ADDR_OF(&index_array[bucket]);

	while (group) {
		uint8_t *refs[4] = {
			(uint8_t *)group->inline_slots,
			(uint8_t *)ADDR_OF(&group->subgroup1),
			(uint8_t *)ADDR_OF(&group->subgroup2),
			(uint8_t *)ADDR_OF(&group->subgroup3),
		};
		for (int i = 0; i < 4; i++) {
			if (!refs[i]) {
				// If we encounter an empty subgroup, there is
				// no value
				return false;
			}
			uint64_t matches =
				furrymap_ctrl_match_h2(group->ctrls[i], h2);
			uint8_t *group_data = refs[i];
			while (matches) {
				size_t slot_idx =
					furrymap_bitset_first(matches);

				void *slot_key = furrymap_slot_key(
					group_data, &map->config, slot_idx
				);

				if (key_equal_fn(
					    key, slot_key, map->config.key_size
				    )) {
					*value = furrymap_slot_value(
						slot_key, &map->config
					);
					return true;
				}
				matches = furrymap_bitset_remove_first(matches);
			}
		}

		group = ADDR_OF(&group->next_group);
	}

	return false;
}

static inline bool
furrymap_put(
	furrymap_t *map, const void *key, const void *value, size_t worker_idx
) {
	furrymap_hash_fn_t hash_fn = (furrymap_hash_fn_t
	)furrymap_func_registry[map->config.hash_fn_id];
	furrymap_key_equal_fn_t key_equal_fn = (furrymap_key_equal_fn_t
	)furrymap_func_registry[map->config.key_equal_fn_id];

	uint64_t hash = hash_fn(key, map->config.key_size, map->seed);
	uint64_t h1 = furrymap_h1(hash);
	uint8_t h2 = furrymap_h2(hash);
	size_t bucket = h1 & map->index_mask;

	furrymap_group_t **index_array = ADDR_OF(&map->index_array);
	furrymap_group_t **chain_link = &index_array[bucket];
	furrymap_group_t *group = ADDR_OF(chain_link);

	size_t chain_length = 0;
process_group:
	while (group) {
		chain_length++;

		uint8_t *refs[4] = {
			(uint8_t *)group->inline_slots,
			(uint8_t *)ADDR_OF(&group->subgroup1),
			(uint8_t *)ADDR_OF(&group->subgroup2),
			(uint8_t *)ADDR_OF(&group->subgroup3),
		};
		furrymap_subgroup_t *subgroup_refs[3] = {
			&group->subgroup1, &group->subgroup2, &group->subgroup3
		};

		for (int i = 0; i < 4; i++) {
			if (!refs[i]) {
				assert(i > 0);

				furrymap_subgroup_t *target =
					subgroup_refs[i - 1];
				furrymap_subgroup_t new_subgroup =
					furrymap_allocate_subgroup(
						map, worker_idx
					);
				if (!new_subgroup) {
					return false;
				}
				SET_OFFSET_OF(target, new_subgroup);
				refs[i] = ADDR_OF(target);
			}
			uint8_t *group_data = refs[i];

			uint64_t matches = furrymap_ctrl_match_empty_or_deleted(
				group->ctrls[i]
			);
			matches |= furrymap_ctrl_match_h2(group->ctrls[i], h2);

			while (matches) {
				size_t slot_idx =
					furrymap_bitset_first(matches);

				void *slot_key = furrymap_slot_key(
					group_data, &map->config, slot_idx
				);

				uint8_t ctrl = furrymap_ctrl_get(
					group->ctrls[i], slot_idx
				);
				uint8_t vacant = ctrl & FURRYMAP_CTRL_EMPTY;

				if (vacant ||
				    key_equal_fn(
					    key, slot_key, map->config.key_size
				    )) {
					void *slot_value = furrymap_slot_value(
						slot_key, &map->config
					);
					memcpy(slot_key,
					       key,
					       map->config.key_size);
					memcpy(slot_value,
					       value,
					       map->config.value_size);

					if (vacant) {
						furrymap_ctrl_set(
							&group->ctrls[i],
							slot_idx,
							h2
						);
						map->total_elements++;
					}
					if (chain_length >
					    map->max_chain_length) {
						map->max_chain_length =
							chain_length;
					}
					return true;
				}
				matches = furrymap_bitset_remove_first(matches);
			}
		}

		chain_link = &group->next_group;
		group = ADDR_OF(chain_link);
	}

	furrymap_group_t *new_group = furrymap_allocate_group(map, worker_idx);
	if (!new_group) {
		return false;
	}

	SET_OFFSET_OF(chain_link, new_group);
	group = new_group;

	goto process_group;
}

// Delete key from map
static inline bool
furrymap_delete(furrymap_t *map, const void *key) {
	furrymap_hash_fn_t hash_fn = (furrymap_hash_fn_t
	)furrymap_func_registry[map->config.hash_fn_id];
	furrymap_key_equal_fn_t key_equal_fn = (furrymap_key_equal_fn_t
	)furrymap_func_registry[map->config.key_equal_fn_id];

	uint64_t hash = hash_fn(key, map->config.key_size, map->seed);
	uint64_t h1 = furrymap_h1(hash);
	uint8_t h2 = furrymap_h2(hash);
	size_t bucket = h1 & map->index_mask;

	furrymap_group_t **index_array = ADDR_OF(&map->index_array);
	furrymap_group_t *group = ADDR_OF(&index_array[bucket]);

	while (group) {
		uint8_t *refs[4] = {
			(uint8_t *)group->inline_slots,
			(uint8_t *)ADDR_OF(&group->subgroup1),
			(uint8_t *)ADDR_OF(&group->subgroup2),
			(uint8_t *)ADDR_OF(&group->subgroup3),
		};
		for (int i = 0; i < 4; i++) {
			if (!refs[i]) {
				// If we encounter an empty subgroup, there is
				// no value
				return false;
			}
			uint64_t matches =
				furrymap_ctrl_match_h2(group->ctrls[i], h2);
			uint8_t *group_data = refs[i];
			while (matches) {
				size_t slot_idx =
					furrymap_bitset_first(matches);

				void *slot_key = furrymap_slot_key(
					group_data, &map->config, slot_idx
				);

				if (key_equal_fn(
					    key, slot_key, map->config.key_size
				    )) {
					bool has_empty =
						furrymap_ctrl_match_empty(
							group->ctrls[i]
						) != 0;
					uint8_t marker =
						has_empty
							? FURRYMAP_CTRL_EMPTY
							: FURRYMAP_CTRL_DELETED;
					furrymap_ctrl_set(
						&group->ctrls[i],
						slot_idx,
						marker
					);
					map->total_elements--;
					return true;
				}
				matches = furrymap_bitset_remove_first(matches);
			}
		}

		group = ADDR_OF(&group->next_group);
	}
	return false;
}

// Get value by key from map (with locking, returns unlocker)
// Check .value member to see if value was found
// The returned unlocker must be used to unlock when done with the value
static inline furrymap_unlocker_t
furrymap_get_safe(furrymap_t *map, const void *key) {
	furrymap_unlocker_t unlocker = {0};

	if (!map) {
		unlocker.value = NULL;
		unlocker.is_locked = false;
		return unlocker;
	}

	furrymap_hash_fn_t hash_fn = (furrymap_hash_fn_t
	)furrymap_func_registry[map->config.hash_fn_id];

	uint64_t hash = hash_fn(key, map->config.key_size, map->seed);
	uint64_t h1 = furrymap_h1(hash);
	size_t bucket = h1 & map->index_mask;

	// Lock the bucket
	furrymap_bucket_lock(map, bucket);

	// Initialize the unlocker
	unlocker.map = map;
	unlocker.bucket = bucket;
	unlocker.is_locked = true;

	// Call the unlocked version
	void *found_value = NULL;
	bool result = furrymap_get(map, key, &found_value);
	unlocker.value = result ? found_value : NULL;

	// If not found, unlock immediately
	if (!result) {
		furrymap_bucket_unlock(map, bucket);
		unlocker.is_locked = false;
	}

	return unlocker;
}

// Insert or update key-value pair in map (with locking)
static inline bool
furrymap_put_safe(
	furrymap_t *map, const void *key, const void *value, size_t worker_idx
) {
	if (!map)
		return false;

	furrymap_hash_fn_t hash_fn = (furrymap_hash_fn_t
	)furrymap_func_registry[map->config.hash_fn_id];

	uint64_t hash = hash_fn(key, map->config.key_size, map->seed);
	uint64_t h1 = furrymap_h1(hash);
	size_t bucket = h1 & map->index_mask;

	furrymap_bucket_lock(map, bucket);
	bool result = furrymap_put(map, key, value, worker_idx);
	furrymap_bucket_unlock(map, bucket);

	return result;
}

// Delete key from map (with locking)
static inline bool
furrymap_delete_safe(furrymap_t *map, const void *key) {
	if (!map)
		return false;

	furrymap_hash_fn_t hash_fn = (furrymap_hash_fn_t
	)furrymap_func_registry[map->config.hash_fn_id];

	uint64_t hash = hash_fn(key, map->config.key_size, map->seed);
	uint64_t h1 = furrymap_h1(hash);
	size_t bucket = h1 & map->index_mask;

	furrymap_bucket_lock(map, bucket);
	bool result = furrymap_delete(map, key);
	furrymap_bucket_unlock(map, bucket);

	return result;
}

// Global function registry - statically initialized
static void *furrymap_func_registry[FURRYMAP_FUNC_COUNT] = {
	[FURRYMAP_HASH_FNV1A] = (void *)furrymap_hash_fnv1a,
	[FURRYMAP_KEY_EQUAL_DEFAULT] = (void *)furrymap_default_key_equal,
	[FURRYMAP_RAND_DEFAULT] = (void *)furrymap_rand_default,
	[FURRYMAP_RAND_SECURE] = (void *)furrymap_rand_secure
};

#endif // FURRYMAP_H
