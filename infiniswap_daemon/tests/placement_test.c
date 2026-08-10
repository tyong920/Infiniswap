#include "infiniswap_placement.h"

#include <stdio.h>
#include <string.h>

struct lcg_rng {
	is_placement_u64 state;
};

static is_placement_u32 lcg_next(void *ctx, is_placement_u32 limit)
{
	struct lcg_rng *rng = ctx;

	if (limit == 0)
		return 0;
	rng->state = rng->state * 6364136223846793005ULL + 1ULL;
	return (is_placement_u32)((rng->state >> 33) % limit);
}

static void fill_candidate(struct is_placement_candidate *candidate,
			   const char *id, is_placement_u32 chunks,
			   is_placement_u32 weight, int healthy, int compatible,
			   enum is_placement_exclude_reason reason)
{
	memset(candidate, 0, sizeof(*candidate));
	strncpy(candidate->provider_id, id, IS_PLACEMENT_PROVIDER_ID_MAX);
	candidate->available_chunks = chunks;
	candidate->placement_weight = weight;
	candidate->healthy = healthy;
	candidate->compatible = compatible;
	candidate->exclude_reason = reason;
}

static int expect_result(const char *name, enum is_placement_result got,
			 enum is_placement_result want)
{
	if (got == want)
		return 0;
	fprintf(stderr, "%s: got %d want %d\n", name, (int)got, (int)want);
	return 1;
}

static int test_validate_sample_size(void)
{
	int failed = 0;

	failed |= expect_result("d=0",
		is_placement_validate_sample_size(0, 2),
		IS_PLACEMENT_INVALID_ARGUMENT);
	failed |= expect_result("d>healthy",
		is_placement_validate_sample_size(3, 2),
		IS_PLACEMENT_INVALID_ARGUMENT);
	failed |= expect_result("empty set",
		is_placement_validate_sample_size(1, 0),
		IS_PLACEMENT_INVALID_ARGUMENT);
	failed |= expect_result("d=1 of 1",
		is_placement_validate_sample_size(1, 1), IS_PLACEMENT_OK);
	failed |= expect_result("d=2 of 3",
		is_placement_validate_sample_size(2, 3), IS_PLACEMENT_OK);
	return failed;
}

static int test_two_providers_prefer_more_capacity(void)
{
	struct is_placement_candidate candidates[2];
	struct is_placement_choice choice;
	struct lcg_rng rng = { .state = 7 };
	enum is_placement_result result;

	fill_candidate(&candidates[0], "provider-a", 1, 100, 1, 1,
		       IS_PLACEMENT_EXCLUDE_NONE);
	fill_candidate(&candidates[1], "provider-b", 8, 100, 1, 1,
		       IS_PLACEMENT_EXCLUDE_NONE);

	result = is_placement_choose(candidates, 2, 2, lcg_next, &rng, &choice);
	if (result != IS_PLACEMENT_OK) {
		fprintf(stderr, "two-provider capacity choose failed: %d\n",
			(int)result);
		return 1;
	}
	if (choice.chosen_index != 1 ||
	    strcmp(candidates[choice.chosen_index].provider_id, "provider-b") !=
		    0) {
		fprintf(stderr, "expected provider-b for higher capacity\n");
		return 1;
	}
	if (choice.sample_size_used != 2 || choice.eligible_count != 2) {
		fprintf(stderr, "unexpected sample accounting\n");
		return 1;
	}
	return 0;
}

static int test_weights_bias_sampling_with_d_equals_one(void)
{
	struct is_placement_candidate candidates[2];
	struct is_placement_choice choice;
	unsigned int a_wins = 0;
	unsigned int b_wins = 0;
	unsigned int trial;

	fill_candidate(&candidates[0], "provider-a", 4, 900, 1, 1,
		       IS_PLACEMENT_EXCLUDE_NONE);
	fill_candidate(&candidates[1], "provider-b", 4, 100, 1, 1,
		       IS_PLACEMENT_EXCLUDE_NONE);

	for (trial = 0; trial < 200; trial++) {
		struct lcg_rng rng = { .state = 1000ULL + trial };

		if (is_placement_choose(candidates, 2, 1, lcg_next, &rng,
					&choice) != IS_PLACEMENT_OK)
			return 1;
		if (choice.chosen_index == 0)
			a_wins++;
		else
			b_wins++;
	}
	if (a_wins < 150 || b_wins == 0) {
		fprintf(stderr,
			"weight bias unexpected: a=%u b=%u\n", a_wins, b_wins);
		return 1;
	}
	return 0;
}

