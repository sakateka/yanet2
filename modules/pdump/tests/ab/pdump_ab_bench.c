/*
 * pdump capture handler microbench: BEFORE (private ring_buffer, per-record
 * fetch_add) vs AFTER (ring object, batched publish). One source, built in
 * each scratch tree by modules/pdump/tests/ab/pdump-ab.sh;
 * PDUMP_BENCH_AFTER selects the side.
 *
 * usage: pdump_ab_bench <pkt_size> <ring_bytes> <all|half> <reader 0|1>
 *                       <publish_batch> <reps> <ms_per_rep> <writer_cpu>
 *                       <reader_cpu> [evict_chunk]
 *
 * Writer: calls the real pdump_handle_packets on a 32-packet input front,
 * filter "ip" (all IPv4, or every other packet non-IPv4), snaplen 16384.
 * Ring is pre-faulted and THP-backed (2 MiB aligned mmap + MADV_HUGEPAGE).
 * Warm-up writes >= 3x the ring so every rep runs in steady overflow.
 * Reader: busy-spinning C mirror of the Go reader on each side (load
 * readable/write acquire, copy <= 512 KiB, recheck readable, drop the
 * overwritten prefix, walk records). BEFORE validates magic, AFTER the
 * frame length, as the Go readers do.
 * evict_chunk (AFTER only, bytes, multiple of 4) replaces the ring's
 * eviction chunk after ring_worker_init, to measure how often the writer
 * should pay the eviction fence; 0 or absent keeps the ring's default.
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include <bpf_impl.h>
#include <pcap/pcap.h>
#include <rte_bpf.h>
#include <rte_eal.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>

#include "common/cache.h"
#include "common/memory_address.h"

#include "lib/dataplane/config/zone.h"
#include "lib/dataplane/module/module.h"
#include "lib/dataplane/module/packet_front.h"
#include "lib/dataplane/packet/data.h"
#include "lib/dataplane/packet/packet.h"
#include "lib/dataplane/pipeline/econtext.h"
#include "lib/dataplane_ut/mempool.h"

#include "modules/pdump/dataplane/config.h"
#include "modules/pdump/dataplane/dataplane.h"

#ifdef PDUMP_BENCH_AFTER
#include "common/ring.h"
#include "objects/ring/api/ring_object.h"
#else
#include "modules/pdump/dataplane/ring.h"
#endif

#define BURST 32
#define SNAPLEN 16384u
#define READ_CHUNK (SNAPLEN * 32u)

static uint64_t
now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void
pin(int cpu) {
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
		fprintf(stderr, "pin %d failed\n", cpu);
		exit(1);
	}
}

static void *
huge_alloc(size_t size) {
	size_t align = 2u << 20;
	size_t len = size + align;
	uint8_t *raw =
		mmap(NULL,
		     len,
		     PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS,
		     -1,
		     0);
	if (raw == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	uint8_t *p = (uint8_t *)(((uintptr_t)raw + align - 1) & ~(align - 1));
	madvise(p, size < align ? align : size, MADV_HUGEPAGE);
	memset(p, 0, size);
	return p;
}

// Allocate zeroed memory aligned to the caller's alignment, with the size
// rounded up to that alignment.
//
// A hand-rolled `aligned_alloc(64, ...)` silently under-aligns any type
// whose natural alignment is larger than 64, e.g. struct ring_worker,
// which is YANET_CACHE_LINE_SIZE-aligned: 128 on an arm64 DPDK build, since
// that build's cache line is twice the x86-64 one. Passing a literal 64 in
// such a case returns an address aligned_alloc(3) only promises mod 64,
// not mod 128, which the compiler is free to assume wrong when it reads
// the type back. Every allocation backing a context or metadata struct in
// this harness goes through this helper with the type's own alignment
// instead.
static void *
xaligned_alloc(size_t align, size_t size) {
	size_t rounded = (size + align - 1) & ~(align - 1);
	void *p = aligned_alloc(align, rounded);
	if (p == NULL) {
		perror("aligned_alloc");
		exit(1);
	}
	memset(p, 0, rounded);
	return p;
}

#define XNEW(type, size) ((type *)xaligned_alloc(_Alignof(type), (size)))

static struct rte_bpf *
compile_filter(void) {
	pcap_t *pcap = pcap_open_dead(DLT_EN10MB, SNAPLEN);
	struct bpf_program bf;
	if (pcap_compile(pcap, &bf, "ip", 1, PCAP_NETMASK_UNKNOWN) != 0) {
		fprintf(stderr, "pcap_compile: %s\n", pcap_geterr(pcap));
		exit(1);
	}
	struct rte_bpf_prm *prm = rte_bpf_convert(&bf);
	pcap_freecode(&bf);
	pcap_close(pcap);
	if (prm == NULL) {
		fprintf(stderr, "rte_bpf_convert failed\n");
		exit(1);
	}
	struct rte_bpf *loaded = rte_bpf_load(prm);
	rte_free(prm);
	if (loaded == NULL) {
		fprintf(stderr, "rte_bpf_load failed\n");
		exit(1);
	}
	uint8_t *buf = xaligned_alloc(YANET_CACHE_LINE_SIZE, loaded->sz);
	memcpy(buf, loaded, loaded->sz);
	rte_bpf_destroy(loaded);
	struct rte_bpf *bpf = (struct rte_bpf *)buf;
	bpf->jit.func = NULL;
	bpf->jit.sz = 0;
	size_t bsz = sizeof(bpf[0]);
	size_t xsz = (size_t)bpf->prm.nb_xsym * sizeof(struct rte_bpf_xsym);
	SET_OFFSET_OF(&bpf->prm.xsym, (struct rte_bpf_xsym *)(buf + bsz));
	SET_OFFSET_OF(&bpf->prm.ins, (struct ebpf_insn *)(buf + bsz + xsz));
	return bpf;
}

/* ---- reader (identical protocol on both sides) ---- */

