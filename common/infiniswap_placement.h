/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#ifndef INFINISWAP_PLACEMENT_H
#define INFINISWAP_PLACEMENT_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u32 is_placement_u32;
typedef u64 is_placement_u64;
#else
#include <stddef.h>
#include <stdint.h>
typedef uint32_t is_placement_u32;
typedef uint64_t is_placement_u64;
#endif

#define IS_PLACEMENT_SAMPLE_DEFAULT 2U
#define IS_PLACEMENT_MAX_PROVIDERS 64U
#define IS_PLACEMENT_PROVIDER_ID_MAX 63U
#define IS_PLACEMENT_WEIGHT_MIN 1U
#define IS_PLACEMENT_WEIGHT_MAX 1000U

enum is_placement_result {
	IS_PLACEMENT_OK = 0,
	IS_PLACEMENT_INVALID_ARGUMENT,
	IS_PLACEMENT_NO_COMPATIBLE_PROVIDER,
	IS_PLACEMENT_INSUFFICIENT_CAPACITY
};

enum is_placement_exclude_reason {
	IS_PLACEMENT_EXCLUDE_NONE = 0,
	IS_PLACEMENT_EXCLUDE_UNHEALTHY,
	IS_PLACEMENT_EXCLUDE_IDENTITY,
	IS_PLACEMENT_EXCLUDE_VERSION,
	IS_PLACEMENT_EXCLUDE_CAPABILITY,
	IS_PLACEMENT_EXCLUDE_POOL,
	IS_PLACEMENT_EXCLUDE_RAIL,
	IS_PLACEMENT_EXCLUDE_ZERO_CAPACITY
};

struct is_placement_candidate {
	char provider_id[IS_PLACEMENT_PROVIDER_ID_MAX + 1U];
	is_placement_u32 available_chunks;
	is_placement_u32 placement_weight;
	int healthy;
	int compatible;
	enum is_placement_exclude_reason exclude_reason;
};

struct is_placement_choice {
	unsigned int chosen_index;
	unsigned int sample_size_used;
	unsigned int eligible_count;
};

typedef is_placement_u32 (*is_placement_rand_fn)(void *ctx,
						 is_placement_u32 limit);

/*
 * Enforce 1 <= sample_size <= healthy_compatible_count.
 * healthy_compatible_count must be at least 1.
 */
enum is_placement_result is_placement_validate_sample_size(
	unsigned int sample_size, unsigned int healthy_compatible_count);

/*
 * Sample up to sample_size healthy, compatible Providers with available
 * capacity, using placement_weight for selection without replacement, then
 * choose the sampled Provider with the most available_chunks. Ties keep the
 * earlier candidate index. rand_fn(ctx, limit) must return a value in
 * [0, limit). Deterministic seeds belong only in tests via rand_fn.
 */
enum is_placement_result is_placement_choose(
	const struct is_placement_candidate *candidates,
	unsigned int candidate_count, unsigned int sample_size,
	is_placement_rand_fn rand_fn, void *rand_ctx,
	struct is_placement_choice *out);

const char *is_placement_exclude_reason_name(
	enum is_placement_exclude_reason reason);

#endif /* INFINISWAP_PLACEMENT_H */
