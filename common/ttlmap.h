#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/random.h>
#include <sys/types.h>

#include "memory.h"
#include "memory_address.h"
#include "memory_block.h"
#include "numutils.h"
#include "rwlock.h"

// ============================================================================
// Constants and Global Registry
// ============================================================================

#define TTLMAP_BUCKET_ENTRIES 5
#define TTLMAP_CHUNK_INDEX_MAX_SIZE (MEMORY_BLOCK_ALLOCATOR_MAX_SIZE / 64)
#define TTLMAP_CHUNK_INDEX_MASK (TTLMAP_CHUNK_INDEX_MAX_SIZE - 1)

// Function registry for cross-process compatibility.
// NOLINTBEGIN(readability-identifier-naming)
typedef enum {
	TTLMAP_HASH_FNV1A,
	TTLMAP_KEY_EQUAL_DEFAULT,
	TTLMAP_RAND_DEFAULT,
	TTLMAP_RAND_SECURE,
	TTLMAP_FUNC_COUNT
} ttlmap_func_id_t;
// NOLINTEND(readability-identifier-naming)

// Global function registry (declared here, defined at the bottom).
static void *ttlmap_func_registry[TTLMAP_FUNC_COUNT];

// Hash function type.
typedef uint64_t (*ttlmap_hash_fn_t)(
	const void *key, size_t key_size, uint32_t seed
);

// Key comparison function type.
typedef bool (*ttlmap_key_equal_fn_t)(
	const void *key1, const void *key2, size_t key_size
);

// Random number generator for hash seed randomization.
// Prevents hash collision attacks and ensures different distributions.
typedef uint64_t (*ttlmap_rand_fn_t)(void);

typedef struct ttlmap_config {
	uint16_t key_size;
	uint16_t value_size;
	uint32_t hash_seed;
	uint16_t worker_count;
	uint32_t index_size;
	uint32_t extra_bucket_count;
	ttlmap_func_id_t hash_fn_id;
	ttlmap_func_id_t key_equal_fn_id;
	ttlmap_func_id_t rand_fn_id;
} ttlmap_config_t;

typedef struct ttlmap_bucket {
	uint16_t sig[TTLMAP_BUCKET_ENTRIES];
	uint32_t deadline[TTLMAP_BUCKET_ENTRIES];
	uint32_t idx[TTLMAP_BUCKET_ENTRIES];
	uint32_t next;
	uint8_t pad[4];
	rwlock_t lock;
} ttlmap_bucket_t;

typedef struct ttlmap_counter {
	uint16_t max_chain;
	uint32_t total_elements;
	uint32_t max_deadline;
} ttlmap_counter_t;

typedef struct ttlmap {
	uint32_t index_mask;
	ttlmap_bucket_t **buckets;

	uint8_t **key_store;
	uint8_t **value_store;

	uint16_t key_size;
	uint16_t value_size;
	uint16_t worker_count;
	uint16_t buckets_chunk_shift;

	uint32_t hash_seed;

	uint32_t keys_in_chunk;
	uint32_t keys_chunk_cnt;
	uint32_t values_in_chunk;
	uint32_t values_chunk_cnt;

	uint16_t hash_fn_id;
	uint16_t key_equal_fn_id;
	uint16_t rand_fn_id;

	ttlmap_bucket_t *extra_buckets;
	uint32_t extra_free_idx;
	uint32_t extra_size;

	uint32_t key_cursor;

	struct ttlmap *next;
	ttlmap_counter_t counters[];
} ttlmap_t;

typedef struct ttlmap_stats {
	size_t total_elements;
	size_t index_size;
	size_t max_chain_length;
	size_t memory_used;
} ttlmap_stats_t;

// ============================================================================
// Default Functions
// ============================================================================

