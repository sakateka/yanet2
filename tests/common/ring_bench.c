/*
 * Benchmark of the old pdump writer against the ring writer at several
 * record sizes, alone and with one concurrent reader per worker.
 *
 * Each per-worker array is one contiguous cache-line-aligned allocation, the
 * layout a block allocator gives, so two-worker runs reproduce the old
 * layout's cache-line sharing and the new one's isolation. Rings are
 * pre-faulted, every timed phase follows a discarded writer-only warm-up,
 * threads start on a barrier, every thread is pinned to its own CPU and the
 * old/new order alternates. Each cell is the median of several runs; there
 * is no pass/fail threshold.
 *
 * The reader rows give every writer a reader thread that runs the
 * production copy-then-recheck protocol, transcribed to C, against the same
 * worker ring on its own CPU: the new ring with its frame, the old pdump
 * ring with its own header and index order. The old writer and its header
 * come from the pdump module, which still owns its rings; that comparison
 * goes away once pdump captures into this ring.
 *
 * CPUs: argv[1] or RING_BENCH_CPUS as "w0,r0[,w1,r1]" (writer and reader of
 * worker 0, then of worker 1); by default the first allowed CPUs past the
 * first one. Runs per cell: RING_BENCH_REPS (default 5).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "common/numutils.h"
#include "common/ring.h"
#include "modules/pdump/dataplane/ring.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
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
// Upper bound of runs per cell, sizing the sample arrays.
#define BENCH_MAX_REPS 32
#define BENCH_DEFAULT_REPS 5

// Ring of the reader rows: pdump's minimum ring size.
#define READER_RING_SIZE (1u << 20)
// Byte budget of one read: pdump's default read chunk, 32 snapshots of its
// default 16 KiB snap length.
#define READER_READ_BUDGET (512u << 10)

static uint64_t
now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
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

// Pin the calling thread to one CPU; a negative CPU leaves it unpinned.
static void
pin_self(int cpu) {
	if (cpu < 0) {
		return;
	}
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
		bench_die("failed to pin a thread: CPU not allowed");
	}
}

static inline void
cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#elif defined(__aarch64__)
	__asm__ volatile("yield" ::: "memory");
#else
	atomic_signal_fence(memory_order_seq_cst);
#endif
}

// CPU assignment: the writer and reader CPU of each of up to two workers.
struct bench_cpus {
	int writer[2];
	int reader[2];
	// Workers that have both a writer and a reader CPU.
	int reader_workers;
};

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
	int cpu;
	uint64_t elapsed_ns; // out
	uint64_t iterations; // out
};

static void *
old_bench_thread(void *arg) {
	struct old_bench_ctx *ctx = arg;
	pin_self(ctx->cpu);
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
	int cpu;
	uint64_t elapsed_ns; // out
	uint64_t iterations; // out
};

static void *
new_bench_thread(void *arg) {
	struct new_bench_ctx *ctx = arg;
	pin_self(ctx->cpu);
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

// Record format a reader parses: the format its writer produces.
enum reader_format {
	// pdump's header: magic-checked, no sequence number.
	READER_FORMAT_OLD,
	// The ring's 8-byte frame with a sequence number.
	READER_FORMAT_NEW,
};

// One reader over one worker's ring, the C transcription of a production Go
// reader: its private cursor, scratch buffer and statistics.
struct reader_ctx {
	enum reader_format format;
	_Atomic uint64_t *write_idx;
	_Atomic uint64_t *readable_idx;
	const uint8_t *data;
	uint64_t mask;
	uint32_t capacity;
	// Declared length of every record the paired writer commits.
	uint32_t record_len;
	pthread_barrier_t *start_barrier;
	const atomic_bool *stop;
	int cpu;

	// The cursor is atomic as in Go, where a waker goroutine polls it.
	_Atomic uint64_t cursor;
	// Copied, not yet parsed bytes: at most a carried partial record plus
	// one read budget.
	uint8_t *buf;
	uint64_t buf_len;
	uint64_t buf_cap;
	// Destination of every returned payload, as the Go reader copies each
	// payload out of its scratch buffer.
	uint8_t *out;
	uint32_t next_seqno;

	uint64_t elapsed_ns; // out
	uint64_t records;    // out
	// Returned records with a wrong length, magic or sequence order.
	uint64_t bad; // out
	// Reads that found the buffered data invalid as a whole.
	uint64_t corrupt; // out
};

// Copy a logical byte range of a ring's data area, wrapping at its end.
static void
reader_copy_range(
	const struct reader_ctx *ctx,
	uint8_t *dst,
	uint64_t start,
	uint64_t size
) {
	uint64_t pos = start & ctx->mask;
	uint64_t tail = (uint64_t)ctx->capacity - pos;
	if (size <= tail) {
		memcpy(dst, ctx->data + pos, size);
		return;
	}
	memcpy(dst, ctx->data + pos, tail);
	memcpy(dst + tail, ctx->data, size - tail);
}

static void
reader_drop_prefix(struct reader_ctx *ctx, uint64_t n) {
	memmove(ctx->buf, ctx->buf + n, ctx->buf_len - n);
	ctx->buf_len -= n;
}

// Parse the whole records at the front of the buffer and drop them.
//
// Returns false for a length outside the ring's bounds, or a bad magic for
// the old format, leaving the buffer for the caller to discard.
static bool
reader_parse(struct reader_ctx *ctx) {
	uint64_t hdr_size = ctx->format == READER_FORMAT_NEW
				    ? RING_RECORD_FRAME_SIZE
				    : sizeof(struct ring_msg_hdr);
	uint64_t parsed = 0;
	while (ctx->buf_len - parsed >= hdr_size) {
		const uint8_t *rec = ctx->buf + parsed;
		uint32_t total_len;
		memcpy(&total_len, rec, sizeof(total_len));
		// The old Go reader has no upper bound; one keeps a torn
		// header from overrunning this fixed buffer.
		if (total_len < hdr_size || total_len > ctx->capacity) {
			return false;
		}
		if (ctx->format == READER_FORMAT_OLD) {
			uint32_t magic;
			memcpy(&magic,
			       rec + offsetof(struct ring_msg_hdr, magic),
			       sizeof(magic));
			if (magic != RING_MSG_MAGIC) {
				return false;
			}
		}
		uint64_t skip = ring_align4(total_len);
		if (skip > ctx->buf_len - parsed) {
			break;
		}

		if (ctx->format == READER_FORMAT_NEW) {
			uint32_t seqno;
			memcpy(&seqno, rec + sizeof(uint32_t), sizeof(seqno));
			// Overwrites skip sequence numbers, never reorder them.
			if ((int32_t)(seqno - ctx->next_seqno) < 0) {
				++ctx->bad;
			}
			ctx->next_seqno = seqno + 1;
		}
		if (total_len != ctx->record_len) {
			++ctx->bad;
		}
		memcpy(ctx->out, rec + hdr_size, total_len - hdr_size);
		++ctx->records;
		parsed += skip;
	}
	reader_drop_prefix(ctx, parsed);
	return true;
}

// One read: snapshot the indices, copy the readable bytes, advance the
// cursor, recheck the readable position, drop the invalidated prefix, parse.
static void
reader_read(struct reader_ctx *ctx) {
	uint64_t write;
	uint64_t readable;
	// Each Go reader loads the indices in its own order.
	if (ctx->format == READER_FORMAT_NEW) {
		write = atomic_load_explicit(
			ctx->write_idx, memory_order_acquire
		);
		readable = atomic_load_explicit(
			ctx->readable_idx, memory_order_acquire
		);
	} else {
		readable = atomic_load_explicit(
			ctx->readable_idx, memory_order_acquire
		);
		write = atomic_load_explicit(
			ctx->write_idx, memory_order_acquire
		);
	}

	uint64_t cursor = atomic_load(&ctx->cursor);
	if (readable > cursor) {
		ctx->buf_len = 0;
		atomic_store(&ctx->cursor, readable);
	} else {
		readable = cursor;
	}
	if (write <= readable) {
		cpu_relax();
		return;
	}

	uint64_t size = write - readable;
	if (size > READER_READ_BUDGET) {
		size = READER_READ_BUDGET;
	}
	uint64_t before = ctx->buf_len;
	if (before + size > ctx->buf_cap) {
		bench_die("reader buffer overflow");
	}
	reader_copy_range(ctx, ctx->buf + before, readable, size);
	ctx->buf_len = before + size;

	// Sequentially consistent, as every Go atomic: it keeps the copy's
	// loads above the recheck below.
	atomic_fetch_add(&ctx->cursor, size);

	uint64_t latest =
		atomic_load_explicit(ctx->readable_idx, memory_order_acquire);
	if (latest > readable) {
		uint64_t diff = latest - readable + before;
		if (diff > ctx->buf_len) {
			ctx->buf_len = 0;
			atomic_store(&ctx->cursor, latest);
			return;
		}
		reader_drop_prefix(ctx, diff);
	}

	if (!reader_parse(ctx)) {
		++ctx->corrupt;
		ctx->buf_len = 0;
		// Only the new reader resumes at the snapshot's write position.
		if (ctx->format == READER_FORMAT_NEW) {
			atomic_store(&ctx->cursor, write);
		}
	}
}

static void *
reader_thread(void *arg) {
	struct reader_ctx *ctx = arg;
	pin_self(ctx->cpu);
	pthread_barrier_wait(ctx->start_barrier);

	uint64_t start = now_ns();
	// After the stop request, drain what the writer left committed.
	for (;;) {
		bool stopping =
			atomic_load_explicit(ctx->stop, memory_order_acquire);
		reader_read(ctx);
		if (stopping &&
		    atomic_load(&ctx->cursor) >=
			    atomic_load_explicit(
				    ctx->write_idx, memory_order_acquire
			    )) {
			break;
		}
	}
	ctx->elapsed_ns = now_ns() - start;
	return NULL;
}

// Set up a reader over one worker ring with pre-faulted buffers.
static void
reader_init(
	struct reader_ctx *ctx,
	enum reader_format format,
	_Atomic uint64_t *write_idx,
	_Atomic uint64_t *readable_idx,
	const uint8_t *data,
	uint32_t capacity
) {
	memset(ctx, 0, sizeof(*ctx));
	ctx->format = format;
	ctx->write_idx = write_idx;
	ctx->readable_idx = readable_idx;
	ctx->data = data;
	ctx->capacity = capacity;
	ctx->mask = capacity - 1;
	ctx->buf_cap = (uint64_t)capacity + READER_READ_BUDGET;
	ctx->buf = alloc_touched(ctx->buf_cap);
	ctx->out = alloc_touched(capacity);
}

static void
reader_free(struct reader_ctx *ctx) {
	free(ctx->buf);
	free(ctx->out);
}

// Outcome of one timed phase across its workers.
struct bench_sample {
	// Average writer cost per committed record.
	double writer_ns;
	// Reader-only metrics, zero when the phase ran no reader. Rate is per
	// reader, averaged over the workers.
	double reader_mrps;
	// Fraction of committed records no reader returned.
	double lost;
	uint64_t bad;
};

// Join the timed phase's readers and fold their statistics into a sample.
static void
reader_collect(
	struct reader_ctx *readers,
	pthread_t *threads,
	int count,
	atomic_bool *stop,
	uint64_t written,
	struct bench_sample *sample
) {
	atomic_store_explicit(stop, true, memory_order_release);
	uint64_t returned = 0;
	double rate = 0;
	for (int i = 0; i < count; ++i) {
		pthread_join(threads[i], NULL);
		returned += readers[i].records;
		sample->bad += readers[i].bad;
		rate += (double)readers[i].records * 1e3 /
			(double)readers[i].elapsed_ns;
		reader_free(&readers[i]);
	}
	sample->reader_mrps = rate / count;
	sample->lost = 1.0 - (double)returned / (double)written;
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

// One timed phase's shape: the ring, its writers and whether each writer
// has a concurrent reader.
struct bench_case {
	uint32_t size;
	uint32_t ring_size;
	// Records per pass before the indices reset; 0 lets the ring evict.
	uint64_t reset_after_records;
	int count;
	bool with_reader;
	const struct bench_cpus *cpus;
};

// Run adjacent old writers through a discarded warm-up and a timed phase.
//
// The warm-up is writer-only. With readers, the indices restart at zero and
// the readers join the writers on the timed phase's start barrier; a
// nonzero reset interval keeps a writer-only phase overflow-free.
static struct bench_sample
run_old(const struct bench_case *bc) {
	uint32_t record_len = bc->size;
	uint32_t payload_len =
		record_len - (uint32_t)sizeof(struct ring_msg_hdr);
	uint8_t *payload = alloc_touched(record_len);
	memset(payload, 0xAB, record_len);

	uint8_t *data[2];
	struct ring_buffer *workers =
		old_alloc_workers(bc->count, bc->ring_size, data);

	pthread_barrier_t warmup_barrier;
	pthread_barrier_init(&warmup_barrier, NULL, (unsigned)bc->count);
	int parties = bc->count * (bc->with_reader ? 2 : 1);
	pthread_barrier_t barrier;
	pthread_barrier_init(&barrier, NULL, (unsigned)parties);

	struct old_bench_ctx ctx[2];
	pthread_t threads[2];
	for (int i = 0; i < bc->count; ++i) {
		ctx[i] = (struct old_bench_ctx){
			.ring = &workers[i],
			.data = data[i],
			.payload = payload,
			.payload_len = payload_len,
			.reset_after_records = bc->reset_after_records,
			.start_barrier = &warmup_barrier,
			.run_ns = BENCH_WARMUP_NS,
			.cpu = bc->cpus->writer[i],
		};
	}
	for (int i = 0; i < bc->count; ++i) {
		pthread_create(&threads[i], NULL, old_bench_thread, &ctx[i]);
	}
	for (int i = 0; i < bc->count; ++i) {
		pthread_join(threads[i], NULL);
	}
	for (int i = 0; i < bc->count; ++i) {
		workers[i].write_idx = 0;
		workers[i].readable_idx = 0;
		ctx[i].run_ns = BENCH_PHASE_NS;
		ctx[i].start_barrier = &barrier;
	}

	atomic_bool stop = false;
	struct reader_ctx readers[2];
	pthread_t reader_threads[2];
	if (bc->with_reader) {
		for (int i = 0; i < bc->count; ++i) {
			reader_init(
				&readers[i],
				READER_FORMAT_OLD,
				&workers[i].write_idx,
				&workers[i].readable_idx,
				data[i],
				bc->ring_size
			);
			readers[i].record_len = record_len;
			readers[i].start_barrier = &barrier;
			readers[i].stop = &stop;
			readers[i].cpu = bc->cpus->reader[i];
			pthread_create(
				&reader_threads[i],
				NULL,
				reader_thread,
				&readers[i]
			);
		}
	}

	for (int i = 0; i < bc->count; ++i) {
		pthread_create(&threads[i], NULL, old_bench_thread, &ctx[i]);
	}
	uint64_t total_ns = 0;
	uint64_t total_iterations = 0;
	for (int i = 0; i < bc->count; ++i) {
		pthread_join(threads[i], NULL);
		total_ns += ctx[i].elapsed_ns;
		total_iterations += ctx[i].iterations;
	}

	struct bench_sample sample = {
		.writer_ns = (double)total_ns / (double)total_iterations,
	};
	if (bc->with_reader) {
		reader_collect(
			readers,
			reader_threads,
			bc->count,
			&stop,
			total_iterations,
			&sample
		);
	}

	pthread_barrier_destroy(&warmup_barrier);
	pthread_barrier_destroy(&barrier);
	free(workers);
	for (int i = 0; i < bc->count; ++i) {
		free(data[i]);
	}
	free(payload);
	return sample;
}

// Same as run_old with the new writer and the new reader format; the
// sequence counter also restarts at zero for the timed phase.
static struct bench_sample
run_new(const struct bench_case *bc) {
	uint32_t record_len = bc->size;
	uint32_t payload_len = record_len - (uint32_t)RING_RECORD_FRAME_SIZE;
	uint8_t *payload = alloc_touched(record_len);
	memset(payload, 0xAB, record_len);

	uint8_t *data[2];
	struct ring_worker *workers =
		new_alloc_workers(bc->count, bc->ring_size, data);

	pthread_barrier_t warmup_barrier;
	pthread_barrier_init(&warmup_barrier, NULL, (unsigned)bc->count);
	int parties = bc->count * (bc->with_reader ? 2 : 1);
	pthread_barrier_t barrier;
	pthread_barrier_init(&barrier, NULL, (unsigned)parties);

	struct new_bench_ctx ctx[2];
	pthread_t threads[2];
	for (int i = 0; i < bc->count; ++i) {
		ctx[i] = (struct new_bench_ctx){
			.ring = &workers[i],
			.data = data[i],
			.payload = payload,
			.payload_len = payload_len,
			.reset_after_records = bc->reset_after_records,
			.start_barrier = &warmup_barrier,
			.run_ns = BENCH_WARMUP_NS,
			.cpu = bc->cpus->writer[i],
		};
	}
	for (int i = 0; i < bc->count; ++i) {
		pthread_create(&threads[i], NULL, new_bench_thread, &ctx[i]);
	}
	for (int i = 0; i < bc->count; ++i) {
		pthread_join(threads[i], NULL);
	}
	for (int i = 0; i < bc->count; ++i) {
		workers[i].write_idx = 0;
		workers[i].readable_idx = 0;
		workers[i].next_seqno = 0;
		ctx[i].run_ns = BENCH_PHASE_NS;
		ctx[i].start_barrier = &barrier;
	}

	atomic_bool stop = false;
	struct reader_ctx readers[2];
	pthread_t reader_threads[2];
	if (bc->with_reader) {
		for (int i = 0; i < bc->count; ++i) {
			reader_init(
				&readers[i],
				READER_FORMAT_NEW,
				&workers[i].write_idx,
				&workers[i].readable_idx,
				data[i],
				bc->ring_size
			);
			readers[i].record_len = record_len;
			readers[i].start_barrier = &barrier;
			readers[i].stop = &stop;
			readers[i].cpu = bc->cpus->reader[i];
			pthread_create(
				&reader_threads[i],
				NULL,
				reader_thread,
				&readers[i]
			);
		}
	}

	for (int i = 0; i < bc->count; ++i) {
		pthread_create(&threads[i], NULL, new_bench_thread, &ctx[i]);
	}
	uint64_t total_ns = 0;
	uint64_t total_iterations = 0;
	for (int i = 0; i < bc->count; ++i) {
		pthread_join(threads[i], NULL);
		total_ns += ctx[i].elapsed_ns;
		total_iterations += ctx[i].iterations;
	}

	struct bench_sample sample = {
		.writer_ns = (double)total_ns / (double)total_iterations,
	};
	if (bc->with_reader) {
		reader_collect(
			readers,
			reader_threads,
			bc->count,
			&stop,
			total_iterations,
			&sample
		);
	}

	pthread_barrier_destroy(&warmup_barrier);
	pthread_barrier_destroy(&barrier);
	free(workers);
	for (int i = 0; i < bc->count; ++i) {
		free(data[i]);
	}
	free(payload);
	return sample;
}

// Parse "w0,r0[,w1,r1]"; returns false on a malformed list.
static bool
parse_cpus(const char *spec, struct bench_cpus *cpus) {
	int vals[4];
	int n = 0;
	const char *pos = spec;
	while (*pos != '\0') {
		char *end;
		long val = strtol(pos, &end, 10);
		if (end == pos || val < 0 || val >= CPU_SETSIZE || n == 4) {
			return false;
		}
		vals[n++] = (int)val;
		if (*end == ',') {
			++end;
		} else if (*end != '\0') {
			return false;
		}
		pos = end;
	}
	if (n != 2 && n != 4) {
		return false;
	}
	*cpus = (struct bench_cpus){
		.writer = {vals[0], n == 4 ? vals[2] : -1},
		.reader = {vals[1], n == 4 ? vals[3] : -1},
		.reader_workers = n / 2,
	};
	return true;
}

// Pick distinct CPUs from the allowed set, skipping the first one, which
// usually carries the system's housekeeping, when enough remain.
static void
default_cpus(struct bench_cpus *cpus) {
	cpu_set_t set;
	int allowed[CPU_SETSIZE];
	int n = 0;
	if (sched_getaffinity(0, sizeof(set), &set) == 0) {
		for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
			if (CPU_ISSET(cpu, &set)) {
				allowed[n++] = cpu;
			}
		}
	}
	int skip = n >= 5 ? 1 : 0;
	int *pick = allowed + skip;
	int avail = n - skip;
	*cpus = (struct bench_cpus){
		.writer = {-1, -1},
		.reader = {-1, -1},
	};
	if (avail >= 4) {
		// Adjacent writers, then their readers.
		*cpus = (struct bench_cpus){
			.writer = {pick[0], pick[1]},
			.reader = {pick[2], pick[3]},
			.reader_workers = 2,
		};
	} else if (avail >= 2) {
		*cpus = (struct bench_cpus){
			.writer = {pick[0], pick[1]},
			.reader = {pick[1], -1},
			.reader_workers = 1,
		};
	}
}

static int
cmp_double(const void *a, const void *b) {
	double x = *(const double *)a;
	double y = *(const double *)b;
	return (x > y) - (x < y);
}

static double
median(const double *vals, int n) {
	double sorted[BENCH_MAX_REPS];
	memcpy(sorted, vals, sizeof(double) * (size_t)n);
	qsort(sorted, (size_t)n, sizeof(double), cmp_double);
	return n % 2 ? sorted[n / 2]
		     : (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
}

enum bench_scenario {
	// Writer-only, indices reset before the ring fills.
	SCENARIO_NO_OVERFLOW,
	// Writer-only, 64 KiB ring evicting on every write.
	SCENARIO_OVERFLOW,
	// Writer-only on the reader rows' ring, their baseline.
	SCENARIO_READER_BASE,
	// The same ring with one concurrent reader per writer.
	SCENARIO_READER,
	SCENARIO_COUNT,
};

enum { SIDE_OLD, SIDE_NEW, SIDE_COUNT };

#define BENCH_SIZES 4

// Samples of every cell across runs.
struct bench_results {
	struct bench_sample cells[BENCH_SIZES][2][SCENARIO_COUNT][SIDE_COUNT]
				 [BENCH_MAX_REPS];
};

// Medians of one cell's metrics across runs, per side.
struct cell_stats {
	double writer_ns[SIDE_COUNT];
	double reader_mrps[SIDE_COUNT];
	double lost_pct[SIDE_COUNT];
	// Summed over all runs.
	unsigned long bad[SIDE_COUNT];
};

static struct cell_stats
cell_stats(
	const struct bench_sample cell[SIDE_COUNT][BENCH_MAX_REPS], int reps
) {
	struct cell_stats st = {0};
	for (int side = 0; side < SIDE_COUNT; ++side) {
		double ns[BENCH_MAX_REPS];
		double rate[BENCH_MAX_REPS];
		double lost[BENCH_MAX_REPS];
		for (int r = 0; r < reps; ++r) {
			ns[r] = cell[side][r].writer_ns;
			rate[r] = cell[side][r].reader_mrps;
			lost[r] = cell[side][r].lost;
			st.bad[side] += (unsigned long)cell[side][r].bad;
		}
		st.writer_ns[side] = median(ns, reps);
		st.reader_mrps[side] = median(rate, reps);
		st.lost_pct[side] = 100 * median(lost, reps);
	}
	return st;
}

// Build the phase shape of one scenario at one record size.
static struct bench_case
scenario_case(
	enum bench_scenario sc,
	uint32_t size,
	int count,
	const struct bench_cpus *cpus
) {
	const uint32_t overflow_ring_size = 1u << 16;
	// Records per no-overflow pass before the indices reset.
	//
	// The count only trades allocation size for reset frequency, so an
	// order-of-magnitude choice is enough.
	const uint32_t records_per_pass = 256;

	struct bench_case bc = {.size = size, .count = count, .cpus = cpus};
	switch (sc) {
	case SCENARIO_NO_OVERFLOW:
		bc.ring_size = (uint32_t
		)next_power_of_two((uint64_t)size * records_per_pass);
		bc.reset_after_records = bc.ring_size / size;
		break;
	case SCENARIO_OVERFLOW:
		bc.ring_size = overflow_ring_size;
		break;
	case SCENARIO_READER_BASE:
		bc.ring_size = READER_RING_SIZE;
		break;
	default:
		bc.ring_size = READER_RING_SIZE;
		bc.with_reader = true;
		break;
	}
	return bc;
}

int
main(int argc, char **argv) {
	const uint32_t sizes[BENCH_SIZES] = {64, 256, 1500, 9000};

	struct bench_cpus cpus;
	const char *spec = argc > 1 ? argv[1] : getenv("RING_BENCH_CPUS");
	if (spec != NULL && *spec != '\0') {
		if (!parse_cpus(spec, &cpus)) {
			bench_die("CPU list must be w0,r0[,w1,r1]");
		}
	} else {
		default_cpus(&cpus);
	}
	int reps = BENCH_DEFAULT_REPS;
	const char *reps_env = getenv("RING_BENCH_REPS");
	if (reps_env != NULL && *reps_env != '\0') {
		reps = atoi(reps_env);
		if (reps < 1 || reps > BENCH_MAX_REPS) {
			bench_die("RING_BENCH_REPS must be 1..32");
		}
	}

	printf("# cpus: writers %d,%d readers %d,%d; median of %d run(s)\n",
	       cpus.writer[0],
	       cpus.writer[1],
	       cpus.reader[0],
	       cpus.reader[1],
	       reps);

	struct bench_results *res = calloc(1, sizeof(*res));
	if (res == NULL) {
		bench_die("failed to allocate results");
	}

	// Alternates which side a phase measures first, so neither
	// systematically runs cold or warm relative to the other.
	bool old_first = true;

	for (int rep = 0; rep < reps; ++rep) {
		for (int si = 0; si < BENCH_SIZES; ++si) {
			for (int count = 1; count <= 2; ++count) {
				for (int sc = 0; sc < SCENARIO_COUNT; ++sc) {
					struct bench_case bc = scenario_case(
						sc, sizes[si], count, &cpus
					);
					if (bc.with_reader &&
					    count > cpus.reader_workers) {
						continue;
					}
					struct bench_sample(*cell
					)[BENCH_MAX_REPS] =
						res->cells[si][count - 1][sc];
					if (old_first) {
						cell[SIDE_OLD][rep] =
							run_old(&bc);
						cell[SIDE_NEW][rep] =
							run_new(&bc);
					} else {
						cell[SIDE_NEW][rep] =
							run_new(&bc);
						cell[SIDE_OLD][rep] =
							run_old(&bc);
					}
					old_first = !old_first;
				}
			}
		}
	}

	printf("# writer only, ns/record\n");
	printf("%-6s %-11s %-7s %12s %12s\n",
	       "size",
	       "scenario",
	       "workers",
	       "old_ns/rec",
	       "new_ns/rec");
	const char *names[] = {"no-overflow", "overflow", "1m-ring"};
	for (int si = 0; si < BENCH_SIZES; ++si) {
		for (int c = 0; c < 2; ++c) {
			for (int sc = 0; sc <= SCENARIO_READER_BASE; ++sc) {
				struct cell_stats st =
					cell_stats(res->cells[si][c][sc], reps);
				printf("%-6u %-11s %-7d %12.2f %12.2f\n",
				       sizes[si],
				       names[sc],
				       c + 1,
				       st.writer_ns[SIDE_OLD],
				       st.writer_ns[SIDE_NEW]);
			}
		}
	}

	printf("# 1 MiB ring with one concurrent reader per writer: writer "
	       "ns/record alone (w) and with the reader (wr), reader "
	       "Mrecords/s, %% of records lost to overwrite, bad = returned "
	       "records with a wrong length, magic or seqno order (all runs)\n"
	);
	printf("%-6s %-7s %8s %8s %8s %8s %9s %9s %7s %7s %5s %5s\n",
	       "size",
	       "workers",
	       "old_w",
	       "old_wr",
	       "new_w",
	       "new_wr",
	       "old_Mrps",
	       "new_Mrps",
	       "old_l%",
	       "new_l%",
	       "o_bad",
	       "n_bad");
	for (int si = 0; si < BENCH_SIZES; ++si) {
		for (int c = 0; c < cpus.reader_workers; ++c) {
			struct cell_stats base = cell_stats(
				res->cells[si][c][SCENARIO_READER_BASE], reps
			);
			struct cell_stats rd = cell_stats(
				res->cells[si][c][SCENARIO_READER], reps
			);
			printf("%-6u %-7d %8.2f %8.2f %8.2f %8.2f %9.2f %9.2f "
			       "%7.1f %7.1f %5lu %5lu\n",
			       sizes[si],
			       c + 1,
			       base.writer_ns[SIDE_OLD],
			       rd.writer_ns[SIDE_OLD],
			       base.writer_ns[SIDE_NEW],
			       rd.writer_ns[SIDE_NEW],
			       rd.reader_mrps[SIDE_OLD],
			       rd.reader_mrps[SIDE_NEW],
			       rd.lost_pct[SIDE_OLD],
			       rd.lost_pct[SIDE_NEW],
			       rd.bad[SIDE_OLD],
			       rd.bad[SIDE_NEW]);
		}
	}

	free(res);
	return 0;
}
