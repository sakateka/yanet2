#include "ipfw.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>


#include "registry.h"
#include "value.h"

#include "classify.h"

#include <stdio.h>


#include "filter/net6_collector.h"

typedef void (*action_get_net6_func)(
	struct filter_action *action,
	struct net6 **net,
	uint32_t *count);

static void
action_get_net6_src(
	struct filter_action *action,
	struct net6 **net,
	uint32_t *count)
{
	*net = action->net6.srcs;
	*count = action->net6.src_count;
}

static void
action_get_net6_dst(
	struct filter_action *action,
	struct net6 **net,
	uint32_t *count)
{
	*net = action->net6.dsts;
	*count = action->net6.dst_count;
}

typedef void (*net6_get_part_func)(
	struct net6 *net,
	uint64_t *addr,
	uint64_t *mask);

static void
net6_get_hi_part(struct net6 *net,
	uint64_t *addr,
	uint64_t *mask)
{
	*addr = net->addr_hi;
	*mask = net->mask_hi;
}

static void
net6_get_lo_part(struct net6 *net,
	uint64_t *addr,
	uint64_t *mask)
{
	*addr = net->addr_lo;
	*mask = net->mask_lo;
}


static int
lpm64_value_iterator(uint32_t value, void *data)
{
	struct value_table *table = (struct value_table *)data;
	return value_table_touch(table, 0, value);
}

static void
net6_collect_values(
	struct net6 *start,
	uint32_t count,
	net6_get_part_func get_part,
	struct lpm *lpm,
	struct value_table *table)
{
	for (struct net6 *net6 = start; net6 < start + count; ++net6) {
		uint64_t addr;
		uint64_t mask;
		get_part(net6, &addr, &mask);
		uint64_t to = addr | ~mask;
		lpm64_collect_values(
			lpm,
			(uint8_t *)&addr,
			(uint8_t *)&to,
			lpm64_value_iterator,
			table);
	}
}

struct net_collect_cxt {
	struct value_table *table;
	struct value_registry *registry;
};

static int
lpm64_registry_iterator(uint32_t value, void *data)
{
	struct value_registry *registry = (struct value_registry *)data;
	return value_registry_collect(registry, value);
}

static void
net6_collect_registry(
	struct net6 *start,
	uint32_t count,
	net6_get_part_func get_part,
	struct lpm *lpm,
	struct value_registry *registry)
{
	for (struct net6 *net6 = start; net6 < start + count; ++net6) {
		uint64_t addr;
		uint64_t mask;
		get_part(net6, &addr, &mask);
		uint64_t to = addr | ~mask;
		lpm64_collect_values(
			lpm,
			(uint8_t *)&addr,
			(uint8_t *)&to,
			lpm64_registry_iterator,
			registry);
	}
}

static int
value_table_touch_action(uint32_t v1, uint32_t v2, uint32_t idx, void *data)
{
	(void) idx;
	struct value_table *table = (struct value_table *)data;
	if (value_table_touch(table, v1, v2) < 0)
		return -1;
	return 0;
}

static int
merge_registry_values(
	struct value_registry *registry1,
	struct value_registry *registry2,
	struct value_table *table)
{
	if (value_table_init(
		table,
		value_registry_capacity(registry1),
		value_registry_capacity(registry2))) {
		return -1;
	}

	for (uint32_t range_idx = 0;
	     range_idx < registry1->range_count; ++range_idx) {
		value_table_new_gen(table);
		value_registry_join_range(
			registry1,
			registry2,
			range_idx,
			value_table_touch_action,
			table);
	}

	value_table_compact(table);

	return 0;
}

struct value_collect_ctx {
	struct value_table *table;
	struct value_registry *registry;
};

static int
value_table_collect_action(uint32_t v1, uint32_t v2, uint32_t idx, void *data)
{
	(void) idx;
	struct value_collect_ctx *collect_ctx =
		(struct value_collect_ctx *)data;
	return value_registry_collect(
		collect_ctx->registry,
		value_table_get(collect_ctx->table, v1, v2));

	return 0;
}

