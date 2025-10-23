#pragma once

#include <stdint.h>

struct agent;
struct cp_module;

struct cp_module *
forward_module_config_init(struct agent *agent, const char *name);

void
forward_module_config_free(struct cp_module *cp_module);

int
forward_module_config_enable_v4(
	struct cp_module *cp_module,
	const uint8_t *from,
	const uint8_t *to,
	const char *src_name,
	const char *dst_name,
	const char *counter_name
);

int
forward_module_config_enable_v6(
	struct cp_module *cp_module,
	const uint8_t *from,
	const uint8_t *to,
	const char *src_name,
	const char *dst_name,
	const char *counter_name
);

int
forward_module_config_enable_l2(
	struct cp_module *cp_module,
	const char *src_name,
	const char *dst_name,
	const char *counter_name
);

uint64_t
forward_module_topology_device_count(struct agent *agent);

// Enables deletion of configurations for the forwarding module.
// @return Returns -1 on error and 0 on success.
int
forward_module_config_delete(struct cp_module *cp_module);
