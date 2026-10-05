#include "config.h"

#include <string.h>

#include <bpf_impl.h>
#include <rte_bpf.h>

#include "lib/dataplane/config/zone.h"
#include "lib/dataplane/module/module.h"
#include "lib/dataplane/module/packet_front.h"
#include "lib/dataplane/packet/data.h"
#include "lib/dataplane/packet/packet.h"
#include "lib/dataplane/pipeline/econtext.h"

#include "lib/controlplane/config/econtext.h"

#include "objects/ring/api/ring_object.h"

#include "record.h"

static inline void
process_queue(
	struct packet *first_pkt,
	struct rte_bpf *bpf,
	struct ring_worker *ring,
	uint8_t *ring_data,
	const struct dp_worker *dp_worker,
	uint32_t snaplen,
	enum pdump_mode queue
) {
	// Stamp every record from the worker clock, which is the same time
	// base the rest of the dataplane uses. Hardware RX timestamps are not
	// portable nanoseconds: mlx5 hands over a raw device counter unless
	// the NIC runs in real-time mode, so they cannot share this field.
	uint64_t timestamp = dp_worker->current_time;

	for (struct packet *pkt = first_pkt; pkt != NULL; pkt = pkt->next) {
		struct rte_mbuf *mbuf = packet_to_mbuf(pkt);

		int rc = rte_bpf_exec(bpf, (void *)mbuf);
		if (!rc) {
			continue;
		}

		// NOTE: We do not support multi-segment mbuf;
		// therefore, data_len must equal pkt_len.
		uint16_t packet_len = rte_pktmbuf_data_len(mbuf);
		uint32_t capture_len =
			packet_len > snaplen ? snaplen : packet_len;
		uint32_t total_len =
			(uint32_t)sizeof(struct ring_record_frame) +
			(uint32_t)sizeof(struct pdump_record_hdr) + capture_len;

		// A record larger than the ring's max record size is refused;
		// the packet still passes, just uncaptured. A full ring never
		// refuses: it evicts published records instead.
		if (ring_worker_prepare(ring, ring_data, total_len)) {
			continue;
		}

		struct pdump_record_hdr hdr = {
			.magic = PDUMP_RECORD_MAGIC,
			.packet_len = packet_len,
			.timestamp = timestamp,
			.worker_idx = (uint32_t)dp_worker->idx,
			// FIXME
			// .pipeline_idx = pkt->pipeline_idx,
			.rx_device_id = pkt->rx_device_id,
			.tx_device_id = pkt->tx_device_id,
			.queue = (uint8_t)queue,
		};

		uint8_t *payload = rte_pktmbuf_mtod(mbuf, uint8_t *);
		ring_worker_write(
			ring,
			ring_data,
			sizeof(struct ring_record_frame),
			(const uint8_t *)&hdr,
			sizeof(hdr)
		);
		ring_worker_write(
			ring,
			ring_data,
			sizeof(struct ring_record_frame) + sizeof(hdr),
			payload,
			capture_len
		);
		ring_worker_commit(ring, total_len);
	}
}

void
pdump_handle_packets(
	struct dp_worker *dp_worker,
	struct module_ectx *module_ectx,
	struct packet_front *packet_front
) {
	struct pdump_module_config *config = container_of(
		module_ectx->abs_cp_module,
		struct pdump_module_config,
		cp_module
	);

	// Resolve the linked ring's worker through the generic object-link
	// path. An absent link, an unresolved object or a worker index past
	// the ring's own count all leave capture disabled for this call.
	struct ring_worker *ring = NULL;
	uint8_t *ring_data = NULL;
	if (config->ring_link_idx != PDUMP_RING_LINK_NONE) {
		struct module_object_link_ectx *link = object_link_get_address(
			module_ectx, config->ring_link_idx
		);
		if (link != NULL) {
			struct cp_object *ring_object =
				link->abs_object_ectx->abs_cp_object;
			if (ring_object != NULL) {
				ring = ring_object_worker(
					ring_object, dp_worker->idx
				);
				ring_data = ring_object_worker_data(
					ring_object, dp_worker->idx
				);
			}
		}
	}

	if (ring != NULL && ring_data != NULL) {
		struct rte_bpf *bpf_shm = ADDR_OF(&config->ebpf_program);
		struct rte_bpf bpf = *bpf_shm;

		bpf.prm.ins = ADDR_OF(&bpf_shm->prm.ins);
		bpf.prm.xsym = NULL;
		bpf.prm.nb_xsym = 0;

		// First, process dropped packets.
		if (config->mode & PDUMP_DROPS &&
		    packet_front->drop.first != NULL) {
			process_queue(
				packet_front->drop.first,
				&bpf,
				ring,
				ring_data,
				dp_worker,
				config->snaplen,
				PDUMP_DROPS
			);
		}

		// Then process the input packets.
		if (config->mode & PDUMP_INPUT &&
		    packet_front->input.first != NULL) {
			process_queue(
				packet_front->input.first,
				&bpf,
				ring,
				ring_data,
				dp_worker,
				config->snaplen,
				PDUMP_INPUT
			);
		}

		// Publish once per call, after both queues, so a call that
		// commits fewer records than the publish batch still leaves
		// them readable when it returns.
		ring_worker_publish(ring);
	}

	// We should always pass the packets in the input queue
	packet_front_pass(packet_front);
}

struct pdump_module {
	struct module module;
};

static void
pdump_module_commit(struct dp_config *dp_config, struct cp_module *cp_module) {
	(void)dp_config;
	(void)cp_module;
}

struct module *
new_module_pdump() {
	struct pdump_module *module =
		(struct pdump_module *)malloc(sizeof(struct pdump_module));

	if (module == NULL) {
		return NULL;
	}

	// The loader copies every field of the returned descriptor, so
	// heap garbage must not survive in the ones this constructor
	// leaves unset.
	memset(module, 0, sizeof(*module));

	snprintf(
		module->module.name, sizeof(module->module.name), "%s", "pdump"
	);
	module->module.handler = pdump_handle_packets;
	module->module.commit_handler = pdump_module_commit;

	return &module->module;
}
