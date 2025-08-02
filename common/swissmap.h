/**
 * @file swissmap.h
 * @brief Header-only Swiss Table hash map implementation in C
 *
 * This is a C implementation of the Swiss Table hash map data structure, based
 * on Google's Abseil Swiss Tables and Go's runtime Swiss map implementation.
 * Swiss Tables use SIMD-friendly parallel slot matching within groups to
 * achieve high performance.
 *
 * ## Architecture Overview
 *
 * The Swiss Table uses a hierarchical structure for efficient hash table
 * operations:
 *
 * - **Map**: Top-level structure that may contain multiple tables using
 * extendible hashing
 * - **Directory**: Array of table pointers for dynamic growth (extendible
 * hashing)
 * - **Table**: Individual hash table with groups of slots
 * - **Group**: Collection of 8 slots plus a 64-bit control word
 * - **Slot**: Storage location for a single key/value pair
 * - **Control Word**: 8 bytes indicating slot state (empty/deleted/full) and
 * hash bits
 *
 * ## Incremental Growth
 *
 * The implementation uses extendible hashing for incremental growth:
 * 1. Maps start with a directory-based structure
 * 2. Directory doubles in size when needed (power of 2)
 * 3. Individual tables can be split when full (localized rehashing)
 * 4. Uses global/local depth to control hash bit usage
 *
 * ## Hash Function
 *
 * Uses a two-part hash for efficient operations:
 * - **H1**: Upper 57 bits used for directory/group selection
 * - **H2**: Lower 7 bits stored in control bytes for parallel matching
 *
 * ## Control Bytes
 *
 * Each slot has a control byte with these states:
 * - 0x80 (0b10000000): Empty slot
 * - 0xFE (0b11111110): Deleted slot (tombstone)
 * - 0x00-0x7F (0b0hhhhhhh): Full slot containing H2 hash bits
 *
 * ```
 * Swiss Map Structure:
 *
 *   swiss_map_t
 *   ├── used,seed,config,dir_len,global_depth
 *   └── dir_ptr ──► Directory [table_ptr[0], table_ptr[1], ...]
 *                                │
 *                                ▼ (H1 >> global_shift)
 *   swiss_table_t (from table_ptr[i])
 *   ├── used,capacity,growth_left,local_depth,index
 *   └── groups ──► Groups Array [group0, group1, ...]
 *                                │
 *                                ▼ (H1 & length_mask)
 *   swiss_group_ref_t (from groups[i])
 *   ├── Control: [ctrl0, ctrl1, ..., ctrl7] (8 bytes)
 *   │             └─ H2 hash bits for matching
 *   └── Slots:   [key0|val0, key1|val1, ..., key7|val7]
 *
 * Hash Function Flow:
 *
 *   Key ──► hash_fn() ──► 64-bit Hash
 *                            │
 *                            ├─► H1 (upper 57) ──► Directory Index
 *                            │   (hash >> global_shift)
 *                            │                           │
 *                            │                           ▼
 *                            │                    table_ptr[i] ──► Table
 *                            │                           │
 *                            │                           ▼ (H1 & table.mask)
 *                            │                    groups[i] ──► Group
 *                            │                           │
 *                            └─► H2 (lower 7) ──► Control Byte Match (SIMD)
 *                                (hash & 0x7F)
 * ```
 *
 *
 * ## Example Usage
 *
 * ```c
 * #include "swissmap.h"
 *
 * // Configure the map
 * swiss_map_config_t config = {0};
 * config.key_size = sizeof(int);
 * config.value_size = sizeof(int);
 * config.hash_fn = swiss_hash_fnv1a;
 * config.key_equal_fn = swiss_default_key_equal;
 * config.alloc_fn = swiss_default_alloc;
 * config.free_fn = swiss_default_free;
 * config.rand_fn = swiss_rand_default; // Used for hash seed randomization
 *
 * // Create and use map
 * swiss_map_t *map = swiss_map_new(&config, 0);
 * int key = 42, value = 100;
 * if (swiss_map_put(map, &key, &value) != 0) {
 *    // handle error
 * };
 *
 * int *found_value;
 * if (swiss_map_get(map, &key, (void**)&found_value)) {
 *     printf("Found value: %d\n", *found_value);
 * }
 *
 * swiss_map_free(map);
 * ```
 *
 * ## Type-Safe Interface
 *
 * ```c
 * // Declare a type-safe map for int->int
 * SWISS_MAP_DECLARE(IntMap, int, int);
 *
 * IntMap_t *map = IntMap_new(0);
 * int key = 42, value = 100;
 * IntMap_put(map, &key, &value);
 * IntMap_free(map);
 * ```
 */

// Future enhancements:
// - Add Read-Copy-Update (RCU) synchronization for concurrent access
// - Consider storing hash values with keys to avoid recomputation

#ifndef SWISSMAP_H
#define SWISSMAP_H

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

#include "memory.h"
#include "memory_address.h"

// Swiss table constants
#define SWISS_GROUP_SLOTS 8 /**< Number of slots per group */
#define MAX_AVG_GROUP_LOAD                                                     \
	7 /**< Maximum average load per group before growth */
#define MAX_TABLE_CAPACITY 1024 /**< Maximum capacity per individual table */

// Control byte values
#define CTRL_EMPTY 0x80	  /**< Empty slot control byte (10000000) */
#define CTRL_DELETED 0xFE /**< Deleted slot control byte (11111110) */

// Bitset constants for parallel operations
#define BITSET_LSB 0x0101010101010101ULL /**< Least significant bit pattern */
#define BITSET_MSB 0x8080808080808080ULL /**< Most significant bit pattern */
#define BITSET_EMPTY (BITSET_LSB * CTRL_EMPTY) /**< Empty bitset pattern */

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

// Function registry for cross-process compatibility
// NOLINTBEGIN(readability-identifier-naming)
typedef enum {
	SWISS_HASH_FNV1A,
	SWISS_KEY_EQUAL_DEFAULT,
	SWISS_ALLOC_SHARED,
	SWISS_FREE_SHARED,
	SWISS_RAND_DEFAULT,
	SWISS_RAND_SECURE,
	SWISS_FUNC_COUNT
} swiss_func_id_t;
// NOLINTEND(readability-identifier-naming)

// Global function registry (declared here, defined at bottom)
static void *swiss_func_registry[SWISS_FUNC_COUNT];

/**
 * @brief Hash function type
 * @param key Pointer to the key data
 * @param key_size Size of the key in bytes
 * @param seed Hash seed for randomization
 * @return 64-bit hash value
 */
typedef uint64_t (*swiss_hash_fn_t)(
	const void *key, size_t key_size, uint64_t seed
);

/**
 * @brief Key comparison function type
 * @param key1 Pointer to first key
 * @param key2 Pointer to second key
 * @param key_size Size of keys in bytes
 * @return true if keys are equal, false otherwise
 */
typedef bool (*swiss_key_equal_fn_t)(
	const void *key1, const void *key2, size_t key_size
);

/**
 * @brief Memory allocation function type with context
 * @param ctx Memory context pointer
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory or NULL on failure
 */
typedef void *(*swiss_alloc_fn_t)(void *ctx, size_t size);

/**
 * @brief Memory deallocation function type with context
 * @param ctx Memory context pointer
 * @param ptr Pointer to memory to free
 * @param size Size of memory block
 */
