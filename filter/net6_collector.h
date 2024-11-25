#ifndef FILTER_NET6_PART_COLLECTOR
#define FILTER_NET6_PART_COLLECTOR

#include <stdint.h>

#include <endian.h>

#include "filter/radix.h"
#include "filter/lpm.h"

static inline uint64_t
net6_part_next(uint64_t value)
{
	return htobe64(be64toh(value) + 1);
}

static inline uint64_t
net6_part_prev(uint64_t value)
{
	return htobe64(be64toh(value) - 1);
}

struct net6_part_collector {
	struct radix radix;
	uint8_t *masks;
	uint32_t mask_count;
	uint32_t count;
};

static int
net6_part_collector_init(struct net6_part_collector *collector)
{
	if (radix_init(&collector->radix))
		return -1;
	collector->masks = NULL;
	collector->mask_count = 0;
	return 0;
}

static void
net6_part_collector_free(struct net6_part_collector *collector)
{
	free(collector->masks);
	radix_free(&collector->radix);
}

static int
net6_part_collector_add_mask(
	struct net6_part_collector *collector,
	uint8_t key_size,
	uint32_t *mask_index)
{
	if (!(collector->mask_count & (collector->mask_count + 1))) {
		uint8_t *masks =
			(uint8_t *)realloc(collector->masks,
					      key_size *
					      (collector->mask_count + 1) * 2);
		if (masks == NULL)
			return -1;
		collector->masks = masks;
	}

	memset(
		collector->masks + collector->mask_count * key_size,
		0,
		key_size);
	*mask_index = collector->mask_count++;
	return 0;
}

static inline void
net6_part_collector_set_mask(
	struct net6_part_collector *collector,
	uint8_t key_size,
	uint32_t mask_index,
	uint8_t prefix)
{
	uint32_t pos = (mask_index * key_size + prefix) / 8;
	collector->masks[pos] |= 1 << prefix;
}

static int
net6_part_collector_add(
	struct net6_part_collector *collector,
	uint8_t key_size,
	const uint8_t *value,
	const uint8_t prefix)
{
	if (!prefix)
		return 0;

	uint32_t mask_index = radix_lookup(&collector->radix, key_size, value);
	if (mask_index == RADIX_VALUE_INVALID) {
		if (net6_part_collector_add_mask(collector, key_size, &mask_index)) {
			return -1;
		}

		if (radix64_insert(&collector->radix, value, mask_index)) {
			/*
			 * Mask added above leaked but this should not be
			 * an issue as the collector assumed to be freed
			 * after an error.
			 */
			return -1;
		}
	}

	net6_part_collector_set_mask(collector, key_size, mask_index, prefix - 1);

	return 0;
}

struct net6_collect_ctx {
	struct net6_part_collector *collector;

	uint8_t stack[64];
	uint32_t values[64];
	uint64_t last_to;

	struct lpm *lpm64;
};

uint32_t
net6_collect_ctx_top_value(struct net_collect_ctx *ctx)
{
	if (ctx->values[ctx->stack_depth - 1] == LPM_VALUE_INVALID) {
		ctx->values[ctx->stack_depth - 1]  = ctx->max_value++;
	}
	return ctx->values[ctx->stack_depth - 1];
}

static inline uint64_t
one_if_zero(uint64_t value)
{
	// endian ignorant
	return (value - 1) / 0xffffffffffffffff;
}

static inline uint64_t
trailing_z_mask(uint64_t value)
{
	return (value ^ (value - 1)) >> (1 - one_if_zero(value));
}

static inline int
net6_part_collector_emit_range(
	const uint8_t *from,
	const uint8_t *to,
	uint32_t value,
	struct net6_collect_ctx *ctx)
{
	if (*(uint64_t *)from == net6_part_next(*(uint64_t *)to)) {
		// /0 prefix
		return lpm64_insert(ctx->lpm64, from, to, value);
	}

	uint64_t from_h = be64toh(*(uint64_t *)from);
	uint64_t to_h = be64toh(*(uint64_t *)to);