static int
collect_registry_values(
	struct value_registry *registry1,
	struct value_registry *registry2,
	struct value_table *table,
	struct value_registry *registry)
{
	if (value_registry_init(registry)) {
		return -1;
	}

	struct value_collect_ctx collect_ctx;
	collect_ctx.table = table;
	collect_ctx.registry = registry;

	for (uint32_t range_idx = 0;
	     range_idx < registry1->range_count; ++range_idx) {
		value_registry_start(registry);
		value_registry_join_range(
			registry1,
			registry2,
			range_idx,
			value_table_collect_action,
			&collect_ctx);
	}

	return 0;
}

static int
merge_and_collect_registry(
	struct value_registry *registry1,
	struct value_registry *registry2,
	struct value_table *table,
	struct value_registry *registry)
{
	if (merge_registry_values(registry1, registry2, table)) {
		return -1;
	}

	if (collect_registry_values(registry1, registry2, table, registry)) {
		value_table_free(table);
		return -1;
	}

	return 0;
}

struct value_set_ctx {
	struct value_table *table;
	struct value_registry *registry;
};

static int
action_list_is_term(struct value_registry *registry, uint32_t range_idx)
{
	struct value_range *range = registry->ranges + range_idx;
	if (range->count == 0)
		return 0;

	uint32_t action_id = registry->values[range->from + range->count - 1];
	return !(action_id & ACTION_NON_TERMINATE);
}

static int
value_table_set_action(uint32_t v1, uint32_t v2, uint32_t idx, void *data)
{
	struct value_set_ctx *set_ctx = (struct value_set_ctx *)data;
	uint32_t prev_value = value_table_get(set_ctx->table, v1, v2);

	if (!action_list_is_term(set_ctx->registry, prev_value)) {
		int res = value_table_touch(set_ctx->table, v1, v2);

		if (res <= 0)
			return res;

		value_registry_start(set_ctx->registry);

		struct value_range *copy_range =
			set_ctx->registry->ranges + prev_value;

		for (uint32_t ridx = copy_range->from;
		     ridx < copy_range->from + copy_range->count;
		     ++ridx) {
			value_registry_collect(
				set_ctx->registry,
				set_ctx->registry->values[ridx]);
		}

		value_registry_collect(set_ctx->registry, idx);
	}

	return 0;
}

static int
set_registry_values(
	struct value_registry *registry1,
	struct value_registry *registry2,
	struct value_table *table,
	struct value_registry *registry)
{
	if (value_table_init(
		table,
		value_registry_capacity(registry1),
		value_registry_capacity(registry2))) {
		return -1;
	}

	if (value_registry_init(registry)) {
		value_table_free(table);
		return -1;
	}
	// Empty action list
	if (value_registry_start(registry)) {
		value_registry_free(registry);
		value_table_free(table);
	}

	struct value_set_ctx set_ctx;
	set_ctx.table = table;
	set_ctx.registry = registry;

	for (uint32_t range_idx = 0;
	     range_idx < registry1->range_count; ++range_idx) {
		value_table_new_gen(table);
		value_registry_join_range(
			registry1,
			registry2,
			range_idx,
			value_table_set_action,
			&set_ctx);
	}

	return 0;
}

static int
collect_net6_values(
	struct filter_action *actions,
	uint32_t count,
	action_get_net6_func get_net6,
	net6_get_part_func get_part,
	struct lpm *lpm,
	struct value_registry *registry)
{

	struct net6_part_collector collector;
	if (net6_part_collector_init(&collector, 8))
		goto error;

	for (struct filter_action *action = actions;
	       action < actions + count;
	       ++action) {
		struct net6 *nets;
		uint32_t net_count;
		get_net6(action, &nets, &net_count);

		for (struct net6 *net6 = nets;
		     net6 < nets + net_count;
		     ++net6) {
			uint64_t addr;
			uint64_t mask;
			get_part(net6, &addr, &mask);
			uint64_t to = addr | ~mask;

			if (range8_collector_add(
				&collector,
				(uint8_t *)&addr,
				(uint8_t *)&to))
				goto error_collector;
		}
	}
	if (net6_part_collector_collect(&collector, 8, lpm)) {
		goto error_collector;
	}

