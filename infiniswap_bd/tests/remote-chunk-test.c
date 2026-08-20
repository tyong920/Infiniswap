// SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause
#include "../is_remote_chunk.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>

#define TEST_THRESHOLD 8ULL
#define TEST_READ_WEIGHT 1U
#define TEST_WRITE_WEIGHT 4U

static const struct is_remote_chunk_config backed_config = {
	.mode = IS_REMOTE_CHUNK_MODE_BACKED,
	.chunk_count = 4,
	.hot_policy = {
		.threshold = TEST_THRESHOLD,
		.read_weight = TEST_READ_WEIGHT,
		.write_weight = TEST_WRITE_WEIGHT,
	},
};

static const struct is_remote_chunk_config remote_only_config = {
	.mode = IS_REMOTE_CHUNK_MODE_REMOTE_ONLY,
	.chunk_count = 4,
	.hot_policy = {
		.threshold = TEST_THRESHOLD,
		.read_weight = TEST_READ_WEIGHT,
		.write_weight = TEST_WRITE_WEIGHT,
	},
};

struct concurrent_commit {
	struct is_remote_chunk_module *module;
	const struct is_remote_chunk_mapping_claim *claim;
	struct is_remote_chunk_provider_handle provider;
	const struct is_remote_chunk_mapping_grant *grants;
	unsigned int grant_count;
	int status;
};

static struct is_remote_chunk_mapping_grant mapping_grant(
	unsigned int logical_chunk, unsigned int provider_chunk)
{
	const struct is_remote_chunk_mapping_grant grant = {
		.logical_chunk = logical_chunk,
		.provider_chunk = provider_chunk,
		.remote_address = 0x100000ULL + logical_chunk * 0x1000ULL,
		.remote_key = 100U + logical_chunk,
	};

	return grant;
}

static int expect_snapshot(struct is_remote_chunk_module *module,
			   unsigned int assigned, unsigned int usable,
			   unsigned int mapping, unsigned int claims,
			   unsigned int placements, const char *name)
{
	struct is_remote_chunk_snapshot *snapshot = NULL;
	int failed = 0;
	int status = is_remote_chunk_snapshot_take(module, &snapshot);

	if (status || !snapshot) {
		fprintf(stderr, "%s: snapshot failed: %d\n", name, status);
		return 1;
	}
	if (snapshot->assigned_chunks != assigned ||
	    snapshot->usable_chunks != usable ||
	    snapshot->mapping_chunks != mapping ||
	    snapshot->active_mapping_claims != claims ||
	    snapshot->placement_count != placements ||
	    snapshot->assigned_chunks < snapshot->usable_chunks) {
		fprintf(stderr,
			"%s: assigned=%u usable=%u mapping=%u claims=%u placements=%u\n",
			name, snapshot->assigned_chunks, snapshot->usable_chunks,
			snapshot->mapping_chunks,
			snapshot->active_mapping_claims, snapshot->placement_count);
		failed = 1;
	}
	is_remote_chunk_snapshot_release(snapshot);
	return failed;
}

