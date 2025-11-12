#include "dataplane.h"
#include "config.h"

#include <rte_ether.h>

#include <stdint.h>

#include "common/timeutils.h"
#include "dataplane/module/module.h"
#include "dataplane/packet/packet.h"
#include "dataplane/worker.h"
#include "dataplane/worker/worker.h"
#include "fwstate/lookup.h"
#include "fwstate/sync.h"
#include "logging/log.h"

struct acl_module {
	struct module module;
};

enum acl_action {
	// NOLINTBEGIN(readability-identifier-naming)
	ACL_UNKNOWN,
	ACL_ACCEPT,
	ACL_DROP,
	ACL_CREATE_STATE,
	ACL_CHECK_STATE,
	// NOLINTEND(readability-identifier-naming)
};

static inline int
acl_handle_v4(
	struct filter *filter,
	struct packet *packet,
	const uint32_t **actions,
	uint32_t *count
) {
	filter_query(filter, packet, actions, count);
	return 0;
}

static inline int
acl_handle_v6(
	struct filter *filter,
	struct packet *packet,
	const uint32_t **actions,
	uint32_t *count
) {
	filter_query(filter, packet, actions, count);
	return 0;
}

static void
acl_handle_packets(
	struct dp_worker *dp_worker,
	struct module_ectx *module_ectx,
	struct packet_front *packet_front
) {
	struct acl_module_config *acl_config = container_of(
		ADDR_OF(&module_ectx->cp_module),
		struct acl_module_config,
		cp_module
	);

	struct filter *compiler = &acl_config->filter;

	struct fwstate_config *fwstate_config = &acl_config->fwstate_cfg;
	struct fw_state_sync_config *sync_config = &fwstate_config->sync_config;
	fwmap_t *fw4state = ADDR_OF(&fwstate_config->fw4state);
	fwmap_t *fw6state = ADDR_OF(&fwstate_config->fw6state);
	fwmap_t *state_table = NULL;

	// Time in nanoseconds is sufficient for keeping state up to 500 years
	uint64_t now = get_time_ns();

	/*
	 * There are two major options:
	 *  - process packets one by one
	 *  - process stages one by one
	 * For the second option we have to split v4 and v6 processing.
	 */
	struct packet *packet;
	while ((packet = packet_list_pop(&packet_front->input)) != NULL) {
		const uint32_t *actions = NULL;
		uint32_t count = 0;

		if (packet->network_header.type ==
		    rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {

			state_table = fw4state;
			acl_handle_v4(compiler, packet, &actions, &count);
		} else if (packet->network_header.type ==
			   rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6)) {

			state_table = fw6state;
			acl_handle_v6(compiler, packet, &actions, &count);
		} else {
			packet_front_output(packet_front, packet);
			continue;
		}

		enum sync_packet_direction push_sync_packet = SYNC_NONE;

		for (uint32_t idx = 0; idx < count; ++idx) {
			if (!(actions[idx] & ACTION_NON_TERMINATE)) {
				switch (actions[idx]) {
				case ACL_ACCEPT:
					packet_front_output(
						packet_front, packet
					);
					break;
				case ACL_DROP:
					packet_front_drop(packet_front, packet);
					break;
				case ACL_CREATE_STATE:
					push_sync_packet = SYNC_INGRESS;
					break;
				case ACL_CHECK_STATE:
					if (fwstate_check_state(
						    state_table,
						    packet,
						    now,
						    &push_sync_packet
					    )) {
						packet_front_output(
							packet_front, packet
						);
					} else {
						packet_front_drop(
							packet_front, packet
						);
					}
					break;
				}
			}
		}

		if (push_sync_packet != SYNC_NONE) {
			// Allocate a new packet for the sync frame
			struct packet *sync_pkt =
				worker_packet_alloc(dp_worker);
			if (unlikely(sync_pkt == NULL)) {
				LOG(ERROR, "failed to allocate sync packet");
				continue;
			}
			if (unlikely(
				    fwstate_craft_state_sync_packet(
					    sync_config,
					    packet,
					    push_sync_packet,
					    sync_pkt
				    ) == -1
			    )) {
				worker_packet_free(sync_pkt);
				LOG(ERROR, "failed to craft sync packet");
				continue;
			}

			packet_front_output(packet_front, sync_pkt);
		}
	}
}

struct module *
new_module_acl() {
	struct acl_module *module =
		(struct acl_module *)malloc(sizeof(struct acl_module));

	if (module == NULL) {
		return NULL;
	}

	snprintf(module->module.name, sizeof(module->module.name), "%s", "acl");
	module->module.handler = acl_handle_packets;

	return &module->module;
}