typedef void (*swiss_free_fn_t)(void *ctx, void *ptr, size_t size);

/**
 * @brief Random number generator function type
 * @return Random 64-bit value
 *
 * This function is used for hash seed randomization during map creation
 * and when the map becomes empty. It provides the initial seed value that
 * helps prevent hash collision attacks and ensures different map instances
 * have different hash distributions.
 */
typedef uint64_t (*swiss_rand_fn_t)(void);

/**
 * @brief Map configuration structure
 *
 * Contains all the type information and function IDs needed to
 * configure a Swiss map for specific key/value types.
 * Function pointers are resolved at runtime from IDs for cross-process
 * compatibility.
 */
typedef struct {
	void *mem_ctx;	   /**< Memory context for allocations */
	size_t key_size;   /**< Size of keys in bytes */
	size_t value_size; /**< Size of values in bytes */
	// Function IDs for cross-process compatibility
	swiss_func_id_t hash_fn_id;	 /**< Hash function ID */
	swiss_func_id_t key_equal_fn_id; /**< Key comparison function ID */
	swiss_func_id_t alloc_fn_id; /**< Memory allocator with context ID */
	swiss_func_id_t free_fn_id;  /**< Memory deallocator with context ID */
	swiss_func_id_t rand_fn_id;  /**< Random number generator for hash seed
					randomization ID */
} swiss_map_config_t;

/**
 * @brief Control group - 8 control bytes packed in uint64_t
 *
 * Each control byte indicates the state of a slot and stores the lower
 * 7 bits of the hash (H2) for parallel matching.
 */
typedef uint64_t swiss_ctrl_group_t;

/**
 * @brief Bitset for parallel slot matching
 *
 * Used to efficiently find matching slots within a group using
 * bit manipulation operations.
 */
typedef uint64_t swiss_bitset_t;

/**
 * @brief Group reference - points to a group of 8 slots
 *
 * References a single group containing control bytes followed by
 * key/value slot data.
 */
typedef struct {
	void *data; /**< Points to group data (ctrl + slots) */
} swiss_group_ref_t;

/**
 * @brief Groups reference - array of groups
 *
 * References an array of groups with efficient indexing using
 * a length mask (length must be power of 2).
 */
typedef struct {
	// TODO: Performance: Consider storing groups in a Structure of
	// Arrays (SoA) format for potential optimization. Currently, they are
	// stored in an Array of Structures (AoS).
	void *data;	      /**< Points to array of groups */
	uint64_t length_mask; /**< length - 1 (length must be power of 2) */
} swiss_groups_ref_t;

/**
 * @brief Probe sequence
 *
 * Manages the probing sequence used to find slots
 * when the primary hash location is occupied.
 */
typedef struct {
	uint64_t mask;	 /**< Mask for wrapping around group array */
	uint64_t offset; /**< Current probe offset */
	uint64_t index;	 /**< Current probe index */
} swiss_probe_seq_t;

/**
 * @brief Single hash table
 *
 * Individual table within the extendible hash structure. Contains
 * groups of slots and metadata for growth management.
 */
typedef struct swiss_table {
	uint16_t used;		   /**< Number of filled slots */
	uint16_t capacity;	   /**< Total number of slots */
	uint16_t growth_left;	   /**< Slots available before rehash */
	uint8_t local_depth;	   /**< Bits used by directory lookups */
	int index;		   /**< Index in directory (-1 if stale) */
	swiss_groups_ref_t groups; /**< Array of slot groups */
} swiss_table_t;

/**
 * @brief Main map structure
 *
 * Top-level hash map structure that manages the directory of tables
 * and implements extendible hashing for incremental growth.
 */
typedef struct swiss_map {
	uint64_t used;		   /**< Total elements across all tables */
	uint64_t seed;		   /**< Hash seed */
	void *dir_ptr;		   /**< Directory pointer */
	int dir_len;		   /**< Directory length */
	uint8_t global_depth;	   /**< Bits for table selection */
	uint8_t global_shift;	   /**< Shift amount for directory lookup */
	swiss_map_config_t config; /**< Type and function configuration */
} swiss_map_t;

// Hash extraction functions

/**
 * @brief Extract H1 (upper bits) from hash value
 * @param hash Full 64-bit hash value
 * @return Upper 57 bits used for directory/group selection
 */
static inline uint64_t
swiss_h1(uint64_t hash) {
	return hash >> 7;
}

/**
 * @brief Extract H2 (lower bits) from hash value
 * @param hash Full 64-bit hash value
 * @return Lower 7 bits stored in control bytes
 */
static inline uint8_t
swiss_h2(uint64_t hash) {
	return (uint8_t)(hash & 0x7F);
}

// Bitset operations

/**
 * @brief Find the first set bit in a bitset
 * @param b Bitset value
 * @return Index of first set bit (0-7)
 */
static inline size_t
swiss_bitset_first(swiss_bitset_t b) {
	return __builtin_ctzll(b) >> 3;
}

/**
 * @brief Remove the first set bit from a bitset
 * @param b Bitset value
 * @return Bitset with first set bit cleared
 */
static inline swiss_bitset_t
swiss_bitset_remove_first(swiss_bitset_t b) {
	return b & (b - 1);
}

/**
 * @brief Remove all bits below a given index
 * @param b Bitset value
 * @param i Index threshold
 * @return Bitset with bits below index i cleared
 */
static inline swiss_bitset_t
swiss_bitset_remove_below(swiss_bitset_t b, size_t i) {
	uint64_t mask = (1ULL << (8 * i)) - 1;
	return b & ~mask;
}

// Control group operations

/**
 * @brief Get control byte at specific index
 * @param ctrl Control group
 * @param i Index (0-7)
 * @return Control byte value
 */
static inline uint8_t
swiss_ctrl_get(swiss_ctrl_group_t ctrl, size_t i) {
	return (uint8_t)((ctrl >> (8 * i)) & 0xFF);
}

/**
 * @brief Set control byte at specific index
 * @param ctrl Pointer to control group
 * @param i Index (0-7)
 * @param value Control byte value to set
 */
static inline void
swiss_ctrl_set(swiss_ctrl_group_t *ctrl, size_t i, uint8_t value) {
	uint64_t mask = 0xFFULL << (8 * i);
	*ctrl = (*ctrl & ~mask) | ((uint64_t)value << (8 * i));
}

/**
 * @brief Set all control bytes to empty state
 * @param ctrl Pointer to control group
 */
static inline void
swiss_ctrl_set_empty(swiss_ctrl_group_t *ctrl) {
	*ctrl = BITSET_EMPTY;
}

// Control group matching functions

/**
 * @brief Find slots matching a specific H2 hash value
 * @param ctrl Control group value
 * @param h2 H2 hash value to match (7 bits)
 * @return Bitset with matching slots set
 */
static inline swiss_bitset_t
swiss_ctrl_match_h2(swiss_ctrl_group_t ctrl, uint8_t h2) {
	// https://graphics.stanford.edu/~seander/bithacks.html##ValueInWord
	uint64_t v = ctrl ^ (BITSET_LSB * h2);
	return ((v - BITSET_LSB) & ~v) & BITSET_MSB;
}