// Default FNV-1a hash function.
static inline uint64_t
ttlmap_hash_fnv1a(const void *key, size_t key_size, uint32_t seed) {
	const uint8_t *data = (const uint8_t *)key;
	uint64_t hash = 14695981039346656037ULL ^ (uint64_t)seed;

	for (size_t i = 0; i < key_size; i++) {
		hash ^= data[i];
		hash *= 1099511628211ULL;
	}

	return hash;
}

static uint64_t ttlmap_rand_lcg_state = 1;

// Simple LCG for testing and general use.
static inline uint64_t
ttlmap_rand_default(void) {
	ttlmap_rand_lcg_state = ttlmap_rand_lcg_state * 1103515245 + 12345;
	return ttlmap_rand_lcg_state;
}

// Secure random using system entropy.
static inline uint64_t
ttlmap_rand_secure(void) {
	uint32_t seed;
	int ret = getrandom(&seed, sizeof(seed), 0);
	(void)ret;
	return seed;
}

// Default key comparison function using memcmp.
static inline bool
ttlmap_default_key_equal(const void *a, const void *b, size_t size) {
	switch (size) {
	case 4:
		return *(uint32_t *)a == *(uint32_t *)b;
	case 8:
		return *(uint64_t *)a == *(uint64_t *)b;
	default:
		return memcmp(a, b, size) == 0;
	}
}

// ============================================================================
// Utility Operations
// ============================================================================

static inline uint8_t *
ttlmap_get_key(ttlmap_t *map, uint32_t idx) {
	uint32_t chunk_idx = 0;
	// chunk_cnt is expected to be small
	while (idx >= map->keys_in_chunk) {
		idx -= map->keys_in_chunk;
		chunk_idx++;
	}

	uint8_t **key_store = ADDR_OF(&map->key_store);
	uint8_t *chunk = (uint8_t *)(ADDR_OF(&key_store[chunk_idx]));
	uint8_t *key_slot = chunk + (size_t)idx * map->key_size;
	return key_slot;
}

static inline uint8_t *
ttlmap_get_value(ttlmap_t *map, uint32_t idx) {

	uint32_t chunk_idx = 0;
	// chunk_cnt is expected to be small
	while (idx >= map->values_in_chunk) {
		idx -= map->values_in_chunk;
		chunk_idx++;
	}
	uint8_t **value_store = ADDR_OF(&map->value_store);
	uint8_t *chunk = (uint8_t *)(ADDR_OF(&value_store[chunk_idx]));
	uint8_t *value_slot = chunk + (size_t)idx * map->value_size;
	return value_slot;
}

static inline size_t
ttlmap_size(const ttlmap_t *map) {
	size_t total = 0;
	if (map) {
		for (size_t i = 0; i < map->worker_count; i++) {
			total += map->counters[i].total_elements;
		}
	}
	return total;
}

static inline bool
ttlmap_empty(const ttlmap_t *map) {
	if (map) {
		for (size_t i = 0; i < map->worker_count; i++) {
			if (map->counters[i].total_elements) {
				return false;
			}
		}
	}
	return true;
}

static inline size_t
ttlmap_max_chain_length(const ttlmap_t *map) {
	size_t chain = 0;
	if (map) {
		for (size_t i = 0; i < map->worker_count; i++) {
			if (chain < map->counters[i].max_chain) {
				chain = map->counters[i].max_chain;
			}
		}
	}
	return chain;
}

