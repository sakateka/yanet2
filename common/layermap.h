#include "memory.h"
#include "memory_address.h"
#include "ttlmap.h"
#include <stdatomic.h>
#include <unistd.h>

typedef struct layermap_list {
	ttlmap_t *layer;
	struct layermap_list *next;
} layermap_list_t;

typedef struct layermap {
	ttlmap_t *active;
	ttlmap_t *read_only;
	layermap_list_t *outdated;
	ttlmap_config_t config;
} layermap_t;

// Creates a new layermap with the specified configuration.
static inline layermap_t *
layermap_new(const ttlmap_config_t *config, struct memory_context *ctx) {
	layermap_t *lmap = memory_balloc(ctx, sizeof(layermap_t));
	if (!lmap) {
		return NULL;
	}
	memset(lmap, 0, sizeof(layermap_t));
	ttlmap_t *active = ttlmap_new(config, ctx);
	if (!active) {
		memory_bfree(ctx, lmap, sizeof(layermap_t));
		return NULL;
	}
	ATOMIC_SET_OFFSET_OF(&lmap->active, active);
	lmap->config = *config;

	return lmap;
}

// Destroys a layermap and releases all associated resources.
static inline void
layermap_destroy(layermap_t *lmap, struct memory_context *ctx) {
	if (!lmap) {
		return;
	}

	ttlmap_t *active_layer_atomic = ATOMIC_ADDR_OF(&lmap->active);
	ttlmap_destroy((ttlmap_t *)active_layer_atomic, ctx);
	ttlmap_t *layer = ATOMIC_ADDR_OF(&lmap->read_only);
	while (layer) {
		ttlmap_t *next = ADDR_OF(&layer->next);
		ttlmap_destroy(layer, ctx);
		layer = next;
	}

	layermap_list_t *outdated_element = ADDR_OF(&lmap->outdated);
	while (outdated_element) {
		layermap_list_t *next = ADDR_OF(&outdated_element->next);
		ttlmap_destroy(outdated_element->layer, ctx);
		memory_bfree(ctx, outdated_element, sizeof(layermap_list_t));
		outdated_element = next;
	}
	memory_bfree(ctx, lmap, sizeof(layermap_t));
}

// Checks if a layer is outdated based on its deadline.
static inline bool
layermap_is_layer_outdated(const ttlmap_t *layer, uint32_t now) {
	// This is safe because we only check read-only layers,
	// so there should be no writes to these layers.
	return ttlmap_max_deadline(layer) <= now;
}

// Rotates the layers, moving the active layer to the read-only list and
// preparing a new active layer. This operation is designed to be called by a
// single writer thread, while other threads can read concurrently.
static inline int
layermap_rotate(layermap_t *lmap, struct memory_context *ctx, uint32_t now) {
	layermap_list_t *outdated_head = NULL;
	// We only check for outdated layers within the read-only list, skipping
	// the "hot" layer (the most recently added read-only layer).
	ttlmap_t *current = NULL;
	ttlmap_t **current_ptr = NULL;
	ttlmap_t *ro_head = ATOMIC_ADDR_OF(&lmap->read_only);
	if (ro_head) {
		current_ptr = &ro_head->next;
		current = ADDR_OF(current_ptr);
	}
	while (current) {
		ttlmap_t *next = ADDR_OF(&current->next);
		if (layermap_is_layer_outdated(current, now)) {
			// Detach the layer from the read-only list. This is
			// safe for concurrent readers because the detached
			// layer's 'next' pointer remains intact, allowing any
			// in-flight readers to continue traversing the list.
			SET_OFFSET_OF(current_ptr, next);
			// We use an externally allocated list element to avoid
			// modifying the 'next' pointer of the outdated ttlmap.
			// This ensures that list traversal remains correct even
			// after the layer is moved.
			layermap_list_t *element =
				memory_balloc(ctx, sizeof(layermap_list_t));
			SET_OFFSET_OF(&element->layer, current);
			// The new element is older than the current head, so
			// prepend it to the outdated list.
			SET_OFFSET_OF(&element->next, outdated_head);
			outdated_head = element;

		} else {
			current_ptr = &current->next;
		}
		current = next;
	}

	ttlmap_t *oldest_layer = NULL;
	layermap_list_t *old_outdated = ADDR_OF(&lmap->outdated);
	if (!old_outdated) {
		oldest_layer = ttlmap_new(&lmap->config, ctx);
		if (!oldest_layer) {
			return -1;
		}
	} else {
		EQUATE_OFFSET(&lmap->outdated, &old_outdated->next);
		oldest_layer = old_outdated->layer;
		memory_bfree(ctx, old_outdated, sizeof(layermap_list_t));
		ttlmap_clear(oldest_layer);
	}

	// These operations extend the chain of layers but do not break it,
	// making them safe for concurrent readers.
	ttlmap_t *hot_layer = ATOMIC_ADDR_OF(&lmap->active);
	SET_OFFSET_OF(&hot_layer->next, ro_head);
	ATOMIC_SET_OFFSET_OF(&lmap->read_only, hot_layer);
	ATOMIC_SET_OFFSET_OF(&lmap->active, oldest_layer);

	// Append the newly collected outdated layers to the end of the main
	// outdated list.
	if (outdated_head) {
		layermap_list_t **tail_ptr = &lmap->outdated;
		layermap_list_t *tail = ADDR_OF(&lmap->outdated);
		if (tail) {
			while (ADDR_OF(&tail->next)) {
				tail = ADDR_OF(&tail->next);
			}
			tail_ptr = &tail->next;
		}
		SET_OFFSET_OF(tail_ptr, outdated_head);
	}

	return 0;
}

// Searches for a key across all layers, from newest to oldest.
static inline int64_t
layermap_get(
	layermap_t *lmap,
	uint16_t worker_idx,
	uint32_t now,
	const void *key,
	void **value,
	rwlock_t **lock
) {
	// The active layer handles writes, so a lock is required for safe
	// access.
	ttlmap_t *active_layer = ATOMIC_ADDR_OF(&lmap->active);
	int64_t result =
		ttlmap_get(active_layer, worker_idx, now, key, value, lock);
	if (result >= 0) {
		return result;
	}
	if (lock && *lock) {
		rwlock_read_unlock(*lock);
		*lock = NULL;
	}
	// Iterate over read-only layers. The first one (the "hot" layer) might
	// still have in-progress writes from when it was the active layer, so
	// we lock it.
	ttlmap_t *hot = ATOMIC_ADDR_OF(&lmap->read_only);
	if (hot) {
		result = ttlmap_get(hot, worker_idx, now, key, value, lock);
		if (result >= 0) {
			return result;
		}

		// Subsequent read-only layers are guaranteed to be immutable
		// and can be read without locking.
		ttlmap_t *layer = ADDR_OF(&hot->next);
		while (layer) {
			result = ttlmap_get(
				layer, worker_idx, now, key, value, NULL
			);
			if (result >= 0) {
				return result;
			}
			layer = ADDR_OF(&layer->next);
		}
	}

	return -1; // Key not found in any layer.
}

// Inserts or updates a key-value pair in the active layer.
static inline int64_t
layermap_put(
	layermap_t *lmap,
	uint16_t worker_idx,
	uint32_t now,
	uint32_t ttl,
	const void *key,
	const void *value,
	rwlock_t **lock
) {
	ttlmap_t *current = ATOMIC_ADDR_OF(&lmap->active);
	if (!current) {
		return -1;
	}

	return ttlmap_put(current, worker_idx, now, ttl, key, value, lock);
}
