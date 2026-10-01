#pragma once

/*
 * Per-worker ring buffer of opaque records: a lock-free single-producer log
 * that another process can read.
 *
 * A worker's metadata and its data area are two independent allocations.
 * The metadata keeps the writer's private state and the reader-visible
 * positions on separate cache lines, so a polling reader never contends
 * with the writer's per-record bookkeeping, and writers on adjacent workers
 * never share a line. The writer never blocks: a full ring evicts whole oldest
 * records instead of stalling. No producer writes to it yet; pdump capture
 * keeps its own rings.
 */

#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "common/cache.h"
#include "common/likely.h"
#include "common/memory_address.h"
#include "common/numutils.h"

// Readers in other languages decode the record frame as little-endian.
_Static_assert(
	__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
	"the ring wire format is little-endian"
);

// Round a record length up to the 4-byte boundary every record starts at.
static inline uint32_t
ring_align4(uint32_t val) {
	return (uint32_t)next_divisible_pow2(val, 4);
}

// Frame preceding every record's opaque payload.
//
// The length covers the frame and the payload combined; the worker-scoped
// sequence number is stamped at commit. Records are only 4-byte aligned, so
// a reader copies the frame out before reading the length instead of
// dereferencing it in place.
struct ring_record_frame {
	uint32_t total_len;
	uint32_t seqno;
};

_Static_assert(
	sizeof(struct ring_record_frame) == 8,
	"ring record frame must be the 8-byte wire size"
);

// Smallest length a record may declare: the frame with an empty payload.
#define RING_RECORD_FRAME_SIZE (sizeof(struct ring_record_frame))

// Writer-private half of a worker's ring metadata.
//
// The positions are the writer's authoritative copies of the published
// ones: no reader loads them, so the per-record stores here never pull the
// line away from the writer's CPU. The size, mask and relative data
// pointer are fixed at creation; a reader loads them once when it attaches.
struct ring_worker_private {
	uint64_t write_idx;
	uint64_t readable_idx;
	uint8_t *data;
	uint32_t next_seqno;
	uint32_t size;
	uint32_t mask;
} __attribute__((aligned(YANET_CACHE_LINE_SIZE)));

// Reader-visible half of a worker's ring metadata.
//
// The writer only release-stores these and never loads them, so a reader
// polling the line costs the writer at most an ownership request per
// publication, not one per access.
struct ring_worker_published {
	_Atomic uint64_t write_idx;
	_Atomic uint64_t readable_idx;
} __attribute__((aligned(YANET_CACHE_LINE_SIZE)));

// Per-worker ring metadata, one per dataplane worker.
//
// The writer-private and the published halves sit on separate cache
// lines, each followed by an unused guard line: a CPU that prefetches
// lines in adjacent pairs then never pulls a half a reader polls together
// with a half the writer stores to, of this worker or a neighbouring one,
// whatever the array's alignment. The positions are logical, unmasked byte
// offsets into the data area; the ring size minus one masks a logical
// offset down to a physical one. The data area lives behind a
// shared-memory relative pointer, resolved to an address in the reading
// process's own mapping before use.
struct ring_worker {
	struct ring_worker_private local;
	uint8_t local_guard[YANET_CACHE_LINE_SIZE];
	struct ring_worker_published published;
	uint8_t published_guard[YANET_CACHE_LINE_SIZE];
} __attribute__((aligned(YANET_CACHE_LINE_SIZE)));

_Static_assert(
	sizeof(struct ring_worker_private) == YANET_CACHE_LINE_SIZE,
	"the writer-private ring metadata must fill exactly one cache line"
);
_Static_assert(
	sizeof(struct ring_worker_published) == YANET_CACHE_LINE_SIZE,
	"the published ring metadata must fill exactly one cache line"
);
_Static_assert(
	sizeof(struct ring_worker) == 4 * YANET_CACHE_LINE_SIZE,
	"ring_worker must be its two halves and their guard lines"
);
_Static_assert(
	_Alignof(struct ring_worker) == YANET_CACHE_LINE_SIZE,
	"ring_worker must be aligned to exactly one cache line"
);

// Set both copies of the write and readable positions.
//
// Only for setup and tests, while neither the writer nor a reader runs.
static inline void
ring_worker_set_positions(
	struct ring_worker *ring, uint64_t write_idx, uint64_t readable_idx
) {
	ring->local.write_idx = write_idx;
	ring->local.readable_idx = readable_idx;
	atomic_store_explicit(
		&ring->published.readable_idx,
		readable_idx,
		memory_order_release
	);
	atomic_store_explicit(
		&ring->published.write_idx, write_idx, memory_order_release
	);
}