static inline void
ttlmap_get_stats(const ttlmap_t *map, ttlmap_stats_t *stats) {
	if (!stats)
		return;

	memset(stats, 0, sizeof(ttlmap_stats_t));
	stats->total_elements = ttlmap_size(map);
	stats->index_size = map->index_mask + 1;
	stats->max_chain_length = ttlmap_max_chain_length(map);

	// Calculate memory usage
	size_t total_memory = 0;

	// 1. Main map structure with counters
	size_t map_size =
		sizeof(ttlmap_t) + sizeof(ttlmap_counter_t) * map->worker_count;
	total_memory += map_size;

	// 2. Bucket chunks array (array of pointers)
	uint32_t chunk_count =
		(map->index_mask >> map->buckets_chunk_shift) + 1;
	size_t chunks_array_size = sizeof(ttlmap_bucket_t *) * chunk_count;
	total_memory += chunks_array_size;

	// 3. Bucket chunks (actual bucket storage)
	size_t index_chunk_size =
		sizeof(ttlmap_bucket_t) *
		((map->index_mask & TTLMAP_CHUNK_INDEX_MASK) + 1);
	total_memory += index_chunk_size * chunk_count;

	// 4. Extra buckets for chaining
	if (map->extra_size > 0) {
		size_t extra_buckets_size =
			sizeof(ttlmap_bucket_t) * map->extra_size;
		total_memory += extra_buckets_size;
	}

	// 5. Key store array (array of pointers to key chunks)
	size_t key_store_array_size = sizeof(uint8_t *) * map->keys_chunk_cnt;
	total_memory += key_store_array_size;

	// 6. Key chunks (actual key storage)
	total_memory += (size_t)map->key_size * (map->index_mask + 1);

	// 7. Value store array (array of pointers to value chunks)
	size_t value_store_array_size =
		sizeof(uint8_t *) * map->values_chunk_cnt;
	total_memory += value_store_array_size;

	// 8. Value chunks (actual value storage)
	total_memory += (size_t)map->value_size * (map->index_mask + 1);

	stats->memory_used = total_memory;
}

static inline int
ttlmap_allocate_chunks(
	struct memory_context *ctx,
	uint8_t **store,
	uint32_t size,
	uint32_t chunk_size,
	uint32_t chunks,
	uint32_t item_size
) {
	for (uint32_t i = 0; i < chunks; i++) {
		uint32_t keys = size > chunk_size ? chunk_size : size;

		size_t chunk_store_size = keys * item_size;
		uint8_t *chunk_store = memory_balloc(ctx, chunk_store_size);
		if (!chunk_store) {
			// Stop point for the deallocation code.
			store[i] = NULL;
			errno = ENOMEM;
			return -1;
		}
		memset(chunk_store, 0, chunk_store_size);
		SET_OFFSET_OF(&store[i], chunk_store);

		if (size <= chunk_size) {
			break; // Allocate keys no more than index_size.
		}
		size -= chunk_size;
	}
	return 0;
}

static inline uint32_t
ttlmap_max_deadline(const ttlmap_t *map) {
	uint32_t deadline = map->counters[0].max_deadline;
	for (size_t i = 1; i < map->worker_count; i++) {
		if (map->counters[i].max_deadline > deadline) {
			deadline = map->counters[i].max_deadline;
		}
	}
	return deadline;
}

static inline int64_t
ttlmap_next_free_key(ttlmap_t *map) {
	if (map->key_cursor > map->index_mask) {
		return -1;
	}
	uint32_t curr_key =
		__atomic_fetch_add(&map->key_cursor, 1, __ATOMIC_RELAXED);
	if (curr_key > map->index_mask) {
		return -1;
	}

	return curr_key;
}

// Utility function to update counters (max_chain, total_elements, and
// max_deadline).
static inline void
ttlmap_update_counters(
	ttlmap_t *map,
	uint16_t worker_idx,
	size_t chain_length,
	int increment_total,
	uint32_t deadline
) {
	ttlmap_counter_t *counter = &map->counters[worker_idx];
	counter->total_elements += increment_total;
	if (chain_length > counter->max_chain) {
		counter->max_chain = chain_length;
	}
	if (deadline > counter->max_deadline) {
		counter->max_deadline = deadline;
	}
}

// ============================================================================
// Core Map Operations
// ============================================================================