/**
 * @brief Find empty slots in control group
 *
 * This function finds which slots in a group are empty and available for use.
 * It returns a bitset where each set bit represents an empty slot at that
 * position.
 *
 * Each slot has a control byte that can be in one of three states:
 *   Empty:   0b10000000 (0x80) - slot is available for insertion
 *   Deleted: 0b11111110 (0xFE) - slot was used but now available
 *   Full:    0b0hhhhhhh - slot contains data with 7 hash bits
 *
 * The trick to finding empty slots is noticing they have a unique pattern:
 * bit 7 is set (1) and bit 1 is unset (0). This combination doesn't happen
 * in deleted or full slots.
 *
 * Here's how the bit manipulation works:
 *
 * For an empty slot (0b10000000):
 *   v << 6       -> 0b00000000  (bit 1 was 0, so bit 7 becomes 0)
 *   ~(v << 6)    -> 0b11111111
 *   v & result   -> 0b10000000  (bit 7 preserved)
 *   & BITSET_MSB -> 0b10000000  (empty slot detected)
 *
 * For a deleted slot (0b11111110):
 *   v << 6       -> 0b10000000  (bit 1 was 1, so bit 7 becomes 1)
 *   ~(v << 6)    -> 0b01111111
 *   v & result   -> 0b01111110  (bit 7 cleared)
 *   & BITSET_MSB -> 0b00000000  (not empty)
 *
 * The expression (v & ~(v << 6)) & BITSET_MSB essentially asks:
 * "Is bit 7 set AND was bit 1 originally unset?" - which is only true for empty
 * slots.
 *
 * @param ctrl Control group value (8 control bytes packed in uint64_t)
 * @return Bitset with bits set at positions corresponding to empty slots
 */
static inline swiss_bitset_t
swiss_ctrl_match_empty(swiss_ctrl_group_t ctrl) {
	uint64_t v = ctrl;
	// A slot is empty if and only if bit 7 is set and bit 1 is not.
	// We could select any of the other bits from 1 to 6 here
	// (e.g. v << 1 would also work).
	return (v & ~(v << 6)) & BITSET_MSB;
}

/**
 * @brief Find empty or deleted slots in control group
 * @param ctrl Control group value
 * @return Bitset with empty or deleted slots set
 */
static inline swiss_bitset_t
swiss_ctrl_match_empty_or_deleted(swiss_ctrl_group_t ctrl) {
	return ctrl & BITSET_MSB;
}

/**
 * @brief Find full slots in control group
 * @param ctrl Control group value
 * @return Bitset with full slots set
 */
static inline swiss_bitset_t
swiss_ctrl_match_full(swiss_ctrl_group_t ctrl) {
	return ~ctrl & BITSET_MSB;
}

// Probe sequence functions

/**
 * @brief Create initial probe sequence for hash lookup
 * @param hash Hash value
 * @param mask Group array mask (length - 1)
 * @return Initial probe sequence
 */
static inline swiss_probe_seq_t
swiss_make_probe_seq(uint64_t hash, uint64_t mask) {
	swiss_probe_seq_t seq;
	seq.mask = mask;
	seq.offset = swiss_h1(hash) & mask;
	seq.index = 0;
	return seq;
}

/**
 * @brief Advance probe sequence to next position
 * @param seq Current probe sequence
 * @return Next probe sequence
 */
static inline swiss_probe_seq_t
swiss_probe_seq_next(swiss_probe_seq_t seq) {
	seq.index++;
	seq.offset = (seq.offset + seq.index) & seq.mask;
	return seq;
}

// Group operations

/**
 * @brief Get pointer to control group from group reference
 * @param group Group reference
 * @return Pointer to control group
 */
static inline swiss_ctrl_group_t *
swiss_group_ctrls(swiss_group_ref_t group) {
	return (swiss_ctrl_group_t *)group.data;
}

/**
 * @brief Get pointer to key at specific slot in group
 * @param group Group reference
 * @param config Map configuration for size calculations
 * @param i Slot index (0-7)
 * @return Pointer to key data
 */
static inline void *
swiss_group_key(
	swiss_group_ref_t group, const swiss_map_config_t *config, size_t i
) {
	size_t slot_size = config->key_size + config->value_size;
	size_t offset = sizeof(swiss_ctrl_group_t) + i * slot_size;
	return (char *)group.data + offset;
}

/**
 * @brief Get pointer to value at specific slot in group
 * @param group Group reference
 * @param config Map configuration for size calculations
 * @param i Slot index (0-7)
 * @return Pointer to value data
 */
static inline void *
swiss_group_value(
	swiss_group_ref_t group, const swiss_map_config_t *config, size_t i
) {
	size_t slot_size = config->key_size + config->value_size;
	size_t offset =
		sizeof(swiss_ctrl_group_t) + i * slot_size + config->key_size;
	return (char *)group.data + offset;
}

// Utility functions

/**
 * @brief Align number up to next power of 2
 * @param n Input number
 * @param overflow Pointer to overflow flag
 * @return Next power of 2, or 0 if overflow
 */
static inline uint64_t
swiss_align_up_pow2(uint64_t n, bool *overflow) {
	if (n == 0) {
		*overflow = false;
		return 0;
	}

	// Find the position of the highest set bit
	int pos = 63 - __builtin_clzll(n - 1);
	if (pos >= 63) {
		*overflow = true;
		return 0;
	}

	uint64_t result = 1ULL << (pos + 1);
	*overflow = (result == 0);
	return result;
}

/**
 * @brief Align size up to alignment boundary
 * @param n Size to align
 * @param align Alignment requirement (must be power of 2)
 * @return Aligned size
 */
static inline size_t
swiss_align_up(size_t n, size_t align) {
	return (n + align - 1) & ~(align - 1);
}

/**
 * @brief Default pseudo random number generator using LCG for hash seeds
 *
 * This implements a Linear Congruential Generator (LCG) using the constants
 * from POSIX rand(). The algorithm is: state = (state * 1103515245 + 12345) %
 * 2^31 https://en.wikipedia.org/wiki/Linear_congruential_generator
 *
 * This is suitable for testing and general use where predictable randomization
 * is acceptable. For production use with sensitive data, consider using
 * swiss_rand_secure which uses system entropy.
 *
 * @return Random 64-bit value
 */
static uint64_t swiss_rand_lcg_state = 1;

static inline uint64_t
swiss_rand_default(void) {
	swiss_rand_lcg_state = swiss_rand_lcg_state * 1103515245 + 12345;
	return swiss_rand_lcg_state;
}

/**
 * @brief Secure random number generator using system entropy for hash seeds
 *
 * This uses getrandom() to obtain cryptographically secure random bytes
 * from the system entropy pool. This is suitable for production use where
 * unpredictable hash seeds are important.
 *
 * @return Random 64-bit value
 */
static inline uint64_t
swiss_rand_secure(void) {
	uint64_t seed;
	ssize_t result = getrandom(&seed, sizeof(seed), 0);
	(void)result; // Suppress unused result warning
	return seed;
}

// Default functions

/**
 * @brief Default key comparison function using memcmp
 * @param a Pointer to first key
 * @param b Pointer to second key
 * @param size Size of keys in bytes
 * @return true if keys are equal, false otherwise
 */
static inline bool
swiss_default_key_equal(const void *a, const void *b, size_t size) {
	return memcmp(a, b, size) == 0;
}

/**
 * @brief Shared memory allocation function using memory context
 * @param ctx Memory context pointer
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory or NULL on failure
 */
