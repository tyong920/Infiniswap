// SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause
#include "../is_remote_chunk.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>

#define TEST_THRESHOLD 8ULL
#define TEST_READ_WEIGHT 1U
#define TEST_WRITE_WEIGHT 4U

_Static_assert(sizeof(struct is_remote_chunk_io_lease) ==
	IS_REMOTE_CHUNK_IO_LEASE_BYTES, "lease storage size changed");
_Static_assert(_Alignof(struct is_remote_chunk_io_lease) >= _Alignof(void *),
	"lease storage is not pointer-aligned");

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

struct test_gate {
	pthread_mutex_t lock;
	pthread_cond_t changed;
	bool open;
};

struct concurrent_commit {
	struct is_remote_chunk_module *module;
	const struct is_remote_chunk_mapping_claim *claim;
	struct is_remote_chunk_provider_handle provider;
	const struct is_remote_chunk_mapping_grant *grants;
	unsigned int grant_count;
	struct test_gate *start;
	int status;
};

struct concurrent_snapshot {
	struct is_remote_chunk_module *module;
	struct test_gate *start;
	int failed;
};

struct concurrent_lease_event {
	struct is_remote_chunk_module *module;
	struct is_remote_chunk_io_lease *lease;
	struct test_gate *start;
	enum is_remote_chunk_io_resolve_result result;
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

static int map_remote_only_chunk(struct is_remote_chunk_module *module,
	struct is_remote_chunk_provider_handle provider,
	unsigned int logical_chunk, unsigned int provider_chunk)
{
	struct is_remote_chunk_mapping_claim claim =
		IS_REMOTE_CHUNK_MAPPING_CLAIM_INIT;
	struct is_remote_chunk_mapping_grant grant =
		mapping_grant(logical_chunk, provider_chunk);
	int status = is_remote_chunk_mapping_begin_explicit(module, provider,
		&logical_chunk, 1, &claim);

	if (status)
		return status;
	status = is_remote_chunk_mapping_commit(module, &claim, provider,
		&grant, 1);
	if (status)
		(void)is_remote_chunk_mapping_abort(module, &claim);
	return status;
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
	struct is_remote_chunk_mapping_grant overflow_grants[] = {
		mapping_grant(0, 33),
		mapping_grant(1, 34),
	};
	int failed = 0;

	overflow_grants[1].remote_address = ~0ULL -
		(IS_REMOTE_CHUNK_SECTORS_PER_CHUNK *
		 IS_REMOTE_CHUNK_SECTOR_BYTES - 2ULL);

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
	if (is_remote_chunk_mapping_commit(module, &claim, provider,
		overflow_grants, 2) != -EINVAL)
		failed = 1;
	failed |= expect_snapshot(module, 0, 0, 2, 1, 0,
		"overflowing remote address commit");
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

static int test_gate_init(struct test_gate *gate)
{
	int status = pthread_mutex_init(&gate->lock, NULL);

	gate->open = false;
	if (status)
		return status;
	status = pthread_cond_init(&gate->changed, NULL);
	if (status)
		(void)pthread_mutex_destroy(&gate->lock);
	return status;
}

static void test_gate_wait(struct test_gate *gate)
{
	pthread_mutex_lock(&gate->lock);
	while (!gate->open)
		pthread_cond_wait(&gate->changed, &gate->lock);
	pthread_mutex_unlock(&gate->lock);
}

static void test_gate_open(struct test_gate *gate)
{
	pthread_mutex_lock(&gate->lock);
	gate->open = true;
	pthread_cond_broadcast(&gate->changed);
	pthread_mutex_unlock(&gate->lock);
}

static void test_gate_destroy(struct test_gate *gate)
{
	(void)pthread_cond_destroy(&gate->changed);
	(void)pthread_mutex_destroy(&gate->lock);
}

static void *run_mapping_commit(void *context)
{
	struct concurrent_commit *commit = context;

	test_gate_wait(commit->start);
	commit->status = is_remote_chunk_mapping_commit(commit->module,
		commit->claim, commit->provider, commit->grants, commit->grant_count);
	return NULL;
}

static void *run_snapshots_during_commit(void *context)
{
	struct concurrent_snapshot *event = context;
	unsigned int index;

	test_gate_wait(event->start);
	for (index = 0; index < 100; index++) {
		struct is_remote_chunk_snapshot *snapshot = NULL;
		bool before;
		bool after;

		if (is_remote_chunk_snapshot_take(event->module, &snapshot)) {
			event->failed = 1;
			break;
		}
		before = snapshot->assigned_chunks == 0 &&
			snapshot->usable_chunks == 0 &&
			snapshot->mapping_chunks == 2 &&
			snapshot->active_mapping_claims == 1 &&
			snapshot->placement_count == 0;
		after = snapshot->assigned_chunks == 2 &&
			snapshot->usable_chunks == 2 &&
			snapshot->mapping_chunks == 0 &&
			snapshot->active_mapping_claims == 0 &&
			snapshot->placement_count == 2;
		if (!before && !after)
			event->failed = 1;
		is_remote_chunk_snapshot_release(snapshot);
	}
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
	struct concurrent_snapshot snapshot_event;
	struct test_gate start;
	pthread_t threads[2];
	pthread_t snapshot_thread;
	unsigned int index;
	unsigned int committed = 0;
	unsigned int stale = 0;
	int failed = 0;

	if (is_remote_chunk_module_create(&remote_only_config, &module) ||
	    is_remote_chunk_provider_handle_create(module, &provider) ||
	    is_remote_chunk_mapping_begin_explicit(module, provider,
		logical_chunks, 2, &claim) || test_gate_init(&start))
		return 1;
	snapshot_event.module = module;
	snapshot_event.start = &start;
	snapshot_event.failed = 0;
	if (pthread_create(&snapshot_thread, NULL, run_snapshots_during_commit,
		&snapshot_event))
		return 1;
	for (index = 0; index < 2; index++) {
		commits[index].module = module;
		commits[index].claim = &claim;
		commits[index].provider = provider;
		commits[index].grants = grants;
		commits[index].grant_count = 2;
		commits[index].start = &start;
		commits[index].status = -999;
		if (pthread_create(&threads[index], NULL, run_mapping_commit,
			&commits[index]))
			return 1;
	}
	test_gate_open(&start);
	for (index = 0; index < 2; index++) {
		(void)pthread_join(threads[index], NULL);
		committed += commits[index].status == 0;
		stale += commits[index].status == -ESTALE;
	}
	(void)pthread_join(snapshot_thread, NULL);
	test_gate_destroy(&start);
	if (committed != 1 || stale != 1 || snapshot_event.failed)
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

static int expect_read_admission(struct is_remote_chunk_module *module,
	unsigned long long sector, unsigned int bytes, int expected_status)
{
	struct is_remote_chunk_io_lease lease = IS_REMOTE_CHUNK_IO_LEASE_INIT;
	struct is_remote_chunk_io_request request = {
		.direction = IS_REMOTE_CHUNK_IO_READ,
		.sector = sector,
		.bytes = bytes,
	};
	struct is_remote_chunk_transport_mapping mapping;
	enum is_remote_chunk_io_resolve_result result;
	int status = is_remote_chunk_io_lease_acquire(module, &request, &lease,
		&mapping);

	if (status != expected_status)
		return 1;
	if (status)
		return 0;
	return is_remote_chunk_io_lease_resolve(module, &lease,
			IS_REMOTE_CHUNK_IO_SUCCESS, &result) ||
		result != IS_REMOTE_CHUNK_IO_RESOLVE_CURRENT ||
		is_remote_chunk_io_lease_release(module, &lease);
}

static int acquire_and_settle_write(struct is_remote_chunk_module *module,
	unsigned long long sector, enum is_remote_chunk_io_outcome outcome)
{
	struct is_remote_chunk_io_lease lease = IS_REMOTE_CHUNK_IO_LEASE_INIT;
	struct is_remote_chunk_io_request request = {
		.direction = IS_REMOTE_CHUNK_IO_WRITE,
		.sector = sector,
		.bytes = 4096,
	};
	struct is_remote_chunk_transport_mapping mapping;
	enum is_remote_chunk_io_resolve_result result;

	return is_remote_chunk_io_lease_acquire(module, &request, &lease,
			&mapping) ||
		is_remote_chunk_io_lease_resolve(module, &lease, outcome, &result) ||
		result != IS_REMOTE_CHUNK_IO_RESOLVE_CURRENT ||
		is_remote_chunk_io_lease_release(module, &lease);
}

static int test_write_outcomes_control_sector_validity(void)
{
	static const enum is_remote_chunk_io_outcome invalid_outcomes[] = {
		IS_REMOTE_CHUNK_IO_FAILURE,
		IS_REMOTE_CHUNK_IO_CANCELLED,
		IS_REMOTE_CHUNK_IO_SUBMISSION_FAILURE,
	};
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	unsigned int index;
	int failed = 0;

	if (is_remote_chunk_module_create(&remote_only_config, &module) ||
	    is_remote_chunk_provider_handle_create(module, &provider) ||
	    map_remote_only_chunk(module, provider, 0, 20))
		return 1;
	for (index = 0; index < sizeof(invalid_outcomes) /
				 sizeof(invalid_outcomes[0]); index++) {
		unsigned long long sector = 16ULL + index * 8ULL;

		failed |= acquire_and_settle_write(module, sector,
			invalid_outcomes[index]);
		failed |= expect_read_admission(module, sector, 4096, -ENODATA);
	}
	failed |= acquire_and_settle_write(module, 64, IS_REMOTE_CHUNK_IO_SUCCESS);
	failed |= expect_read_admission(module, 64, 4096, 0);
	failed |= expect_read_admission(module, 66, 1024, 0);
	failed |= expect_read_admission(module, 72, 512, -ENODATA);
	failed |= expect_read_admission(module, 71, 1024, -ENODATA);

	/* A new write hides an older valid copy before transport submission. */
	{
		struct is_remote_chunk_io_lease lease = IS_REMOTE_CHUNK_IO_LEASE_INIT;
		struct is_remote_chunk_io_request request = {
			.direction = IS_REMOTE_CHUNK_IO_WRITE,
			.sector = 64,
			.bytes = 4096,
		};
		struct is_remote_chunk_transport_mapping mapping;
		enum is_remote_chunk_io_resolve_result result;

		if (is_remote_chunk_io_lease_acquire(module, &request, &lease,
			&mapping))
			failed = 1;
		failed |= expect_read_admission(module, 64, 4096, -ENODATA);
		if (is_remote_chunk_io_lease_resolve(module, &lease,
			IS_REMOTE_CHUNK_IO_FAILURE, &result) ||
		    is_remote_chunk_io_lease_release(module, &lease))
			failed = 1;
		failed |= expect_read_admission(module, 64, 4096, -ENODATA);
	}
	/* A failed overlapping write clears validity restored by another lease. */
	{
		struct is_remote_chunk_io_lease successful =
			IS_REMOTE_CHUNK_IO_LEASE_INIT;
		struct is_remote_chunk_io_lease failed_lease =
			IS_REMOTE_CHUNK_IO_LEASE_INIT;
		struct is_remote_chunk_io_request request = {
			.direction = IS_REMOTE_CHUNK_IO_WRITE,
			.sector = 112,
			.bytes = 4096,
		};
		struct is_remote_chunk_transport_mapping mapping;
		enum is_remote_chunk_io_resolve_result result;

		if (is_remote_chunk_io_lease_acquire(module, &request, &successful,
			&mapping) ||
		    is_remote_chunk_io_lease_acquire(module, &request, &failed_lease,
			&mapping) ||
		    is_remote_chunk_io_lease_resolve(module, &successful,
			IS_REMOTE_CHUNK_IO_SUCCESS, &result) ||
		    is_remote_chunk_io_lease_resolve(module, &failed_lease,
			IS_REMOTE_CHUNK_IO_FAILURE, &result) ||
		    is_remote_chunk_io_lease_release(module, &successful) ||
		    is_remote_chunk_io_lease_release(module, &failed_lease))
			failed = 1;
		failed |= expect_read_admission(module, 112, 4096, -ENODATA);
	}
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_illegal_lease_lifetimes_report_typed_invariants(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_io_lease lease = IS_REMOTE_CHUNK_IO_LEASE_INIT;
	struct is_remote_chunk_io_request request = {
		.direction = IS_REMOTE_CHUNK_IO_WRITE,
		.sector = 80,
		.bytes = 4096,
	};
	struct is_remote_chunk_transport_mapping mapping;
	struct is_remote_chunk_snapshot *snapshot = NULL;
	enum is_remote_chunk_io_resolve_result result;
	int failed = 0;

	if (is_remote_chunk_module_create(&remote_only_config, &module) ||
	    is_remote_chunk_provider_handle_create(module, &provider) ||
	    map_remote_only_chunk(module, provider, 0, 20) ||
	    is_remote_chunk_io_lease_acquire(module, &request, &lease, &mapping))
		return 1;
	if (is_remote_chunk_module_destroy(module) != -EBUSY ||
	    is_remote_chunk_io_lease_release(module, &lease) != -EPERM)
		failed = 1;
	if (is_remote_chunk_io_lease_resolve(module, &lease,
		IS_REMOTE_CHUNK_IO_SUCCESS, &result) ||
	    is_remote_chunk_io_lease_resolve(module, &lease,
		IS_REMOTE_CHUNK_IO_SUCCESS, &result) != -EALREADY ||
	    is_remote_chunk_io_lease_release(module, &lease) ||
	    is_remote_chunk_io_lease_release(module, &lease) != -EALREADY)
		failed = 1;
	if (is_remote_chunk_snapshot_take(module, &snapshot) ||
	    snapshot->active_io_leases != 0 || snapshot->invariant_count != 3 ||
	    snapshot->latest_invariant !=
		IS_REMOTE_CHUNK_INVARIANT_LEASE_DUPLICATE_RELEASE)
		failed = 1;
	is_remote_chunk_snapshot_release(snapshot);
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static void *run_lease_resolve(void *context)
{
	struct concurrent_lease_event *event = context;

	test_gate_wait(event->start);
	event->status = is_remote_chunk_io_lease_resolve(event->module,
		event->lease, IS_REMOTE_CHUNK_IO_SUCCESS, &event->result);
	return NULL;
}

static void *run_lease_release(void *context)
{
	struct concurrent_lease_event *event = context;

	test_gate_wait(event->start);
	event->status = is_remote_chunk_io_lease_release(event->module,
		event->lease);
	return NULL;
}

static int run_concurrent_lease_event_pair(
	struct concurrent_lease_event events[2],
	void *(*entry)(void *))
{
	struct test_gate start;
	pthread_t threads[2];
	unsigned int created = 0;
	unsigned int index;
	int failed = 0;

	if (test_gate_init(&start))
		return 1;
	for (index = 0; index < 2; index++) {
		events[index].start = &start;
		events[index].status = -999;
		if (pthread_create(&threads[index], NULL, entry, &events[index])) {
			failed = 1;
			break;
		}
		created++;
	}
	test_gate_open(&start);
	for (index = 0; index < created; index++) {
		if (pthread_join(threads[index], NULL))
			failed = 1;
	}
	test_gate_destroy(&start);
	return failed;
}

static int test_concurrent_lease_events_have_one_winner(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_io_lease lease = IS_REMOTE_CHUNK_IO_LEASE_INIT;
	struct is_remote_chunk_io_request request = {
		.direction = IS_REMOTE_CHUNK_IO_WRITE,
		.sector = 96,
		.bytes = 4096,
	};
	struct is_remote_chunk_transport_mapping mapping;
	struct concurrent_lease_event events[2];
	struct is_remote_chunk_snapshot *snapshot = NULL;
	enum is_remote_chunk_io_resolve_result cleanup_result;
	unsigned int index;
	unsigned int succeeded;
	unsigned int duplicate;
	int failed = 0;

	if (is_remote_chunk_module_create(&remote_only_config, &module) ||
	    is_remote_chunk_provider_handle_create(module, &provider) ||
	    map_remote_only_chunk(module, provider, 0, 20) ||
	    is_remote_chunk_io_lease_acquire(module, &request, &lease, &mapping))
		return 1;
	for (index = 0; index < 2; index++) {
		events[index].module = module;
		events[index].lease = &lease;
		events[index].result = IS_REMOTE_CHUNK_IO_RESOLVE_NONE;
	}
	if (run_concurrent_lease_event_pair(events, run_lease_resolve)) {
		(void)is_remote_chunk_io_lease_resolve(module, &lease,
			IS_REMOTE_CHUNK_IO_SUCCESS, &cleanup_result);
		(void)is_remote_chunk_io_lease_release(module, &lease);
		(void)is_remote_chunk_module_destroy(module);
		return 1;
	}
	succeeded = 0;
	duplicate = 0;
	for (index = 0; index < 2; index++) {
		succeeded += events[index].status == 0;
		duplicate += events[index].status == -EALREADY;
	}
	if (succeeded != 1 || duplicate != 1)
		failed = 1;
	if (run_concurrent_lease_event_pair(events, run_lease_release)) {
		(void)is_remote_chunk_io_lease_release(module, &lease);
		(void)is_remote_chunk_module_destroy(module);
		return 1;
	}
	succeeded = 0;
	duplicate = 0;
	for (index = 0; index < 2; index++) {
		succeeded += events[index].status == 0;
		duplicate += events[index].status == -EALREADY;
	}
	if (succeeded != 1 || duplicate != 1)
		failed = 1;
	if (is_remote_chunk_snapshot_take(module, &snapshot) ||
	    snapshot->active_io_leases != 0 || snapshot->invariant_count != 2 ||
	    snapshot->latest_invariant !=
		IS_REMOTE_CHUNK_INVARIANT_LEASE_DUPLICATE_RELEASE)
		failed = 1;
	is_remote_chunk_snapshot_release(snapshot);
	failed |= expect_read_admission(module, 96, 4096, 0);
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

struct concurrent_lease_lifetime {
	struct is_remote_chunk_module *module;
	struct is_remote_chunk_io_lease lease;
	struct test_gate *start;
	unsigned long long sector;
	int status;
};

static void *run_lease_lifetime(void *context)
{
	struct concurrent_lease_lifetime *event = context;
	struct is_remote_chunk_io_request request = {
		.direction = IS_REMOTE_CHUNK_IO_WRITE,
		.sector = event->sector,
		.bytes = 4096,
	};
	struct is_remote_chunk_transport_mapping mapping;
	enum is_remote_chunk_io_resolve_result result;

	test_gate_wait(event->start);
	event->status = is_remote_chunk_io_lease_acquire(event->module, &request,
		&event->lease, &mapping);
	if (!event->status)
		event->status = is_remote_chunk_io_lease_resolve(event->module,
			&event->lease, IS_REMOTE_CHUNK_IO_SUCCESS, &result);
	if (!event->status && result != IS_REMOTE_CHUNK_IO_RESOLVE_CURRENT)
		event->status = -EINVAL;
	if (!event->status)
		event->status = is_remote_chunk_io_lease_release(event->module,
			&event->lease);
	return NULL;
}

static int test_concurrent_independent_lease_lifetimes(void)
{
	enum { LIFETIME_COUNT = 16 };
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct concurrent_lease_lifetime events[LIFETIME_COUNT];
	struct is_remote_chunk_snapshot *snapshot = NULL;
	struct test_gate start;
	pthread_t threads[LIFETIME_COUNT];
	unsigned int created = 0;
	unsigned int index;
	int failed = 0;

	if (is_remote_chunk_module_create(&remote_only_config, &module) ||
	    is_remote_chunk_provider_handle_create(module, &provider) ||
	    map_remote_only_chunk(module, provider, 0, 20) ||
	    test_gate_init(&start))
		return 1;
	for (index = 0; index < LIFETIME_COUNT; index++) {
		events[index].module = module;
		events[index].lease = (struct is_remote_chunk_io_lease)
			IS_REMOTE_CHUNK_IO_LEASE_INIT;
		events[index].start = &start;
		events[index].sector = 128ULL + index * 8ULL;
		events[index].status = -999;
		if (pthread_create(&threads[index], NULL, run_lease_lifetime,
			&events[index])) {
			failed = 1;
			break;
		}
		created++;
	}
	test_gate_open(&start);
	for (index = 0; index < created; index++) {
		if (pthread_join(threads[index], NULL) || events[index].status)
			failed = 1;
	}
	test_gate_destroy(&start);
	if (is_remote_chunk_snapshot_take(module, &snapshot) ||
	    snapshot->active_io_leases != 0 || snapshot->invariant_count != 0)
		failed = 1;
	is_remote_chunk_snapshot_release(snapshot);
	for (index = 0; index < created; index++)
		failed |= expect_read_admission(module, events[index].sector,
			4096, 0);
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_io_admission_is_atomic_and_returns_mapping(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_io_lease read_lease = IS_REMOTE_CHUNK_IO_LEASE_INIT;
	struct is_remote_chunk_io_lease write_lease = IS_REMOTE_CHUNK_IO_LEASE_INIT;
	struct is_remote_chunk_transport_mapping mapping = { 0 };
	enum is_remote_chunk_io_resolve_result resolve_result;
	const struct is_remote_chunk_io_request first_read = {
		.direction = IS_REMOTE_CHUNK_IO_READ,
		.sector = 0,
		.bytes = 4096,
	};
	const struct is_remote_chunk_io_request write = {
		.direction = IS_REMOTE_CHUNK_IO_WRITE,
		.sector = 8,
		.bytes = 4096,
	};
	const struct is_remote_chunk_io_request valid_read = {
		.direction = IS_REMOTE_CHUNK_IO_READ,
		.sector = 8,
		.bytes = 4096,
	};
	const struct is_remote_chunk_io_request cross_chunk = {
		.direction = IS_REMOTE_CHUNK_IO_WRITE,
		.sector = IS_REMOTE_CHUNK_SECTORS_PER_CHUNK - 1,
		.bytes = 1024,
	};
	const struct is_remote_chunk_io_request unmapped = {
		.direction = IS_REMOTE_CHUNK_IO_WRITE,
		.sector = IS_REMOTE_CHUNK_SECTORS_PER_CHUNK,
		.bytes = 512,
	};
	int failed = 0;

	if (is_remote_chunk_module_create(&remote_only_config, &module) ||
	    is_remote_chunk_provider_handle_create(module, &provider) ||
	    map_remote_only_chunk(module, provider, 0, 20))
		return 1;
	if (is_remote_chunk_io_lease_acquire(module, &first_read, &read_lease,
		&mapping) != -ENODATA)
		failed = 1;
	if (is_remote_chunk_io_lease_acquire(module, &cross_chunk, &write_lease,
		&mapping) != -ERANGE)
		failed = 1;
	if (is_remote_chunk_io_lease_acquire(module, &unmapped, &write_lease,
		&mapping) != -ENXIO)
		failed = 1;
	if (is_remote_chunk_io_lease_acquire(module, &write, &write_lease,
		&mapping) ||
	    !is_remote_chunk_provider_handle_equal(mapping.provider, provider) ||
	    mapping.remote_address != 0x101000ULL || mapping.remote_key != 100U ||
	    mapping.provider_chunk != 20 || mapping.logical_chunk != 0)
		failed = 1;
	if (is_remote_chunk_io_lease_acquire(module, &write, &write_lease,
		&mapping) != -EBUSY)
		failed = 1;
	if (is_remote_chunk_io_lease_resolve(module, &write_lease,
		IS_REMOTE_CHUNK_IO_SUCCESS, &resolve_result) ||
	    resolve_result != IS_REMOTE_CHUNK_IO_RESOLVE_CURRENT ||
	    is_remote_chunk_io_lease_release(module, &write_lease))
		failed = 1;
	if (is_remote_chunk_io_lease_acquire(module, &write, &write_lease,
		&mapping) != -EALREADY)
		failed = 1;
	if (is_remote_chunk_io_lease_acquire(module, &valid_read, &read_lease,
		&mapping) ||
	    is_remote_chunk_io_lease_resolve(module, &read_lease,
		IS_REMOTE_CHUNK_IO_SUCCESS, &resolve_result) ||
	    resolve_result != IS_REMOTE_CHUNK_IO_RESOLVE_CURRENT ||
	    is_remote_chunk_io_lease_release(module, &read_lease))
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
		test_destroy_rejects_active_claim() |
		test_io_admission_is_atomic_and_returns_mapping() |
		test_write_outcomes_control_sector_validity() |
		test_illegal_lease_lifetimes_report_typed_invariants() |
		test_concurrent_lease_events_have_one_winner() |
		test_concurrent_independent_lease_lifetimes();

	if (failed)
		fprintf(stderr, "Remote Chunk public-interface tests failed\n");
	return failed;
}
