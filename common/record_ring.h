#pragma once

/*
 * Per-worker ring buffer of opaque records: a lock-free single-producer log
 * that another process can read.
 *
 * A worker's metadata and its data area are two independent allocations;
 * cache-line isolating the metadata keeps writers on adjacent workers from
 * sharing a line. The writer never blocks: a full ring evicts whole oldest
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

// Per-worker ring metadata, one per dataplane worker.
//
// Cache-line aligned and sized so consecutive workers in an array never
// share a line under concurrent single-writer access. The write and
// readable positions are logical, unmasked byte offsets into the data
// area; the ring size minus one masks a logical offset down to a
// physical one. The data area itself lives behind a shared-memory
// relative pointer, resolved to an address in the reading process's own
// mapping before use.
struct ring_worker {
	_Atomic uint64_t write_idx;
	_Atomic uint64_t readable_idx;
	uint32_t next_seqno;
	uint32_t size;
	uint32_t mask;
	uint8_t *data;
} __attribute__((aligned(YANET_CACHE_LINE_SIZE)));

_Static_assert(
	sizeof(struct ring_worker) % YANET_CACHE_LINE_SIZE == 0,
	"ring_worker size must be a whole number of cache lines"
);
_Static_assert(
	_Alignof(struct ring_worker) == YANET_CACHE_LINE_SIZE,
	"ring_worker must be aligned to exactly one cache line"
);

// Make an eviction visible to readers before any byte of the evicted
// records is overwritten.
//
// A release store only orders the accesses before it, so a weakly ordered
// CPU (arm64) may expose the new bytes ahead of the new readable position;
// a reader copying them would then pass its post-copy recheck and accept a
// torn record. Cost: one barrier on arm64 per evicting prepare (`dmb ish`,
// or `dmb ishld` plus `dmb ishst` from newer GCC), roughly tens of cycles
// with no memory traffic; no instruction on x86-64, where it only keeps
// the compiler from moving the data stores above it.
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
	if (unlikely(total_len > ring->size)) {
		errno = E2BIG;
		return -1;
	}
	// The raw length is checked before alignment so it cannot wrap; a
	// power-of-two capacity then bounds the aligned length too.
	uint32_t aligned_total_len = ring_align4(total_len);

	// This worker is the sole writer of both positions, so relaxed loads
	// return its own latest stores; readers never write them.
	uint64_t write_idx =
		atomic_load_explicit(&ring->write_idx, memory_order_relaxed);
	uint64_t readable_idx =
		atomic_load_explicit(&ring->readable_idx, memory_order_relaxed);
	uint64_t free_limit = ring->size - aligned_total_len;
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
		uint8_t *pos = data + (readable_idx & ring->mask);
		uint32_t evicted_len;
		memcpy(&evicted_len, pos, sizeof(evicted_len));

		if (unlikely(
			    evicted_len < RING_RECORD_FRAME_SIZE ||
			    evicted_len > ring->size ||
			    readable_idx + ring_align4(evicted_len) > write_idx
		    )) {
			readable_idx = write_idx;
			break;
		}
		readable_idx += ring_align4(evicted_len);
	} while (write_idx - readable_idx > free_limit);

	atomic_store_explicit(
		&ring->readable_idx, readable_idx, memory_order_release
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
	assert(ring->size >= offset + size);

	// Sole writer: a relaxed load returns this worker's own last store.
	uint64_t write_idx =
		atomic_load_explicit(&ring->write_idx, memory_order_relaxed);
	uint64_t written = 0;
	while (written < size) {
		uint64_t pos = (write_idx + offset + written) & ring->mask;
		uint64_t tail = ring->size - pos;
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
// Publication is a release store of a locally computed position: this
// worker is the ring's sole writer, so the update needs no
// read-modify-write. Returns the stamped sequence number, which wraps from
// UINT32_MAX to 0.
static inline uint32_t
ring_worker_commit(
	struct ring_worker *ring, uint8_t *data, uint32_t total_len
) {
	uint32_t seqno = ring->next_seqno;
	ring->next_seqno = seqno + 1;

	struct ring_record_frame frame = {
		.total_len = total_len, .seqno = seqno
	};
	ring_worker_write(
		ring, data, 0, (const uint8_t *)&frame, sizeof(frame)
	);

	uint64_t next_write_idx =
		atomic_load_explicit(&ring->write_idx, memory_order_relaxed) +
		ring_align4(total_len);
	atomic_store_explicit(
		&ring->write_idx, next_write_idx, memory_order_release
	);

	return seqno;
}