static inline void *
swiss_shared_alloc(void *ctx, size_t size) {
	if (!ctx)
		return NULL;
	return memory_balloc((struct memory_context *)ctx, size);
}

/**
 * @brief Shared memory deallocation function using memory context
 * @param ctx Memory context pointer
 * @param ptr Pointer to memory to free
 * @param size Size of memory block
 */
static inline void
swiss_shared_free(void *ctx, void *ptr, size_t size) {
	if (!ctx || !ptr)
		return;
	memory_bfree((struct memory_context *)ctx, ptr, size);
}

/**
 * @brief Default FNV-1a hash function
 *
 * https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function#FNV-1a_hash
 *
 * @param key Pointer to key data
 * @param key_size Size of key in bytes
 * @param seed Hash seed for randomization
 * @return 64-bit hash value
 */
static inline uint64_t
swiss_hash_fnv1a(const void *key, size_t key_size, uint64_t seed) {
	const uint8_t *data = (const uint8_t *)key;
	uint64_t hash = 14695981039346656037ULL ^ seed;

	for (size_t i = 0; i < key_size; i++) {
		hash ^= data[i];
		hash *= 1099511628211ULL;
	}

	return hash;
}

// Groups operations

/**
 * @brief Get reference to specific group in groups array
 * @param groups Groups reference
 * @param config Map configuration for size calculations
 * @param i Group index
 * @return Reference to the specified group
 */
static inline swiss_group_ref_t
swiss_groups_group(
	swiss_groups_ref_t *groups, const swiss_map_config_t *config, uint64_t i
) {
	size_t slot_size = config->key_size + config->value_size;
	size_t group_size =
		sizeof(swiss_ctrl_group_t) + SWISS_GROUP_SLOTS * slot_size;
	size_t offset = i * group_size;

	swiss_group_ref_t group;
	group.data = (char *)ADDR_OF(&groups->data) + offset;
	return group;
}

/**
 * @brief Create new array of groups
 * @param config Map configuration for size calculations
 * @param length Number of groups to create (must be power of 2)
 * @return Groups reference with initialized empty groups
 */
static inline int
swiss_init_groups(
	swiss_groups_ref_t *groups,
	const swiss_map_config_t *config,
	uint64_t length
) {
	size_t slot_size = config->key_size + config->value_size;
	size_t group_size =
		sizeof(swiss_ctrl_group_t) + SWISS_GROUP_SLOTS * slot_size;

	swiss_alloc_fn_t alloc_fn =
		(swiss_alloc_fn_t)swiss_func_registry[config->alloc_fn_id];
	void *allocated_data = alloc_fn(config->mem_ctx, length * group_size);
	if (!allocated_data) {
		return -1;
	}

	// Now groups is in shared memory, so SET_OFFSET_OF will work correctly
	SET_OFFSET_OF(&groups->data, allocated_data);
	groups->length_mask = length - 1;

	// Initialize all control groups to empty
	for (uint64_t i = 0; i < length; i++) {
		swiss_group_ref_t group = swiss_groups_group(groups, config, i);
		swiss_ctrl_set_empty(swiss_group_ctrls(group));
	}

	return 0;
}

// Table operations

/**
 * @brief Create new hash table
 * @param config Map configuration
 * @param capacity Desired capacity (will be rounded up to power of 2)
 * @param index Index in directory
 * @param local_depth Local depth for extendible hashing
 * @return Pointer to new table or NULL on failure
 */
static inline swiss_table_t *
swiss_table_new(
	const swiss_map_config_t *config,
	uint64_t capacity,
	int index,
	uint8_t local_depth
) {
	if (capacity < SWISS_GROUP_SLOTS) {
		capacity = SWISS_GROUP_SLOTS;
	}

	if (capacity > MAX_TABLE_CAPACITY) {
		errno = EINVAL;
		return NULL;
	}

	bool overflow;
	capacity = swiss_align_up_pow2(capacity, &overflow);
	if (overflow) {
		errno = EINVAL;
		return NULL;
	}

	swiss_alloc_fn_t alloc_fn =
		(swiss_alloc_fn_t)swiss_func_registry[config->alloc_fn_id];
	swiss_free_fn_t free_fn =
		(swiss_free_fn_t)swiss_func_registry[config->free_fn_id];
	swiss_table_t *table = alloc_fn(config->mem_ctx, sizeof(swiss_table_t));
	if (!table) {
		errno = ENOMEM;
		return NULL;
	}

	table->index = index;
	table->local_depth = local_depth;
	table->used = 0;
	table->capacity = (uint16_t)capacity;

	uint64_t group_count = capacity / SWISS_GROUP_SLOTS;
	if (swiss_init_groups(&table->groups, config, group_count) != 0) {
		free_fn(config->mem_ctx, table, sizeof(swiss_table_t));
		errno = ENOMEM;
		return NULL;
	}

	// Calculate growth_left
	if (capacity <= SWISS_GROUP_SLOTS) {
		table->growth_left = capacity - 1;
	} else {
		table->growth_left =
			(capacity * MAX_AVG_GROUP_LOAD) / SWISS_GROUP_SLOTS;
	}

	return table;
}

/**
 * @brief Free hash table and its resources
 * @param table Table to free
 * @param config Map configuration for deallocation
 */
static inline void
swiss_table_free(swiss_table_t *table, const swiss_map_config_t *config) {
	if (!table)
		return;

	if (table->groups.data) {
		size_t slot_size = config->key_size + config->value_size;
		size_t group_size = sizeof(swiss_ctrl_group_t) +
				    SWISS_GROUP_SLOTS * slot_size;
		uint64_t group_count = table->groups.length_mask + 1;
		swiss_free_fn_t free_fn = (swiss_free_fn_t
		)swiss_func_registry[config->free_fn_id];
		free_fn(config->mem_ctx,
			ADDR_OF(&table->groups.data),
			group_count * group_size);
	}
	swiss_free_fn_t free_fn =
		(swiss_free_fn_t)swiss_func_registry[config->free_fn_id];
	free_fn(config->mem_ctx, table, sizeof(swiss_table_t));
}

/**
 * @brief Look up value by key in table
 * @param table Table to search
 * @param config Map configuration
 * @param map Parent map (for hash seed)
 * @param key Key to search for
 * @param value Output pointer for found value
 * @return true if key found, false otherwise
 */
static inline bool
swiss_table_get(
	swiss_table_t *table,
	const swiss_map_config_t *config,
	swiss_map_t *map,
	const void *key,
	void **value
) {
	swiss_hash_fn_t hash_fn =
		(swiss_hash_fn_t)swiss_func_registry[config->hash_fn_id];
	uint64_t hash = hash_fn(key, config->key_size, map->seed);

	swiss_probe_seq_t seq =
		swiss_make_probe_seq(hash, table->groups.length_mask);
	uint8_t h2 = swiss_h2(hash);

	while (true) {
		swiss_group_ref_t group =
			swiss_groups_group(&table->groups, config, seq.offset);
		swiss_ctrl_group_t ctrl = *swiss_group_ctrls(group);

		swiss_bitset_t match = swiss_ctrl_match_h2(ctrl, h2);

		while (match != 0) {
			size_t i = swiss_bitset_first(match);

			void *slot_key = swiss_group_key(group, config, i);
			swiss_key_equal_fn_t key_equal_fn =
				(swiss_key_equal_fn_t
				)swiss_func_registry[config->key_equal_fn_id];
			if (key_equal_fn(key, slot_key, config->key_size)) {
				*value = swiss_group_value(group, config, i);
				return true;
			}
			match = swiss_bitset_remove_first(match);
		}

		swiss_bitset_t empty_match = swiss_ctrl_match_empty(ctrl);
		if (empty_match != 0) {
			return false;
		}

		seq = swiss_probe_seq_next(seq);
	}
}

