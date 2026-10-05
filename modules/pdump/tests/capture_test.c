/*
 * Pdump capture onto a ring object.
 *
 * Pins the producer contract: one publish per handler call, no seqno gap
 * across a filtered packet, a record payload laid out as the 32-byte
 * pdump metadata followed by the captured bytes, an oversize record
 * skipped without consuming a sequence number, mode selecting which
 * queue's packets a config captures, and a missing ring link leaving
 * capture disabled without touching any ring. The module's own
 * control-plane API cannot link into this binary (its cgo exports and
 * DPDK stubs need a Go host process), so the fixture below builds the
 * config, the object link and the eBPF filter by hand, the way
 * modules/fwstate/fuzzing/fwstate.c builds a module_ectx with a
 * hand-linked object.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bpf_impl.h>
#include <pcap/pcap.h>
#include <rte_bpf.h>
#include <rte_eal.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>

#include "api/agent.h"

#include "common/memory_address.h"
#include "common/ring.h"
#include "common/test_assert.h"

#include "lib/dataplane/config/zone.h"
#include "lib/dataplane/module/packet_front.h"
#include "lib/dataplane/packet/data.h"
#include "lib/dataplane/packet/packet.h"
#include "lib/dataplane/pipeline/econtext.h"

#include "lib/dataplane_ut/dataplane_ut.h"
#include "lib/errors/errors.h"
#include "lib/logging/log.h"

#include "modules/pdump/dataplane/config.h"
#include "modules/pdump/dataplane/dataplane.h"
#include "modules/pdump/dataplane/record.h"

#include "objects/ring/api/ring_object.h"

#define PDUMP_TEST_MEMORY_LIMIT (4u * 1024u * 1024u)
#define PDUMP_TEST_RING_CAPACITY 4096u
#define PDUMP_TEST_TIMESTAMP 123456789ULL
#define PDUMP_TEST_RX_DEVICE_ID 7
#define PDUMP_TEST_TX_DEVICE_ID 9

// Compile the classic "ip" filter into a ready-to-run eBPF program.
//
// Accepts every Ethernet frame whose ethertype is IPv4 and rejects every
// other frame, independent of the IP header's own validity. Mirrors the
// pointer layout modules/pdump/api/controlplane.c's filter setter builds
// for a configured filter -- duplicated here because that API cannot link
// into this binary. Unlike that production call, this helper frees the
// rte_bpf_convert output with rte_free, not free: in a build that links
// the real DPDK allocator, only rte_free matches the rte_zmalloc that
// produced it. Returns NULL and leaves err set on failure; the caller
// frees a non-NULL result with free().
static struct rte_bpf *
compile_accept_ip_filter(uint32_t snaplen, yanet_error **err) {
	pcap_t *pcap = pcap_open_dead(DLT_EN10MB, snaplen);
	if (pcap == NULL) {
		yanet_error_add(err, "pcap_open_dead failed");
		return NULL;
	}

	struct bpf_program bf;
	if (pcap_compile(pcap, &bf, "ip", 1, PCAP_NETMASK_UNKNOWN) != 0) {
		yanet_error_add(
			err, "pcap_compile failed: %s", pcap_geterr(pcap)
		);
		pcap_close(pcap);
		return NULL;
	}

	struct rte_bpf_prm *prm = rte_bpf_convert(&bf);
	pcap_freecode(&bf);
	pcap_close(pcap);
	if (prm == NULL) {
		yanet_error_add(err, "rte_bpf_convert failed");
		return NULL;
	}

	struct rte_bpf *loaded = rte_bpf_load(prm);
	rte_free(prm);
	if (loaded == NULL) {
		yanet_error_add(err, "rte_bpf_load failed");
		return NULL;
	}

	uint64_t buf_sz = loaded->sz;
	uint8_t *buf = malloc(buf_sz);
	if (buf == NULL) {
		yanet_error_add(err, "failed to allocate the filter copy");
		rte_bpf_destroy(loaded);
		return NULL;
	}
	memcpy(buf, loaded, buf_sz);
	rte_bpf_destroy(loaded);

	struct rte_bpf *bpf = (struct rte_bpf *)buf;
	// JIT is not wired into the dataplane's own copy either; keep the
	// interpreter path the real handler always runs.
	bpf->jit.func = NULL;
	bpf->jit.sz = 0;

	size_t bsz = sizeof(bpf[0]);
	size_t xsz = (size_t)bpf->prm.nb_xsym * sizeof(struct rte_bpf_xsym);
	SET_OFFSET_OF(&bpf->prm.xsym, (struct rte_bpf_xsym *)(buf + bsz));
	SET_OFFSET_OF(&bpf->prm.ins, (struct ebpf_insn *)(buf + bsz + xsz));

	return bpf;
}

// One pdump capture harness: a ring object, a hand-linked module_ectx
// resolving to it, and an "ip" eBPF filter at a chosen snaplen.
//
// Stands in for what the control plane and the ectx build normally wire
// up for a published generation, since neither runs for a config built by
// hand.
struct pdump_capture_fixture {
	struct agent *agent;
	struct cp_object *ring_object;
	struct ring_worker *ring;
	uint8_t *ring_data;
	struct rte_bpf *bpf;
	struct module *module;
	struct pdump_module_config config;
	struct module_ectx module_ectx;
	struct module_object_link_ectx link;
	struct object_ectx ring_object_ectx;
	struct dp_worker dp_worker;
};

// Parameters for build_pdump_capture_fixture, grouped into one struct
// because the builder already takes more arguments than fit comfortably
// as positional parameters.
struct pdump_capture_fixture_params {
	const char *agent_name;
	const char *ring_name;
	uint32_t capacity;
	uint32_t snaplen;
	uint64_t worker_idx;
};

static int
build_pdump_capture_fixture(
	struct dataplane_ut *ut,
	const struct pdump_capture_fixture_params *params,
	struct pdump_capture_fixture *fx
) {
	memset(fx, 0, sizeof(*fx));
	yanet_error *err = NULL;

	struct yanet_shm *shm = dataplane_ut_shm(ut);
	TEST_ASSERT_NOT_NULL(shm, "dataplane_ut_shm returned NULL");

	fx->agent = agent_attach(
		shm, 0, params->agent_name, PDUMP_TEST_MEMORY_LIMIT, &err
	);
	TEST_ASSERT_NOT_NULL(
		fx->agent,
		"agent_attach failed: %s",
		err ? yanet_error_message(err) : "?"
	);

	fx->ring_object = ring_object_config_new(
		fx->agent,
		params->ring_name,
		params->capacity,
		RING_PUBLISH_BATCH_DEFAULT,
		&err
	);
	TEST_ASSERT_NOT_NULL(
		fx->ring_object,
		"ring_object_config_new failed: %s",
		err ? yanet_error_message(err) : "?"
	);

	fx->ring = ring_object_worker(fx->ring_object, params->worker_idx);
	fx->ring_data =
		ring_object_worker_data(fx->ring_object, params->worker_idx);
	TEST_ASSERT_NOT_NULL(
		fx->ring,
		"ring object has no worker %lu",
		(unsigned long)params->worker_idx
	);
	TEST_ASSERT_NOT_NULL(
		fx->ring_data,
		"ring object worker %lu has no data area",
		(unsigned long)params->worker_idx
	);

	fx->bpf = compile_accept_ip_filter(params->snaplen, &err);
	TEST_ASSERT_NOT_NULL(
		fx->bpf,
		"compile_accept_ip_filter failed: %s",
		err ? yanet_error_message(err) : "?"
	);

	fx->config.snaplen = params->snaplen;
	fx->config.mode = PDUMP_INPUT;
	fx->config.ring_link_idx = 0;
	SET_OFFSET_OF(&fx->config.ebpf_program, fx->bpf);

	fx->ring_object_ectx.abs_cp_object = fx->ring_object;
	fx->link.abs_object_ectx = &fx->ring_object_ectx;
	fx->module_ectx.object_link_count = 1;
	fx->module_ectx.abs_object_links = &fx->link;
	fx->module_ectx.abs_cp_module = &fx->config.cp_module;

	fx->dp_worker.idx = params->worker_idx;
	fx->dp_worker.current_time = PDUMP_TEST_TIMESTAMP;

	fx->module = new_module_pdump();
	TEST_ASSERT_NOT_NULL(fx->module, "new_module_pdump failed");

	return TEST_SUCCESS;
}

static void
destroy_pdump_capture_fixture(struct pdump_capture_fixture *fx) {
	free(fx->module);
	free(fx->bpf);
	if (fx->ring_object != NULL) {
		yanet_error *err = NULL;
		ring_object_config_free(fx->ring_object, &err);
		yanet_error_free(err);
	}
	if (fx->agent != NULL) {
		agent_detach(fx->agent);
	}
}

static void
free_packet_list(struct packet_list *list) {
	struct packet *packet;
	while ((packet = packet_list_pop(list)) != NULL) {
		rte_pktmbuf_free(packet_to_mbuf(packet));
	}
}

// One test case: a fixture and the packet front driven through its
// handler, built and torn down together by the case's run_..._test
// wrapper.
//
// The wrapper destroys the case after its body returns, whether the body
// returned success or an assertion inside it returned early, so a
// failing case never leaks the fixture's agent or ring object into the
// cases that run after it.
struct pdump_capture_case {
	struct pdump_capture_fixture fx;
	struct packet_front pf;
};

static void
destroy_pdump_capture_case(struct pdump_capture_case *tc) {
	free_packet_list(&tc->pf.output);
	free_packet_list(&tc->pf.input);
	free_packet_list(&tc->pf.drop);
	destroy_pdump_capture_fixture(&tc->fx);
}

// Build one Ethernet frame mbuf with an incrementing byte pattern and the
// given ethertype, so a captured prefix can be checked against the
// source bytes and the "ip" filter sees exactly the ethertype asked for.
//
// Returns NULL on mbuf exhaustion.
static struct packet *
build_test_packet(struct dataplane_ut *ut, bool ipv4, uint16_t total_len) {
	struct rte_mbuf *mbuf = dataplane_ut_alloc_mbuf(ut);
	if (mbuf == NULL) {
		return NULL;
	}
	struct packet *packet = mbuf_to_packet(mbuf);
	memset(packet, 0, sizeof(*packet));
	packet->mbuf = mbuf;

	uint8_t *data = (uint8_t *)rte_pktmbuf_append(mbuf, total_len);
	if (data == NULL) {
		rte_pktmbuf_free(mbuf);
		return NULL;
	}
	for (uint16_t i = 0; i < total_len; ++i) {
		data[i] = (uint8_t)i;
	}
	if (total_len >= 14) {
		data[12] = ipv4 ? 0x08 : 0x99;
		data[13] = ipv4 ? 0x00 : 0x99;
	}
	packet->data_len = packet_data_len(packet);
	return packet;
}

// One pdump record as a reader decodes it: the generic frame, the
// metadata block and the captured bytes.
//
// Copied out field by field, because a ring record is only 4-byte
// aligned.
struct pdump_test_record {
	uint32_t total_len;
	uint32_t seqno;
	struct pdump_record_hdr hdr;
	uint8_t payload[256];
};

// Copy one record at the given logical ring position into rec, the way
// a real reader must: every field is copied out before it is read,
// because a ring record is only 4-byte aligned.
static void
read_ring_record(
	struct ring_worker *ring,
	uint8_t *ring_data,
	uint64_t pos,
	struct pdump_test_record *rec
) {
	uint32_t mask = ring->local.mask;

	struct ring_record_frame frame;
	for (size_t i = 0; i < sizeof(frame); ++i) {
		((uint8_t *)&frame)[i] = ring_data[(pos + i) & mask];
	}
	rec->total_len = frame.total_len;
	rec->seqno = frame.seqno;

	uint64_t hdr_pos = pos + sizeof(frame);
	for (size_t i = 0; i < sizeof(rec->hdr); ++i) {
		((uint8_t *)&rec->hdr)[i] = ring_data[(hdr_pos + i) & mask];
	}

	uint32_t payload_len = frame.total_len - (uint32_t)sizeof(frame) -
			       (uint32_t)sizeof(rec->hdr);
	if (payload_len > sizeof(rec->payload)) {
		payload_len = sizeof(rec->payload);
	}
	uint64_t payload_pos = hdr_pos + sizeof(rec->hdr);
	for (uint32_t i = 0; i < payload_len; ++i) {
		rec->payload[i] = ring_data[(payload_pos + i) & mask];
	}
}

// Read a published ring position with the acquire order a real reader
// must use, even though this single-threaded harness has no concurrent
// writer to order against.
static uint64_t
load_published(const _Atomic uint64_t *pos) {
	return atomic_load_explicit(pos, memory_order_acquire);
}

// A handler call that commits fewer records than the ring's publish batch
// still leaves them readable at the published write position once it
// returns, because the handler publishes once after both queues.
static int
run_pdump_capture_publish_after_call_body(
	struct dataplane_ut *ut, struct pdump_capture_case *tc
) {
	const int packet_count = 3;
	TEST_ASSERT(
		packet_count < (int)RING_PUBLISH_BATCH_DEFAULT,
		"the scenario needs fewer packets than the publish batch"
	);

	for (int i = 0; i < packet_count; ++i) {
		struct packet *packet = build_test_packet(ut, true, 60);
		TEST_ASSERT_NOT_NULL(packet, "build_test_packet failed");
		packet_front_input(&tc->pf, packet);
	}

	tc->fx.module->handler(&tc->fx.dp_worker, &tc->fx.module_ectx, &tc->pf);

	TEST_ASSERT_EQUAL(
		(long)packet_list_count(&tc->pf.input),
		0L,
		"the handler must always pass every input packet"
	);
	TEST_ASSERT_EQUAL(
		(long)packet_list_count(&tc->pf.output),
		(long)packet_count,
		"every packet must reach the output list"
	);

	uint64_t write_idx = load_published(&tc->fx.ring->published.write_idx);
	uint64_t readable_idx =
		load_published(&tc->fx.ring->published.readable_idx);
	TEST_ASSERT_EQUAL(
		(long)readable_idx,
		0L,
		"no eviction is expected in this scenario"
	);
	TEST_ASSERT(
		write_idx > 0,
		"the write position must already be published when the call "
		"returns"
	);

	uint64_t pos = 0;
	int seen = 0;
	while (pos < write_idx) {
		struct pdump_test_record rec;
		read_ring_record(tc->fx.ring, tc->fx.ring_data, pos, &rec);
		TEST_ASSERT_EQUAL(
			(long)rec.seqno,
			(long)seen,
			"record %d must carry seqno %d",
			seen,
			seen
		);
		pos += ring_align4(rec.total_len);
		++seen;
	}
	TEST_ASSERT_EQUAL(
		(long)seen,
		(long)packet_count,
		"every committed record must be visible past the published "
		"write position"
	);
	TEST_ASSERT_EQUAL(
		(long)pos,
		(long)write_idx,
		"records must exactly fill the published range"
	);

	return TEST_SUCCESS;
}

static int
run_pdump_capture_publish_after_call_test(struct dataplane_ut *ut) {
	struct pdump_capture_case tc;
	packet_front_init(&tc.pf);

	struct pdump_capture_fixture_params params = {
		.agent_name = "pdump-publish",
		.ring_name = "ring-publish",
		.capacity = PDUMP_TEST_RING_CAPACITY,
		.snaplen = 128,
		.worker_idx = 0,
	};
	int res = build_pdump_capture_fixture(ut, &params, &tc.fx);
	if (res == TEST_SUCCESS) {
		res = run_pdump_capture_publish_after_call_body(ut, &tc);
	}

	destroy_pdump_capture_case(&tc);
	return res;
}

// A packet the filter rejects consumes no seqno: the committed records
// from one handler call carry contiguous sequence numbers even when
// rejected packets ran between them.
static int
run_pdump_capture_filtered_no_seqno_gap_body(
	struct dataplane_ut *ut, struct pdump_capture_case *tc
) {
	const bool accept[] = {true, false, true, false, true};
	const int total = (int)(sizeof(accept) / sizeof(accept[0]));
	int expected_records = 0;
	for (int i = 0; i < total; ++i) {
		if (accept[i]) {
			++expected_records;
		}
	}

	for (int i = 0; i < total; ++i) {
		struct packet *packet = build_test_packet(ut, accept[i], 60);
		TEST_ASSERT_NOT_NULL(packet, "build_test_packet failed");
		packet_front_input(&tc->pf, packet);
	}

	tc->fx.module->handler(&tc->fx.dp_worker, &tc->fx.module_ectx, &tc->pf);

	TEST_ASSERT_EQUAL(
		(long)packet_list_count(&tc->pf.output),
		(long)total,
		"every packet, filtered or not, must reach the output list"
	);

	uint64_t write_idx = load_published(&tc->fx.ring->published.write_idx);
	uint64_t pos = 0;
	int seen = 0;
	while (pos < write_idx) {
		struct pdump_test_record rec;
		read_ring_record(tc->fx.ring, tc->fx.ring_data, pos, &rec);
		TEST_ASSERT_EQUAL(
			(long)rec.seqno,
			(long)seen,
			"the filtered packets interspersed between accepted "
			"ones must leave no seqno gap"
		);
		pos += ring_align4(rec.total_len);
		++seen;
	}
	TEST_ASSERT_EQUAL(
		(long)seen,
		(long)expected_records,
		"only the accepted packets must produce a record"
	);

	return TEST_SUCCESS;
}

static int
run_pdump_capture_filtered_no_seqno_gap_test(struct dataplane_ut *ut) {
	struct pdump_capture_case tc;
	packet_front_init(&tc.pf);

	struct pdump_capture_fixture_params params = {
		.agent_name = "pdump-nogap",
		.ring_name = "ring-nogap",
		.capacity = PDUMP_TEST_RING_CAPACITY,
		.snaplen = 128,
		.worker_idx = 0,
	};
	int res = build_pdump_capture_fixture(ut, &params, &tc.fx);
	if (res == TEST_SUCCESS) {
		res = run_pdump_capture_filtered_no_seqno_gap_body(ut, &tc);
	}

	destroy_pdump_capture_case(&tc);
	return res;
}

// A record's payload is the 32-byte pdump metadata, naming the capturing
// worker, the original packet and its devices, followed by
// min(snaplen, data_len) bytes of the packet itself.
static int
run_pdump_capture_record_metadata_and_payload_body(
	struct dataplane_ut *ut, struct pdump_capture_case *tc
) {
	const uint32_t snaplen = 40;
	const uint16_t packet_len = 60;

	struct packet *packet = build_test_packet(ut, true, packet_len);
	TEST_ASSERT_NOT_NULL(packet, "build_test_packet failed");
	packet->rx_device_id = PDUMP_TEST_RX_DEVICE_ID;
	packet->tx_device_id = PDUMP_TEST_TX_DEVICE_ID;
	// The source bytes the captured record's payload is expected to
	// match, snapshotted before capture runs.
	uint8_t expected[40];
	memcpy(expected,
	       rte_pktmbuf_mtod(packet_to_mbuf(packet), uint8_t *),
	       snaplen);
	packet_front_input(&tc->pf, packet);

	tc->fx.module->handler(&tc->fx.dp_worker, &tc->fx.module_ectx, &tc->pf);

	uint64_t write_idx = load_published(&tc->fx.ring->published.write_idx);
	TEST_ASSERT(write_idx > 0, "the accepted packet must produce a record");

	struct pdump_test_record rec;
	read_ring_record(tc->fx.ring, tc->fx.ring_data, 0, &rec);

	uint32_t expected_total_len =
		(uint32_t)sizeof(struct ring_record_frame) +
		(uint32_t)sizeof(struct pdump_record_hdr) + snaplen;
	TEST_ASSERT_EQUAL(
		(long)rec.total_len,
		(long)expected_total_len,
		"total_len must be the frame, the metadata and the capped "
		"capture length"
	);
	TEST_ASSERT_EQUAL(
		(long)rec.hdr.magic,
		(long)PDUMP_RECORD_MAGIC,
		"magic must identify the metadata block"
	);
	TEST_ASSERT_EQUAL(
		(long)rec.hdr.worker_idx,
		(long)tc->fx.dp_worker.idx,
		"worker_idx must name the capturing worker"
	);
	TEST_ASSERT_EQUAL(
		(long)rec.hdr.packet_len,
		(long)packet_len,
		"packet_len must be the original packet length, not the "
		"capped capture length"
	);
	TEST_ASSERT_EQUAL(
		(long)rec.hdr.timestamp,
		(long)PDUMP_TEST_TIMESTAMP,
		"timestamp must be the worker clock the handler read"
	);
	TEST_ASSERT_EQUAL(
		(long)rec.hdr.rx_device_id,
		(long)PDUMP_TEST_RX_DEVICE_ID,
		"rx_device_id must name the packet's receiving device"
	);
	TEST_ASSERT_EQUAL(
		(long)rec.hdr.tx_device_id,
		(long)PDUMP_TEST_TX_DEVICE_ID,
		"tx_device_id must name the packet's transmitting device"
	);
	TEST_ASSERT_EQUAL(
		(long)rec.hdr.queue,
		(long)PDUMP_INPUT,
		"queue must name the input list"
	);
	TEST_ASSERT_EQUAL(
		memcmp(rec.payload, expected, snaplen),
		0,
		"the captured bytes must be the packet's first min(snaplen, "
		"data_len) bytes"
	);

	return TEST_SUCCESS;
}

static int
run_pdump_capture_record_metadata_and_payload_test(struct dataplane_ut *ut) {
	struct pdump_capture_case tc;
	packet_front_init(&tc.pf);

	struct pdump_capture_fixture_params params = {
		.agent_name = "pdump-metadata",
		.ring_name = "ring-metadata",
		.capacity = PDUMP_TEST_RING_CAPACITY,
		.snaplen = 40,
		.worker_idx = 1,
	};
	int res = build_pdump_capture_fixture(ut, &params, &tc.fx);
	if (res == TEST_SUCCESS) {
		res = run_pdump_capture_record_metadata_and_payload_body(
			ut, &tc
		);
	}

	destroy_pdump_capture_case(&tc);
	return res;
}

// A config in PDUMP_DROPS mode tags its records with the drop queue and
// leaves the drop list itself untouched; it never looks at the input
// list.
static int
run_pdump_capture_drops_mode_body(
	struct dataplane_ut *ut, struct pdump_capture_case *tc
) {
	tc->fx.config.mode = PDUMP_DROPS;

	struct packet *packet = build_test_packet(ut, true, 60);
	TEST_ASSERT_NOT_NULL(packet, "build_test_packet failed");
	packet_front_drop(&tc->pf, packet);

	tc->fx.module->handler(&tc->fx.dp_worker, &tc->fx.module_ectx, &tc->pf);

	TEST_ASSERT_EQUAL(
		(long)packet_list_count(&tc->pf.drop),
		1L,
		"a DROPS-mode config must leave the drop list for the "
		"pipeline to route, not drain it"
	);
	TEST_ASSERT_EQUAL(
		(long)packet_list_count(&tc->pf.output),
		0L,
		"a DROPS-mode config must not read the input list"
	);

	uint64_t write_idx = load_published(&tc->fx.ring->published.write_idx);
	TEST_ASSERT(write_idx > 0, "the dropped packet must produce a record");

	struct pdump_test_record rec;
	read_ring_record(tc->fx.ring, tc->fx.ring_data, 0, &rec);
	TEST_ASSERT_EQUAL(
		(long)rec.hdr.queue,
		(long)PDUMP_DROPS,
		"a record captured from the drop list must carry the drop "
		"queue tag"
	);

	return TEST_SUCCESS;
}

static int
run_pdump_capture_drops_mode_test(struct dataplane_ut *ut) {
	struct pdump_capture_case tc;
	packet_front_init(&tc.pf);

	struct pdump_capture_fixture_params params = {
		.agent_name = "pdump-drops",
		.ring_name = "ring-drops",
		.capacity = PDUMP_TEST_RING_CAPACITY,
		.snaplen = 128,
		.worker_idx = 0,
	};
	int res = build_pdump_capture_fixture(ut, &params, &tc.fx);
	if (res == TEST_SUCCESS) {
		res = run_pdump_capture_drops_mode_body(ut, &tc);
	}

	destroy_pdump_capture_case(&tc);
	return res;
}

// A config in PDUMP_ALL mode tags each record with the queue its packet
// came from: the drop queue first, since the handler drains it before
// the input queue.
static int
run_pdump_capture_all_mode_body(
	struct dataplane_ut *ut, struct pdump_capture_case *tc
) {
	tc->fx.config.mode = PDUMP_ALL;

	struct packet *dropped = build_test_packet(ut, true, 60);
	TEST_ASSERT_NOT_NULL(dropped, "build_test_packet failed");
	packet_front_drop(&tc->pf, dropped);

	struct packet *input = build_test_packet(ut, true, 60);
	TEST_ASSERT_NOT_NULL(input, "build_test_packet failed");
	packet_front_input(&tc->pf, input);

	tc->fx.module->handler(&tc->fx.dp_worker, &tc->fx.module_ectx, &tc->pf);

	uint64_t write_idx = load_published(&tc->fx.ring->published.write_idx);
	uint64_t pos = 0;

	struct pdump_test_record drop_rec;
	read_ring_record(tc->fx.ring, tc->fx.ring_data, pos, &drop_rec);
	TEST_ASSERT_EQUAL(
		(long)drop_rec.hdr.queue,
		(long)PDUMP_DROPS,
		"the first record under ALL mode must come from the drop "
		"queue, processed before the input queue"
	);
	pos += ring_align4(drop_rec.total_len);
	TEST_ASSERT(pos < write_idx, "a second record must follow");

	struct pdump_test_record input_rec;
	read_ring_record(tc->fx.ring, tc->fx.ring_data, pos, &input_rec);
	TEST_ASSERT_EQUAL(
		(long)input_rec.hdr.queue,
		(long)PDUMP_INPUT,
		"the second record under ALL mode must come from the input "
		"queue"
	);
	pos += ring_align4(input_rec.total_len);
	TEST_ASSERT_EQUAL(
		(long)pos,
		(long)write_idx,
		"only these two records must be published"
	);

	return TEST_SUCCESS;
}

static int
run_pdump_capture_all_mode_test(struct dataplane_ut *ut) {
	struct pdump_capture_case tc;
	packet_front_init(&tc.pf);

	struct pdump_capture_fixture_params params = {
		.agent_name = "pdump-all-mode",
		.ring_name = "ring-all-mode",
		.capacity = PDUMP_TEST_RING_CAPACITY,
		.snaplen = 128,
		.worker_idx = 0,
	};
	int res = build_pdump_capture_fixture(ut, &params, &tc.fx);
	if (res == TEST_SUCCESS) {
		res = run_pdump_capture_all_mode_body(ut, &tc);
	}

	destroy_pdump_capture_case(&tc);
	return res;
}

// A config with no ring link leaves capture disabled: nothing is
// written to the ring it would otherwise use, and every packet still
// passes.
static int
run_pdump_capture_no_ring_link_body(
	struct dataplane_ut *ut, struct pdump_capture_case *tc
) {
	tc->fx.config.ring_link_idx = PDUMP_RING_LINK_NONE;

	struct packet *packet = build_test_packet(ut, true, 60);
	TEST_ASSERT_NOT_NULL(packet, "build_test_packet failed");
	packet_front_input(&tc->pf, packet);

	tc->fx.module->handler(&tc->fx.dp_worker, &tc->fx.module_ectx, &tc->pf);

	TEST_ASSERT_EQUAL(
		(long)packet_list_count(&tc->pf.input),
		0L,
		"the handler must still pass the packet with no ring link"
	);
	TEST_ASSERT_EQUAL(
		(long)packet_list_count(&tc->pf.output),
		1L,
		"the handler must still pass the packet with no ring link"
	);
	TEST_ASSERT_EQUAL(
		(long)load_published(&tc->fx.ring->published.write_idx),
		0L,
		"a config with no ring link must never write to the ring it "
		"would otherwise use"
	);

	return TEST_SUCCESS;
}

static int
run_pdump_capture_no_ring_link_test(struct dataplane_ut *ut) {
	struct pdump_capture_case tc;
	packet_front_init(&tc.pf);

	struct pdump_capture_fixture_params params = {
		.agent_name = "pdump-no-link",
		.ring_name = "ring-no-link",
		.capacity = PDUMP_TEST_RING_CAPACITY,
		.snaplen = 128,
		.worker_idx = 0,
	};
	int res = build_pdump_capture_fixture(ut, &params, &tc.fx);
	if (res == TEST_SUCCESS) {
		res = run_pdump_capture_no_ring_link_body(ut, &tc);
	}

	destroy_pdump_capture_case(&tc);
	return res;
}

// A record too large for the ring is skipped without consuming a
// sequence number: a later packet whose record does fit still gets the
// sequence number the skipped one would have taken.
static int
run_pdump_capture_oversize_record_skipped_body(
	struct dataplane_ut *ut, struct pdump_capture_case *tc
) {
	uint32_t max = ring_object_max_record_len(tc->fx.ring_object);
	TEST_ASSERT_EQUAL(
		(long)max,
		60L,
		"a capacity-64 ring's max record length must be 60, the "
		"scenario this case is built around"
	);

	// 40 (frame + metadata) + 50 captured bytes = 90, over max: refused.
	struct packet *oversize = build_test_packet(ut, true, 50);
	TEST_ASSERT_NOT_NULL(oversize, "build_test_packet failed");
	packet_front_input(&tc->pf, oversize);

	// 40 + 20 = 60, exactly max: accepted.
	struct packet *fitting = build_test_packet(ut, true, 20);
	TEST_ASSERT_NOT_NULL(fitting, "build_test_packet failed");
	packet_front_input(&tc->pf, fitting);

	tc->fx.module->handler(&tc->fx.dp_worker, &tc->fx.module_ectx, &tc->pf);

	TEST_ASSERT_EQUAL(
		(long)packet_list_count(&tc->pf.output),
		2L,
		"both packets must pass even though only one is captured"
	);

	uint64_t write_idx = load_published(&tc->fx.ring->published.write_idx);
	TEST_ASSERT(write_idx > 0, "the fitting packet must produce a record");

	struct pdump_test_record rec;
	read_ring_record(tc->fx.ring, tc->fx.ring_data, 0, &rec);
	TEST_ASSERT_EQUAL(
		(long)rec.seqno,
		0L,
		"the skipped oversize record must not have consumed a "
		"sequence number"
	);
	TEST_ASSERT_EQUAL(
		(long)rec.hdr.packet_len,
		20L,
		"the one record present must be the later, fitting packet"
	);
	TEST_ASSERT_EQUAL(
		(long)(ring_align4(rec.total_len)),
		(long)write_idx,
		"only one record must be published; the oversize packet left "
		"no trace"
	);

	return TEST_SUCCESS;
}

static int
run_pdump_capture_oversize_record_skipped_test(struct dataplane_ut *ut) {
	struct pdump_capture_case tc;
	packet_front_init(&tc.pf);

	struct pdump_capture_fixture_params params = {
		.agent_name = "pdump-oversize",
		.ring_name = "ring-oversize",
		.capacity = 64,
		.snaplen = 64,
		.worker_idx = 0,
	};
	int res = build_pdump_capture_fixture(ut, &params, &tc.fx);
	if (res == TEST_SUCCESS) {
		res = run_pdump_capture_oversize_record_skipped_body(ut, &tc);
	}

	destroy_pdump_capture_case(&tc);
	return res;
}

int
main(void) {
	log_enable_name("debug");

	// A minimal, hugepage-free EAL setup: just enough for rte_bpf_load's
	// mmap-based allocation and rte_malloc's heap, since this harness
	// never touches a NIC or a hugepage-backed mempool.
	char prog_name[] = "pdump_capture_test";
	char *eal_argv[] = {
		prog_name,
		"--no-huge",
		"--no-pci",
		"-m",
		"128",
		"--iova-mode=va"
	};
	if (rte_eal_init(sizeof(eal_argv) / sizeof(eal_argv[0]), eal_argv) <
	    0) {
		fprintf(stderr, "rte_eal_init failed\n");
		return 1;
	}

	const char *objs_to_load[] = {RING_OBJECT_TYPE};
	struct dataplane_ut_config cfg = {
		.cp_memory = 1u << 26,
		.dp_memory = 1u << 20,
		.worker_count = 2,
		.objects_to_load = objs_to_load,
		.objects_to_load_count = 1,
	};

	struct dataplane_ut *ut = dataplane_ut_new(&cfg);
	if (ut == NULL) {
		fprintf(stderr, "dataplane_ut_new failed\n");
		return 1;
	}

	struct test_case {
		const char *name;
		int (*func)(struct dataplane_ut *ut);
	};

	struct test_case cases[] = {
		{"publish_after_call", run_pdump_capture_publish_after_call_test
		},
		{"filtered_no_seqno_gap",
		 run_pdump_capture_filtered_no_seqno_gap_test},
		{"record_metadata_and_payload",
		 run_pdump_capture_record_metadata_and_payload_test},
		{"drops_mode", run_pdump_capture_drops_mode_test},
		{"all_mode", run_pdump_capture_all_mode_test},
		{"no_ring_link", run_pdump_capture_no_ring_link_test},
		{"oversize_record_skipped",
		 run_pdump_capture_oversize_record_skipped_test},
	};

	int res = TEST_SUCCESS;
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		LOG(INFO, "%s running", cases[i].name);
		res = cases[i].func(ut);
		if (res != TEST_SUCCESS) {
			LOG(ERROR, "%s failed", cases[i].name);
			break;
		}
		LOG(INFO, "%s passed", cases[i].name);
	}

	dataplane_ut_free(ut);

	return (res == TEST_SUCCESS) ? 0 : 1;
}
