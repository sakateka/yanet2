#include "controlplane.h"
#include "common/container_of.h"
#include "config.h"

#include "controlplane/agent/agent.h"

struct decap_module_config *
decap_module_config_init(struct agent *agent, const char *name) {
	struct dp_config *dp_config = ADDR_OF(&agent->dp_config);

	uint64_t index;
	if (dp_config_lookup_module(dp_config, "decap", &index)) {
		return NULL;
	}

	struct decap_module_config *config =
		(struct decap_module_config *)memory_balloc(
			&agent->memory_context,
			sizeof(struct decap_module_config)
		);
	if (config == NULL)
		return NULL;

	config->module_data.index = index;
	strncpy(config->module_data.name,
		name,
		sizeof(config->module_data.name) - 1);
	memory_context_init_from(
		&config->module_data.memory_context,
		&agent->memory_context,
		name
	);

	// From the point all allocations are made on local memory context
	struct memory_context *memory_context =
		&config->module_data.memory_context;
	if (lpm_init(&config->prefixes4, memory_context))
		goto error_lpm_v4;
	if (lpm_init(&config->prefixes6, memory_context))
		goto error_lpm_v6;

error_lpm_v6:
	lpm_free(&config->prefixes4);

error_lpm_v4:
	memory_bfree(
		&agent->memory_context,
		config,
		sizeof(struct decap_module_config)
	);
	return NULL;
}

int
decap_module_config_add_prefix_v4(
	struct module_data *module_data, const uint8_t *from, const uint8_t *to
) {
	struct decap_module_config *config = container_of(
		module_data, struct decap_module_config, module_data
	);
	return lpm_insert(&config->prefixes4, 4, from, to, 1);
}

int
decap_module_config_add_prefix_v6(
	struct module_data *module_data, const uint8_t *from, const uint8_t *to
) {
	struct decap_module_config *config = container_of(
		module_data, struct decap_module_config, module_data
	);
	return lpm_insert(&config->prefixes6, 16, from, to, 1);
}
