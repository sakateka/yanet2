#include "device.h"

#include <pthread.h>

#include <rte_ethdev.h>

static void *
worker_thread_start(void *arg) {
	struct dataplane_worker *worker = (struct dataplane_worker *)arg;

	worker_exec(worker);

	return NULL;
}

int
dataplane_device_start(
	struct dataplane *dataplane, struct dataplane_device *device
) {
	(void)dataplane;

	pthread_attr_t wrk_th_attr;
	pthread_attr_init(&wrk_th_attr);

	for (uint32_t wrk_idx = 0; wrk_idx < device->worker_count; ++wrk_idx) {
		struct dataplane_worker *worker = device->workers + wrk_idx;
		pthread_create(
			&worker->thread_id,
			&wrk_th_attr,
			worker_thread_start,
			worker
		);
	}

	pthread_attr_destroy(&wrk_th_attr);

	return 0;
}

void
dataplane_device_stop(struct dataplane_device *device) {
	for (uint32_t wrk_idx = 0; wrk_idx < device->worker_count; ++wrk_idx) {
		struct dataplane_worker *worker = device->workers + wrk_idx;
		pthread_join(worker->thread_id, NULL);
	}
}

int
dataplane_dpdk_port_init(
	struct dataplane *dataplane,
	struct dataplane_device *device,
	uint32_t device_id,
	const char *name,
	uint16_t worker_count,
	uint16_t numa_id
) {
	(void)numa_id;

	device->device_id = device_id;
	device->worker_count = 0;

	// FIXME handle errors
	if (rte_eth_dev_get_port_by_name(name, &device->port_id)) {
		return -1;
	}

	struct rte_eth_conf port_conf;
	memset(&port_conf, 0, sizeof(struct rte_eth_conf));
	// FIXME: do not use constant here
	port_conf.rxmode.max_lro_pkt_size = 8000;

	//	port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;

	// FIXME: copy paste from sock-dev init routine
	if (rte_eth_dev_configure(
		    device->port_id, worker_count, worker_count, &port_conf
	    )) {
		return -1;
	}

	if (rte_eth_dev_set_mtu(device->port_id, 7500)) {
		return -1;
	}

	// FIXME handle errors
	device->workers = (struct dataplane_worker *)malloc(
		sizeof(struct dataplane_worker) * worker_count
	);

	for (device->worker_count = 0; device->worker_count < worker_count;
	     ++device->worker_count) {
		// FIXME: handle errors
		dataplane_worker_init(
			dataplane,
			device,
			device->workers + device->worker_count,
			device->worker_count
		);
	}

	// FIXME handle errors
	rte_eth_dev_start(device->port_id);

	return 0;
}

int
dataplane_dpdk_port_get_mac(
	struct dataplane_device *device, struct rte_ether_addr *ether_addr
) {
	return rte_eth_macaddr_get(device->port_id, ether_addr);
}
