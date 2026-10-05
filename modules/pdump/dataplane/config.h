#pragma once

#include <stdint.h>

#include "lib/controlplane/config/cp_module.h"

#include "mode.h"

struct rte_bpf;

// Sentinel for "no ring linked". object_link_get_address returns NULL for
// any index >= object_link_count, so a config with no link at this slot
// resolves to no ring and the handler captures nothing.
#define PDUMP_RING_LINK_NONE UINT64_MAX

struct pdump_module_config {
	struct cp_module cp_module;

	char *filter;
	struct rte_bpf *ebpf_program;
	enum pdump_mode mode;
	uint32_t snaplen;

	// Object link index of the ring this config captures into, declared
	// via cp_module_link_object and resolved at ectx build time into a
	// per-worker object_ectx entry. PDUMP_RING_LINK_NONE marks an absent
	// link.
	uint64_t ring_link_idx;
};
