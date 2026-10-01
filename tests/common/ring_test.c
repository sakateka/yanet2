/*
 * Tests for the ring writer's on-wire behavior: wrap, eviction, invalid
 * sizes, sequence-counter wraparound and multi-worker isolation.
 *
 * A full ring evicts whole records at a record boundary, a corrupt length
 * drops the backlog instead of walking it, and an invalid record size never
 * touches the ring or its sequence counter.
 */

#include "common/test_assert.h"

#include "common/ring.h"

#include "lib/logging/log.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Build a zeroed ring of the given size and its data area, which the caller
// frees; the data area is NULL when its allocation fails.
static struct ring_worker
init_test_ring(uint32_t size, uint8_t **data) {
	struct ring_worker ring = {0};
	*data = calloc(1, size);
	ring.local.size = size;
	ring.local.mask = size - 1;
	return ring;
}

// Reader-visible readable position, after checking that the writer
// published exactly its own copy.
static long
published_readable(struct ring_worker *ring) {
	uint64_t published = atomic_load(&ring->published.readable_idx);
	if (published != ring->local.readable_idx) {
		LOG(ERROR,
		    "published readable position %lu differs from the "
		    "writer's %lu",
		    (unsigned long)published,
		    (unsigned long)ring->local.readable_idx);
		abort();
	}
	return (long)published;
}

// Reader-visible write position, after checking that the writer published
// exactly its own copy.
static long
published_write(struct ring_worker *ring) {
	uint64_t published = atomic_load(&ring->published.write_idx);
	if (published != ring->local.write_idx) {
		LOG(ERROR,
		    "published write position %lu differs from the writer's "
		    "%lu",
		    (unsigned long)published,
		    (unsigned long)ring->local.write_idx);
		abort();
	}
	return (long)published;
}

// A record straddling the ring's physical boundary round-trips its opaque
// payload byte-for-byte.
static int
run_ring_wrap_roundtrip_test() {
	const uint32_t ring_size = 32;
	uint8_t *data;
	struct ring_worker ring = init_test_ring(ring_size, &data);
	TEST_ASSERT_NOT_NULL(data, "failed to allocate ring data");

	// A write position 12 bytes before the end keeps the 8-byte frame
	// inside the boundary and wraps the payload: [28,32) then [0,4).
	ring_worker_set_positions(&ring, ring_size - 12, ring_size - 12);

	const uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	uint32_t total_len = RING_RECORD_FRAME_SIZE + sizeof(payload);

	TEST_ASSERT_EQUAL(
		ring_worker_prepare(&ring, data, total_len),
		0,
		"prepare must succeed on an empty ring"
	);
	ring_worker_write(
		&ring, data, RING_RECORD_FRAME_SIZE, payload, sizeof(payload)
	);
	ring_worker_commit(&ring, data, total_len);

	uint8_t roundtrip[8];
	for (size_t i = 0; i < sizeof(payload); ++i) {
		uint64_t pos = (ring_size - 12 + RING_RECORD_FRAME_SIZE + i) &
			       ring.local.mask;
		roundtrip[i] = data[pos];
	}
	TEST_ASSERT_EQUAL(
		memcmp(roundtrip, payload, sizeof(payload)),
		0,
		"payload must round-trip unchanged across the physical wrap"
	);

	free(data);
	return TEST_SUCCESS;
}

// Write one fixed-size record whose payload repeats one byte, so a later
// read can tell which record occupies a slot.
//
// Aborts if the record is refused, since every caller passes a valid size.
static void
write_fixed_record(
	struct ring_worker *ring,
	uint8_t *data,
	uint8_t fill,
	uint32_t total_len
) {
	uint32_t payload_len = total_len - RING_RECORD_FRAME_SIZE;
	uint8_t payload[64];
	memset(payload, fill, payload_len);

	if (ring_worker_prepare(ring, data, total_len) != 0) {
		LOG(ERROR, "ring_worker_prepare(%u) failed", total_len);
		abort();
	}
	ring_worker_write(
		ring, data, RING_RECORD_FRAME_SIZE, payload, payload_len
	);
	ring_worker_commit(ring, data, total_len);
}