struct reader_args {
	_Atomic uint64_t *write_idx;
	_Atomic uint64_t *readable_idx;
	const uint8_t *data;
	uint64_t mask;
	int cpu;
	_Atomic int stop;
	uint64_t records;
	uint64_t resets;
};

static uint32_t
rd_u32(const uint8_t *p) {
	uint32_t v;
	memcpy(&v, p, 4);
	return v;
}

static void *
reader_main(void *arg) {
	struct reader_args *ra = arg;
	pin(ra->cpu);
	uint8_t *buf = malloc(2 * READ_CHUNK + 65536);
	size_t len = 0;
	uint64_t read_idx = 0;
	uint64_t records = 0, resets = 0;
	while (!atomic_load_explicit(&ra->stop, memory_order_relaxed)) {
		uint64_t readable = atomic_load_explicit(
			ra->readable_idx, memory_order_acquire
		);
		uint64_t write = atomic_load_explicit(
			ra->write_idx, memory_order_acquire
		);
		if (readable > read_idx) {
			len = 0;
			read_idx = readable;
		} else {
			readable = read_idx;
		}
		if (write <= readable) {
			continue;
		}
		uint64_t size = write - readable;
		if (size > READ_CHUNK) {
			size = READ_CHUNK;
		}
		size_t before = len;
		uint64_t s = readable & ra->mask;
		uint64_t e = (s + size) & ra->mask;
		if (e > s) {
			memcpy(buf + len, ra->data + s, size);
		} else {
			uint64_t first = ra->mask + 1 - s;
			memcpy(buf + len, ra->data + s, first);
			memcpy(buf + len + first, ra->data, size - first);
		}
		len += size;
		read_idx += size;
		uint64_t latest = atomic_load_explicit(
			ra->readable_idx, memory_order_acquire
		);
		size_t off = 0;
		if (latest > readable) {
			uint64_t diff = latest - readable + before;
			if (diff > len) {
				len = 0;
				read_idx = latest;
				resets++;
				continue;
			}
			off = diff;
		}
		while (len - off >= 8) {
			uint32_t total = rd_u32(buf + off);
#ifdef PDUMP_BENCH_AFTER
			bool bad = total < 8 || total > ra->mask + 1;
#else
			bool bad = rd_u32(buf + off + 4) != RING_MSG_MAGIC ||
				   total < sizeof(struct ring_msg_hdr);
#endif
			if (bad) {
				len = 0;
				off = 0;
				resets++;
#ifdef PDUMP_BENCH_AFTER
				read_idx = write;
#endif
				break;
			}
			size_t skip = (total + 3) & ~3u;
			if (skip > len - off) {
				break;
			}
			records++;
			off += skip;
		}
		if (off > 0 && len > 0) {
			memmove(buf, buf + off, len - off);
			len -= off;
		}
	}
	ra->records = records;
	ra->resets = resets;
	free(buf);
	return NULL;
}