/**
 * @brief Find or create slot for key in table
 * @param table Table to insert into
 * @param config Map configuration
 * @param map Parent map (for hash seed and counters)
 * @param hash Pre-computed hash value
 * @param key Key to insert
 * @param ok Output flag indicating success
 * @return Pointer to value slot or NULL if table needs rehashing
 */
static inline void *
swiss_table_put_slot(
	swiss_table_t *table,
	const swiss_map_config_t *config,
	swiss_map_t *map,
	uint64_t hash,
	const void *key,
	bool *ok
) {
	swiss_probe_seq_t seq =
		swiss_make_probe_seq(hash, table->groups.length_mask);
	uint8_t h2 = swiss_h2(hash);

	swiss_group_ref_t first_deleted_group = {0};
	size_t first_deleted_slot = 0;
	bool found_deleted = false;

	while (true) {
		swiss_group_ref_t group =
			swiss_groups_group(&table->groups, config, seq.offset);
		swiss_ctrl_group_t *ctrl = swiss_group_ctrls(group);

		swiss_bitset_t match = swiss_ctrl_match_h2(*ctrl, h2);

		// Look for existing key
		while (match != 0) {
			size_t i = swiss_bitset_first(match);

			void *slot_key = swiss_group_key(group, config, i);
			swiss_key_equal_fn_t key_equal_fn =
				(swiss_key_equal_fn_t
				)swiss_func_registry[config->key_equal_fn_id];
			if (key_equal_fn(key, slot_key, config->key_size)) {
				// Key exists, use existing slot
				// memcpy(slot_key, key, config->key_size);
				*ok = true;
				return swiss_group_value(group, config, i);
			}
			match = swiss_bitset_remove_first(match);
		}

		// Look for empty or deleted slots
		swiss_bitset_t empty_or_deleted =
			swiss_ctrl_match_empty_or_deleted(*ctrl);
		if (empty_or_deleted != 0) {
			size_t i = swiss_bitset_first(empty_or_deleted);
			uint8_t ctrl_byte = swiss_ctrl_get(*ctrl, i);

			if (ctrl_byte == CTRL_DELETED) {
				// Remember first deleted slot
				if (!found_deleted) {
					first_deleted_group = group;
					first_deleted_slot = i;
					found_deleted = true;
				}
				// Continue looking for empty slot or end of
				// probe sequence
			} else {
				// Found empty slot - end of probe sequence

				// Use deleted slot if we found one
				if (found_deleted) {
					group = first_deleted_group;
					i = first_deleted_slot;
					ctrl = swiss_group_ctrls(group);
					table->growth_left++; // Will be
							      // decremented
							      // below
				}

				// Check if we have room to grow
				if (table->growth_left > 0) {
					void *slot_key = swiss_group_key(
						group, config, i
					);
					void *slot_value = swiss_group_value(
						group, config, i
					);

					// Copy key to the new slot
					memcpy(slot_key, key, config->key_size);

					swiss_ctrl_set(ctrl, i, h2);
					table->growth_left--;
					table->used++;
					map->used++;

					*ok = true;
					return slot_value;
				}

				// Need to rehash
				*ok = false;
				return NULL;
			}
		}

		seq = swiss_probe_seq_next(seq);
	}
}

/**
 * @brief Delete key from table
 * @param table Table to delete from
 * @param config Map configuration
 * @param map Parent map (for hash seed and counters)
 * @param hash Pre-computed hash value
 * @param key Key to delete
 */
static inline void
swiss_table_delete(
	swiss_table_t *table,
	const swiss_map_config_t *config,
	swiss_map_t *map,
	uint64_t hash,
	const void *key
) {
	swiss_probe_seq_t seq =
		swiss_make_probe_seq(hash, table->groups.length_mask);
	uint8_t h2 = swiss_h2(hash);

	while (true) {
		swiss_group_ref_t group =
			swiss_groups_group(&table->groups, config, seq.offset);
		swiss_ctrl_group_t *ctrl = swiss_group_ctrls(group);

		swiss_bitset_t match = swiss_ctrl_match_h2(*ctrl, h2);

		while (match != 0) {
			size_t i = swiss_bitset_first(match);

			void *slot_key = swiss_group_key(group, config, i);
			swiss_key_equal_fn_t key_equal_fn =
				(swiss_key_equal_fn_t
				)swiss_func_registry[config->key_equal_fn_id];
			if (key_equal_fn(key, slot_key, config->key_size)) {
				table->used--;
				map->used--;

				// Check if group has empty slots
				swiss_bitset_t empty_match =
					swiss_ctrl_match_empty(*ctrl);
				if (empty_match != 0) {
					// Mark as empty instead of deleted when
					// group already has empty slots. This
					// optimization allows search operations
					// to terminate earlier since probing
					// stops at the first empty slot
					// encountered.
					swiss_ctrl_set(ctrl, i, CTRL_EMPTY);
					table->growth_left++;
				} else {
					swiss_ctrl_set(ctrl, i, CTRL_DELETED);
				}

				return;
			}
			match = swiss_bitset_remove_first(match);
		}

		swiss_bitset_t empty_match = swiss_ctrl_match_empty(*ctrl);
		if (empty_match != 0) {
			return;
		}

		seq = swiss_probe_seq_next(seq);
	}
}

/**
 * @brief Clear all entries from table
 * @param table Table to clear
 * @param config Map configuration
 */
static inline void
swiss_table_clear(swiss_table_t *table, const swiss_map_config_t *config) {
	uint64_t group_count = table->groups.length_mask + 1;

	for (uint64_t i = 0; i < group_count; i++) {
		swiss_group_ref_t group =
			swiss_groups_group(&table->groups, config, i);
		swiss_ctrl_group_t *ctrl = swiss_group_ctrls(group);

		// Clear all slots in the group
		// size_t slot_size =
		// 	config->key_size + config->value_size;
		// char *slots = (char *)ADDR_OF(&group.data) +
		// sizeof(swiss_ctrl_group_t); memset(slots, 0,
		// SWISS_GROUP_SLOTS * slot_size);

		swiss_ctrl_set_empty(ctrl);
	}

	table->used = 0;

	// Reset growth_left
	if (table->capacity <= SWISS_GROUP_SLOTS) {
		table->growth_left = table->capacity - 1;
	} else {
		table->growth_left = (table->capacity * MAX_AVG_GROUP_LOAD) /
				     SWISS_GROUP_SLOTS;
	}
}

/**
 * @brief Rehash all entries from old table into new table
 * @param old_table Source table to rehash from
 * @param new_table Destination table to rehash into
 * @param config Map configuration
 * @param map Parent map for hash seed
 */
