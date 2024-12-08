#include "module.h"

#include <string.h>

struct module_config_registry *
module_registry_lookup(
	struct module_registry *module_registry,
	const char *module_name)
{
	for (uint32_t idx = 0;
	     idx < module_registry->module_count;
	     ++idx) {
		struct module_config_registry *module_config_registry =
			module_registry->modules + idx;

		if (!strncmp(
			module_config_registry->module->name,
			module_name,
			MODULE_NAME_LEN)) {
			// TODO: error code
			return module_config_registry;
		}
	}

	return NULL;
}

struct module_config *
module_config_registry_lookup(
	struct module_config_registry *module_config_registry,
	const char *module_config_name)
{
	for (uint32_t idx = 0;
	     idx < module_config_registry->config_count;
	     ++idx) {
		if (!strncmp(
			module_config_name,
			module_config_registry->configs[idx]->name,
			MODULE_CONFIG_NAME_LEN)) {
			return module_config_registry->configs[idx];
		}
	}

	return NULL;
}

int
module_registry_configure(
	struct module_registry *module_registry,
	const char *module_name,
	const char *module_config_name,
	const void *data)
{
	struct module_config_registry *module_config_registry =
		module_registry_lookup(module_registry, module_name);

	if (module_config_registry == NULL) {
		// TODO: error code
		return -1;
	}

	for (uint32_t idx = 0;
	     idx < module_config_registry->config_count;
	     ++idx) {
		struct module_config **config = module_config_registry->configs + idx;
		if (!strncmp(module_config_name, (*config)->name, MODULE_CONFIG_NAME_LEN)) {
			return module_configure(
				module_config_registry->module,
				module_config_name,
				data,
				*config,
				config);
		}
	}

	// FIXME: array extending
	if (module_config_registry->config_count % 8 == 0) {
		struct module_config **new_configs =
			(struct module_config **)
			realloc(
				module_config_registry->configs,
				sizeof(struct module_config **) *
				module_config_registry->config_count + 8);
		if (new_configs == NULL) {
			//TODO: error code
			return -1;
		}
		module_config_registry->configs = new_configs;
	}
	struct module_config **config =
		module_config_registry->configs +
		module_config_registry->config_count;

	if (module_configure(
		module_config_registry->module,
		module_config_name,
		data,
		*config,
		config)) {
		// TODO: error code
		return -1;
	}

	++(module_config_registry->config_count);
	return 0;
}