// Free a TTLMap and all its resources.
static inline void
ttlmap_destroy(ttlmap_t *map, struct memory_context *ctx) {
	if (!map) {
		return;
	}

	ttlmap_bucket_t **chunks = ADDR_OF(&map->buckets);

	if (chunks) {
		size_t chunk_count =
			(map->index_mask >> map->buckets_chunk_shift) + 1;
		size_t chunk_size =
			sizeof(ttlmap_bucket_t) *
			((map->index_mask & TTLMAP_CHUNK_INDEX_MASK) + 1);

		for (size_t i = 0; i < chunk_count; i++) {
			if (!chunks[i]) { // In case of allocation failure, the
					  // first null pointer indicates the
					  // failed allocation.
				break;
			}
			ttlmap_bucket_t *buckets = ADDR_OF(&chunks[i]);
			// Free the bucket array.
			memory_bfree(ctx, buckets, chunk_size);
		}
		memory_bfree(
			ctx, chunks, sizeof(ttlmap_bucket_t *) * chunk_count
		);
	}
	if (map->extra_buckets) {
		memory_bfree(
			ctx,
			ADDR_OF(&map->extra_buckets),
			sizeof(ttlmap_bucket_t) * map->extra_size
		);
	}

	uint8_t **key_chunks = ADDR_OF(&map->key_store);
	if (key_chunks) {
		size_t key_chunk_size = map->keys_in_chunk * map->key_size;
		for (size_t i = 0; i < map->keys_chunk_cnt; i++) {
			if (!key_chunks[i]) { // In case of allocation failure,
					      // the first null pointer
					      // indicates the failed
					      // allocation.
				break;
			}
			uint8_t *kchunk = ADDR_OF(&key_chunks[i]);
			memory_bfree(ctx, kchunk, key_chunk_size);
		}
		memory_bfree(
			ctx, key_chunks, sizeof(uint8_t *) * map->keys_chunk_cnt
		);
	}

	uint8_t **value_chunks = ADDR_OF(&map->value_store);
	if (value_chunks) {
		size_t value_chunk_size =
			map->values_in_chunk * map->value_size;
		for (size_t i = 0; i < map->values_chunk_cnt; i++) {
			if (!value_chunks[i]) { // In case of allocation
						// failure, the first null
						// pointer indicates the failed
						// allocation.
				break;
			}
			uint8_t *vchunk = ADDR_OF(&value_chunks[i]);
			memory_bfree(ctx, vchunk, value_chunk_size);
		}
		memory_bfree(
			ctx,
			value_chunks,
			sizeof(uint8_t *) * map->values_chunk_cnt
		);
	}

	size_t map_size =
		sizeof(ttlmap_t) + sizeof(ttlmap_counter_t) * map->worker_count;
	memory_bfree(ctx, map, map_size);
}

