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
 * records instead of stalling. Records are committed one by one and
 * published in batches, so one store to the reader-visible line covers a
 * whole batch, and eviction frees space in chunks, so one store and one
 * fence cover many evicted records. No producer writes to it yet; pdump
 * capture keeps its own rings.
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

// Largest eviction chunk, in bytes.
//
// An evicting write frees at least this much space (or its own length, if
// larger), so in steady overflow one readable-position store and one fence
// cover every record that fits in the chunk: tens of small records. A
// larger chunk drops more history ahead of time and makes each eviction
// walk read more record frames at once, lines a reader may hold.
#define RING_EVICT_CHUNK_MAX 4096u
// Eviction chunk of a small ring as a divisor of its capacity, so the
// chunk never drops more than this share of the history ahead of time.
#define RING_EVICT_CHUNK_SHARE 16u

// Writer-private half of a worker's ring metadata.
//
// The positions are the writer's authoritative copies of the published
// ones: no reader loads them, so the per-record stores here never pull the
// line away from the writer's CPU. The last published write position marks
// the start of the batch of committed records not yet published, and the
// eviction cursor is the record boundary the writer has walked to ahead of
// the next eviction. The size, mask, eviction chunk and relative data
// pointer are fixed at creation; a reader loads the size, mask and data
// pointer once when it attaches.
struct ring_worker_private {
	uint64_t write_idx;
	uint64_t readable_idx;
	uint64_t published_write_idx;
	uint64_t evict_idx;
	uint8_t *data;
	uint32_t next_seqno;
	uint32_t size;
	uint32_t mask;
	uint32_t evict_chunk;
} __attribute__((aligned(YANET_CACHE_LINE_SIZE)));

// Reader-visible half of a worker's ring metadata.
//
// The writer only release-stores these and never loads them, so a reader
// polling the line costs the writer at most an ownership request per
// publication or eviction, not one per record.
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

// Eviction chunk of a ring of the given power-of-two capacity: a share of
// the capacity, capped, and a multiple of the record alignment.
static inline uint32_t
ring_evict_chunk(uint32_t size) {
	uint32_t chunk = size / RING_EVICT_CHUNK_SHARE;
	if (chunk > RING_EVICT_CHUNK_MAX) {
		chunk = RING_EVICT_CHUNK_MAX;
	}
	return chunk & ~3u;
}

// Set up an empty ring of the given power-of-two capacity, leaving the
// data area pointer to the caller.
//
// Only while neither the writer nor a reader runs.
static inline void
ring_worker_init(struct ring_worker *ring, uint32_t size) {
	memset(ring, 0, sizeof(*ring));
	ring->local.size = size;
	ring->local.mask = size - 1;
	ring->local.evict_chunk = ring_evict_chunk(size);
}

// Set every copy of the write and readable positions, leaving no
// unpublished batch.
//
// Only for setup and tests, while neither the writer nor a reader runs.
static inline void
ring_worker_set_positions(
	struct ring_worker *ring, uint64_t write_idx, uint64_t readable_idx
) {
	ring->local.write_idx = write_idx;
	ring->local.readable_idx = readable_idx;
	ring->local.published_write_idx = write_idx;
	ring->local.evict_idx = readable_idx;
	atomic_store_explicit(
		&ring->published.readable_idx,
		readable_idx,
		memory_order_release
	);
	atomic_store_explicit(
		&ring->published.write_idx, write_idx, memory_order_release
	);
}

// Largest total, in bytes, of one batch: the aligned lengths of the
// records committed between two publications, the one being prepared
// included.
//
// Within it, evicting every published record always frees a whole chunk
// beyond the record, so a batch never has to evict its own records. It is
// also the largest record a ring accepts.
static inline uint32_t
ring_worker_batch_max(const struct ring_worker *ring) {
	return ring->local.size - ring->local.evict_chunk;
}

// Bytes the unpublished batch may still grow by before it must be
// published.
//
// A producer whose next record's aligned length exceeds it publishes
// first.
static inline uint64_t
ring_worker_batch_room(const struct ring_worker *ring) {
	uint64_t pending =
		ring->local.write_idx - ring->local.published_write_idx;
	return ring_worker_batch_max(ring) - pending;
}

// Make an eviction visible to readers before any byte of the evicted
// records is overwritten.
//
// A release store only orders the accesses before it, so a weakly ordered
// CPU (arm64) may expose the new bytes ahead of the new readable position;
// a reader copying them would then pass its post-copy recheck and accept a
// torn record. Cost: one barrier on arm64 per eviction chunk (`dmb ish`,
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

// Logical position just past the whole record at a readable position, or
// the bound when the record's length is corrupt.
//
// Corrupt means below a frame, above the ring, or running past the bound;
// the raw length is range-checked before alignment so it cannot wrap.
static inline uint64_t
ring_evict_next(
	const struct ring_worker *ring,
	const uint8_t *data,
	uint64_t readable_idx,
	uint64_t bound
) {
	uint32_t len;
	memcpy(&len, data + (readable_idx & ring->local.mask), sizeof(len));
	if (unlikely(
		    len < RING_RECORD_FRAME_SIZE || len > ring->local.size ||
		    readable_idx + ring_align4(len) > bound
	    )) {
		return bound;
	}
	return readable_idx + ring_align4(len);
}

