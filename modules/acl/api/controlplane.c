#include <errno.h>
#include <stdint.h>

#include "controlplane.h"
#include "fwstate_cp.h"

#include "common/memory_address.h"
#include "config.h"
#include "modules/fwstate/dataplane/config.h"

#include "common/container_of.h"

#include "controlplane/agent/agent.h"

struct cp_module *
acl_module_config_init(struct agent *agent, const char *name) {
	struct acl_module_config *config =
		(struct acl_module_config *)memory_balloc(
			&agent->memory_context, sizeof(struct acl_module_config)
		);
	if (config == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	if (cp_module_init(
		    &config->cp_module,
		    agent,
		    "acl",
		    name,
		    acl_module_config_free
	    )) {
		int prev_errno = errno;
		acl_module_config_free(&config->cp_module);
		errno = prev_errno;
		return NULL;
	}

	SET_OFFSET_OF(&config->targets, NULL);
	config->target_count = 0;

	memset(&config->filter_vlan, 0, sizeof(config->filter_vlan));

	memset(&config->filter_ip4, 0, sizeof(config->filter_ip4));

	memset(&config->filter_ip6, 0, sizeof(config->filter_ip6));

	// Initialize fwstate_cfg with NULL pointers
	memset(&config->fwstate_cfg, 0, sizeof(struct fwstate_config));

	return &config->cp_module;
}

void
acl_module_config_free(struct cp_module *cp_module) {
	struct acl_module_config *config =
		container_of(cp_module, struct acl_module_config, cp_module);

	struct agent *agent = ADDR_OF(&cp_module->agent);

	// Note: We don't destroy fwstate_cfg maps here because they're owned by
	// the fwstate module. We only stored offsets to them.

	memory_bfree(
		&agent->memory_context, config, sizeof(struct acl_module_config)
	);
}

FILTER_DECLARE(FWD_FILTER_VLAN_TAG, &attribute_device, &attribute_vlan);

FILTER_DECLARE(
	FWD_FILTER_IP4_PROTO_TAG,
	&attribute_device,
	&attribute_vlan,
	&attribute_net4_src,
	&attribute_net4_dst,
	&attribute_proto_range
);

FILTER_DECLARE(
	FWD_FILTER_IP6_PROTO_TAG,
	&attribute_device,
	&attribute_vlan,
	&attribute_net6_src,
	&attribute_net6_dst,
	&attribute_proto_range
);

int
acl_module_config_update(
	struct cp_module *cp_module,
	struct acl_rule *acl_rules,
	uint32_t rule_count
) {
	struct acl_module_config *config =
		container_of(cp_module, struct acl_module_config, cp_module);

	for (uint64_t idx = 0; idx < rule_count; ++idx) {
		struct acl_rule *rule = acl_rules + idx;
		for (uint64_t idx = 0; idx < rule->device_count; ++idx) {
			if (cp_module_link_device(
				    cp_module,
				    rule->devices[idx].name,
				    &rule->devices[idx].id
			    )) {
				goto error;
			}
		}
	}

	struct acl_target *targets = (struct acl_target *)memory_balloc(
		&cp_module->memory_context,
		sizeof(struct acl_target) * rule_count
	);
	if (targets == NULL) {
		goto error;
	}

	SET_OFFSET_OF(&config->targets, targets);
	config->target_count = rule_count;

	for (uint32_t idx = 0; idx < rule_count; ++idx) {
		struct acl_rule *acl_rule = acl_rules + idx;
		targets[idx].action = acl_rule->action;
		if ((targets[idx].counter_id = counter_registry_register(
			     &cp_module->counter_registry, acl_rule->counter, 2
		     )) == (uint64_t)-1) {
			goto error_target;
		}
	}

	// Create per filter rule list
	struct filter_rule *filter_rules = (struct filter_rule *)malloc(
		sizeof(struct filter_rule) * rule_count
	);
	if (filter_rules == NULL) {
		goto error_target;
	}
	uint64_t filter_rule_idx;

	// Build vlan rules
	filter_rule_idx = 0;
	for (uint32_t idx = 0; idx < rule_count; ++idx) {
		struct acl_rule *acl_rule = acl_rules + idx;
		if (acl_rule->net6.src_count || acl_rule->net6.dst_count ||
		    acl_rule->net4.src_count || acl_rule->net4.dst_count) {
			continue;
		}

		struct filter_rule *filter_rule =
			filter_rules + filter_rule_idx++;
		filter_rule->device_count = acl_rule->device_count;
		filter_rule->devices = acl_rule->devices;

		filter_rule->vlan_range_count = acl_rule->vlan_range_count;
		filter_rule->vlan_ranges = acl_rule->vlan_ranges;

		filter_rule->action = idx;
	}

	if (FILTER_INIT(
		    &config->filter_vlan,
		    FWD_FILTER_VLAN_TAG,
		    filter_rules,
		    filter_rule_idx,
		    &cp_module->memory_context
	    )) {
	}

	// Build ip4 rules
	filter_rule_idx = 0;
	for (uint32_t idx = 0; idx < rule_count; ++idx) {
		struct acl_rule *acl_rule = acl_rules + idx;
		if (!acl_rule->net4.src_count || !acl_rule->net4.dst_count) {
			continue;
		}

		struct filter_rule *filter_rule =
			filter_rules + filter_rule_idx++;
		filter_rule->device_count = acl_rule->device_count;
		filter_rule->devices = acl_rule->devices;

		filter_rule->vlan_range_count = acl_rule->vlan_range_count;
		filter_rule->vlan_ranges = acl_rule->vlan_ranges;

		filter_rule->net4 = acl_rule->net4;

		filter_rule->transport.proto_count =
			acl_rule->proto_range_count;
		filter_rule->transport.protos = acl_rule->proto_ranges;

		filter_rule->action = idx;
	}

	if (FILTER_INIT(
		    &config->filter_ip4,
		    FWD_FILTER_IP4_PROTO_TAG,
		    filter_rules,
		    filter_rule_idx,
		    &cp_module->memory_context
	    )) {
	}

	// Build ip6 rules
	filter_rule_idx = 0;
	for (uint32_t idx = 0; idx < rule_count; ++idx) {
		struct acl_rule *acl_rule = acl_rules + idx;
		if (!acl_rule->net6.src_count || !acl_rule->net6.dst_count) {
			continue;
		}

		struct filter_rule *filter_rule =
			filter_rules + filter_rule_idx++;
		filter_rule->device_count = acl_rule->device_count;
		filter_rule->devices = acl_rule->devices;

		filter_rule->vlan_range_count = acl_rule->vlan_range_count;
		filter_rule->vlan_ranges = acl_rule->vlan_ranges;

		filter_rule->net6 = acl_rule->net6;

		filter_rule->transport.proto_count =
			acl_rule->proto_range_count;
		filter_rule->transport.protos = acl_rule->proto_ranges;

		filter_rule->action = idx;
	}

	if (FILTER_INIT(
		    &config->filter_ip6,
		    FWD_FILTER_IP6_PROTO_TAG,
		    filter_rules,
		    filter_rule_idx,
		    &cp_module->memory_context
	    )) {
	}

	free(filter_rules);

	return 0;

error_target:
	memory_bfree(
		&cp_module->memory_context,
		targets,
		sizeof(struct acl_target) * rule_count
	);

error:
	return -1;
}

void
acl_module_config_set_fwstate_config(
	struct cp_module *cp_module, struct cp_module *fwstate_cp_module
) {
	struct acl_module_config *config =
		container_of(cp_module, struct acl_module_config, cp_module);

	struct fwstate_module_config *fwstate_config = container_of(
		fwstate_cp_module, struct fwstate_module_config, cp_module
	);

	config->fwstate_cfg.sync_config = fwstate_config->cfg.sync_config;
	EQUATE_OFFSET(
		&config->fwstate_cfg.fw4state, &fwstate_config->cfg.fw4state
	);
	EQUATE_OFFSET(
		&config->fwstate_cfg.fw6state, &fwstate_config->cfg.fw6state
	);
}
