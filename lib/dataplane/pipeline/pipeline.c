#include "pipeline.h"

#include "dataplane/module/module.h"

#include "dataplane/config/zone.h"

void
pipeline_process(
	struct dp_config *dp_config,
	struct cp_config_gen *cp_config_gen,
	uint64_t pipeline_idx,
	struct packet_front *packet_front
) {
	struct cp_pipeline *cp_pipeline =
		cp_config_gen_get_pipeline(cp_config_gen, pipeline_idx);
	if (cp_pipeline == NULL) {
		packet_list_concat(&packet_front->drop, &packet_front->output);
		packet_list_init(&packet_front->output);
		return;
	}

	uint64_t *module_indexes = cp_pipeline->module_indexes;

	for (uint64_t stage_idx = 0; stage_idx < cp_pipeline->length;
	     ++stage_idx) {
		struct module_data *module_data = cp_config_gen_get_module(
			cp_config_gen, module_indexes[stage_idx]
		);

		uint64_t module_index = module_data->index;
		struct dp_module *dp_module =
			ADDR_OF(&dp_config->dp_modules) + module_index;

		packet_front_switch(packet_front);

		dp_module->handler(dp_config, module_data, packet_front);
	}
}