static inline ttlmap_t *
ttlmap_new(const ttlmap_config_t *config, struct memory_context *ctx) {
	uint32_t index_size = config->index_size;
	uint32_t extra_size = config->extra_bucket_count;

	if (index_size < 16) {
		index_size = 16;
	}
	// Ensure index_size is a power of 2.
	index_size = align_up_pow2(index_size);
	if (!index_size) {
		errno = EINVAL;
		return NULL;
	}

	if (extra_size) {
		if (extra_size > TTLMAP_CHUNK_INDEX_MAX_SIZE) {
			errno = EINVAL;
			return NULL;
		}
		extra_size = align_up_pow2(extra_size);
	}

	uint32_t keys_per_chunk =
		MEMORY_BLOCK_ALLOCATOR_MAX_SIZE / config->key_size;
	// Ceiling division: (index_size + keys_per_chunk - 1) / keys_per_chunk.
	// But ensure at least 1 chunk even if keys_per_chunk is 0.
	uint32_t keys_chunk_cnt =
		(index_size + keys_per_chunk - 1) / keys_per_chunk;

	uint32_t values_per_chunk =
		MEMORY_BLOCK_ALLOCATOR_MAX_SIZE / config->value_size;
	// Ceiling division with minimum of 1.
	uint32_t values_chunk_cnt =
		(index_size + values_per_chunk - 1) / values_per_chunk;

	// Check for overflow.
	if (keys_per_chunk * keys_chunk_cnt < index_size ||
	    values_per_chunk * values_chunk_cnt < index_size) {
		errno = EINVAL;
		return NULL;
	}

	ttlmap_rand_fn_t rand_fn =
		(ttlmap_rand_fn_t)ttlmap_func_registry[config->rand_fn_id];

	size_t map_size = sizeof(ttlmap_t) +
			  sizeof(ttlmap_counter_t) * config->worker_count;
	ttlmap_t *map = memory_balloc(ctx, map_size);
	if (!map) {
		errno = ENOMEM;
		return NULL;
	}
	memset(map, 0, map_size);

	map->key_size = config->key_size;
	map->value_size = config->value_size;
	map->hash_seed =
		config->hash_seed ? config->hash_seed : (uint32_t)rand_fn();
	map->worker_count = config->worker_count;

	map->hash_fn_id = config->hash_fn_id;
	map->key_equal_fn_id = config->key_equal_fn_id;
	map->rand_fn_id = config->rand_fn_id;

	map->index_mask = index_size - 1;
	map->buckets_chunk_shift = __builtin_popcount(TTLMAP_CHUNK_INDEX_MASK);

	map->extra_size = extra_size;
	map->extra_free_idx = 1; // idx 0 should not be used

	map->keys_in_chunk =
		index_size > keys_per_chunk ? keys_per_chunk : index_size;
	map->keys_chunk_cnt = keys_chunk_cnt;
	map->key_cursor = 0;

	map->values_in_chunk =
		index_size > values_per_chunk ? values_per_chunk : index_size;
	map->values_chunk_cnt = values_chunk_cnt;

	size_t extra_buckets_size = 0;
	ttlmap_bucket_t **chunks = NULL;
	ttlmap_bucket_t *extra_buckets = NULL;
	uint8_t **key_store = NULL;
	uint8_t **value_store = NULL;

	// Allocate index.
	uint32_t chunk_count =
		(map->index_mask >> map->buckets_chunk_shift) + 1;
	size_t chunks_array_size = sizeof(ttlmap_bucket_t *) * chunk_count;
	if (!(chunks = memory_balloc(ctx, chunks_array_size))) {
		errno = ENOMEM;
		goto fail;
	}
	SET_OFFSET_OF(&map->buckets, chunks);

	size_t index_chunk_size =
		sizeof(ttlmap_bucket_t) *
		((map->index_mask & TTLMAP_CHUNK_INDEX_MASK) + 1);
	for (uint32_t i = 0; i < chunk_count; i++) {
		ttlmap_bucket_t *chunk = memory_balloc(ctx, index_chunk_size);
		if (!chunk) {
			// Stop point for the deallocation code.
			chunks[i] = NULL;
			errno = ENOMEM;
			goto fail;
		}
		memset(chunk, 0, index_chunk_size);
		SET_OFFSET_OF(&chunks[i], chunk);
	}

	// Extra buckets do not add keys and values; they only
	// add bucket space for chaining. Map size is still limited to
	// index_size and cannot exceed this value.
	if (extra_size > 0) {
		extra_buckets_size = sizeof(ttlmap_bucket_t) * extra_size;
		if (!(extra_buckets = memory_balloc(ctx, extra_buckets_size))) {
			errno = ENOMEM;
			goto fail;
		}
		memset(extra_buckets, 0, extra_buckets_size);
		SET_OFFSET_OF(&map->extra_buckets, extra_buckets);
	}

	// Key/Value store.
	// Allocate keys storage chunks array.
	size_t key_store_array_size = sizeof(uint8_t *) * map->keys_chunk_cnt;
	if (!(key_store = memory_balloc(ctx, key_store_array_size))) {
		errno = ENOMEM;
		goto fail;
	}
	SET_OFFSET_OF(&map->key_store, key_store);

	if (ttlmap_allocate_chunks(
		    ctx,
		    (uint8_t **)key_store,
		    index_size,
		    map->keys_in_chunk,
		    map->keys_chunk_cnt,
		    map->key_size
	    ) == -1) {
		goto fail;
	}

	// Allocate values storage chunks array.
	size_t value_store_array_size =
		sizeof(uint8_t *) * map->values_chunk_cnt;
	if (!(value_store = memory_balloc(ctx, value_store_array_size))) {
		errno = ENOMEM;
		goto fail;
	}
	SET_OFFSET_OF(&map->value_store, value_store);

	if (ttlmap_allocate_chunks(
		    ctx,
		    value_store,
		    index_size,
		    map->values_in_chunk,
		    map->values_chunk_cnt,
		    map->value_size
	    ) == -1) {
		goto fail;
	}

	return map;

fail:
	ttlmap_destroy(map, ctx);
	return NULL;
}