static inline void
swiss_table_rehash(
	swiss_table_t *old_table,
	swiss_table_t *new_table,
	const swiss_map_config_t *config,
	swiss_map_t *map
) {
	uint64_t old_group_count = old_table->groups.length_mask + 1;

	for (uint64_t g = 0; g < old_group_count; g++) {
		swiss_group_ref_t group =
			swiss_groups_group(&old_table->groups, config, g);
		swiss_ctrl_group_t ctrl = *swiss_group_ctrls(group);

		for (size_t i = 0; i < SWISS_GROUP_SLOTS; i++) {
			uint8_t c = swiss_ctrl_get(ctrl, i);
			if (c == CTRL_EMPTY || c == CTRL_DELETED) {
				continue;
			}

			void *key = swiss_group_key(group, config, i);
			void *value = swiss_group_value(group, config, i);

			swiss_hash_fn_t hash_fn = (swiss_hash_fn_t
			)swiss_func_registry[config->hash_fn_id];
			uint64_t hash =
				hash_fn(key, config->key_size, map->seed);

			// Insert into new table without checking capacity
			bool ok;
			void *new_slot = swiss_table_put_slot(
				new_table, config, map, hash, key, &ok
			);
			if (ok && new_slot) {
				memcpy(new_slot, value, config->value_size);
				// Adjust counters since swiss_table_put_slot
				// increments them
				new_table->used--;
				map->used--;
			}
		}
	}
}

/**
 * @brief Grow table to double capacity
 * @param table Table to grow
 * @param config Map configuration
 * @param map Parent map
 * @return Pointer to new table or NULL on failure
 */
static inline swiss_table_t *
swiss_table_grow(
	swiss_table_t *table, const swiss_map_config_t *config, swiss_map_t *map
) {
	uint64_t new_capacity = table->capacity * 2;
	if (new_capacity > MAX_TABLE_CAPACITY) {
		return NULL; // Table too large, needs splitting
	}

	swiss_table_t *new_table = swiss_table_new(
		config, new_capacity, table->index, table->local_depth
	);
	if (!new_table) {
		return NULL;
	}

	// Rehash all entries from old table to new table
	swiss_table_rehash(table, new_table, config, map);

	return new_table;
}

/**
 * @brief Split table into two tables
 * @param table Table to split (will be reused as left split)
 * @param config Map configuration
 * @param map Parent map
 * @param right Output pointer for right split table (newly allocated)
 * @return 0 on success, -1 on failure
 *
 * This function optimizes memory allocation by reusing the existing table
 * as the left split and only allocating one new table for the right split.
 * Entries that belong to the right split are moved from the original table
 * to the new right table.
 */
static inline int
swiss_table_split(
	swiss_table_t *table,
	const swiss_map_config_t *config,
	swiss_map_t *map,
	swiss_table_t **right
) {
	uint8_t new_local_depth = table->local_depth + 1;

	// Reuse existing table as left split, only allocate right split
	*right = swiss_table_new(
		config, table->capacity, table->index, new_local_depth
	);

	if (!*right) {
		return -1;
	}

	// Update left table's local depth
	table->local_depth = new_local_depth;

	uint64_t old_group_count = table->groups.length_mask + 1;
	uint64_t split_mask = 1ULL << (64 - new_local_depth);

	// Iterate through existing entries and move those that belong to right
	// split
	for (uint64_t g = 0; g < old_group_count; g++) {
		swiss_group_ref_t group =
			swiss_groups_group(&table->groups, config, g);
		swiss_ctrl_group_t *ctrl = swiss_group_ctrls(group);
		swiss_bitset_t empty_match = swiss_ctrl_match_empty(*ctrl);

		for (size_t i = 0; i < SWISS_GROUP_SLOTS; i++) {
			uint8_t c = swiss_ctrl_get(*ctrl, i);
			if (c == CTRL_EMPTY || c == CTRL_DELETED) {
				continue;
			}

			void *key = swiss_group_key(group, config, i);
			void *value = swiss_group_value(group, config, i);

			swiss_hash_fn_t hash_fn = (swiss_hash_fn_t
			)swiss_func_registry[config->hash_fn_id];
			uint64_t hash =
				hash_fn(key, config->key_size, map->seed);

			// If entry belongs to right split, move it
			if (hash & split_mask) {
				// Insert into right table
				bool ok;
				void *new_slot = swiss_table_put_slot(
					*right, config, map, hash, key, &ok
				);
				if (likely(ok && new_slot)) {
					memcpy(new_slot,
					       value,
					       config->value_size);
					// swiss_table_put_slot already
					// incremented (*right)->used and
					// map->used We need to decrement
					// (*left)->used since we're removing
					// from it We need to decrement
					// map->used once more to balance the
					// increment from put_slot (since we're
					// moving, not adding a new entry)
					table->used--;
					map->used--;

					// If the group had empty slots before
					// the split, we can mark the moved slot
					// as empty. Otherwise, we must mark it
					// as deleted to preserve the probe
					// chain.
					if (empty_match != 0) {
						swiss_ctrl_set(
							ctrl, i, CTRL_EMPTY
						);
						table->growth_left++;
					} else {
						swiss_ctrl_set(
							ctrl, i, CTRL_DELETED
						);
					}
				} else {
					// Very unlikely, as the right table
					// should have a free slot.
					swiss_table_free(*right, config);
					*right = NULL;
					return -1;
				}
			}
		}
	}

	return 0;
}

/**
 * @brief Expand directory to double size
 * @param map Map to expand directory for
 * @return 0 on success, -1 on failure
 */
static inline int
swiss_map_expand_directory(swiss_map_t *map) {
	int new_dir_len = map->dir_len * 2;
	if (new_dir_len < map->dir_len) {
		errno = ENOSPC;
		return -1;
	}
	swiss_alloc_fn_t alloc_fn =
		(swiss_alloc_fn_t)swiss_func_registry[map->config.alloc_fn_id];
	swiss_free_fn_t free_fn =
		(swiss_free_fn_t)swiss_func_registry[map->config.free_fn_id];
	swiss_table_t **new_directory = (swiss_table_t **)alloc_fn(
		map->config.mem_ctx, new_dir_len * sizeof(swiss_table_t *)
	);
	if (!new_directory) {
		errno = ENOMEM;
		return -1;
	}

	swiss_table_t **old_directory =
		(swiss_table_t **)ADDR_OF(&map->dir_ptr);

	// Copy existing entries, duplicating each entry
	for (int i = 0; i < map->dir_len; i++) {
		swiss_table_t *table =
			(swiss_table_t *)ADDR_OF(&old_directory[i]);
		SET_OFFSET_OF(&new_directory[2 * i], table);
		SET_OFFSET_OF(&new_directory[2 * i + 1], table);

		// Update table index if this is the first occurrence
		if (table->index == i) {
			table->index = 2 * i;
		}
	}

	free_fn(map->config.mem_ctx,
		old_directory,
		map->dir_len * sizeof(swiss_table_t *));
	SET_OFFSET_OF(&map->dir_ptr, new_directory);
	map->dir_len = new_dir_len;
	map->global_depth++;
	map->global_shift = 64 - map->global_depth;

	return 0;
}

/**
 * @brief Replace table in directory with new table
 * @param map Map containing directory
 * @param new_table New table to install
 */
