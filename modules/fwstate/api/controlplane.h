#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct memory_context;
struct fwstate_config;
struct fw_state_sync_config;
struct agent;
struct cp_module;

// Create and initialize fwstate configuration (without maps)
struct fwstate_config *
fwstate_config_create(struct memory_context *memory_context);

// Create firewall state maps for the configuration
int
fwstate_config_create_maps(
	struct memory_context *memory_context,
	struct fwstate_config *config,
	uint32_t index_size,
	uint32_t extra_bucket_count,
	uint16_t worker_count
);

// Destroy fwstate configuration
void
fwstate_config_destroy(
	struct fwstate_config *config, struct memory_context *memory_context
);

// Configure state synchronization settings
int
fwstate_config_set_sync(
	struct fwstate_config *config,
	const struct fw_state_sync_config *sync_config
);

// Transfer maps from old config to new config (for reconfiguration)
void
fwstate_config_transfer_maps(
	struct fwstate_config *new_config, struct fwstate_config *old_config
);

// Initialize fwstate module configuration
struct cp_module *
fwstate_module_config_init(struct agent *agent, const char *name);

// Free fwstate module configuration (this call leaks the maps if they were not
// freed before)
void
fwstate_module_config_free(struct cp_module *cp_module);

// Get fwstate config pointer from fwstate module
struct fwstate_config *
fwstate_module_get_fwstate_config(struct cp_module *cp_module);

// Get fwstate_config with map offsets converted to global offsets from shm base
// This copies the fwstate_config and converts relative map pointers to global
// offsets shm parameter is the yanet_shm pointer (base of shared memory)
// Returns 0 on success, -1 on error
int
fwstate_get_config_with_global_offset(
	struct cp_module *cp_module,
	void *shm,
	struct fwstate_config *out_config
);

// Get the size (number of entries) of a firewall state map
// Returns the number of entries in the map, or 0 if map is NULL
size_t
fwstate_config_get_map_size(const struct fwstate_config *config, bool is_ipv6);