	struct value_table table;
	if (value_table_init(&table, 1, collector.count))
		goto error_vtab;

	for (struct filter_action *action = actions;
	       action < actions + count;
	       ++action) {
		value_table_new_gen(&table);

		struct net6 *nets;
		uint32_t net_count;
		get_net6(action, &nets, &net_count);

		net6_collect_values(
			nets,
			net_count,
			get_part,
			lpm,
			&table);
	}

	value_table_compact(&table);
	lpm64_remap(lpm, &table);
	lpm64_compact(lpm);

	if (value_registry_init(registry))
		goto error_reg;

	for (struct filter_action *action = actions;
	       action < actions + count;
	       ++action) {
		value_registry_start(registry);

		struct net6 *nets;
		uint32_t net_count;
		get_net6(action, &nets, &net_count);

		net6_collect_registry(
			nets,
			net_count,
			get_part,
			lpm,
			registry);
	}

	value_table_free(&table);
	return 0;

error_reg:
	value_table_free(&table);
error_collector:
	range_collector_free(&collector);

error_vtab:

error:
	return -1;
}

typedef void (*action_get_port_range_func)(
	struct filter_action *action,
	struct filter_port_range **ranges,
	uint32_t *count);

static void
get_port_range_src(
	struct filter_action *action,
	struct filter_port_range **ranges,
	uint32_t *count)
{
	*ranges = action->transport.srcs;
	*count = action->transport.src_count;
}

static void
get_port_range_dst(
	struct filter_action *action,
	struct filter_port_range **ranges,
	uint32_t *count)
{
	*ranges = action->transport.dsts;
	*count = action->transport.dst_count;
}

static int
collect_port_values(
	struct filter_action *actions,
	uint32_t count,
	action_get_port_range_func get_port_range,
	struct value_registry *registry)
{
	struct value_table table;
	if (value_table_init(&table, 1, 65536))
		return -1;

	for (struct filter_action *action = actions;
	       action < actions + count;
	       ++action) {
		value_table_new_gen(&table);

		struct filter_port_range *port_ranges;
		uint32_t port_range_count;
		get_port_range(action, &port_ranges, &port_range_count);
		for (struct filter_port_range *ports = port_ranges;
		     ports < port_ranges + port_range_count;
		     ++ports) {
			if (ports->to - ports->from == 65535)
				continue;
			for (uint32_t port = ports->from;
			     port <= ports->to;
			     ++port) {
				value_table_touch(&table, 0, port);
			}
		}
	}

	value_table_compact(&table);

	if (value_registry_init(registry))
		goto error_reg;

	for (struct filter_action *action = actions;
	       action < actions + count;
	       ++action) {
		value_registry_start(registry);

		struct filter_port_range *port_ranges;
		uint32_t port_range_count;
		get_port_range(action, &port_ranges, &port_range_count);
		for (struct filter_port_range *ports = port_ranges;
		     ports < port_ranges + port_range_count;
		     ++ports) {
			for (uint32_t port = ports->from;
			     port <= ports->to;
			     ++port) {
				value_registry_collect(
					registry,
					value_table_get(&table, 0, port));
			}
		}
	}

	value_table_free(&table);
	return 0;

error_reg:
	value_table_free(&table);
	return -1;
}

/*
static int
filter_table_copy(
	struct filter_table *ftab,
	struct value_table *vtab)
{
	if (filter_table_init(ftab, vtab->h_dim, vtab->v_dim))
		return -1;

	memcpy(ftab->values, vtab->values, sizeof(uint32_t) * vtab->h_dim * vtab->v_dim);
	return 0;
}
*/

