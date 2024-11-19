#ifndef FILTER_IPFW_H
#define FILTER_IPFW_H

#include <stdint.h>

#include "common/network.h"


#define ACTION_NON_TERMINATE 0x80000000


#include "lpm.h"
#include "registry.h"

struct filter_network6 {
	uint32_t src_count;
	uint32_t dst_count;
	struct ipfw_net6 *srcs;
	struct ipfw_net6 *dsts;
};

struct filter_network4 {
	uint32_t src_count;
	uint32_t dst_count;
	struct ipfw_net4 *srcs;
	struct ipfw_net4 *dsts;
};

struct filter_port_range {
	uint16_t from;
	uint16_t to;
};

struct filter_transport {
	uint16_t proto_flags;
	uint16_t src_count;
	uint16_t dst_count;
	struct filter_port_range *srcs;
	struct filter_port_range *dsts;
};

struct filter {
};

struct ipfw_filter_action {
	struct filter_net6_filter net6;
	struct ipfw_net4_filter net4;
	struct ipfw_transport_filter transport;
	uint32_t action;
};

struct fw_filter;


typedef int (*collect_filter_values)(
	actions *ipfw_filter_action,
	uint32_t count,
	struct fw_filter *filter
);



struct ipfw_packet_filter {
	struct filter filter;

	struct lpm32 src_net4;
	struct lpm32 dst_net4;

	struct lpm64 src_net6_hi;
	struct lpm64 src_net6_lo;
	struct lpm64 dst_net6_hi;
	struct lpm64 dst_net6_lo;

	uint32_t src_port[65536];
	uint32_t dst_port[65536];
	uint16_t proto_flag[65536];



	filter_classify classify[6];
	struct filter_lookup lookups[5];
	struct filter_table tables[5];


	struct value_registry vtab123_registry;
	uint32_t *actions;
};

int
ipfw_packet_filter_create(
	struct ipfw_filter_action *actions,
	uint32_t count,
	struct ipfw_packet_filter *filter);

#endif