	while (from_h != to_h + 1) {
		uint64_t delta = to_h - from_h + 1;
		delta >>= 1;
		delta |= delta >> 1;
		delta |= delta >> 2;
		delta |= delta >> 4;
		delta |= delta >> 8;
		delta |= delta >> 16;
		delta |= delta >> 32;

		uint64_t mask = trailing_z_mask(from_h);
		mask &= delta & mask;

		uint64_t from_be = htobe64(from_h);
		uint64_t to_be = htobe64(from_h | mask);

		if (lpm64_insert(ctx->lpm64, (uint8_t *)&from_be, (uint8_t *)&to_be, value))
			return -1;

		from_h = (from_h | mask) + 1;
	}
	return 0;
}

static int
net6_part_collector_add_network(
	uint8_t key_size,
	const uint8_t *from,
	uint8_t prefix,
	struct net6_collect_ctx *ctx)
{
	while (ctx->stack_depth > 0) {
		if (range_fits(
			ctx->stack[ctx->stack_depth - 1].to,
			ctx->stack[ctx->stack_depth - 1].prefix,
			from)) {
			break;
		}
		uint64_t upper_mask =
			~(ctx->stack[ctx->stack_depth - 1].to ^
			  ctx->stack[ctx->stack_depth - 1].from);
		if (!((*(uint64_t *)from ^ ctx->stack[ctx->stack_depth - 1].from) & upper_mask)) {
			break;
		}

		if (!(ctx->last_to == ctx->stack[ctx->stack_depth - 1].to)) {
			if (net6_part_collector_emit_range(
				net6_part_next(ctx->last_to),
				ctx->stack[ctx->stack_depth - 1].to,
				net6_collect_ctx_top_value(ctx),
				ctx))
				return -1;

			ctx->last_to = ctx->stack[ctx->stack_depth - 1].to;
		}

		--ctx->stack_depth;
	}

	if (ctx->stack_depth > 0 &&
	    !(net6_part_next(ctx->last_to) == from)) {
		if (net6_part_collector_emit_range(
			net6_part_next(ctx->last_to),
			net6_part_prev(ctx->stack[ctx->stack_depth - 1].from),
			net6_collect_ctx_top_value(ctx),
			ctx)) {
			return -1;
		}

		ctx->last_to = net6_part_prev(ctx->stack[ctx->stack_depth - 1].from);
	}

	ctx->last_to = net6_part_prev(from);

	ctx->stack[ctx->stack_depth] = (struct net6_stack){from, to};
	ctx->values[ctx->stack_depth] = LPM_VALUE_INVALID;
	ctx->stack_depth++;

	return 0;
}

static int
net6_part_collector_iterate(
	uint8_t key_size,
	const uint8_t *key,
	uint32_t value,
	void *data)
{
	struct net6_collect_ctx *ctx = (struct net6_collect_ctx *)data;

	const uint8_t *mask = ctx->collector->masks + value * key_size;

	for (uint8_t idx = 0; idx < key_size; ++idx) {
		uint8_t mask_item = mask[idx];
		while (mask_item) {
			uint8_t prefix = idx * 8 + __builtin_ctzll(mask_item);
			mask_item ^= 0x01 << prefix;
			if (net6_part_collector_add_network(
				key_size,
				key,
				prefix,
				ctx))
				return -1;
		}
	}

	return 0;
}

static int
net6_part_collector_collect(
	struct net6_part_collector *collector,
	struct lpm *lpm64)
{
	struct net6_collect_ctx ctx;
	ctx.collector = collector;
	ctx.max_value = 0;

	ctx.lpm64 = lpm64;
	lpm_init(ctx.lpm64);

	ctx.stack[0] = (struct net6_stack){0, -1};
	ctx.values[0] = LPM_VALUE_INVALID;
	ctx.stack_depth = 1;
	ctx.last_to = -1;

//FIXME handle errors

	if (radix64_walk(&collector->radix64, net6_part_collector_iterate, &ctx))
		goto error;

	while (ctx.stack_depth > 0) {
		if (!(ctx.last_to == ctx.stack[ctx.stack_depth - 1].to)
		    || ctx.max_value == 0) {
			net6_part_collector_emit_range(
				net6_part_next(ctx.last_to),
				ctx.stack[ctx.stack_depth - 1].to,
				net6_collect_ctx_top_value(&ctx),
				&ctx);
			ctx.last_to = ctx.stack[ctx.stack_depth - 1].to;
		}
		--ctx.stack_depth;
	}

	collector->count = ctx.max_value;

	return 0;

error:
	lpm_free(ctx.lpm64);

	return -1;
}


#endif
