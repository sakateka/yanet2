/*
 * Benchmark of the old pdump writer against the ring writer at several
 * record sizes, single-threaded and with two threads on adjacent workers.
 *
 * Each per-worker array is one contiguous 64-byte-aligned allocation, the
 * layout a block allocator gives, so two-worker runs reproduce the old
 * layout's cache-line sharing and the new one's isolation. Rings are
 * pre-faulted, every timed phase follows a discarded warm-up, threads start
 * on a barrier and the old/new order alternates. Sizes run with and without
 * eviction on every write. It prints ns/record with no pass/fail threshold.
 */

#include "modules/pdump/dataplane/ring.h"
#include "objects/ring/dataplane/ring.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Minimum duration of a timed phase, so the fixed cost of creating and
// joining the phase's threads is negligible next to what it measures.
#define BENCH_PHASE_NS (100 * 1000 * 1000ull)
// Duration of the discarded warm-up phase that precedes every timed one.
#define BENCH_WARMUP_NS (20 * 1000 * 1000ull)
// Iterations between deadline checks, so reading the clock does not bias
// the smallest record size's measurement.
#define BENCH_CHECK_BATCH 256

static uint64_t
now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint32_t
next_pow2(uint32_t val) {
	val -= 1;
	val |= val >> 1;
	val |= val >> 2;
	val |= val >> 4;
	val |= val >> 8;
	val |= val >> 16;
	return val + 1;
}

// Abort the benchmark loudly: a failed setup step would otherwise time a
// run that is not measuring what it claims.
static void
bench_die(const char *what) {
	fprintf(stderr, "ring_bench: %s\n", what);
	abort();
}

// Allocate a buffer and touch every byte, so the first write in a timed
// phase never pays a page fault. Aborts on failure.
static uint8_t *
alloc_touched(size_t size) {
	uint8_t *block = malloc(size);
	if (block == NULL) {
		bench_die("failed to allocate a buffer");
	}
	memset(block, 0, size);
	return block;
}

// Old writer: pdump's own writer over its fixed-size message header.
static void
old_write_record(
	struct ring_buffer *ring,
	uint8_t *data,
	uint8_t *payload,
	uint32_t payload_len
) {
	struct ring_msg_hdr hdr;
	memset(&hdr, 0, sizeof(hdr));
	hdr.total_len = (uint32_t)(sizeof(hdr) + payload_len);
	hdr.magic = RING_MSG_MAGIC;
	hdr.packet_len = payload_len;
	pdump_ring_write_msg(ring, data, &hdr, payload);
}

// New writer: the ring writer over the 8-byte generic frame.
static void
new_write_record(
	struct ring_worker *ring,
	uint8_t *data,
	uint8_t *payload,
	uint32_t payload_len
) {
	uint32_t total_len = (uint32_t)(RING_RECORD_FRAME_SIZE + payload_len);
	if (ring_worker_prepare(ring, data, total_len) != 0) {
		bench_die("ring_worker_prepare refused a benchmark record");
	}
	ring_worker_write(
		ring, data, RING_RECORD_FRAME_SIZE, payload, payload_len
	);
	ring_worker_commit(ring, data, total_len);
}

struct old_bench_ctx {
	struct ring_buffer *ring;
	uint8_t *data;
	uint8_t *payload;
	uint32_t payload_len;
	// Records written per pass before the indices reset to keep this
	// scenario overflow-free; 0 lets the ring evict naturally instead.
	uint64_t reset_after_records;
	pthread_barrier_t *start_barrier;
	uint64_t run_ns;
	uint64_t elapsed_ns; // out
	uint64_t iterations; // out
};

static void *
old_bench_thread(void *arg) {
	struct old_bench_ctx *ctx = arg;
	pthread_barrier_wait(ctx->start_barrier);

	uint64_t start = now_ns();
	uint64_t deadline = start + ctx->run_ns;
	uint64_t iterations = 0;
	uint64_t since_reset = 0;
	for (;;) {
		for (uint64_t b = 0; b < BENCH_CHECK_BATCH; ++b) {
			if (ctx->reset_after_records != 0 &&
			    since_reset >= ctx->reset_after_records) {
				ctx->ring->write_idx = 0;
				ctx->ring->readable_idx = 0;
				since_reset = 0;
			}
			old_write_record(
				ctx->ring,
				ctx->data,
				ctx->payload,
				ctx->payload_len
			);
			++since_reset;
			++iterations;
		}
		if (now_ns() >= deadline) {
			break;
		}
	}
	ctx->elapsed_ns = now_ns() - start;
	ctx->iterations = iterations;
	return NULL;
}