int
filter_compiler_init(
	struct filter_compiler *filter,
	struct filter_action *actions,
	uint32_t count)
{
	struct value_registry src_net6_lo_registry;
	struct value_registry dst_net6_hi_registry;
	struct value_registry dst_net6_lo_registry;
	struct value_registry src_port_registry;
	struct value_registry dst_port_registry;


	struct value_registry src_net6_hi_registry;
	collect_net6_values(
		actions,
		count,
		action_get_net6_src,
		net6_get_hi_part,
		&filter->src_net6_hi,
		&src_net6_hi_registry);

	collect_net6_values(
		actions,
		count,
		action_get_net6_src,
		net6_get_lo_part,
		&filter->src_net6_lo,
		&src_net6_lo_registry);

	collect_net6_values(
		actions,
		count,
		action_get_net6_dst,
		net6_get_hi_part,
		&filter->dst_net6_hi,
		&dst_net6_hi_registry);

	collect_net6_values(
		actions,
		count,
		action_get_net6_dst,
		net6_get_lo_part,
		&filter->dst_net6_lo,
		&dst_net6_lo_registry);

	collect_port_values(
		actions,
		count,
		get_port_range_src,
		&src_port_registry);

	collect_port_values(
		actions,
		count,
		get_port_range_dst,
		&dst_port_registry);



	struct value_table vtab1;
	struct value_registry vtab1_registry;
	merge_and_collect_registry(
		&src_net6_hi_registry,
		&dst_net6_hi_registry,
		&vtab1,
		&vtab1_registry);

	struct value_table vtab2;
	struct value_registry vtab2_registry;
	merge_and_collect_registry(
		&src_net6_lo_registry,
		&dst_net6_lo_registry,
		&vtab2,
		&vtab2_registry);

	struct value_table vtab3;
	struct value_registry vtab3_registry;
	merge_and_collect_registry(
		&src_port_registry,
		&dst_port_registry,
		&vtab3,
		&vtab3_registry);

	struct value_table vtab12;
	struct value_registry vtab12_registry;
	merge_and_collect_registry(
		&vtab1_registry,
		&vtab2_registry,
		&vtab12,
		&vtab12_registry);

/*
	struct value_table vtab123;
	struct value_registry vtab123_registry;
	merge_and_collect_registry(
		&vtab12_registry,
		&vtab3_registry,
		&vtab123,
		&vtab123_registry);


*/


	struct value_table vtab123;
//	struct value_registry vtab123_registry;
	set_registry_values(
		&vtab12_registry,
		&vtab3_registry,
		&vtab123,
		&filter->v6_lookups.result_registry);

/*
	filter->classify[0] = filter_classify_src_net_hi;
	filter->classify[1] = filter_classify_dst_net_hi;
	filter->classify[2] = filter_classify_src_net_lo;
	filter->classify[3] = filter_classify_dst_net_lo;
	filter->classify[4] = filter_classify_src_port;
	filter->classify[5] = filter_classify_dst_port;
	filter->filter.classify_count = 6;
	filter->filter.classify = filter->classify;

	// src hi X dst hi
	filter->lookups[0] = (struct filter_lookup){
		.first_arg = 0,
		.second_arg = 1,
		.table_idx = 0,
	};
	// src lo X dst lo
	filter->lookups[1] = (struct filter_lookup){
		.first_arg = 2,
		.second_arg = 3,
		.table_idx = 1,
	};
	// src port X dst port
	filter->lookups[2] = (struct filter_lookup){
		.first_arg = 4,
		.second_arg = 5,
		.table_idx = 2,
	};
	// src X dst
	filter->lookups[3] = (struct filter_lookup){
		.first_arg = 6,
		.second_arg = 7,
		.table_idx = 3,
	};
	// net X port
	filter->lookups[4] = (struct filter_lookup){
		.first_arg = 9,
		.second_arg = 8,
		.table_idx = 4,
	};

	filter->filter.lookup_count = 5;
	filter->filter.lookups = filter->lookups;

	filter_table_copy(filter->tables + 0, &vtab1);
	filter_table_copy(filter->tables + 1, &vtab2);
	filter_table_copy(filter->tables + 2, &vtab3);
	filter_table_copy(filter->tables + 3, &vtab12);
	filter_table_copy(filter->tables + 4, &vtab123);

	filter->filter.tables = filter->tables;

	filter->actions = (uint32_t *)malloc(sizeof(uint32_t) * count);
	for (uint32_t idx = 0; idx < count; ++idx)
		filter->actions[idx] = actions[idx].action;
*/
	return 0;
}