static int test_unhealthy_and_incompatible_are_excluded(void)
{
	struct is_placement_candidate candidates[3];
	struct is_placement_choice choice;
	struct lcg_rng rng = { .state = 42 };
	enum is_placement_result result;

	fill_candidate(&candidates[0], "provider-a", 10, 100, 0, 1,
		       IS_PLACEMENT_EXCLUDE_UNHEALTHY);
	fill_candidate(&candidates[1], "provider-b", 10, 100, 1, 0,
		       IS_PLACEMENT_EXCLUDE_CAPABILITY);
	fill_candidate(&candidates[2], "provider-c", 3, 100, 1, 1,
		       IS_PLACEMENT_EXCLUDE_NONE);

	result = is_placement_choose(candidates, 3, 1, lcg_next, &rng, &choice);
	if (result != IS_PLACEMENT_OK || choice.chosen_index != 2 ||
	    choice.eligible_count != 1) {
		fprintf(stderr, "excluded providers still eligible\n");
		return 1;
	}
	return 0;
}

static int test_zero_capacity_is_insufficient(void)
{
	struct is_placement_candidate candidates[2];
	struct is_placement_choice choice;
	struct lcg_rng rng = { .state = 1 };

	fill_candidate(&candidates[0], "provider-a", 0, 100, 1, 1,
		       IS_PLACEMENT_EXCLUDE_ZERO_CAPACITY);
	fill_candidate(&candidates[1], "provider-b", 0, 100, 1, 1,
		       IS_PLACEMENT_EXCLUDE_ZERO_CAPACITY);
	return expect_result(
		"zero capacity",
		is_placement_choose(candidates, 2, 2, lcg_next, &rng, &choice),
		IS_PLACEMENT_INSUFFICIENT_CAPACITY);
}

static int test_three_providers_deterministic_seed(void)
{
	struct is_placement_candidate candidates[3];
	struct is_placement_choice first;
	struct is_placement_choice second;
	struct lcg_rng rng_a = { .state = 12345 };
	struct lcg_rng rng_b = { .state = 12345 };

	fill_candidate(&candidates[0], "provider-a", 5, 100, 1, 1,
		       IS_PLACEMENT_EXCLUDE_NONE);
	fill_candidate(&candidates[1], "provider-b", 5, 100, 1, 1,
		       IS_PLACEMENT_EXCLUDE_NONE);
	fill_candidate(&candidates[2], "provider-c", 9, 100, 1, 1,
		       IS_PLACEMENT_EXCLUDE_NONE);

	if (is_placement_choose(candidates, 3, 2, lcg_next, &rng_a, &first) !=
		    IS_PLACEMENT_OK ||
	    is_placement_choose(candidates, 3, 2, lcg_next, &rng_b, &second) !=
		    IS_PLACEMENT_OK) {
		fprintf(stderr, "deterministic three-provider choose failed\n");
		return 1;
	}
	if (first.chosen_index != second.chosen_index ||
	    first.sample_size_used != second.sample_size_used) {
		fprintf(stderr, "same seed produced different placement\n");
		return 1;
	}
	if (candidates[first.chosen_index].available_chunks < 5) {
		fprintf(stderr, "chosen provider lost capacity race\n");
		return 1;
	}
	return 0;
}

static int test_exclude_reason_names(void)
{
	if (strcmp(is_placement_exclude_reason_name(
			   IS_PLACEMENT_EXCLUDE_IDENTITY),
		   "identity") != 0 ||
	    strcmp(is_placement_exclude_reason_name(
			   IS_PLACEMENT_EXCLUDE_RAIL),
		   "rail") != 0 ||
	    strcmp(is_placement_exclude_reason_name(
			   IS_PLACEMENT_EXCLUDE_VERSION),
		   "version") != 0) {
		fprintf(stderr, "exclude reason names mismatch\n");
		return 1;
	}
	return 0;
}

int main(void)
{
	int failed = 0;

	failed |= test_validate_sample_size();
	failed |= test_two_providers_prefer_more_capacity();
	failed |= test_weights_bias_sampling_with_d_equals_one();
	failed |= test_unhealthy_and_incompatible_are_excluded();
	failed |= test_zero_capacity_is_insufficient();
	failed |= test_three_providers_deterministic_seed();
	failed |= test_exclude_reason_names();
	if (failed)
		fprintf(stderr, "placement tests failed\n");
	return failed ? 1 : 0;
}