struct new_bench_ctx {
	struct ring_worker *ring;
	uint8_t *data;
	uint8_t *payload;
	uint32_t payload_len;
	uint64_t reset_after_records;
	pthread_barrier_t *start_barrier;
	uint64_t run_ns;
	uint64_t elapsed_ns; // out
	uint64_t iterations; // out
};

static void *
new_bench_thread(void *arg) {
	struct new_bench_ctx *ctx = arg;
	pthread_barrier_wait(ctx->start_barrier);

	uint64_t start = now_ns();
	uint64_t deadline = start + ctx->run_ns;
	uint64_t iterations = 0;
	uint64_t since_reset = 0;
	for (;;) {
		for (uint64_t b = 0; b < BENCH_CHECK_BATCH; ++b) {
			if (ctx->reset_after_records != 0 &&
			    since_reset >= ctx->reset_after_records) {
				ctx->ring->write_idx = 0;
				ctx->ring->readable_idx = 0;
				since_reset = 0;
			}
			new_write_record(
				ctx->ring,
				ctx->data,
				ctx->payload,
				ctx->payload_len
			);
			++since_reset;
			++iterations;
		}
		if (now_ns() >= deadline) {
			break;
		}
	}
	ctx->elapsed_ns = now_ns() - start;
	ctx->iterations = iterations;
	return NULL;
}

// Allocate adjacent workers' metadata as one contiguous 64-byte-aligned
// array, plus each worker's own pre-faulted data area.
//
// The array reproduces the layout a block allocator gives, independent of
// stack alignment. Aborts on failure.
static struct ring_buffer *
old_alloc_workers(int count, uint32_t ring_size, uint8_t *data[2]) {
	struct ring_buffer *workers;
	if (posix_memalign(
		    (void **)&workers,
		    64,
		    sizeof(struct ring_buffer) * (size_t)count
	    ) != 0) {
		bench_die("failed to allocate old worker metadata");
	}
	memset(workers, 0, sizeof(struct ring_buffer) * (size_t)count);

	for (int i = 0; i < count; ++i) {
		data[i] = alloc_touched(ring_size);
		workers[i].size = ring_size;
		workers[i].mask = ring_size - 1;
	}
	return workers;
}

// Same as old_alloc_workers, aligned to the new struct's own cache-line
// alignment, which exceeds 64 bytes on 128-byte cache-line targets.
static struct ring_worker *
new_alloc_workers(int count, uint32_t ring_size, uint8_t *data[2]) {
	struct ring_worker *workers;
	if (posix_memalign(
		    (void **)&workers,
		    _Alignof(struct ring_worker),
		    sizeof(struct ring_worker) * (size_t)count
	    ) != 0) {
		bench_die("failed to allocate new worker metadata");
	}
	memset(workers, 0, sizeof(struct ring_worker) * (size_t)count);

	for (int i = 0; i < count; ++i) {
		data[i] = alloc_touched(ring_size);
		workers[i].size = ring_size;
		workers[i].mask = ring_size - 1;
	}
	return workers;
}

// Run adjacent workers concurrently through a discarded warm-up and a timed
// phase, returning the timed phase's average ns/record.
//
// Both threads in a phase start together on a barrier. A nonzero reset
// interval keeps a phase overflow-free by resetting the indices between
// passes; zero lets the ring evict naturally.
static double
run_old(uint32_t size,
	uint32_t ring_size,
	uint64_t reset_after_records,
	int count) {
	uint32_t payload_len = size - (uint32_t)sizeof(struct ring_msg_hdr);
	uint8_t *payload = alloc_touched(size);
	memset(payload, 0xAB, size);

	uint8_t *data[2];
	struct ring_buffer *workers = old_alloc_workers(count, ring_size, data);

	pthread_barrier_t barrier;
	pthread_barrier_init(&barrier, NULL, (unsigned)count);

	struct old_bench_ctx ctx[2];
	pthread_t threads[2];
	for (int i = 0; i < count; ++i) {
		ctx[i] = (struct old_bench_ctx){
			.ring = &workers[i],
			.data = data[i],
			.payload = payload,
			.payload_len = payload_len,
			.reset_after_records = reset_after_records,
			.start_barrier = &barrier,
			.run_ns = BENCH_WARMUP_NS,
		};
	}
	for (int i = 0; i < count; ++i) {
		pthread_create(&threads[i], NULL, old_bench_thread, &ctx[i]);
	}
	for (int i = 0; i < count; ++i) {
		pthread_join(threads[i], NULL);
	}
	for (int i = 0; i < count; ++i) {
		workers[i].write_idx = 0;
		workers[i].readable_idx = 0;
		ctx[i].run_ns = BENCH_PHASE_NS;
	}

	for (int i = 0; i < count; ++i) {
		pthread_create(&threads[i], NULL, old_bench_thread, &ctx[i]);
	}
	uint64_t total_ns = 0;
	uint64_t total_iterations = 0;
	for (int i = 0; i < count; ++i) {
		pthread_join(threads[i], NULL);
		total_ns += ctx[i].elapsed_ns;
		total_iterations += ctx[i].iterations;
	}

	pthread_barrier_destroy(&barrier);
	free(workers);
	for (int i = 0; i < count; ++i) {
		free(data[i]);
	}
	free(payload);

	return (double)total_ns / (double)total_iterations;
}