static int test_explicit_mapping_commit_is_atomic(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_mapping_claim claim =
		IS_REMOTE_CHUNK_MAPPING_CLAIM_INIT;
	const unsigned int logical_chunks[] = { 0, 1 };
	struct is_remote_chunk_mapping_grant grants[] = {
		mapping_grant(0, 20),
		mapping_grant(1, 21),
	};
	int failed = 0;

	if (is_remote_chunk_module_create(&remote_only_config, &module) ||
	    is_remote_chunk_provider_handle_create(module, &provider) ||
	    is_remote_chunk_mapping_begin_explicit(module, provider,
		logical_chunks, 2, &claim))
		return 1;
	failed |= expect_snapshot(module, 0, 0, 2, 1, 0, "mapping begin");
	if (is_remote_chunk_mapping_commit(module, &claim, provider, grants, 2))
		failed = 1;
	failed |= expect_snapshot(module, 2, 2, 0, 0, 2, "mapping commit");
	{
		struct is_remote_chunk_snapshot *snapshot = NULL;

		if (is_remote_chunk_snapshot_take(module, &snapshot) ||
		    snapshot->provider_count != 1 ||
		    snapshot->providers[0].assigned_chunks != 2 ||
		    snapshot->providers[0].usable_chunks != 2 ||
		    !is_remote_chunk_provider_handle_equal(
			snapshot->providers[0].provider, provider) ||
		    !snapshot->placements[0].usable ||
		    !snapshot->placements[1].usable)
			failed = 1;
		is_remote_chunk_snapshot_release(snapshot);
	}
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_malformed_commit_does_not_partially_map(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_mapping_claim claim =
		IS_REMOTE_CHUNK_MAPPING_CLAIM_INIT;
	const unsigned int logical_chunks[] = { 0, 1 };
	struct is_remote_chunk_mapping_grant grants[] = {
		mapping_grant(0, 30),
		mapping_grant(2, 31),
	};
	struct is_remote_chunk_mapping_grant duplicate_provider_grants[] = {
		mapping_grant(0, 32),
		mapping_grant(1, 32),
	};
	int failed = 0;

	if (is_remote_chunk_module_create(&remote_only_config, &module) ||
	    is_remote_chunk_provider_handle_create(module, &provider) ||
	    is_remote_chunk_mapping_begin_explicit(module, provider,
		logical_chunks, 2, &claim))
		return 1;
	if (is_remote_chunk_mapping_commit(module, &claim, provider, grants, 1) !=
	    -EINVAL)
		failed = 1;
	failed |= expect_snapshot(module, 0, 0, 2, 1, 0,
		"partial mapping commit");
	if (is_remote_chunk_mapping_commit(module, &claim, provider, grants, 2) !=
	    -EINVAL)
		failed = 1;
	failed |= expect_snapshot(module, 0, 0, 2, 1, 0,
		"malformed mapping commit");
	if (is_remote_chunk_mapping_commit(module, &claim, provider,
		duplicate_provider_grants, 2) != -EINVAL)
		failed = 1;
	failed |= expect_snapshot(module, 0, 0, 2, 1, 0,
		"duplicate Provider chunk commit");
	failed |= is_remote_chunk_mapping_abort(module, &claim) != 0;
	failed |= expect_snapshot(module, 0, 0, 0, 0, 0, "mapping abort");
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_duplicate_and_wrong_provider_commits_are_rejected(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle first_provider;
	struct is_remote_chunk_provider_handle second_provider;
	struct is_remote_chunk_mapping_claim claim =
		IS_REMOTE_CHUNK_MAPPING_CLAIM_INIT;
	const unsigned int logical_chunk = 0;
	struct is_remote_chunk_mapping_grant grant = mapping_grant(0, 40);
	int failed = 0;

	if (is_remote_chunk_module_create(&remote_only_config, &module) ||
	    is_remote_chunk_provider_handle_create(module, &first_provider) ||
	    is_remote_chunk_provider_handle_create(module, &second_provider) ||
	    is_remote_chunk_mapping_begin_explicit(module, first_provider,
		&logical_chunk, 1, &claim))
		return 1;
	if (is_remote_chunk_mapping_commit(module, &claim, second_provider,
		&grant, 1) != -ESTALE)
		failed = 1;
	if (is_remote_chunk_mapping_commit(module, &claim, first_provider,
		&grant, 1))
		failed = 1;
	if (is_remote_chunk_mapping_commit(module, &claim, first_provider,
		&grant, 1) != -ESTALE)
		failed = 1;
	if (is_remote_chunk_mapping_abort(module, &claim) != -ESTALE)
		failed = 1;
	failed |= expect_snapshot(module, 1, 1, 0, 0, 1,
		"stale mapping claim");
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_delayed_claim_cannot_commit_after_abort_and_remap(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_mapping_claim old_claim =
		IS_REMOTE_CHUNK_MAPPING_CLAIM_INIT;
	struct is_remote_chunk_mapping_claim current_claim =
		IS_REMOTE_CHUNK_MAPPING_CLAIM_INIT;
	const unsigned int logical_chunk = 3;
	struct is_remote_chunk_mapping_grant old_grant = mapping_grant(3, 60);
	struct is_remote_chunk_mapping_grant current_grant = mapping_grant(3, 61);
	int failed = 0;

	if (is_remote_chunk_module_create(&remote_only_config, &module) ||
	    is_remote_chunk_provider_handle_create(module, &provider) ||
	    is_remote_chunk_mapping_begin_explicit(module, provider,
		&logical_chunk, 1, &old_claim) ||
	    is_remote_chunk_mapping_abort(module, &old_claim) ||
	    is_remote_chunk_mapping_begin_explicit(module, provider,
		&logical_chunk, 1, &current_claim))
		return 1;
	if (is_remote_chunk_mapping_commit(module, &old_claim, provider,
		&old_grant, 1) != -ESTALE)
		failed = 1;
	failed |= expect_snapshot(module, 0, 0, 1, 1, 0,
		"delayed mapping claim");
	if (is_remote_chunk_mapping_commit(module, &current_claim, provider,
		&current_grant, 1))
		failed = 1;
	failed |= expect_snapshot(module, 1, 1, 0, 0, 1,
		"current mapping claim");
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_provider_handles_are_epoch_stable_and_module_scoped(void)
{
	struct is_remote_chunk_module *first_module = NULL;
	struct is_remote_chunk_module *second_module = NULL;
	struct is_remote_chunk_provider_handle first;
	struct is_remote_chunk_provider_handle next_epoch;
	struct is_remote_chunk_provider_handle other_module;
	struct is_remote_chunk_mapping_claim claim =
		IS_REMOTE_CHUNK_MAPPING_CLAIM_INIT;
	const unsigned int logical_chunk = 0;
	int failed = 0;

	if (is_remote_chunk_module_create(&remote_only_config, &first_module) ||
	    is_remote_chunk_module_create(&remote_only_config, &second_module) ||
	    is_remote_chunk_provider_handle_create(first_module, &first) ||
	    is_remote_chunk_provider_handle_create(first_module, &next_epoch) ||
	    is_remote_chunk_provider_handle_create(second_module, &other_module))
		return 1;
	if (is_remote_chunk_provider_handle_equal(first, next_epoch) ||
	    is_remote_chunk_provider_handle_equal(first, other_module))
		failed = 1;
	if (is_remote_chunk_mapping_begin_explicit(first_module, other_module,
		&logical_chunk, 1, &claim) != -ESTALE)
		failed = 1;
	failed |= expect_snapshot(first_module, 0, 0, 0, 0, 0,
		"foreign Provider handle");
	failed |= is_remote_chunk_module_destroy(first_module) != 0;
	failed |= is_remote_chunk_module_destroy(second_module) != 0;
	return failed;
}

static int test_begin_validates_full_batch_before_transition(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_mapping_claim claim =
		IS_REMOTE_CHUNK_MAPPING_CLAIM_INIT;
	const unsigned int duplicate_chunks[] = { 1, 1 };
	const unsigned int invalid_chunks[] = { 2, 9 };
	int failed = 0;

	if (is_remote_chunk_module_create(&remote_only_config, &module) ||
	    is_remote_chunk_provider_handle_create(module, &provider))
		return 1;
	if (is_remote_chunk_mapping_begin_explicit(module, provider,
		duplicate_chunks, 2, &claim) != -EINVAL)
		failed = 1;
	failed |= expect_snapshot(module, 0, 0, 0, 0, 0,
		"duplicate mapping batch");
	if (is_remote_chunk_mapping_begin_explicit(module, provider,
		invalid_chunks, 2, &claim) != -ERANGE)
		failed = 1;
	failed |= expect_snapshot(module, 0, 0, 0, 0, 0,
		"invalid mapping batch");
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_hot_range_policy_and_activity_drive_mapping(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_mapping_claim claim =
		IS_REMOTE_CHUNK_MAPPING_CLAIM_INIT;
	struct is_remote_chunk_mapping_grant grant;
	struct is_remote_chunk_snapshot *snapshot = NULL;
	struct is_remote_chunk_hot_policy policy = {
		.threshold = 10,
		.read_weight = 3,
		.write_weight = 7,
	};
	unsigned int logical_chunk = 99;
	bool mapping_needed = true;
	int failed = 0;

	if (is_remote_chunk_module_create(&backed_config, &module) ||
	    is_remote_chunk_provider_handle_create(module, &provider))
		return 1;
	if (is_remote_chunk_hot_policy_set(module, &policy, &mapping_needed) ||
	    mapping_needed)
		failed = 1;
	if (is_remote_chunk_note_activity(module, 2, 1,
		IS_REMOTE_CHUNK_ACTIVITY_READ, &mapping_needed) || mapping_needed)
		failed = 1;
	if (is_remote_chunk_note_activity(module, 2, 1,
		IS_REMOTE_CHUNK_ACTIVITY_WRITE, &mapping_needed) || !mapping_needed)
		failed = 1;
	if (is_remote_chunk_snapshot_take(module, &snapshot) ||
	    snapshot->hot_ranges != 1 || snapshot->mapping_candidates != 1 ||
	    snapshot->hot_policy.threshold != 10 ||
	    snapshot->hot_policy.read_weight != 3 ||
	    snapshot->hot_policy.write_weight != 7)
		failed = 1;
	is_remote_chunk_snapshot_release(snapshot);
	if (is_remote_chunk_mapping_begin_hot(module, provider, &claim,
		&logical_chunk) || logical_chunk != 2)
		failed = 1;
	grant = mapping_grant(logical_chunk, 50);
	if (is_remote_chunk_mapping_commit(module, &claim, provider, &grant, 1))
		failed = 1;
	failed |= expect_snapshot(module, 1, 1, 0, 0, 1, "Hot Range commit");

	if (is_remote_chunk_note_activity(module, 1, 1,
		IS_REMOTE_CHUNK_ACTIVITY_READ, &mapping_needed) || mapping_needed)
		failed = 1;
	policy.threshold = 3;
	if (is_remote_chunk_hot_policy_set(module, &policy, &mapping_needed) ||
	    !mapping_needed)
		failed = 1;
	if (is_remote_chunk_mapping_begin_hot(module, provider, &claim,
		&logical_chunk) || logical_chunk != 1 ||
	    is_remote_chunk_mapping_abort(module, &claim))
		failed = 1;
	if (is_remote_chunk_note_activity(module, 3, 2,
		IS_REMOTE_CHUNK_ACTIVITY_WRITE, &mapping_needed) != -ERANGE)
		failed = 1;
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static void *run_mapping_commit(void *context)
{
	struct concurrent_commit *commit = context;

	commit->status = is_remote_chunk_mapping_commit(commit->module,
		commit->claim, commit->provider, commit->grants, commit->grant_count);
	return NULL;
}

static int test_concurrent_duplicate_commit_has_one_winner(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_mapping_claim claim =
		IS_REMOTE_CHUNK_MAPPING_CLAIM_INIT;
	const unsigned int logical_chunks[] = { 0, 1 };
	const struct is_remote_chunk_mapping_grant grants[] = {
		{ 0, 70, 0x200000, 170 },
		{ 1, 71, 0x201000, 171 },
	};
	struct concurrent_commit commits[2];
	pthread_t threads[2];
	unsigned int index;
	unsigned int committed = 0;
	unsigned int stale = 0;
	int failed = 0;

	if (is_remote_chunk_module_create(&remote_only_config, &module) ||
	    is_remote_chunk_provider_handle_create(module, &provider) ||
	    is_remote_chunk_mapping_begin_explicit(module, provider,
		logical_chunks, 2, &claim))
		return 1;
	for (index = 0; index < 2; index++) {
		commits[index].module = module;
		commits[index].claim = &claim;
		commits[index].provider = provider;
		commits[index].grants = grants;
		commits[index].grant_count = 2;
		commits[index].status = -999;
		if (pthread_create(&threads[index], NULL, run_mapping_commit,
			&commits[index]))
			return 1;
	}
	for (index = 0; index < 2; index++) {
		(void)pthread_join(threads[index], NULL);
		committed += commits[index].status == 0;
		stale += commits[index].status == -ESTALE;
	}
	if (committed != 1 || stale != 1)
		failed = 1;
	failed |= expect_snapshot(module, 2, 2, 0, 0, 2,
		"concurrent mapping commit");
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_destroy_rejects_active_claim(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_mapping_claim claim =
		IS_REMOTE_CHUNK_MAPPING_CLAIM_INIT;
	const unsigned int logical_chunk = 0;
	int failed = 0;

	if (is_remote_chunk_module_create(&remote_only_config, &module) ||
	    is_remote_chunk_provider_handle_create(module, &provider) ||
	    is_remote_chunk_mapping_begin_explicit(module, provider,
		&logical_chunk, 1, &claim))
		return 1;
	if (is_remote_chunk_module_destroy(module) != -EBUSY)
		failed = 1;
	if (is_remote_chunk_mapping_abort(module, &claim))
		failed = 1;
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

int main(void)
{
	int failed = test_explicit_mapping_commit_is_atomic() |
		test_malformed_commit_does_not_partially_map() |
		test_duplicate_and_wrong_provider_commits_are_rejected() |
		test_delayed_claim_cannot_commit_after_abort_and_remap() |
		test_provider_handles_are_epoch_stable_and_module_scoped() |
		test_begin_validates_full_batch_before_transition() |
		test_hot_range_policy_and_activity_drive_mapping() |
		test_concurrent_duplicate_commit_has_one_winner() |
		test_destroy_rejects_active_claim();

	if (failed)
		fprintf(stderr, "Remote Chunk mapping tests failed\n");
	return failed;
}