// Make an eviction visible to readers before any byte of the evicted
// records is overwritten.
//
// A release store only orders the accesses before it, so a weakly ordered
// CPU (arm64) may expose the new bytes ahead of the new readable position;
// a reader copying them would then pass its post-copy recheck and accept a
// torn record. Cost: one barrier on arm64 per evicting prepare (`dmb ish`,
// or `dmb ishld` plus `dmb ishst` from newer GCC), which also waits for
// the readable-position store to reach the published line, a round trip
// when a reader holds that line; no instruction on x86-64, where it only
// keeps the compiler from moving the data stores above it.
static inline void
ring_evict_fence(void) {
	// arm64 check branch only: build with -DRING_TEST_NO_EVICT_FENCE to
	// reproduce the pre-fence writer.
#ifndef RING_TEST_NO_EVICT_FENCE
	atomic_thread_fence(memory_order_release);
#endif
}

// Check that a record of the given length fits the ring, evicting whole
// oldest records until it does, without writing any record bytes.
//
// Returns 0 on success, or -1 with errno EINVAL for a length below the frame
// size or E2BIG for one above the ring's capacity; a failure leaves every
// index untouched. The data area arrives already resolved to a local
// address, so repeated calls for one record do not each resolve it. An
// eviction publishes only its final readable position, once.
static inline int
ring_worker_prepare(
	struct ring_worker *ring, uint8_t *data, uint32_t total_len
) {
	if (unlikely(total_len < RING_RECORD_FRAME_SIZE)) {
		errno = EINVAL;
		return -1;
	}
	if (unlikely(total_len > ring->local.size)) {
		errno = E2BIG;
		return -1;
	}
	// The raw length is checked before alignment so it cannot wrap; a
	// power-of-two capacity then bounds the aligned length too.
	uint32_t aligned_total_len = ring_align4(total_len);

	// The writer works from its private positions alone and never loads
	// the published line a reader may be polling.
	uint64_t write_idx = ring->local.write_idx;
	uint64_t readable_idx = ring->local.readable_idx;
	uint64_t free_limit = ring->local.size - aligned_total_len;
	if (write_idx - readable_idx <= free_limit) {
		return 0;
	}

	// Walk whole oldest records until this one fits, then publish the
	// final position once.
	//
	// A reader only needs the new boundary visible before any byte it
	// covers is overwritten, so one release store plus the fence below
	// suffices; the readable position is not an eviction counter. A
	// corrupt length at the walk position (below a frame, above the ring,
	// or past the write position) drops everything up to the write
	// position. The raw length is range-checked before alignment so it
	// cannot wrap.
	do {
		uint8_t *pos = data + (readable_idx & ring->local.mask);
		uint32_t evicted_len;
		memcpy(&evicted_len, pos, sizeof(evicted_len));

		if (unlikely(
			    evicted_len < RING_RECORD_FRAME_SIZE ||
			    evicted_len > ring->local.size ||
			    readable_idx + ring_align4(evicted_len) > write_idx
		    )) {
			readable_idx = write_idx;
			break;
		}
		readable_idx += ring_align4(evicted_len);
	} while (write_idx - readable_idx > free_limit);

	ring->local.readable_idx = readable_idx;
	atomic_store_explicit(
		&ring->published.readable_idx,
		readable_idx,
		memory_order_release
	);
	ring_evict_fence();
	return 0;
}

// Copy one chunk of a not yet published record at the given offset from its
// start, wrapping at the ring's physical end.
//
// A record may be assembled from several chunks at increasing offsets, for
// instance a fixed private header followed by payload bytes with no
// intermediate copy, as long as every chunk lands within the length the
// preceding prepare reserved.
static inline void
ring_worker_write(
	struct ring_worker *ring,
	uint8_t *data,
	uint64_t offset,
	const uint8_t *payload,
	uint64_t size
) {
	assert(ring->local.size >= offset + size);

	uint64_t write_idx = ring->local.write_idx;
	uint64_t written = 0;
	while (written < size) {
		uint64_t pos =
			(write_idx + offset + written) & ring->local.mask;
		uint64_t tail = ring->local.size - pos;
		uint64_t remaining = size - written;
		uint64_t chunk = remaining > tail ? tail : remaining;

		assert(chunk > 0);
		memcpy(data + pos, payload + written, chunk);
		written += chunk;
	}
}

// Write the record frame, stamp it with the worker's next sequence number
// and publish the record to readers.
//
// Publication is a release store of the writer's private position: this
// worker is the ring's sole writer, so the update needs no
// read-modify-write. Returns the stamped sequence number, which wraps from
// UINT32_MAX to 0.
static inline uint32_t
ring_worker_commit(
	struct ring_worker *ring, uint8_t *data, uint32_t total_len
) {
	uint32_t seqno = ring->local.next_seqno;
	ring->local.next_seqno = seqno + 1;

	struct ring_record_frame frame = {
		.total_len = total_len, .seqno = seqno
	};
	ring_worker_write(
		ring, data, 0, (const uint8_t *)&frame, sizeof(frame)
	);

	uint64_t next_write_idx =
		ring->local.write_idx + ring_align4(total_len);
	ring->local.write_idx = next_write_idx;
	atomic_store_explicit(
		&ring->published.write_idx, next_write_idx, memory_order_release
	);

	return seqno;
}
