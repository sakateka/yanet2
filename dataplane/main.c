#include <stdio.h>

#include "common/log.h"
#include "config.h"
#include "dataplane.h"

int
main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "%s", "usage: yanet-dataplane <config>");
		return -1;
	}
	log_enable_name("info");

	struct dataplane_config *config;
	FILE *config_file = fopen(argv[1], "r");
	LOG(INFO, "initialize the dataplane config");
	if (dataplane_config_init(config_file, &config)) {
		LOG(ERROR, "invalid config file: %s", argv[1]);
		return -1;
	}
	// FIXME: re-enable log level name from config
	dataplane_log_enable("info");

	struct dataplane dataplane;

	LOG(INFO, "initialize dataplane");
	// FIXME: dataplane error handling
	int rc = dataplane_init(&dataplane, argv[0], config);
	if (rc != 0) {
		LOG(ERROR, "failed to initialize dataplane");
		return -1;
	}

	LOG(INFO, "start dataplane");
	dataplane_start(&dataplane);

	// FIXME: infinite sleep effectively
	LOG(INFO, "wait dataplane");
	dataplane_stop(&dataplane);
	LOG(INFO, "dataplane is stopped");

	LOG(INFO, "deallocate dataplane");
	dataplane_config_free(config);
	return 0;
}
