#include <errno.h>
#include <stdint.h>

#include "controlplane.h"

#include "common/memory_address.h"
#include "config.h"

#include "common/container_of.h"

#include "controlplane/agent/agent.h"
#include "modules/fwstate/api/controlplane.h"

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

	// Initialize fwstate_cfg with NULL pointers
	// It will be set later via acl_module_set_fwstate_config
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

int
acl_module_compile(
	struct cp_module *cp_module,
	struct filter_rule *actions,
	uint32_t action_count
) {
	struct acl_module_config *config =
		container_of(cp_module, struct acl_module_config, cp_module);

	const struct filter_attribute *attributes[4] = {
		&attribute_net4_src,
		&attribute_net4_dst,
		&attribute_port_src,
		&attribute_port_dst
	};

	return filter_init(
		&config->filter,
		attributes,
		4,
		actions,
		action_count,
		&cp_module->memory_context
	);
}

int
acl_module_set_fwstate_config(
	struct cp_module *cp_module,
	void *shm,
	struct fwstate_config fwstate_cfg
) {
	struct acl_module_config *config =
		container_of(cp_module, struct acl_module_config, cp_module);

	if (shm == NULL) {
		errno = EINVAL;
		return -1;
	}

	// Copy sync_config by value
	memcpy(&config->fwstate_cfg.sync_config,
	       &fwstate_cfg.sync_config,
	       sizeof(struct fw_state_sync_config));

	// The fwstate_cfg passed in has global offsets to maps
	// We need to convert them to absolute pointers from shm base
	// and then store as local offsets
	if (fwstate_cfg.fw4state != NULL) {
		fwmap_t *ptr = (fwmap_t *)((uintptr_t)shm +
					   (uintptr_t)fwstate_cfg.fw4state);
		SET_OFFSET_OF(&config->fwstate_cfg.fw4state, ptr);
	} else {
		config->fwstate_cfg.fw4state = NULL;
	}

	if (fwstate_cfg.fw6state != NULL) {
		fwmap_t *ptr = (fwmap_t *)((uintptr_t)shm +
					   (uintptr_t)fwstate_cfg.fw6state);
		SET_OFFSET_OF(&config->fwstate_cfg.fw6state, ptr);
	} else {
		config->fwstate_cfg.fw6state = NULL;
	}

	return 0;
}
