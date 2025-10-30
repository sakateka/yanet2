#include "device.h"
#include "dataplane.h"

#include <pthread.h>

#include "common/strutils.h"
#include "dpdk.h"
#include "logging/log.h"

int
dataplane_device_start(
	struct dataplane *dataplane, struct dataplane_device *device
) {
	(void)dataplane;

	LOG(INFO,
	    "start dataplane device id=%u with %d workers",
	    device->device_id,
	    device->worker_count);
	dpdk_port_start(device->port_id);

	for (uint32_t wrk_idx = 0; wrk_idx < device->worker_count; ++wrk_idx) {
		struct dataplane_worker *worker = device->workers + wrk_idx;
		dataplane_worker_start(worker);
	}

	return 0;
}

void
dataplane_device_stop(struct dataplane_device *device) {
	for (uint32_t wrk_idx = 0; wrk_idx < device->worker_count; ++wrk_idx) {
		struct dataplane_worker *worker = device->workers + wrk_idx;
		dataplane_worker_stop(worker);
	}
}

int
dataplane_device_init(
	struct dataplane *dataplane,
	struct dataplane_device *device,
	uint32_t device_id,
	struct dataplane_device_config *config
) {
	device->device_id = device_id;
	device->worker_count = 0;

	if (dpdk_port_init(
		    config->port_name,
		    &device->port_id,
		    config->rss_hash,
		    config->worker_count,
		    config->worker_count,
		    config->mtu,
		    config->max_lro_packet_size
	    )) {

		LOG(ERROR, "failed to init dpdk port %s", config->port_name);
		return -1;
	}

	strtcpy(device->port_name, config->port_name, 80);

	device->workers = (struct dataplane_worker *)malloc(
		sizeof(struct dataplane_worker) * config->worker_count
	);
	if (device->workers == NULL) {
		LOG(ERROR, "failed to allocate memory for device workers");
		errno = ENOMEM;
		return -1;
	}

	for (device->worker_count = 0;
	     device->worker_count < config->worker_count;
	     ++device->worker_count) {
		if (dataplane_worker_init(
			    dataplane,
			    device,
			    device->workers + device->worker_count,
			    device->worker_count,
			    config->workers + device->worker_count
		    )) {
			return -1;
		}
	}

	return 0;
}

int
dataplane_device_get_mac(
	struct dataplane_device *device, struct rte_ether_addr *ether_addr
) {
	return dpdk_port_get_mac(device->port_id, ether_addr);
}