static inline void
swiss_map_replace_table(swiss_map_t *map, swiss_table_t *new_table) {
	int entries = 1 << (map->global_depth - new_table->local_depth);
	swiss_table_t **directory = (swiss_table_t **)ADDR_OF(&map->dir_ptr);

	for (int i = 0; i < entries; i++) {
		SET_OFFSET_OF(&directory[new_table->index + i], new_table);
	}
}

/**
 * @brief Install split tables in directory
 * @param map Map containing directory
 * @param left Left split table (reused from original, stays at same index)
 * @param right Right split table (newly allocated, needs new index)
 */
static inline int
swiss_map_install_split(
	swiss_map_t *map, swiss_table_t *left, swiss_table_t *right
) {
	if (left->local_depth > map->global_depth) {
		// Need to expand directory first
		if (swiss_map_expand_directory(map) != 0) {
			return -1; // Failed to expand
		}
		// After expansion, left->index has been updated by
		// swiss_map_expand_directory
	}

	// Left table is already in the directory at the correct index
	// (swiss_map_expand_directory updated it if expansion occurred)

	// Install right table at new index
	// The number of entries per split table is based on the new local depth
	// After directory expansion, global_depth should be >=
	// left->local_depth
	int entries = 1 << (map->global_depth - left->local_depth);
	right->index = left->index + entries;
	swiss_map_replace_table(map, right);
	return 0;
}

// Map operations

/**
 * @brief Get directory index for hash value
 * @param map Map instance
 * @param hash Hash value
 * @return Directory index
 */
static inline uint64_t
swiss_map_directory_index(const swiss_map_t *map, uint64_t hash) {
	if (map->dir_len == 1) {
		return 0;
	}

	uint64_t shift = map->global_shift & 63;
	uint64_t result = hash >> shift;
	return result;
}

/**
 * @brief Get table at directory index
 * @param map Map instance
 * @param i Directory index
 * @return Pointer to table
 */
static inline swiss_table_t *
swiss_map_directory_at(const swiss_map_t *map, size_t i) {
	swiss_table_t **directory = (swiss_table_t **)ADDR_OF(&map->dir_ptr);
	return (swiss_table_t *)ADDR_OF(&directory[i]);
}

/**
 * @brief Create new Swiss map
 * @param config Map configuration
 * @param hint Size hint for initial capacity
 * @return Pointer to new map or NULL on failure
 *
 * This function initializes a new Swiss map with the given configuration.
 * It uses the config's rand_fn to generate an initial hash seed for the map.
 */
static inline swiss_map_t *
swiss_map_new(const swiss_map_config_t *config, size_t hint) {
	// Ensure minimum capacity
	size_t effective_hint =
		(hint < SWISS_GROUP_SLOTS) ? SWISS_GROUP_SLOTS : hint;

	// Calculate target capacity
	uint64_t target_capacity =
		effective_hint / MAX_AVG_GROUP_LOAD * SWISS_GROUP_SLOTS;
	if (target_capacity < effective_hint) {
		// Overflow
		errno = EINVAL;
		return NULL;
	}

	// Ensure minimum directory size
	uint64_t dir_size =
		(target_capacity + MAX_TABLE_CAPACITY - 1) / MAX_TABLE_CAPACITY;
	if (dir_size == 0) {
		dir_size = 1;
	}

	bool overflow;
	dir_size = swiss_align_up_pow2(dir_size, &overflow);
	if (overflow) {
		errno = EINVAL;
		return NULL;
	}

	// Check that dir_size fits in int32_t range
	if (dir_size > INT_MAX) {
		errno = EINVAL;
		return NULL;
	}

	// Get all function pointers at the top
	swiss_alloc_fn_t alloc_fn =
		(swiss_alloc_fn_t)swiss_func_registry[config->alloc_fn_id];
	swiss_free_fn_t free_fn =
		(swiss_free_fn_t)swiss_func_registry[config->free_fn_id];
	swiss_rand_fn_t rand_fn =
		(swiss_rand_fn_t)swiss_func_registry[config->rand_fn_id];

	swiss_map_t *map =
		(swiss_map_t *)alloc_fn(config->mem_ctx, sizeof(swiss_map_t));
	if (!map) {
		errno = ENOMEM;
		return NULL;
	}

	memset(map, 0, sizeof(swiss_map_t));
	map->config = *config;
	map->seed = rand_fn();

	map->global_depth = __builtin_ctzll(dir_size);
	map->global_shift = 64 - map->global_depth;

	swiss_table_t **directory = (swiss_table_t **)alloc_fn(
		config->mem_ctx, dir_size * sizeof(swiss_table_t *)
	);
	if (!directory) {
		free_fn(config->mem_ctx, map, sizeof(swiss_map_t));
		return NULL;
	}

	for (uint64_t i = 0; i < dir_size; i++) {
		swiss_table_t *table = swiss_table_new(
			config,
			target_capacity / dir_size,
			(int)i,
			map->global_depth
		);
		SET_OFFSET_OF(&directory[i], table);
		if (!table) {
			// Cleanup on failure
			for (uint64_t j = 0; j < i; j++) {
				swiss_table_t *cleanup_table =
					(swiss_table_t *)ADDR_OF(&directory[j]);
				swiss_table_free(cleanup_table, config);
			}
			free_fn(config->mem_ctx,
				directory,
				dir_size * sizeof(swiss_table_t *));
			free_fn(config->mem_ctx, map, sizeof(swiss_map_t));
			return NULL;
		}
	}

	SET_OFFSET_OF(&map->dir_ptr, directory);
	map->dir_len = (int)dir_size;

	return map;
}

/**
 * @brief Free Swiss map and all its resources
 * @param map Map to free
 */
static inline void
swiss_map_free(swiss_map_t *map) {
	if (!map)
		return;

	// Full map with directory
	swiss_table_t **directory = (swiss_table_t **)ADDR_OF(&map->dir_ptr);
	swiss_table_t *last_table = NULL;

	for (int i = 0; i < map->dir_len; i++) {
		swiss_table_t *table = (swiss_table_t *)ADDR_OF(&directory[i]);
		if (table != last_table) {
			swiss_table_free(table, &map->config);
			last_table = table;
		}
	}
	swiss_free_fn_t free_fn =
		(swiss_free_fn_t)swiss_func_registry[map->config.free_fn_id];
	free_fn(map->config.mem_ctx,
		ADDR_OF(&map->dir_ptr),
		map->dir_len * sizeof(swiss_table_t *));
	free_fn(map->config.mem_ctx, map, sizeof(swiss_map_t));
}

/**
 * @brief Get value by key from map
 * @param map Map to search
 * @param key Key to search for
 * @param value Output pointer for found value
 * @return true if key found, false otherwise
 */
static inline bool
swiss_map_get(swiss_map_t *map, const void *key, void **value) {
	if (!map || map->used == 0) {
		return false;
	}

	swiss_hash_fn_t hash_fn =
		(swiss_hash_fn_t)swiss_func_registry[map->config.hash_fn_id];
	uint64_t hash = hash_fn(key, map->config.key_size, map->seed);
	uint64_t idx = swiss_map_directory_index(map, hash);
	swiss_table_t *table = swiss_map_directory_at(map, idx);

	return swiss_table_get(table, &map->config, map, key, value);
}

/**
 * @brief Get or create slot for key in map
 * @param map Map to insert into
 * @param key Key to insert
 * @return Pointer to value slot or NULL on failure
 */
