#include <errno.h>
#include <string.h>

#include "controlplane.h"

#include "common/container_of.h"
#include "controlplane/agent/agent.h"
#include "fwstate/config.h"
#include "fwstate/fwmap.h"
#include "modules/fwstate/dataplane/config.h"

struct fwstate_config *
fwstate_config_create(struct memory_context *memory_context) {
	// Allocate fwstate_config structure
	struct fwstate_config *config = (struct fwstate_config *)memory_balloc(
		memory_context, sizeof(struct fwstate_config)
	);
	if (config == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	// Initialize pointers to NULL; maps will be created separately
	config->fw4state = NULL;
	config->fw6state = NULL;

	// Initialize sync configuration with defaults
	memset(&config->sync_config, 0, sizeof(config->sync_config));

	// Set default timeouts
	config->sync_config.timeouts.tcp_syn_ack = FW_STATE_DEFAULT_TIMEOUT;
	config->sync_config.timeouts.tcp_syn = FW_STATE_DEFAULT_TIMEOUT;
	config->sync_config.timeouts.tcp_fin = FW_STATE_DEFAULT_TIMEOUT;
	config->sync_config.timeouts.tcp = FW_STATE_DEFAULT_TIMEOUT;
	config->sync_config.timeouts.udp = 30e9;      // 30 seconds
	config->sync_config.timeouts.default_ = 16e9; // 16 seconds

	return config;
}

int
fwstate_config_create_maps(
	struct memory_context *memory_context,
	struct fwstate_config *config,
	uint32_t index_size,
	uint32_t extra_bucket_count,
	uint16_t worker_count
) {
	if (config == NULL) {
		errno = EINVAL;
		return -1;
	}

	// Verify maps do not already exist
	if (config->fw4state != NULL || config->fw6state != NULL) {
		errno = EEXIST;
		return -1;
	}

	// Apply defaults if parameters are not provided
	if (index_size == 0) {
		index_size = 1024 * 1024; // Default: 1M entries
	}
	if (extra_bucket_count == 0) {
		extra_bucket_count = 1024; // Default: 1024 extra buckets
	}
	if (worker_count == 0) {
		worker_count = 1; // Default: 1 worker
	}

	// Configure IPv4 firewall state map
	fwmap_config_t fw4_config = {
		.key_size = sizeof(struct fw4_state_key),
		.value_size = sizeof(struct fw_state_value),
		.hash_seed = 0,
		.worker_count = worker_count,
		.index_size = index_size,
		.extra_bucket_count = extra_bucket_count,
		.hash_fn_id = FWMAP_HASH_FNV1A,
		.key_equal_fn_id = FWMAP_KEY_EQUAL_FW4,
		.rand_fn_id = FWMAP_RAND_DEFAULT,
		.copy_key_fn_id = FWMAP_COPY_KEY_FW4,
		.copy_value_fn_id = FWMAP_COPY_VALUE_FWSTATE,
		.merge_value_fn_id = FWMAP_MERGE_VALUE_FWSTATE,
	};

	fwmap_t *fw4state = fwmap_new(&fw4_config, memory_context);
	if (fw4state == NULL) {
		return -1;
	}
	SET_OFFSET_OF(&config->fw4state, fw4state);

	// Configure IPv6 firewall state map
	fwmap_config_t fw6_config = {
		.key_size = sizeof(struct fw6_state_key),
		.value_size = sizeof(struct fw_state_value),
		.hash_seed = 0,
		.worker_count = worker_count,
		.index_size = index_size,
		.extra_bucket_count = extra_bucket_count,
		.hash_fn_id = FWMAP_HASH_FNV1A,
		.key_equal_fn_id = FWMAP_KEY_EQUAL_FW6,
		.rand_fn_id = FWMAP_RAND_DEFAULT,
		.copy_key_fn_id = FWMAP_COPY_KEY_FW6,
		.copy_value_fn_id = FWMAP_COPY_VALUE_FWSTATE,
		.merge_value_fn_id = FWMAP_MERGE_VALUE_FWSTATE,
	};

	fwmap_t *fw6state = fwmap_new(&fw6_config, memory_context);
	if (fw6state == NULL) {
		fwmap_t *fw4 = ADDR_OF(&config->fw4state);
		fwmap_destroy(fw4, memory_context);
		config->fw4state = NULL;
		return -1;
	}
	SET_OFFSET_OF(&config->fw6state, fw6state);

	return 0;
}

void
fwstate_config_destroy(
	struct fwstate_config *config, struct memory_context *memory_context
) {
	if (config->fw4state != NULL) {
		fwmap_t *fw4 = ADDR_OF(&config->fw4state);
		fwmap_destroy(fw4, memory_context);
		config->fw4state = NULL;
	}
	if (config->fw6state != NULL) {
		fwmap_t *fw6 = ADDR_OF(&config->fw6state);
		fwmap_destroy(fw6, memory_context);
		config->fw6state = NULL;
	}
}

int
fwstate_config_set_sync(
	struct fwstate_config *config,
	const struct fw_state_sync_config *sync_config
) {
	if (sync_config == NULL) {
		errno = EINVAL;
		return -1;
	}

	// Copy synchronization configuration
	memcpy(&config->sync_config,
	       sync_config,
	       sizeof(struct fw_state_sync_config));

	return 0;
}

struct cp_module *
fwstate_module_config_init(struct agent *agent, const char *name) {
	struct fwstate_module_config *config =
		(struct fwstate_module_config *)memory_balloc(
			&agent->memory_context,
			sizeof(struct fwstate_module_config)
		);
	if (config == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	if (cp_module_init(
		    &config->cp_module,
		    agent,
		    "fwstate",
		    name,
		    fwstate_module_config_free
	    )) {
		goto fail;
	}

	struct memory_context *memory_context =
		&config->cp_module.memory_context;

	// Create fwstate configuration
	struct fwstate_config *fwstate_cfg =
		fwstate_config_create(memory_context);
	if (fwstate_cfg == NULL) {
		goto fail;
	}

	// Store offset reference to fwstate_config
	SET_OFFSET_OF(&config->fwstate_cfg, fwstate_cfg);

	return &config->cp_module;

fail: {
	int prev_errno = errno;
	fwstate_module_config_free(&config->cp_module);
	errno = prev_errno;
	return NULL;
}
}

void
fwstate_module_config_free(struct cp_module *cp_module) {
	struct fwstate_module_config *config = container_of(
		cp_module, struct fwstate_module_config, cp_module
	);

	struct agent *agent = ADDR_OF(&cp_module->agent);

	// FIXME: when to destroy the maps, ideally its lifetime should match
	// the lifetime of the dataplane process (or more precisely the pipeline
	// that includes the ACL module)

	// Destroy fwstate configuration if present
	if (config->fwstate_cfg != NULL) {
		struct fwstate_config *fwstate_cfg =
			ADDR_OF(&config->fwstate_cfg);
		fwstate_config_destroy(fwstate_cfg, &agent->memory_context);
		memory_bfree(
			&agent->memory_context,
			fwstate_cfg,
			sizeof(struct fwstate_config)
		);
	}

	memory_bfree(
		&agent->memory_context,
		config,
		sizeof(struct fwstate_module_config)
	);
}

struct fwstate_config *
fwstate_module_get_fwstate_config(struct cp_module *cp_module) {
	struct fwstate_module_config *config = container_of(
		cp_module, struct fwstate_module_config, cp_module
	);

	return ADDR_OF(&config->fwstate_cfg);
}

int
fwstate_get_config_with_global_offset(
	struct cp_module *cp_module,
	void *shm,
	struct fwstate_config *out_config
) {
	if (shm == NULL || out_config == NULL) {
		errno = EINVAL;
		return -1;
	}

	struct fwstate_config *fwstate_cfg =
		fwstate_module_get_fwstate_config(cp_module);
	if (fwstate_cfg == NULL) {
		errno = ENOENT;
		return -1;
	}

	// Copy synchronization configuration
	memcpy(&out_config->sync_config,
	       &fwstate_cfg->sync_config,
	       sizeof(struct fw_state_sync_config));

	// Convert relative map pointers to global offsets from shared memory
	// base
	fwmap_t *fw4 = ADDR_OF(&fwstate_cfg->fw4state);
	fwmap_t *fw6 = ADDR_OF(&fwstate_cfg->fw6state);

	if (fw4 != NULL) {
		out_config->fw4state =
			(fwmap_t *)((uintptr_t)fw4 - (uintptr_t)shm);
	} else {
		out_config->fw4state = NULL;
	}

	if (fw6 != NULL) {
		out_config->fw6state =
			(fwmap_t *)((uintptr_t)fw6 - (uintptr_t)shm);
	} else {
		out_config->fw6state = NULL;
	}

	return 0;
}

void
fwstate_config_transfer_maps(
	struct fwstate_config *new_config, struct fwstate_config *old_config
) {
	// Transfer map offset references from old config to new config
	EQUATE_OFFSET(&new_config->fw4state, &old_config->fw4state);
	EQUATE_OFFSET(&new_config->fw6state, &old_config->fw6state);

	// Clear old config pointers to prevent double-free and use after free
	old_config->fw4state = NULL;
	old_config->fw6state = NULL;
}

size_t
fwstate_config_get_map_size(const struct fwstate_config *config, bool is_ipv6) {
	if (config == NULL) {
		return 0;
	}

	fwmap_t *map;
	if (is_ipv6) {
		if (config->fw6state == NULL) {
			return 0;
		}
		map = ADDR_OF(&config->fw6state);
	} else {
		if (config->fw4state == NULL) {
			return 0;
		}
		map = ADDR_OF(&config->fw4state);
	}

	return fwmap_size(map);
}
