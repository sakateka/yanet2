#include <stddef.h>

#include "ring_layout.h"

#include "common/record_ring.h"

struct ring_worker_layout
ring_layout_worker(void) {
	return (struct ring_worker_layout){
		.size = sizeof(struct ring_worker),
		.align = _Alignof(struct ring_worker),
		.cache_line_size = YANET_CACHE_LINE_SIZE,
		.write_idx_offset = offsetof(struct ring_worker, write_idx),
		.readable_idx_offset =
			offsetof(struct ring_worker, readable_idx),
		.size_offset = offsetof(struct ring_worker, size),
		.mask_offset = offsetof(struct ring_worker, mask),
	};
}