// A full ring evicts whole oldest records, landing the readable position on
// a record boundary, and leaves every surviving record's bytes intact.
static int
run_ring_overwrite_evicts_whole_records_test() {
	const uint32_t ring_size = 64;
	const uint32_t record_len = 16;
	uint8_t *data;
	struct ring_worker ring = init_test_ring(ring_size, &data);
	TEST_ASSERT_NOT_NULL(data, "failed to allocate ring data");

	// Fill the ring exactly: four 16-byte records, one per fill byte.
	write_fixed_record(&ring, data, 0xA0, record_len);
	write_fixed_record(&ring, data, 0xA1, record_len);
	write_fixed_record(&ring, data, 0xA2, record_len);
	write_fixed_record(&ring, data, 0xA3, record_len);
	TEST_ASSERT_EQUAL(
		published_readable(&ring), 0L, "a full ring must not evict yet"
	);

	// A fifth record forces exactly one eviction to make room.
	write_fixed_record(&ring, data, 0xA4, record_len);
	TEST_ASSERT_EQUAL(
		published_readable(&ring),
		(long)record_len,
		"eviction must land on a record boundary"
	);

	// Records 1-3 occupy physical [16,64) and must be untouched.
	uint8_t expected;
	expected = 0xA1;
	for (uint32_t i = 16 + RING_RECORD_FRAME_SIZE; i < 32; ++i) {
		TEST_ASSERT_EQUAL(
			data[i], expected, "record 1 payload must survive"
		);
	}
	expected = 0xA2;
	for (uint32_t i = 32 + RING_RECORD_FRAME_SIZE; i < 48; ++i) {
		TEST_ASSERT_EQUAL(
			data[i], expected, "record 2 payload must survive"
		);
	}
	expected = 0xA3;
	for (uint32_t i = 48 + RING_RECORD_FRAME_SIZE; i < 64; ++i) {
		TEST_ASSERT_EQUAL(
			data[i], expected, "record 3 payload must survive"
		);
	}
	// Record 4 overwrote record 0's physical slot at [0,16).
	expected = 0xA4;
	for (uint32_t i = RING_RECORD_FRAME_SIZE; i < 16; ++i) {
		TEST_ASSERT_EQUAL(
			data[i],
			expected,
			"record 4 must occupy the evicted slot"
		);
	}

	free(data);
	return TEST_SUCCESS;
}

// One reservation that must free several records evicts all of them and
// lands the readable position on the boundary after the last one.
static int
run_ring_eviction_spans_multiple_records_test() {
	const uint32_t ring_size = 64;
	uint8_t *data;
	struct ring_worker ring = init_test_ring(ring_size, &data);
	TEST_ASSERT_NOT_NULL(data, "failed to allocate ring data");

	// Four 16-byte records fill the ring; a 40-byte record needs three of
	// them gone, leaving only the fourth (at [48,64)) readable.
	write_fixed_record(&ring, data, 0xB0, 16);
	write_fixed_record(&ring, data, 0xB1, 16);
	write_fixed_record(&ring, data, 0xB2, 16);
	write_fixed_record(&ring, data, 0xB3, 16);

	TEST_ASSERT_EQUAL(
		ring_worker_prepare(&ring, data, 40),
		0,
		"prepare must succeed by evicting"
	);
	TEST_ASSERT_EQUAL(
		published_readable(&ring),
		48L,
		"eviction must stop on the first boundary that fits the record"
	);
	TEST_ASSERT_EQUAL(
		published_write(&ring), 64L, "prepare must not move write_idx"
	);

	free(data);
	return TEST_SUCCESS;
}

