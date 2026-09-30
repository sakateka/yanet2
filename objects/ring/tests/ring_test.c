/*
 * Pins the ring writer's on-wire behavior across wrap, eviction, invalid
 * sizes, sequence-counter wraparound, and multi-worker isolation.
 *
 * Physical wrap round-trips the opaque payload untouched, a full ring
 * evicts whole records at a record boundary rather than tearing one in
 * half, an invalid record size never touches the ring or its sequence
 * counter, the sequence counter wraps from UINT32_MAX to 0 without a gap,
 * and one worker's writes never perturb another worker's metadata.
 */

#include "common/test_assert.h"

#include "objects/ring/dataplane/ring.h"

#include "lib/logging/log.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Build a zeroed ring of size bytes, writing the freshly calloc'd data
// area to *data (NULL on allocation failure); the caller frees it.
static struct ring_worker
init_test_ring(uint32_t size, uint8_t **data) {
	struct ring_worker ring = {0};
	*data = calloc(1, size);
	ring.size = size;
	ring.mask = size - 1;
	return ring;
}

// A record straddling the ring's physical boundary round-trips its opaque
// payload byte-for-byte.
static int
run_ring_wrap_roundtrip_test() {
	const uint32_t ring_size = 32;
	uint8_t *data;
	struct ring_worker ring = init_test_ring(ring_size, &data);
	TEST_ASSERT_NOT_NULL(data, "failed to allocate ring data");

	// Placing write_idx at size-12 puts the 8-byte frame just inside the
	// boundary and the 8-byte payload straddling it: physical bytes
	// [28,32) then [0,4).
	ring.write_idx = ring_size - 12;
	ring.readable_idx = ring.write_idx;

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
			       ring.mask;
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

// Write one fixed-size record through the full prepare/write/commit
// sequence, filling its payload with one repeated byte so a later read
// can identify which record occupies a given slot.
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

	ring_worker_prepare(ring, data, total_len);
	ring_worker_write(
		ring, data, RING_RECORD_FRAME_SIZE, payload, payload_len
	);
	ring_worker_commit(ring, data, total_len);
}

// A full ring evicts whole oldest records, landing readable_idx exactly on
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
		(long)ring.readable_idx, 0L, "a full ring must not evict yet"
	);

	// A fifth record forces exactly one eviction to make room.
	write_fixed_record(&ring, data, 0xA4, record_len);
	TEST_ASSERT_EQUAL(
		(long)ring.readable_idx,
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

// A record whose declared size is below the frame or above the ring's
// capacity is refused before any index or byte is touched.
static int
run_ring_prepare_rejects_invalid_size_test() {
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
		(long)ring.write_idx,
		0L,
		"a rejected prepare must not move write_idx"
	);
	TEST_ASSERT_EQUAL(
		(long)ring.readable_idx,
		0L,
		"a rejected prepare must not move readable_idx"
	);

	rc = ring_worker_prepare(&ring, data, ring_size + 4);
	TEST_ASSERT_EQUAL(rc, -1, "above capacity must be rejected");
	TEST_ASSERT_EQUAL(errno, E2BIG, "above capacity must set E2BIG");
	TEST_ASSERT_EQUAL(
		(long)ring.write_idx,
		0L,
		"a rejected prepare must not move write_idx"
	);
	TEST_ASSERT_EQUAL(
		(long)ring.readable_idx,
		0L,
		"a rejected prepare must not move readable_idx"
	);

	free(data);
	return TEST_SUCCESS;
}

// A total_len whose 4-byte alignment wraps u32 back down to a small value
// is still rejected as oversize: aligning must never be trusted before the
// raw value has been checked against capacity.
static int
run_ring_prepare_rejects_oversize_alignment_wraparound_test() {
	const uint32_t ring_size = 32;
	uint8_t *data;
	struct ring_worker ring = init_test_ring(ring_size, &data);
	TEST_ASSERT_NOT_NULL(data, "failed to allocate ring data");

	uint32_t oversize_values[] = {
		ring_size + 1, 0xFFFFFFFD, 0xFFFFFFFE, 0xFFFFFFFF
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
			(long)ring.write_idx,
			0L,
			"total_len %u must not move write_idx",
			total_len
		);
		TEST_ASSERT_EQUAL(
			(long)ring.readable_idx,
			0L,
			"total_len %u must not move readable_idx",
			total_len
		);
		TEST_ASSERT_EQUAL(
			(long)ring.next_seqno,
			0L,
			"total_len %u must not touch next_seqno",
			total_len
		);
	}

	free(data);
	return TEST_SUCCESS;
}

// The per-worker sequence counter wraps from UINT32_MAX to 0 and keeps
// numbering contiguous across the wrap.
static int
run_ring_seqno_wrap_test() {
	const uint32_t ring_size = 32;
	uint8_t *data;
	struct ring_worker ring = init_test_ring(ring_size, &data);
	TEST_ASSERT_NOT_NULL(data, "failed to allocate ring data");

	ring.next_seqno = UINT32_MAX;

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
// or data, byte-for-byte, when the two sit in one contiguous, cache-line
// strided array exactly as ring_object lays out its per-worker array.
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

// Two sequential commits on the same worker produce contiguous seqnos.
static int
run_ring_sequential_producers_contiguous_seqno_test() {
	const uint32_t ring_size = 64;
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

	free(data);
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
		{"prepare_rejects_invalid_size",
		 run_ring_prepare_rejects_invalid_size_test},
		{"prepare_rejects_oversize_alignment_wraparound",
		 run_ring_prepare_rejects_oversize_alignment_wraparound_test},
		{"seqno_wrap", run_ring_seqno_wrap_test},
		{"multi_worker_isolation", run_ring_multi_worker_isolation_test
		},
		{"sequential_producers_contiguous_seqno",
		 run_ring_sequential_producers_contiguous_seqno_test},
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
