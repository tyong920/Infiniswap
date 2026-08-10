/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */

#include "infiniswap_placement.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

enum is_placement_result is_placement_validate_sample_size(
	unsigned int sample_size, unsigned int healthy_compatible_count)
{
	if (healthy_compatible_count == 0 || sample_size == 0 ||
	    sample_size > healthy_compatible_count)
		return IS_PLACEMENT_INVALID_ARGUMENT;
	return IS_PLACEMENT_OK;
}

const char *is_placement_exclude_reason_name(
	enum is_placement_exclude_reason reason)
{
	switch (reason) {
	case IS_PLACEMENT_EXCLUDE_NONE:
		return "none";
	case IS_PLACEMENT_EXCLUDE_UNHEALTHY:
		return "unhealthy";
	case IS_PLACEMENT_EXCLUDE_IDENTITY:
		return "identity";
	case IS_PLACEMENT_EXCLUDE_VERSION:
		return "version";
	case IS_PLACEMENT_EXCLUDE_CAPABILITY:
		return "capability";
	case IS_PLACEMENT_EXCLUDE_POOL:
		return "pool";
	case IS_PLACEMENT_EXCLUDE_RAIL:
		return "rail";
	case IS_PLACEMENT_EXCLUDE_ZERO_CAPACITY:
		return "zero-capacity";
	default:
		return "unknown";
	}
}

static int candidate_eligible(const struct is_placement_candidate *candidate)
{
	return candidate && candidate->healthy && candidate->compatible &&
	       candidate->available_chunks > 0 &&
	       candidate->placement_weight >= IS_PLACEMENT_WEIGHT_MIN &&
	       candidate->placement_weight <= IS_PLACEMENT_WEIGHT_MAX &&
	       candidate->provider_id[0] != '\0';
}

enum is_placement_result is_placement_choose(
	const struct is_placement_candidate *candidates,
	unsigned int candidate_count, unsigned int sample_size,
	is_placement_rand_fn rand_fn, void *rand_ctx,
	struct is_placement_choice *out)
{
	unsigned int eligible[IS_PLACEMENT_MAX_PROVIDERS];
	unsigned int eligible_count = 0;
	unsigned int sampled[IS_PLACEMENT_MAX_PROVIDERS];
	unsigned int sampled_count = 0;
	unsigned int remaining[IS_PLACEMENT_MAX_PROVIDERS];
	unsigned int remaining_count = 0;
	unsigned int index;
	unsigned int chosen_eligible;
	is_placement_u32 best_chunks;
	int saw_compatible = 0;

	if (!candidates || !rand_fn || !out ||
	    candidate_count == 0 ||
	    candidate_count > IS_PLACEMENT_MAX_PROVIDERS)
		return IS_PLACEMENT_INVALID_ARGUMENT;

	memset(out, 0, sizeof(*out));
	for (index = 0; index < candidate_count; index++) {
		if (candidates[index].healthy && candidates[index].compatible)
			saw_compatible = 1;
		if (candidate_eligible(&candidates[index]))
			eligible[eligible_count++] = index;
	}
	out->eligible_count = eligible_count;
	if (eligible_count == 0) {
		return saw_compatible ? IS_PLACEMENT_INSUFFICIENT_CAPACITY
				      : IS_PLACEMENT_NO_COMPATIBLE_PROVIDER;
	}

	if (sample_size == 0)
		return IS_PLACEMENT_INVALID_ARGUMENT;
	if (sample_size > eligible_count)
		sample_size = eligible_count;

	for (index = 0; index < eligible_count; index++)
		remaining[index] = eligible[index];
	remaining_count = eligible_count;

	while (sampled_count < sample_size && remaining_count > 0) {
		is_placement_u32 total_weight = 0;
		is_placement_u32 pick;
		is_placement_u32 cursor = 0;
		unsigned int selected = 0;

		for (index = 0; index < remaining_count; index++) {
			total_weight +=
				candidates[remaining[index]].placement_weight;
			if (total_weight <
			    candidates[remaining[index]].placement_weight)
				return IS_PLACEMENT_INVALID_ARGUMENT;
		}
		pick = rand_fn(rand_ctx, total_weight);
		for (index = 0; index < remaining_count; index++) {
			cursor += candidates[remaining[index]].placement_weight;
			if (pick < cursor) {
				selected = index;
				break;
			}
		}
		sampled[sampled_count++] = remaining[selected];
		remaining[selected] = remaining[remaining_count - 1U];
		remaining_count--;
	}

	chosen_eligible = sampled[0];
	best_chunks = candidates[chosen_eligible].available_chunks;
	for (index = 1; index < sampled_count; index++) {
		unsigned int candidate_index = sampled[index];

		if (candidates[candidate_index].available_chunks > best_chunks) {
			best_chunks = candidates[candidate_index].available_chunks;
			chosen_eligible = candidate_index;
		} else if (candidates[candidate_index].available_chunks ==
				   best_chunks &&
			   candidate_index < chosen_eligible) {
			chosen_eligible = candidate_index;
		}
	}

	out->chosen_index = chosen_eligible;
	out->sample_size_used = sampled_count;
	return IS_PLACEMENT_OK;
}
