#ifndef FILTER_NET6_COLLECTOR
#define FILTER_NET6_COLLECTOR

#include <stdint.h>

#include <endian.h>

#include "filter/radix.h"
#include "filter/lpm.h"

static inline uint64_t
net6_next(uint64_t value)
{
	return htobe64(be64toh(value) + 1);
}

static inline uint64_t
net6_prev(uint64_t value)
{
	return htobe64(be64toh(value) - 1);
}

struct net6_collector {
	struct radix64 radix64;
	uint64_t *masks;
	uint32_t mask_count;
	uint32_t count;
};

static int
net6_collector_init(struct net6_collector *collector)
{
	if (radix64_init(&collector->radix64))
		return -1;
	collector->masks = NULL;
	collector->mask_count = 0;
	return 0;
}

static void
net6_collector_free(struct net6_collector *collector)
{
	free(collector->masks);
	radix64_free(&collector->radix64);
}

static int
net6_collector_add_mask(struct net6_collector *collector, uint32_t *mask_index)
{
	if (!(collector->mask_count & (collector->mask_count + 1))) {
		uint64_t *masks =
			(uint64_t *)realloc(collector->masks,
					      sizeof(uint64_t) *
					      (collector->mask_count + 1) * 2);
		if (masks == NULL)
			return -1;
		collector->masks = masks;
	}

	memset(collector->masks + collector->mask_count, 0, sizeof(uint64_t));
	*mask_index = collector->mask_count++;
	return 0;
}

static int
net6_collector_add(
	struct net6_collector *collector,
	uint64_t value,
	uint64_t mask)
{
	if (!mask)
		return 0;

	uint32_t mask_index = radix64_lookup(&collector->radix64, value);
	if (mask_index == RADIX_VALUE_INVALID) {
		if (net6_collector_add_mask(collector, &mask_index)) {
			return -1;
		}

		if (radix64_insert(&collector->radix64, value, mask_index)) {
			// FIXME: one mask item leaked here but well
			return -1;
		}
	}

	uint8_t prefix = __builtin_popcountll(mask);
	collector->masks[mask_index] |= 1 << (prefix - 1);

	return 0;
}

struct net6_stack {
	uint64_t from;
	uint64_t to;
};

struct net6_collect_ctx {
	struct net6_collector *collector;

	struct net6_stack stack[64];
	uint32_t values[64];
	uint32_t stack_depth;

	uint32_t max_value;
	uint64_t last_to;

	struct lpm64 *lpm64;
};

static inline uint32_t
net6_collect_ctx_top_value(struct net6_collect_ctx *ctx)
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

static int
net6_collector_emit_range(
	uint64_t from,
	uint64_t to,
	uint32_t value,
	struct net6_collect_ctx *ctx)
{
	if (from == net6_next(to)) {
		// /0 prefix
		return lpm64_insert(ctx->lpm64, from, to, value);
	}

	from = be64toh(from);
	to = be64toh(to);

	while (from != to + 1) {
		uint64_t delta = to - from + 1;
		delta >>= 1;
		delta |= delta >> 1;
		delta |= delta >> 2;
		delta |= delta >> 4;
		delta |= delta >> 8;
		delta |= delta >> 16;
		delta |= delta >> 32;

		uint64_t mask = trailing_z_mask(from);
		mask &= delta & mask;

		if (lpm64_insert(ctx->lpm64, htobe64(from), htobe64(from | mask), value))
			return -1;

		from = (from | mask) + 1;
	}
	return 0;
}

static int
net6_collector_add_network(
	uint64_t from,
	uint64_t to,
	struct net6_collect_ctx *ctx)
{
	//FIXME handle errors
	while (ctx->stack_depth > 0) {
		uint64_t upper_mask =
			~(ctx->stack[ctx->stack_depth - 1].to ^
			  ctx->stack[ctx->stack_depth - 1].from);
		if (!((from ^ ctx->stack[ctx->stack_depth - 1].from) & upper_mask)) {
			break;
		}

		if (!(ctx->last_to == ctx->stack[ctx->stack_depth - 1].to)) {
			if (net6_collector_emit_range(
				net6_next(ctx->last_to),
				ctx->stack[ctx->stack_depth - 1].to,
				net6_collect_ctx_top_value(ctx),
				ctx))
				return -1;

			ctx->last_to = ctx->stack[ctx->stack_depth - 1].to;
		}

		--ctx->stack_depth;
	}

	if (ctx->stack_depth > 0 &&
	    !(net6_next(ctx->last_to) == from)) {
		if (net6_collector_emit_range(
			net6_next(ctx->last_to),
			net6_prev(ctx->stack[ctx->stack_depth - 1].from),
			net6_collect_ctx_top_value(ctx),
			ctx)) {
			return -1;
		}

		ctx->last_to = net6_prev(ctx->stack[ctx->stack_depth - 1].from);
	}

	ctx->last_to = net6_prev(from);

	ctx->stack[ctx->stack_depth] = (struct net6_stack){from, to};
	ctx->values[ctx->stack_depth] = LPM_VALUE_INVALID;
	ctx->stack_depth++;

	return 0;
}

static int
net6_collector_iterate(
	uint64_t key,
	uint32_t value,
	void *data)
{
	struct net6_collect_ctx *ctx = (struct net6_collect_ctx *)data;
	uint64_t mask = ctx->collector->masks[value];

	while (mask) {
		uint64_t shift = __builtin_ctzll(mask);
		uint64_t from = key;
		uint64_t to = from | be64toh(0x7fffffffffffffff >> shift); // big endian
		if (net6_collector_add_network(from, to, ctx))
			return -1;
		mask ^= 0x01 << shift;
	}

	return 0;
}

static int
net6_collector_collect(
	struct net6_collector *collector,
	struct lpm64 *lpm64)
{
	struct net6_collect_ctx ctx;
	ctx.collector = collector;
	ctx.max_value = 0;

	ctx.lpm64 = lpm64;
	lpm64_init(ctx.lpm64);

	ctx.stack[0] = (struct net6_stack){0, -1};
	ctx.values[0] = LPM_VALUE_INVALID;
	ctx.stack_depth = 1;
	ctx.last_to = -1;

//FIXME handle errors

	if (radix64_iterate(&collector->radix64, net6_collector_iterate, &ctx))
		goto error;

	while (ctx.stack_depth > 0) {
		if (!(ctx.last_to == ctx.stack[ctx.stack_depth - 1].to)
		    || ctx.max_value == 0) {
			net6_collector_emit_range(
				net6_next(ctx.last_to),
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
	lpm64_free(ctx.lpm64);

	return -1;
}


#endif
