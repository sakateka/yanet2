#pragma once

/*
 * Layout and framing facts about the ring writer as the C archive was
 * compiled, for bindings that must agree with it.
 *
 * Kept apart from the object lifecycle so a probe links without the
 * control-plane libraries the lifecycle needs.
 */

#include <stdint.h>

// Layout of the per-worker ring metadata as this archive was compiled.
//
// Go reads worker fields through its own cgo view of the struct; comparing
// both views catches a cache line size or field order that differs between
// the meson build and the cgo flags.
struct ring_worker_layout {
	uint64_t size;
	uint64_t align;
	uint64_t cache_line_size;
	uint64_t write_idx_offset;
	uint64_t readable_idx_offset;
	uint64_t size_offset;
	uint64_t mask_offset;
};

// Report the archive's per-worker ring metadata layout.
struct ring_worker_layout
ring_layout_worker(void);

// Round val with the archive's record alignment.
uint32_t
ring_layout_align4(uint32_t val);
