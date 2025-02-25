#pragma once

#include <stddef.h>

#include "common/memory.h"

struct dp_config;
struct cp_config;

struct agent {
	struct block_allocator block_allocator;
	struct memory_context memory_context;
	struct dp_config *dp_config;
	struct cp_config *cp_config;
};

struct module_data;

struct agent *
agent_connect(
	const char *storage_name, const char *agent_name, size_t memory_limit
);

int
agent_update_modules(
	struct agent *agent,
	size_t module_count,
	struct module_data **module_datas
);

struct module_config {
	char type[80];
	char name[80];
};

struct pipeline_config {
	uint64_t length;
	struct module_config modules[0];
};

int
agent_update_pipelines(
	struct agent *agent,
	size_t pipeline_count,
	struct pipeline_config *pipelines[]
);

struct pipeline_config *
pipeline_config_create(uint64_t length);

void
pipeline_config_free(struct pipeline_config *config);

void
pipeline_config_set_module(
	struct pipeline_config *config,
	uint64_t index,
	const char *type,
	const char *name
);