// A corrupt length at the oldest record drops the whole backlog, catching
// the readable position up to the write position, never mid-record.
//
// Corrupt means zero, shorter than a frame, larger than the ring (including
// values whose alignment wraps u32), or running past the write position.
static int
run_ring_eviction_corrupt_length_catches_up_test() {
	const uint32_t ring_size = 64;
	uint8_t *data;
	struct ring_worker ring = init_test_ring(ring_size, &data);
	TEST_ASSERT_NOT_NULL(data, "failed to allocate ring data");

	write_fixed_record(&ring, data, 0xC0, 16);
	write_fixed_record(&ring, data, 0xC1, 16);
	write_fixed_record(&ring, data, 0xC2, 16);
	write_fixed_record(&ring, data, 0xC3, 16);

	struct {
		const char *name;
		uint32_t total_len;
	} cases[] = {
		{"zero length", 0},
		{"length 1", 1},
		{"length 2", 2},
		{"length 3", 3},
		{"length 4", 4},
		{"length 5", 5},
		{"length 6", 6},
		{"length 7", 7},
		{"length past the write position", 32 + 64},
		{"length one above the ring size", 64 + 1},
		{"length near UINT32_MAX", UINT32_MAX - 3},
		{"length whose alignment wraps", UINT32_MAX},
	};
	// The oldest frame's sequence word holds a plausible length and the
	// probe record needs only one frame of room.
	//
	// A walk that trusted a short length would step 4 or 8 bytes into the
	// record and stop there, mid-record.
	const uint32_t planted_len = 4;
	memcpy(data + sizeof(uint32_t), &planted_len, sizeof(planted_len));
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		// Rewind to the full ring and corrupt the oldest frame.
		ring_worker_set_positions(&ring, ring.local.write_idx, 0);
		memcpy(data, &cases[i].total_len, sizeof(cases[i].total_len));

		TEST_ASSERT_EQUAL(
			ring_worker_prepare(
				&ring, data, RING_RECORD_FRAME_SIZE
			),
			0,
			"%s: prepare must still succeed",
			cases[i].name
		);
		TEST_ASSERT_EQUAL(
			published_readable(&ring),
			published_write(&ring),
			"%s: readable_idx must catch up to write_idx",
			cases[i].name
		);
	}

	free(data);
	return TEST_SUCCESS;
}

// A record whose declared size is below the frame is refused before any
// index or byte is touched.
static int
run_ring_prepare_rejects_undersize_test() {
	const uint32_t ring_size = 32;
	uint8_t *data;
	struct ring_worker ring = init_test_ring(ring_size, &data);
	TEST_ASSERT_NOT_NULL(data, "failed to allocate ring data");

	int rc = ring_worker_prepare(&ring, data, RING_RECORD_FRAME_SIZE - 1);
	TEST_ASSERT_EQUAL(rc, -1, "below the frame size must be rejected");
	TEST_ASSERT_EQUAL(
		errno, EINVAL, "below the frame size must set EINVAL"
	);
	TEST_ASSERT_EQUAL(
		published_write(&ring),
		0L,
		"a rejected prepare must not move write_idx"
	);
	TEST_ASSERT_EQUAL(
		published_readable(&ring),
		0L,
		"a rejected prepare must not move readable_idx"
	);

	free(data);
	return TEST_SUCCESS;
}

// A length above the ring's capacity is rejected as oversize, including one
// whose 4-byte alignment wraps u32 to a small value: the raw value is
// checked before alignment.
static int
run_ring_prepare_rejects_oversize_alignment_wraparound_test() {
	const uint32_t ring_size = 32;
	uint8_t *data;
	struct ring_worker ring = init_test_ring(ring_size, &data);
	TEST_ASSERT_NOT_NULL(data, "failed to allocate ring data");

	uint32_t oversize_values[] = {
		ring_size + 1, ring_size + 4, 0xFFFFFFFD, 0xFFFFFFFE, 0xFFFFFFFF
	};
	for (size_t i = 0;
	     i < sizeof(oversize_values) / sizeof(oversize_values[0]);
	     ++i) {
		uint32_t total_len = oversize_values[i];

		int rc = ring_worker_prepare(&ring, data, total_len);
		TEST_ASSERT_EQUAL(
			rc,
			-1,
			"total_len %u must be rejected as oversize",
			total_len
		);
		TEST_ASSERT_EQUAL(
			errno, E2BIG, "total_len %u must set E2BIG", total_len
		);
		TEST_ASSERT_EQUAL(
			published_write(&ring),
			0L,
			"total_len %u must not move write_idx",
			total_len
		);
		TEST_ASSERT_EQUAL(
			published_readable(&ring),
			0L,
			"total_len %u must not move readable_idx",
			total_len
		);
		TEST_ASSERT_EQUAL(
			(long)ring.local.next_seqno,
			0L,
			"total_len %u must not touch next_seqno",
			total_len
		);
	}

	free(data);
	return TEST_SUCCESS;
}