static inline int64_t
ttlmap_get(
	ttlmap_t *map,
	uint16_t worker_idx,
	uint32_t now,
	const void *key,
	void **value,
	rwlock_t **lock
) {
	(void)worker_idx;

	ttlmap_hash_fn_t hash_fn =
		(ttlmap_hash_fn_t)ttlmap_func_registry[map->hash_fn_id];
	ttlmap_key_equal_fn_t key_equal_fn = (ttlmap_key_equal_fn_t
	)ttlmap_func_registry[map->key_equal_fn_id];

	uint64_t hash64 = hash_fn(key, map->key_size, map->hash_seed);
	uint32_t hash = (uint32_t)hash64;
	uint32_t sec_hash = (uint32_t)(hash64 >> 32);
	// Use primary hash or secondary hash (fallback to avoid zero).
	hash = hash ? hash : sec_hash;
	uint16_t sig = hash >> 16;
	sig = sig ? sig : 1;

	uint32_t chunk_idx =
		(hash & map->index_mask) >> map->buckets_chunk_shift;
	uint32_t bucket_idx = hash & map->index_mask & TTLMAP_CHUNK_INDEX_MASK;

	ttlmap_bucket_t *extra = ADDR_OF(&map->extra_buckets);
	ttlmap_bucket_t **chunks = ADDR_OF(&map->buckets);
	ttlmap_bucket_t *buckets = ADDR_OF(&chunks[chunk_idx]);
	ttlmap_bucket_t *bucket = &buckets[bucket_idx];

	if (lock) {
		rwlock_read_lock(&bucket->lock);
		*lock = &bucket->lock;
	}
	while (bucket) {
		for (size_t i = 0; i < TTLMAP_BUCKET_ENTRIES; i++) {
			if (bucket->sig[i] == sig &&
			    bucket->deadline[i] > now) {
				uint32_t key_idx = bucket->idx[i];
				uint8_t *other = ttlmap_get_key(map, key_idx);
				if (key_equal_fn(key, other, map->key_size)) {
					if (value != NULL) {
						*value = ttlmap_get_value(
							map, key_idx
						);
					}
					return key_idx;
				}
			} else if (!bucket->sig[i]) {
				return -1; // Early return on first empty slot;
					   // there should be no entries after
					   // an empty slot.
			}
		}

		bucket = bucket->next ? &extra[bucket->next] : NULL;
	}
	return -1;
}