static double
run_new(uint32_t size,
	uint32_t ring_size,
	uint64_t reset_after_records,
	int count) {
	uint32_t payload_len = size - (uint32_t)RING_RECORD_FRAME_SIZE;
	uint8_t *payload = alloc_touched(size);
	memset(payload, 0xAB, size);

	uint8_t *data[2];
	struct ring_worker *workers = new_alloc_workers(count, ring_size, data);

	pthread_barrier_t barrier;
	pthread_barrier_init(&barrier, NULL, (unsigned)count);

	struct new_bench_ctx ctx[2];
	pthread_t threads[2];
	for (int i = 0; i < count; ++i) {
		ctx[i] = (struct new_bench_ctx){
			.ring = &workers[i],
			.data = data[i],
			.payload = payload,
			.payload_len = payload_len,
			.reset_after_records = reset_after_records,
			.start_barrier = &barrier,
			.run_ns = BENCH_WARMUP_NS,
		};
	}
	for (int i = 0; i < count; ++i) {
		pthread_create(&threads[i], NULL, new_bench_thread, &ctx[i]);
	}
	for (int i = 0; i < count; ++i) {
		pthread_join(threads[i], NULL);
	}
	for (int i = 0; i < count; ++i) {
		workers[i].write_idx = 0;
		workers[i].readable_idx = 0;
		ctx[i].run_ns = BENCH_PHASE_NS;
	}

	for (int i = 0; i < count; ++i) {
		pthread_create(&threads[i], NULL, new_bench_thread, &ctx[i]);
	}
	uint64_t total_ns = 0;
	uint64_t total_iterations = 0;
	for (int i = 0; i < count; ++i) {
		pthread_join(threads[i], NULL);
		total_ns += ctx[i].elapsed_ns;
		total_iterations += ctx[i].iterations;
	}

	pthread_barrier_destroy(&barrier);
	free(workers);
	for (int i = 0; i < count; ++i) {
		free(data[i]);
	}
	free(payload);

	return (double)total_ns / (double)total_iterations;
}

int
main(void) {
	const uint32_t sizes[] = {64, 256, 1500, 9000};
	const uint32_t overflow_ring_size = 1u << 16;
	// Records per no-overflow pass before the indices reset.
	//
	// The count only trades allocation size for reset frequency, so an
	// order-of-magnitude choice is enough.
	const uint32_t records_per_pass = 256;

	printf("%-6s %-10s %-6s %12s %12s\n",
	       "size",
	       "scenario",
	       "workers",
	       "old_ns/rec",
	       "new_ns/rec");

	// Alternates which side a phase measures first, so neither
	// systematically runs cold or warm relative to the other.
	bool old_first = true;

	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
		uint32_t size = sizes[i];
		uint32_t no_overflow_ring_size =
			next_pow2(size * records_per_pass);
		uint64_t no_overflow_reset_after = no_overflow_ring_size / size;

		for (int count = 1; count <= 2; ++count) {
			double old_ns;
			double new_ns;
			if (old_first) {
				old_ns =
					run_old(size,
						no_overflow_ring_size,
						no_overflow_reset_after,
						count);
				new_ns =
					run_new(size,
						no_overflow_ring_size,
						no_overflow_reset_after,
						count);
			} else {
				new_ns =
					run_new(size,
						no_overflow_ring_size,
						no_overflow_reset_after,
						count);
				old_ns =
					run_old(size,
						no_overflow_ring_size,
						no_overflow_reset_after,
						count);
			}
			old_first = !old_first;
			printf("%-6u %-10s %-6d %12.2f %12.2f\n",
			       size,
			       "no-overflow",
			       count,
			       old_ns,
			       new_ns);

			double old_overflow_ns;
			double new_overflow_ns;
			if (old_first) {
				old_overflow_ns = run_old(
					size, overflow_ring_size, 0, count
				);
				new_overflow_ns = run_new(
					size, overflow_ring_size, 0, count
				);
			} else {
				new_overflow_ns = run_new(
					size, overflow_ring_size, 0, count
				);
				old_overflow_ns = run_old(
					size, overflow_ring_size, 0, count
				);
			}
			old_first = !old_first;
			printf("%-6u %-10s %-6d %12.2f %12.2f\n",
			       size,
			       "overflow",
			       count,
			       old_overflow_ns,
			       new_overflow_ns);
		}
	}

	return 0;
}