// The per-worker sequence counter starts at 0, numbers commits contiguously,
// and wraps from UINT32_MAX to 0 without a gap.
static int
run_ring_seqno_wrap_test() {
	const uint32_t ring_size = 32;
	uint8_t *data;
	struct ring_worker ring = init_test_ring(ring_size, &data);
	TEST_ASSERT_NOT_NULL(data, "failed to allocate ring data");

	uint32_t first =
		ring_worker_commit(&ring, data, RING_RECORD_FRAME_SIZE);
	uint32_t second =
		ring_worker_commit(&ring, data, RING_RECORD_FRAME_SIZE);
	TEST_ASSERT_EQUAL((long)first, 0L, "first commit must be seqno 0");
	TEST_ASSERT_EQUAL(
		(long)second, (long)first + 1, "commits must be contiguous"
	);

	ring.local.next_seqno = UINT32_MAX;

	uint32_t seqno =
		ring_worker_commit(&ring, data, RING_RECORD_FRAME_SIZE);
	TEST_ASSERT_EQUAL(
		(long)seqno,
		(long)UINT32_MAX,
		"the last seqno before wrap must be used"
	);

	seqno = ring_worker_commit(&ring, data, RING_RECORD_FRAME_SIZE);
	TEST_ASSERT_EQUAL((long)seqno, 0L, "seqno must wrap to 0 contiguously");

	free(data);
	return TEST_SUCCESS;
}

// Writes to one worker's ring never perturb an adjacent worker's metadata
// or data when both sit in one cache-line-strided array, as in the object.
static int
run_ring_multi_worker_isolation_test() {
	struct ring_worker workers[2] = {0};
	uint8_t *data0;
	uint8_t *data1;
	workers[0] = init_test_ring(64, &data0);
	workers[1] = init_test_ring(64, &data1);
	TEST_ASSERT_NOT_NULL(data0, "failed to allocate worker 0 data");
	TEST_ASSERT_NOT_NULL(data1, "failed to allocate worker 1 data");

	TEST_ASSERT_EQUAL(
		(long)((uint8_t *)&workers[1] - (uint8_t *)&workers[0]),
		(long)sizeof(struct ring_worker),
		"adjacent workers must be exactly one struct apart, leaving no "
		"gap a stride bug could hide in"
	);

	struct ring_worker snapshot = workers[1];
	uint8_t data1_snapshot[64];
	memcpy(data1_snapshot, data1, sizeof(data1_snapshot));

	for (int i = 0; i < 32; ++i) {
		write_fixed_record(&workers[0], data0, (uint8_t)i, 16);
	}

	TEST_ASSERT_EQUAL(
		memcmp(&workers[1], &snapshot, sizeof(struct ring_worker)),
		0,
		"worker 1's entire metadata struct must be untouched by worker "
		"0's writes"
	);
	TEST_ASSERT_EQUAL(
		memcmp(data1, data1_snapshot, sizeof(data1_snapshot)),
		0,
		"worker 1's data area must be untouched by worker 0's writes"
	);

	free(data0);
	free(data1);
	return TEST_SUCCESS;
}

int
main(void) {
	log_enable_name("debug");

	struct test_case {
		const char *name;
		int (*func)();
	};

	struct test_case cases[] = {
		{"wrap_roundtrip", run_ring_wrap_roundtrip_test},
		{"overwrite_evicts_whole_records",
		 run_ring_overwrite_evicts_whole_records_test},
		{"eviction_spans_multiple_records",
		 run_ring_eviction_spans_multiple_records_test},
		{"eviction_corrupt_length_catches_up",
		 run_ring_eviction_corrupt_length_catches_up_test},
		{"prepare_rejects_undersize",
		 run_ring_prepare_rejects_undersize_test},
		{"prepare_rejects_oversize_alignment_wraparound",
		 run_ring_prepare_rejects_oversize_alignment_wraparound_test},
		{"seqno_wrap", run_ring_seqno_wrap_test},
		{"multi_worker_isolation", run_ring_multi_worker_isolation_test
		},
	};

	int failed = 0;
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		if (cases[i].func() != TEST_SUCCESS) {
			LOG(ERROR, "%s failed", cases[i].name);
			failed = 1;
			continue;
		}
		LOG(INFO, "%s passed", cases[i].name);
	}

	return failed;
}
