#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#include "common/memory.h"

#include "dataplane/module/module.h"

#include "dataplane/config/topology.h"

#include "controlplane/agent/agent.h"

#include "controlplane/config/zone.h"

struct dp_config;

struct dp_module {
	char name[80];
	module_handler handler;
};

struct dp_worker {
	uint64_t gen;
	uint64_t iterations;
	uint64_t rx_count;
	uint64_t tx_count;
	uint64_t remote_rx_count;
	uint64_t remote_tx_count;
	uint64_t pad[10];
};

struct dp_config {
	uint32_t numa_map;
	uint32_t numa_idx;
	uint64_t storage_size;

	struct block_allocator block_allocator;
	struct memory_context memory_context;

	pid_t config_lock;

	struct dp_topology dp_topology;
	uint64_t module_count;
	struct dp_module *dp_modules;

	struct cp_config *cp_config;

	uint64_t worker_count;
	struct dp_worker **workers;
};

bool
dp_config_try_lock(struct dp_config *dp_config);

void
dp_config_lock(struct dp_config *dp_config);

bool
dp_config_unlock(struct dp_config *dp_config);

int
dp_config_lookup_module(
	struct dp_config *dp_config, const char *name, uint64_t *index
);

void
dp_config_wait_for_gen(struct dp_config *dp_config, uint64_t gen);
