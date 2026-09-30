#pragma once

/*
 * Generic per-worker ring buffer of opaque records, shared by any module
 * that needs a lock-free single-producer log readable from another
 * process. No producer uses it yet; pdump capture still keeps its own,
 * separate rings.
 *
 * A worker's metadata and its data area are two independent allocations;
 * cache-line isolating the metadata keeps concurrent writers on adjacent
 * workers from ever touching the same line. The writer never blocks: a
 * full ring evicts whole oldest records rather than stalling.
 */

#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "common/cache.h"
#include "common/memory_address.h"

#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

// Round a record length up to the 4-byte boundary every record starts at.
static inline uint32_t
ring_align4(uint32_t val) {
	return (val + 3) & ~(uint32_t)3;
}

// Frame preceding every record's opaque payload.
//
// total_len covers the frame and the payload combined; seqno is the
// worker-scoped sequence number the frame is stamped with at commit.
// Records are only 4-byte aligned, so a reader must copy this frame out
// before reading total_len, never dereference it in place.
struct ring_record_frame {
	uint32_t total_len;
	uint32_t seqno;
};

_Static_assert(
	sizeof(struct ring_record_frame) == 8,
	"ring record frame must be the 8-byte wire size"
);

// Smallest total_len a record may declare: the frame with an empty payload.
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
// Release ordering on the eviction itself only covers earlier accesses, so
// a weakly ordered CPU (arm64) may expose the new bytes first; a reader
// copying them would then pass its post-copy recheck and accept a torn
// record. Cost: no instruction on x86-64, which never reorders stores; one
// `dmb ish` on arm64, paid only by records that evicted something. It
// stalls later memory accesses until earlier ones are complete, roughly
// tens of cycles, and moves no data and makes no memory traffic.
static inline void
ring_evict_fence(void) {
	// arm64 check branch only: build with -DRING_TEST_NO_EVICT_FENCE to
	// reproduce the pre-fence writer.
#ifndef RING_TEST_NO_EVICT_FENCE
	atomic_thread_fence(memory_order_release);
#endif
}

// Validate a record fits the ring and evict whole oldest records until it
// does, without writing anything.
//
// Returns 0 on success. Returns -1 with errno EINVAL when total_len is
// below the frame size or E2BIG when its aligned span exceeds the ring's
// capacity; either failure leaves every index untouched. data is the
// worker's data area, already resolved by the caller via ADDR_OF so
// repeated calls for the same record do not each re-resolve it.
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
	// ring->size is a power of two (checked at object creation), so
	// aligning any total_len that passed the check above can never
	// exceed it; checking the raw value first, before alignment, is
	// what keeps a total_len near UINT32_MAX from wrapping to a small
	// aligned length and slipping past this guard.
	uint32_t aligned_total_len = ring_align4(total_len);

	// While the occupied space (write_idx - readable_idx) exceeds the
	// space this record needs, advance readable_idx to evict old
	// records.
	bool evicted = false;
	while ((ring->write_idx - ring->readable_idx) >
	       (ring->size - aligned_total_len)) {
		uint8_t *pos = data + (ring->readable_idx & ring->mask);
		uint32_t evicted_len;
		memcpy(&evicted_len, pos, sizeof(evicted_len));
		evicted_len = ring_align4(evicted_len);

		if (unlikely(
			    !evicted_len ||
			    ring->readable_idx + evicted_len > ring->write_idx
		    )) {
			// Invalid data at the current position: advancing
			// further would either exceed write_idx or loop
			// forever. Drop everything by catching up to
			// write_idx instead.
			atomic_store_explicit(
				&ring->readable_idx,
				ring->write_idx,
				memory_order_release
			);
			ring_evict_fence();
			return 0;
		}

		atomic_fetch_add_explicit(
			&ring->readable_idx, evicted_len, memory_order_release
		);
		evicted = true;
	}

	if (evicted) {
		ring_evict_fence();
	}

	return 0;
}

// Copy one chunk of a record's bytes at offset bytes from the record's
// (not yet published) start, wrapping at the ring's physical boundary.
//
// A record may be assembled from several chunks written at increasing
// offsets — for instance a fixed private header followed by payload bytes
// with no intermediate copy — as long as every chunk lands within the
// total_len reserved by ring_worker_prepare.
static inline void
ring_worker_write(
	struct ring_worker *ring,
	uint8_t *data,
	uint64_t offset,
	const uint8_t *payload,
	uint64_t size
) {
	assert(ring->size >= offset + size);

	uint64_t written = 0;
	while (written < size) {
		uint64_t pos =
			(ring->write_idx + offset + written) & ring->mask;
		uint64_t tail = ring->size - pos;
		uint64_t remaining = size - written;
		uint64_t chunk = remaining > tail ? tail : remaining;

		assert(chunk > 0);
		memcpy(data + pos, payload + written, chunk);
		written += chunk;
	}
}

// Write the record frame, stamp and advance the worker's sequence counter,
// and publish the record by making it visible to readers.
//
// Publication is a plain release store of the position this call computes
// locally, not a read-modify-write: this worker is the sole writer of its
// ring, so nothing else can race the update, and write_idx never
// needs a fetch_add's atomicity here. Returns the seqno this record was
// stamped with, wrapping from UINT32_MAX to 0.
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

	uint64_t next_write_idx = ring->write_idx + ring_align4(total_len);
	atomic_store_explicit(
		&ring->write_idx, next_write_idx, memory_order_release
	);

	return seqno;
}