static int
cmp_u64(const void *a, const void *b) {
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

int
main(int argc, char **argv) {
	if (argc != 10 && argc != 11) {
		fprintf(stderr,
			"usage: %s size ring all|half reader batch reps ms "
			"wcpu rcpu [evict_chunk]\n",
			argv[0]);
		return 2;
	}
	uint32_t pkt_size = (uint32_t)atoi(argv[1]);
	uint32_t ring_size = (uint32_t)strtoul(argv[2], NULL, 0);
	bool half = strcmp(argv[3], "half") == 0;
	int with_reader = atoi(argv[4]);
	uint32_t batch = (uint32_t)atoi(argv[5]);
	int reps = atoi(argv[6]);
	int ms = atoi(argv[7]);
	int wcpu = atoi(argv[8]);
	int rcpu = atoi(argv[9]);
	uint32_t evict_chunk = argc == 11 ? (uint32_t)atoi(argv[10]) : 0;
	(void)batch;
	// The chunk must leave room for the largest record (frame, 32-byte
	// metadata and SNAPLEN of payload) or the writer can never commit it.
	if (evict_chunk % 4 != 0 ||
	    (evict_chunk != 0 && evict_chunk > ring_size - (SNAPLEN + 64u))) {
		fprintf(stderr, "bad evict_chunk %u\n", evict_chunk);
		return 2;
	}

	char lcores[32];
	snprintf(lcores, sizeof(lcores), "%d", wcpu);
	char *eal_argv[] = {
		"pdump_ab_bench",
		"--no-huge",
		"--no-pci",
		"-m",
		"256",
		"--iova-mode=va",
		"-l",
		lcores,
		"--log-level=error",
		"--no-telemetry"
	};
	if (rte_eal_init(sizeof(eal_argv) / sizeof(eal_argv[0]), eal_argv) <
	    0) {
		fprintf(stderr, "rte_eal_init failed\n");
		return 1;
	}
	pin(wcpu);

	struct rte_bpf *bpf = compile_filter();

	struct rte_mempool *mp = test_mempool_create_sized(256);
	struct packet_front pf;
	packet_front_init(&pf);
	for (int i = 0; i < BURST; ++i) {
		struct rte_mbuf *m = rte_pktmbuf_alloc(mp);
		struct packet *p = mbuf_to_packet(m);
		memset(p, 0, sizeof(*p));
		p->mbuf = m;
		uint8_t *d = (uint8_t *)rte_pktmbuf_append(m, pkt_size);
		if (d == NULL) {
			fprintf(stderr, "append %u failed\n", pkt_size);
			return 1;
		}
		for (uint32_t j = 0; j < pkt_size; ++j) {
			d[j] = (uint8_t)j;
		}
		bool ip = !half || (i % 2 == 0);
		d[12] = ip ? 0x08 : 0x99;
		d[13] = ip ? 0x00 : 0x99;
		p->data_len = packet_data_len(p);
		packet_front_input(&pf, p);
	}

	uint8_t *data = huge_alloc(ring_size);

	struct pdump_module_config *config =
		XNEW(struct pdump_module_config, sizeof(*config));
	config->mode = PDUMP_INPUT;
	config->snaplen = SNAPLEN;
	SET_OFFSET_OF(&config->ebpf_program, bpf);

	_Static_assert(sizeof(struct module_ectx) <= 4096, "ectx");
	struct module_ectx *ectx = XNEW(struct module_ectx, 4096);
	ectx->abs_cp_module = &config->cp_module;

	struct reader_args ra;
	memset(&ra, 0, sizeof(ra));
	ra.data = data;
	ra.mask = ring_size - 1;
	ra.cpu = rcpu;

#ifdef PDUMP_BENCH_AFTER
	struct ring_object *obj = XNEW(struct ring_object, 4096);
	struct ring_worker *rw = XNEW(struct ring_worker, sizeof(*rw) * 2);
	ring_worker_init(rw, ring_size, batch);
	if (evict_chunk != 0) {
		rw->local.evict_chunk = evict_chunk;
	}
	evict_chunk = rw->local.evict_chunk;
	SET_OFFSET_OF(&rw->local.data, data);
	obj->worker_count = 1;
	obj->capacity = ring_size;
	obj->publish_batch = batch;
	SET_OFFSET_OF(&obj->workers, rw);
	struct object_ectx *oectx = XNEW(struct object_ectx, 4096);
	oectx->abs_cp_object = &obj->cp_object;
	struct module_object_link_ectx *link =
		XNEW(struct module_object_link_ectx, 4096);
	link->abs_object_ectx = oectx;
	ectx->object_link_count = 1;
	ectx->abs_object_links = link;
	config->ring_link_idx = 0;
	ra.write_idx = &rw->published.write_idx;
	ra.readable_idx = &rw->published.readable_idx;
#define WRITTEN() (rw->local.write_idx)
#else
	struct ring_buffer *rb = XNEW(struct ring_buffer, sizeof(*rb));
	rb->size = ring_size;
	rb->mask = ring_size - 1;
	SET_OFFSET_OF(&rb->data, data);
	SET_OFFSET_OF(&config->rings, rb);
	ra.write_idx = &rb->write_idx;
	ra.readable_idx = &rb->readable_idx;
#define WRITTEN() (atomic_load(&rb->write_idx))
#endif

	struct module *module = new_module_pdump();
	struct dp_worker worker;
	memset(&worker, 0, sizeof(worker));
	worker.idx = 0;
	worker.current_time = 123456789;

	pthread_t rt;
	if (with_reader) {
		pthread_create(&rt, NULL, reader_main, &ra);
	}

#define CALL()                                                                 \
	do {                                                                   \
		module->handler(&worker, ectx, &pf);                           \
		pf.input = pf.output;                                          \
		packet_list_init(&pf.output);                                  \
	} while (0)

	// Warm-up: >= 3 ring wraps and >= 100 ms.
	uint64_t t0 = now_ns();
	while (WRITTEN() < 3ull * ring_size || now_ns() - t0 < 100000000ull) {
		for (int k = 0; k < 64; ++k) {
			CALL();
		}
	}

	double *res = calloc(reps, sizeof(double));
	for (int r = 0; r < reps; ++r) {
		uint64_t calls = 0;
		uint64_t start = now_ns(), end;
		uint64_t limit = (uint64_t)ms * 1000000ull;
		do {
			for (int k = 0; k < 64; ++k) {
				CALL();
			}
			calls += 64;
			end = now_ns();
		} while (end - start < limit);
		res[r] = (double)(end - start) / (double)(calls * BURST);
	}
	if (with_reader) {
		atomic_store(&ra.stop, 1);
		pthread_join(rt, NULL);
	}
	double sorted[reps];
	memcpy(sorted, res, sizeof(double) * reps);
	qsort(sorted, reps, sizeof(double), cmp_u64);
	printf("%s size=%u ring=%u filter=%s reader=%d batch=%u "
	       "chunk=%u ns/pkt min=%.2f med=%.2f max=%.2f rd_records=%lu "
	       "rd_resets=%lu\n",
#ifdef PDUMP_BENCH_AFTER
	       "AFTER ",
#else
	       "BEFORE",
#endif
	       pkt_size,
	       ring_size,
	       half ? "half" : "all",
	       with_reader,
#ifdef PDUMP_BENCH_AFTER
	       batch,
#else
	       0u,
#endif
	       evict_chunk,
	       sorted[0],
	       sorted[reps / 2],
	       sorted[reps - 1],
	       (unsigned long)ra.records,
	       (unsigned long)ra.resets);
	return 0;
}