// Walk the eviction cursor one published record further while it is less
// than a chunk ahead of the readable position.
//
// Called on every write that needs no eviction, so in steady overflow the
// cursor keeps pace with the writer one frame load at a time, overlapped
// with the write, and the next eviction finds its chunk already walked
// instead of stalling on a chain of dependent loads. It only reads frames.
static inline void
ring_worker_walk_ahead(struct ring_worker *ring, const uint8_t *data) {
	uint64_t evict_idx = ring->local.evict_idx;
	uint64_t batch_idx = ring->local.published_write_idx;
	if (evict_idx - ring->local.readable_idx < ring->local.evict_chunk &&
	    evict_idx < batch_idx) {
		ring->local.evict_idx =
			ring_evict_next(ring, data, evict_idx, batch_idx);
	}
}

// Evict whole oldest records until a chunk of space is free, then publish
// the new readable position once and fence it off from the overwrites.
//
// The walk resumes from the eviction cursor. Only published records are
// evicted, so the unpublished batch, which no reader has seen, never evicts
// itself; a corrupt length drops every published record in one step. A
// batch over its limit, which only a build without assertions lets
// through, evicts its own oldest records as a last resort: they are lost,
// and the reader sees a sequence gap.
static inline void
ring_worker_evict(
	struct ring_worker *ring, const uint8_t *data, uint32_t aligned_len
) {
	uint64_t write_idx = ring->local.write_idx;
	uint64_t batch_idx = ring->local.published_write_idx;
	uint64_t readable_idx = ring->local.evict_idx;
	if (readable_idx < ring->local.readable_idx) {
		readable_idx = ring->local.readable_idx;
	}

	uint32_t free_target = ring->local.evict_chunk;
	if (free_target < aligned_len) {
		free_target = aligned_len;
	}
	uint64_t keep_limit = ring->local.size - free_target;
	while (write_idx - readable_idx > keep_limit && readable_idx < batch_idx
	) {
		readable_idx =
			ring_evict_next(ring, data, readable_idx, batch_idx);
	}
	uint64_t fit_limit = ring->local.size - aligned_len;
	while (unlikely(write_idx - readable_idx > fit_limit)) {
		readable_idx =
			ring_evict_next(ring, data, readable_idx, write_idx);
	}

	ring->local.readable_idx = readable_idx;
	ring->local.evict_idx = readable_idx;
	atomic_store_explicit(
		&ring->published.readable_idx,
		readable_idx,
		memory_order_release
	);
	ring_evict_fence();
}

// Check that a record of the given length fits the ring, evicting a chunk
// of whole oldest records when it does not, without writing any record
// bytes.
//
// Returns 0 on success, or -1 with errno EINVAL for a length below the
// frame size or E2BIG for one above the batch limit; a failure leaves
// every index untouched. The caller keeps each batch within the batch
// limit, which an assertion checks. The data area arrives already resolved
// to a local address, so repeated calls for one record do not each
// resolve it.
static inline int
ring_worker_prepare(
	struct ring_worker *ring, uint8_t *data, uint32_t total_len
) {
	if (unlikely(total_len < RING_RECORD_FRAME_SIZE)) {
		errno = EINVAL;
		return -1;
	}
	// The raw length is checked before alignment so it cannot wrap; the
	// limit is a multiple of the alignment, so it bounds the aligned
	// length too.
	if (unlikely(total_len > ring_worker_batch_max(ring))) {
		errno = E2BIG;
		return -1;
	}
	uint32_t aligned_total_len = ring_align4(total_len);
	assert(aligned_total_len <= ring_worker_batch_room(ring));

	// The writer works from its private positions alone and never loads
	// the published line a reader may be polling.
	uint64_t occupied = ring->local.write_idx - ring->local.readable_idx;
	if (likely(occupied <= ring->local.size - aligned_total_len)) {
		ring_worker_walk_ahead(ring, data);
		return 0;
	}
	ring_worker_evict(ring, data, aligned_total_len);
	return 0;
}

// Copy one chunk of a not yet committed record at the given offset from its
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
// and add the record to the unpublished batch.
//
// Readers see nothing of it until the next publication. Returns the stamped
// sequence number, which wraps from UINT32_MAX to 0.
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

	ring->local.write_idx += ring_align4(total_len);
	return seqno;
}

// Publish every record committed since the last publication with one
// release store of the write position; a no-op when there is none.
//
// A producer calls it once at the end of each call or burst. This worker is
// the ring's sole writer, so the update needs no read-modify-write, and the
// writer compares against its private copy instead of loading the
// published line.
static inline void
ring_worker_publish(struct ring_worker *ring) {
	uint64_t write_idx = ring->local.write_idx;
	if (write_idx == ring->local.published_write_idx) {
		return;
	}
	ring->local.published_write_idx = write_idx;
	atomic_store_explicit(
		&ring->published.write_idx, write_idx, memory_order_release
	);
}
