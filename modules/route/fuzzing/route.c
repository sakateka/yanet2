#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "dataplane/module/module.h"
#include "dataplane/module/testing.h"
#include "dataplane/packet/packet.h"
#include "modules/route/api/controlplane.h"
#include "modules/route/dataplane/config.h"
#include "modules/route/dataplane/dataplane.h"

#define ARENA_SIZE (1 << 20)
#define MBUF_MAX_SIZE 8196
#define RTE_PKTMBUF_HEADROOM 256

struct route_fuzzing_params {
	struct module *module; /**< Pointer to the module being tested */
	struct module_data *module_data; /**< Module configuration */

	void *arena;
	void *payload_arena;
	struct block_allocator ba;
	struct memory_context mctx;
};

static struct route_fuzzing_params fuzz_params = {
	.module_data = NULL,
};

static int
route_test_config(struct module_data **module_data) {
	struct route_module_config *config =
		(struct route_module_config *)memory_balloc(
			&fuzz_params.mctx, sizeof(struct route_module_config)
		);

	if (!config) {
		return -ENOMEM;
	}

	// Initialize module_data fields
	strtcpy(config->module_data.name,
		"route_test",
		sizeof(config->module_data.name));
	memory_context_init_from(
		&config->module_data.memory_context,
		&fuzz_params.mctx,
		"route_test"
	);

	config->module_data.index = 0;
	config->module_data.agent = NULL;
	config->module_data.free_handler = route_module_config_free;

	struct memory_context *memory_context =
		&config->module_data.memory_context;
	if (lpm_init(&config->lpm_v4, memory_context)) {
		goto error_lpm_v4;
	}
	if (lpm_init(&config->lpm_v6, memory_context)) {
		goto error_lpm_v6;
	}
	config->route_count = 0;
	config->routes = NULL;

	config->route_list_count = 0;
	config->route_lists = NULL;

	config->route_index_count = 0;
	config->route_indexes = NULL;

	struct module_data *rmc = &config->module_data;

	int route_idx = route_module_config_add_route(
		rmc,
		(struct ether_addr){
			.addr = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06},
		},
		(struct ether_addr){
			.addr = {0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c},
		}
	);
	if (route_idx == -1) {
		goto error_lpm_v6;
	}

	int route_list_idx = route_module_config_add_route_list(
		rmc, 1, (uint32_t[]){route_idx}
	);
	if (route_list_idx == -1) {
		goto error_lpm_v6;
	}

	// 127.0.0.0/24
	int rc = route_module_config_add_prefix_v4(
		rmc,
		(uint8_t[4]){127, 0, 0, 0},
		(uint8_t[4]){127, 0, 0, 0xff},
		route_list_idx
	);
	if (rc != 0) {
		goto error_lpm_v6;
	}
	// fe80::0/96
	rc = route_module_config_add_prefix_v6(
		rmc,
		(uint8_t[16]){0xfe, 0x80, [15] = 0},
		(uint8_t[16]
		){0xfe, 0x80, [12] = 0xff, [13] = 0xff, [14] = 0xff, [15] = 0xff
		},
		route_list_idx
	);
	if (rc != 0) {
		goto error_lpm_v6;
	}

	*module_data = (struct module_data *)config;
	return 0;

error_lpm_v6:
	lpm_free(&config->lpm_v4);

error_lpm_v4:
	memory_bfree(
		&fuzz_params.mctx, config, sizeof(struct route_module_config)
	);
	return -EINVAL;
}

static int
fuzz_setup() {
	fuzz_params.arena = malloc(ARENA_SIZE);
	if (fuzz_params.arena == NULL) {
		return EXIT_FAILURE;
	}

	block_allocator_init(&fuzz_params.ba);
	block_allocator_put_arena(
		&fuzz_params.ba, fuzz_params.arena, ARENA_SIZE
	);

	memory_context_init(
		&fuzz_params.mctx, "route fuzzing", &fuzz_params.ba
	);

	fuzz_params.module = new_module_route();
	fuzz_params.payload_arena = memory_balloc(
		&fuzz_params.mctx,
		sizeof(struct packet_front) + MBUF_MAX_SIZE * 4
	);
	if (fuzz_params.payload_arena == NULL) {
		return -ENOMEM;
	}

	return route_test_config(&fuzz_params.module_data);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) { // NOLINT
	if (fuzz_params.module == NULL) {
		if (fuzz_setup() != 0) {
			exit(1); // Proper setup is essential for continuing
		}
	}

	if (size > (MBUF_MAX_SIZE - RTE_PKTMBUF_HEADROOM)) {
		return 0;
	}
	struct test_data payload[] = {{.payload = data, .size = size}};

	struct packet_front *pf = testing_packet_front(
		payload,
		fuzz_params.payload_arena,
		sizeof(struct packet_front) + MBUF_MAX_SIZE * 4,
		1,
		MBUF_MAX_SIZE
	);

	if (parse_packet(pf->input.first)) {
		return 0;
	}

	// Process packet through route module
	fuzz_params.module->handler(NULL, fuzz_params.module_data, pf);

	return 0;
}