static inline void *
swiss_map_put_slot(swiss_map_t *map, const void *key) {
	if (!map)
		return NULL;

	swiss_hash_fn_t hash_fn =
		(swiss_hash_fn_t)swiss_func_registry[map->config.hash_fn_id];
	uint64_t hash = hash_fn(key, map->config.key_size, map->seed);

	while (true) {
		uint64_t idx = swiss_map_directory_index(map, hash);
		swiss_table_t *table = swiss_map_directory_at(map, idx);

		bool ok;
		void *result = swiss_table_put_slot(
			table, &map->config, map, hash, key, &ok
		);
		if (ok) {
			return result;
		}

		// First, try to grow the table.
		swiss_table_t *new_table =
			swiss_table_grow(table, &map->config, map);
		if (new_table) {
			swiss_map_replace_table(map, new_table);
			swiss_table_free(table, &map->config);
			continue; // Retry with the new, larger table.
		}

		// If growth is not possible, split the table.
		swiss_table_t *right;
		if (swiss_table_split(table, &map->config, map, &right) == 0) {
			if (swiss_map_install_split(map, table, right) != 0) {
				return NULL;
			};
			continue; // Retry with the new split tables.
		}

		// If both growth and splitting fail, we can't proceed.
		return NULL;
	}
}

/**
 * @brief Insert key-value pair into map
 * @param map Map to insert into
 * @param key Key to insert
 * @param value Value to insert
 * @return 0 on success, -1 on failure
 */
static inline int
swiss_map_put(swiss_map_t *map, const void *key, const void *value) {
	void *slot = swiss_map_put_slot(map, key);
	if (slot) {
		memcpy(slot, value, map->config.value_size);
		return 0;
	}
	return -1;
}

/**
 * @brief Delete key from map
 * @param map Map to delete from
 * @param key Key to delete
 * @return true if key was found and deleted, false otherwise
 *
 * When the map becomes empty after deletion, this function uses the config's
 * rand_fn to generate a new hash seed for security.
 */
static inline bool
swiss_map_delete(swiss_map_t *map, const void *key) {
	if (!map || map->used == 0) {
		return false;
	}

	swiss_hash_fn_t hash_fn =
		(swiss_hash_fn_t)swiss_func_registry[map->config.hash_fn_id];
	uint64_t hash = hash_fn(key, map->config.key_size, map->seed);

	size_t old_used = map->used;

	uint64_t idx = swiss_map_directory_index(map, hash);
	swiss_table_t *table = swiss_map_directory_at(map, idx);
	swiss_table_delete(table, &map->config, map, hash, key);

	if (map->used == 0) {
		swiss_rand_fn_t rand_fn = (swiss_rand_fn_t
		)swiss_func_registry[map->config.rand_fn_id];
		map->seed = rand_fn();
	}

	return map->used < old_used;
}

/**
 * @brief Clear all entries from map
 * @param map Map to clear
 *
 * This function removes all entries from the map and uses the config's
 * rand_fn to generate a new hash seed for security.
 */
static inline void
swiss_map_clear(swiss_map_t *map) {
	if (!map || map->used == 0) {
		return;
	}

	swiss_table_t **directory = (swiss_table_t **)ADDR_OF(&map->dir_ptr);
	swiss_table_t *last_table = NULL;

	for (int i = 0; i < map->dir_len; i++) {
		swiss_table_t *table = (swiss_table_t *)ADDR_OF(&directory[i]);
		if (table != last_table) {
			swiss_table_clear(table, &map->config);
			last_table = table;
		}
	}

	map->used = 0;
	swiss_rand_fn_t rand_fn =
		(swiss_rand_fn_t)swiss_func_registry[map->config.rand_fn_id];
	map->seed = rand_fn();
}

/**
 * @brief Get number of elements in map
 * @param map Map to query
 * @return Number of elements
 */
static inline size_t
swiss_map_size(const swiss_map_t *map) {
	return map ? map->used : 0;
}

/**
 * @brief Check if map is empty
 * @param map Map to check
 * @return true if map is empty, false otherwise
 */
static inline bool
swiss_map_empty(const swiss_map_t *map) {
	return !map || map->used == 0;
}

// Helper macros for type-safe map creation

/**
 * @brief Declare a type-safe map for specific key/value types
 * @param name Name prefix for the generated types and functions
 * @param key_type Type of keys
 * @param value_type Type of values
 */
#define SWISS_MAP_DECLARE(name, key_type, value_type)                          \
	typedef struct {                                                       \
		swiss_map_t *map;                                              \
	} name##_t;                                                            \
                                                                               \
	static inline name##_t *name##_new(                                    \
		struct memory_context *ctx, size_t hint                        \
	) {                                                                    \
		swiss_map_config_t config = {0};                               \
		config.key_size = sizeof(key_type);                            \
		config.value_size = sizeof(value_type);                        \
		config.hash_fn_id = SWISS_HASH_FNV1A;                          \
		config.key_equal_fn_id = SWISS_KEY_EQUAL_DEFAULT;              \
		config.alloc_fn_id = SWISS_ALLOC_SHARED;                       \
		config.free_fn_id = SWISS_FREE_SHARED;                         \
		config.rand_fn_id = SWISS_RAND_DEFAULT;                        \
		config.mem_ctx = ctx;                                          \
		name##_t *m =                                                  \
			(name##_t *)swiss_shared_alloc(ctx, sizeof(name##_t)); \
		if (m) {                                                       \
			m->map = swiss_map_new(&config, hint);                 \
		}                                                              \
		return m;                                                      \
	}                                                                      \
                                                                               \
	static inline void name##_free(name##_t *m) {                          \
		if (m && m->map) {                                             \
			struct memory_context *ctx =                           \
				(struct memory_context *)                      \
					m->map->config.mem_ctx;                \
			swiss_map_free(m->map);                                \
			swiss_shared_free(ctx, m, sizeof(name##_t));           \
		}                                                              \
	}                                                                      \
                                                                               \
	static inline bool name##_get(                                         \
		name##_t *m, const key_type *key, value_type **value           \
	) {                                                                    \
		return swiss_map_get(m->map, key, (void **)value);             \
	}                                                                      \
                                                                               \
	static inline int name##_put(                                          \
		name##_t *m, const key_type *key, const value_type *value      \
	) {                                                                    \
		return swiss_map_put(m->map, key, value);                      \
	}                                                                      \
                                                                               \
	static inline bool name##_delete(name##_t *m, const key_type *key) {   \
		return swiss_map_delete(m->map, key);                          \
	}                                                                      \
                                                                               \
	static inline size_t name##_size(const name##_t *m) {                  \
		return swiss_map_size(m->map);                                 \
	}

// Global function registry - statically initialized
static void *swiss_func_registry[SWISS_FUNC_COUNT] = {
	[SWISS_HASH_FNV1A] = (void *)swiss_hash_fnv1a,
	[SWISS_KEY_EQUAL_DEFAULT] = (void *)swiss_default_key_equal,
	[SWISS_ALLOC_SHARED] = (void *)swiss_shared_alloc,
	[SWISS_FREE_SHARED] = (void *)swiss_shared_free,
	[SWISS_RAND_DEFAULT] = (void *)swiss_rand_default,
	[SWISS_RAND_SECURE] = (void *)swiss_rand_secure
};

#endif // SWISSMAP_H
