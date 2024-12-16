#include "pipeline.h"

#include <stdlib.h>

#include "module.h"

int
pipeline_init(struct pipeline *pipeline) {
	pipeline->module_configs = NULL;

	return 0;
}

void
pipeline_process(
	struct pipeline *pipeline, struct pipeline_front *pipeline_front
) {
	/*
	 * TODO: 8-byte aligned read and write should be atomic but we
	 * have to ensure it.
	 */
	for (struct pipeline_module_config *module_config =
		     pipeline->module_configs;
	     module_config != NULL;
	     module_config = module_config->next) {

		// Connect previous output to the next input. */
		pipeline_front_switch(pipeline_front);

		// Invoke module instance.
		module_process(
			module_config->module,
			module_config->config,
			pipeline_front
		);
	}
}

int
pipeline_configure(
	struct pipeline *pipeline,
	struct pipeline_module_config_ref *module_config_refs,
	uint32_t config_size,
	struct module_registry *module_registry
) {

	/*
	 * New pipeline configuration chain placed into continuous
	 * memory chunk.
	 */
	struct pipeline_module_config *new_module_configs =
		(struct pipeline_module_config *)malloc(
			sizeof(struct pipeline_module_config) * config_size
		);
	if (new_module_configs == NULL) {
		return -1;
	}

	for (uint32_t idx = 0; idx < config_size; ++idx) {
		struct pipeline_module_config_ref *module_config_ref =
			module_config_refs + idx;

		struct module_config_registry *module_config_registry =
			module_registry_lookup(
				module_registry, module_config_ref->module_name
			);
		if (module_config_registry == NULL)
			goto error;

		struct module_config *module_config =
			module_config_registry_lookup(
				module_config_registry,
				module_config_ref->config_name
			);
		if (module_config == NULL)
			goto error;

		new_module_configs[idx] = (struct pipeline_module_config
		){new_module_configs + idx + 1,
		  module_config_registry->module,
		  module_config};
	}

	new_module_configs[config_size - 1].next = NULL;

	// FIXME module config reference handling

	/*
	 * Now all modules are configured so it is high time to replace
	 * the pipeline chain.
	 */
	pipeline->module_configs = new_module_configs;
	// FIXME: free the previous pipeline module chain

	return 0;

error:
	free(new_module_configs);
	return -1;
}