static inline int64_t
ttlmap_put(
	ttlmap_t *map,
	uint16_t worker_idx,
	uint32_t now,
	uint32_t ttl,
	const void *key,
	const void *value,
	rwlock_t **lock
) {
	ttlmap_hash_fn_t hash_fn =
		(ttlmap_hash_fn_t)ttlmap_func_registry[map->hash_fn_id];
	ttlmap_key_equal_fn_t key_equal_fn = (ttlmap_key_equal_fn_t
	)ttlmap_func_registry[map->key_equal_fn_id];

	uint64_t hash64 = hash_fn(key, map->key_size, map->hash_seed);
	uint32_t hash = (uint32_t)hash64;
	uint32_t sec_hash = (uint32_t)(hash64 >> 32);
	// Use primary hash or secondary hash (fallback to avoid zero).
	hash = hash ? hash : sec_hash;
	uint16_t sig = hash >> 16;
	sig = sig ? sig : 1;

	uint32_t chunk_idx =
		(hash & map->index_mask) >> map->buckets_chunk_shift;
	uint32_t bucket_idx = hash & map->index_mask & TTLMAP_CHUNK_INDEX_MASK;

	uint32_t deadline = now + ttl;

	ttlmap_bucket_t *extra = ADDR_OF(&map->extra_buckets);
	ttlmap_bucket_t **chunks = ADDR_OF(&map->buckets);
	ttlmap_bucket_t *buckets = ADDR_OF(&chunks[chunk_idx]);
	ttlmap_bucket_t *bucket = &buckets[bucket_idx];

	if (lock) {
		rwlock_write_lock(&bucket->lock);
		*lock = &bucket->lock;
	}

	size_t chain_length = 0;
	ttlmap_bucket_t *last_bucket = bucket;

	bool has_free = false;
	uint32_t vacant_slot = 0;
	ttlmap_bucket_t *bucket_to_insert = NULL;

	while (bucket) {
		chain_length += 1;

		// Search for and update existing key.
		for (uint32_t i = 0; i < TTLMAP_BUCKET_ENTRIES; i++) {
			if (bucket->sig[i] == sig &&
			    bucket->deadline[i] > now) {
				uint32_t idx = bucket->idx[i];
				uint8_t *other = ttlmap_get_key(map, idx);
				if (key_equal_fn(key, other, map->key_size)) {
					uint8_t *value_ptr =
						ttlmap_get_value(map, idx);
					memcpy(value_ptr, value, map->value_size
					);
					// Update deadline for existing entry.
					bucket->deadline[i] = deadline;
					ttlmap_update_counters(
						map,
						worker_idx,
						chain_length,
						0,
						deadline
					);
					return idx;
				}
			} else if (!bucket_to_insert) {
				if (!bucket->sig[i]) {
					has_free = true;
					vacant_slot = i;
					bucket_to_insert = bucket;
					break;
				} else if (bucket->deadline[i] < now) {
					vacant_slot = i;
					bucket_to_insert = bucket;
				}
			}
		}
		last_bucket = bucket;

		if (has_free) {
			// A free slot is mutually exclusive with the next
			// bucket, so if we found a free slot, there should be
			// no next bucket.
			break;
		}

		bucket = bucket->next ? &extra[bucket->next] : NULL;
	}

	if (bucket_to_insert) {
		// Insert new key-value pair into an empty slot in existing
		// buckets.
		int64_t idx = (int64_t)bucket_to_insert->idx[vacant_slot];
		if (has_free) {
			idx = ttlmap_next_free_key(map);
			if (idx == -1) {
				return -1;
			}
			bucket_to_insert->idx[vacant_slot] = (uint32_t)idx;
		}

		// Get the key slot and store the key.
		uint8_t *new_key = ttlmap_get_key(map, (uint32_t)idx);
		memcpy(new_key, key, map->key_size);

		uint8_t *value_ptr = ttlmap_get_value(map, idx);
		memcpy(value_ptr, value, map->value_size);

		// Store signature and key index in the bucket.
		bucket_to_insert->sig[vacant_slot] = sig;
		bucket_to_insert->deadline[vacant_slot] = deadline;

		// Update counters.
		ttlmap_update_counters(
			map, worker_idx, chain_length, (int)has_free, deadline
		);

		return idx;
	}

	// All slots in the existing chain are full; need to allocate a new
	// bucket.
	if (map->extra_free_idx >= map->extra_size) {
		// No more extra buckets available.
		return -1;
	}

	// Allocate new extra bucket.
	uint32_t new_bucket_idx =
		__atomic_fetch_add(&map->extra_free_idx, 1, __ATOMIC_RELAXED);
	if (new_bucket_idx >= map->extra_size) {
		return -1;
	}

	ttlmap_bucket_t *new_bucket = &extra[new_bucket_idx];
	// NOTE: Free extra buckets are already zero-initialized
	// (they are zeroed at creation and during clear calls).
	new_bucket->next = 0;

	// Allocate new key.
	int64_t idx = ttlmap_next_free_key(map);
	if (idx == -1) {
		// No more space for keys.
		return -1;
	}

	// Store key and value.
	uint8_t *new_key = ttlmap_get_key(map, (uint32_t)idx);
	uint8_t *value_ptr = ttlmap_get_value(map, idx);
	memcpy(new_key, key, map->key_size);
	memcpy(value_ptr, value, map->value_size);

	// Initialize new bucket with the key.
	new_bucket->sig[0] = sig;
	new_bucket->idx[0] = (uint32_t)idx;
	new_bucket->deadline[0] = deadline;

	last_bucket->next = new_bucket_idx;

	// Update counters.
	chain_length++;
	ttlmap_update_counters(
		map, worker_idx, chain_length, 1, new_bucket->deadline[0]
	);

	return idx;
}

static inline void
ttlmap_clear(ttlmap_t *map) {
	if (!map) {
		return;
	}

	// 1. Clear all primary buckets.
	ttlmap_bucket_t **chunks = ADDR_OF(&map->buckets);
	if (chunks) {
		uint32_t chunk_count =
			(map->index_mask >> map->buckets_chunk_shift) + 1;
		size_t index_chunk_size =
			sizeof(ttlmap_bucket_t) *
			((map->index_mask & TTLMAP_CHUNK_INDEX_MASK) + 1);

		for (uint32_t i = 0; i < chunk_count; i++) {
			ttlmap_bucket_t *buckets = ADDR_OF(&chunks[i]);
			if (buckets) {
				memset(buckets, 0, index_chunk_size);
			}
		}
	}

	// 2. Clear extra buckets.
	if (map->extra_buckets) {
		ttlmap_bucket_t *extra_buckets = ADDR_OF(&map->extra_buckets);
		memset(extra_buckets,
		       0,
		       sizeof(ttlmap_bucket_t) * map->extra_size);
	}

	// 3. Reset extra bucket free index.
	map->extra_free_idx = 1;

	// 4. Reset key cursor.
	map->key_cursor = 0;

	// 5. Reset counters.
	memset(map->counters, 0, sizeof(ttlmap_counter_t) * map->worker_count);
}

/**
 * Thread-safe wrapper for ttlmap_put using a global read-write lock.
 * Simple wrapper that acquires a write lock, calls the unsafe put, then
 * releases the lock.
 */
static inline int
ttlmap_put_safe(
	ttlmap_t *map,
	uint16_t worker_idx,
	uint32_t now,
	uint32_t ttl,
	const void *key,
	const void *value
) {
	rwlock_t *lock = NULL;
	int result = ttlmap_put(map, worker_idx, now, ttl, key, value, &lock);
	if (lock) {
		rwlock_write_unlock(lock);
	}
	return result;
}

// Global function registry - statically initialized.
static void *ttlmap_func_registry[TTLMAP_FUNC_COUNT] = {
	[TTLMAP_HASH_FNV1A] = (void *)ttlmap_hash_fnv1a,
	[TTLMAP_KEY_EQUAL_DEFAULT] = (void *)ttlmap_default_key_equal,
	[TTLMAP_RAND_DEFAULT] = (void *)ttlmap_rand_default,
	[TTLMAP_RAND_SECURE] = (void *)ttlmap_rand_secure
};
