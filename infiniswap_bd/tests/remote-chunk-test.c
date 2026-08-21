// SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause
#include "../is_remote_chunk.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEST_THRESHOLD 8ULL
#define TEST_READ_WEIGHT 1U
#define TEST_WRITE_WEIGHT 4U

#ifdef IS_REMOTE_CHUNK_ALLOCATION_TEST
static bool track_allocations;
static unsigned int tracked_allocations;

extern void *__real_calloc(size_t count, size_t size);

void *__wrap_calloc(size_t count, size_t size)
{
	if (track_allocations)
		tracked_allocations++;
	return __real_calloc(count, size);
}
#endif

_Static_assert(sizeof(struct is_remote_chunk_io_lease) ==
	IS_REMOTE_CHUNK_IO_LEASE_BYTES, "lease storage size changed");
_Static_assert(_Alignof(struct is_remote_chunk_io_lease) >= _Alignof(void *),
	"lease storage is not pointer-aligned");

static const struct is_remote_chunk_provider_config one_provider[] = {
	{ .identifier = "provider-a", .placement_weight = 100 },
};

static const struct is_remote_chunk_provider_config two_providers[] = {
	{ .identifier = "provider-a", .placement_weight = 100 },
	{ .identifier = "provider-b", .placement_weight = 200 },
};

static const struct is_remote_chunk_provider_config three_providers[] = {
	{ .identifier = "provider-a", .placement_weight = 100 },
	{ .identifier = "provider-b", .placement_weight = 100 },
	{ .identifier = "provider-c", .placement_weight = 100 },
};

static const struct is_remote_chunk_config backed_config = {
	.mode = IS_REMOTE_CHUNK_MODE_BACKED,
	.chunk_count = 4,
	.hot_policy = {
		.threshold = TEST_THRESHOLD,
		.read_weight = TEST_READ_WEIGHT,
		.write_weight = TEST_WRITE_WEIGHT,
	},
	.providers = one_provider,
	.provider_count = 1,
	.placement_sample_size = 1,
	.placement_seed = 7,
};

static const struct is_remote_chunk_config backed_two_provider_config = {
	.mode = IS_REMOTE_CHUNK_MODE_BACKED,
	.chunk_count = 4,
	.hot_policy = {
		.threshold = TEST_THRESHOLD,
		.read_weight = TEST_READ_WEIGHT,
		.write_weight = TEST_WRITE_WEIGHT,
	},
	.providers = two_providers,
	.provider_count = 2,
	.placement_sample_size = 2,
	.placement_seed = 7,
};

static const struct is_remote_chunk_config remote_only_config = {
	.mode = IS_REMOTE_CHUNK_MODE_REMOTE_ONLY,
	.chunk_count = 4,
	.hot_policy = {
		.threshold = TEST_THRESHOLD,
		.read_weight = TEST_READ_WEIGHT,
		.write_weight = TEST_WRITE_WEIGHT,
	},
	.providers = one_provider,
	.provider_count = 1,
	.placement_sample_size = 1,
	.placement_seed = 7,
};

static const struct is_remote_chunk_config remote_only_two_provider_config = {
	.mode = IS_REMOTE_CHUNK_MODE_REMOTE_ONLY,
	.chunk_count = 4,
	.hot_policy = {
		.threshold = TEST_THRESHOLD,
		.read_weight = TEST_READ_WEIGHT,
		.write_weight = TEST_WRITE_WEIGHT,
	},
	.providers = two_providers,
	.provider_count = 2,
	.placement_sample_size = 2,
	.placement_seed = 7,
};

static const struct is_remote_chunk_config backed_empty_config = {
	.mode = IS_REMOTE_CHUNK_MODE_BACKED,
	.chunk_count = 4,
	.hot_policy = {
		.threshold = TEST_THRESHOLD,
		.read_weight = TEST_READ_WEIGHT,
		.write_weight = TEST_WRITE_WEIGHT,
	},
	.placement_sample_size = 2,
	.placement_seed = 7,
};

static int create_single_provider_module(
	const struct is_remote_chunk_config *config,
	struct is_remote_chunk_module **module_out,
	struct is_remote_chunk_provider_handle *provider_out)
{
	return is_remote_chunk_module_create(config, provider_out, 1, module_out);
}

static int create_two_provider_module(
	const struct is_remote_chunk_config *config,
	struct is_remote_chunk_module **module_out,
	struct is_remote_chunk_provider_handle *first_out,
	struct is_remote_chunk_provider_handle *second_out)
{
	struct is_remote_chunk_provider_handle providers[2];
	int status = is_remote_chunk_module_create(config, providers, 2,
		module_out);

	if (!status) {
		*first_out = providers[0];
		*second_out = providers[1];
	}
	return status;
}

struct test_gate {
	pthread_mutex_t lock;
	pthread_cond_t changed;
	bool open;
};

struct concurrent_observation {
	struct is_remote_chunk_module *module;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_provider_observation observation;
	struct test_gate *start;
	enum is_remote_chunk_provider_observation_result result;
};

struct concurrent_next_mapping {
	struct is_remote_chunk_module *module;
	struct is_remote_chunk_mapping_request request;
	struct test_gate *start;
	enum is_remote_chunk_next_mapping_result result;
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

static unsigned long long monotonic_deadline_after_ms(unsigned int timeout_ms)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return 0;
	return (unsigned long long)now.tv_sec * 1000000000ULL +
		(unsigned long long)now.tv_nsec +
		(unsigned long long)timeout_ms * 1000000ULL;
}

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

static int commit_mapping_request(struct is_remote_chunk_module *module,
	const struct is_remote_chunk_mapping_request *request,
	unsigned int provider_chunk)
{
	struct is_remote_chunk_mapping_grant grant =
		mapping_grant(request->logical_chunk, provider_chunk);

	return is_remote_chunk_mapping_commit(module, &request->claim,
		request->provider, &grant, 1);
}

static int observe_provider_available(
	struct is_remote_chunk_module *module,
	struct is_remote_chunk_provider_handle provider,
	unsigned int available_chunks)
{
	struct is_remote_chunk_snapshot *snapshot = NULL;
	struct is_remote_chunk_provider_observation observation = {
		.available_chunks = available_chunks,
		.exclusion = available_chunks ?
			IS_REMOTE_CHUNK_PROVIDER_EXCLUDE_NONE :
			IS_REMOTE_CHUNK_PROVIDER_EXCLUDE_ZERO_CAPACITY,
		.placement_eligible = available_chunks > 0,
	};
	unsigned int index;
	int status;

	status = is_remote_chunk_snapshot_take(module, &snapshot);
	if (status)
		return status;
	for (index = 0; index < snapshot->provider_count; index++) {
		if (!is_remote_chunk_provider_handle_equal(
			snapshot->providers[index].provider, provider))
			continue;
		observation.sequence = snapshot->providers[index].has_observation ?
			snapshot->providers[index].observation_sequence + 1ULL : 1ULL;
		break;
	}
	is_remote_chunk_snapshot_release(snapshot);
	if (!observation.sequence)
		return -ESTALE;
	return is_remote_chunk_provider_observe(module, provider, &observation) ==
		IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_APPLIED ? 0 : -EINVAL;
}

static int begin_backed_mapping(
	struct is_remote_chunk_module *module,
	struct is_remote_chunk_provider_handle expected_provider,
	struct is_remote_chunk_mapping_claim *claim_out,
	unsigned int *logical_chunk_out)
{
	struct is_remote_chunk_mapping_request request = { 0 };

	if (observe_provider_available(module, expected_provider,
		IS_REMOTE_CHUNK_MAX_CHUNKS))
		return -EINVAL;
	if (is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST)
		return -ENOENT;
	if (!is_remote_chunk_provider_handle_equal(request.provider,
		expected_provider)) {
		(void)is_remote_chunk_mapping_abort(module, &request.claim);
		return -ESTALE;
	}
	*claim_out = request.claim;
	*logical_chunk_out = request.logical_chunk;
	return 0;
}

static int make_chunk_hot(struct is_remote_chunk_module *module,
	unsigned int logical_chunk)
{
	bool mapping_needed = false;
	int status;

	status = is_remote_chunk_note_activity(module, logical_chunk, 1,
		IS_REMOTE_CHUNK_ACTIVITY_WRITE, &mapping_needed);
	if (!status)
		status = is_remote_chunk_note_activity(module, logical_chunk, 1,
			IS_REMOTE_CHUNK_ACTIVITY_WRITE, &mapping_needed);
	return status ? status : (mapping_needed ? 0 : -EINVAL);
}

static int map_backed_chunk(struct is_remote_chunk_module *module,
	struct is_remote_chunk_provider_handle provider,
	unsigned int logical_chunk, unsigned int provider_chunk)
{
	struct is_remote_chunk_mapping_request request = { 0 };
	struct is_remote_chunk_mapping_grant grant =
		mapping_grant(logical_chunk, provider_chunk);
	int status;

	status = make_chunk_hot(module, logical_chunk);
	if (!status)
		status = begin_backed_mapping(module, provider, &request.claim,
			&request.logical_chunk);
	if (!status && request.logical_chunk != logical_chunk)
		status = -EINVAL;
	if (!status)
		request.provider = provider;
	if (!status)
		status = is_remote_chunk_mapping_commit(module, &request.claim,
			request.provider, &grant, 1);
	if (status)
		(void)is_remote_chunk_mapping_abort(module, &request.claim);
	return status;
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

static int test_module_creation_copies_complete_provider_roster(void)
{
	struct is_remote_chunk_provider_config providers[] = {
		{ .identifier = "provider-a", .placement_weight = 100 },
		{ .identifier = "provider-b", .placement_weight = 200 },
	};
	struct is_remote_chunk_config config = backed_two_provider_config;
	struct is_remote_chunk_provider_handle handles[2];
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_snapshot *snapshot = NULL;
	int failed = 0;

	config.providers = providers;
	if (is_remote_chunk_module_create(&config, handles, 2, &module))
		return 1;
	strcpy(providers[0].identifier, "changed");
	providers[0].placement_weight = 999;
	config.placement_sample_size = 1;
	config.placement_seed = 99;
	if (is_remote_chunk_snapshot_take(module, &snapshot) ||
	    snapshot->mode != IS_REMOTE_CHUNK_MODE_BACKED ||
	    snapshot->chunk_count != 4 ||
	    snapshot->hot_policy.threshold != TEST_THRESHOLD ||
	    snapshot->hot_policy.read_weight != TEST_READ_WEIGHT ||
	    snapshot->hot_policy.write_weight != TEST_WRITE_WEIGHT ||
	    snapshot->provider_count != 2 ||
	    strcmp(snapshot->providers[0].identifier, "provider-a") ||
	    snapshot->providers[0].placement_weight != 100 ||
	    strcmp(snapshot->providers[1].identifier, "provider-b") ||
	    snapshot->providers[1].placement_weight != 200 ||
	    snapshot->placement_sample_size != 2 ||
	    snapshot->placement_seed != 7 ||
	    !is_remote_chunk_provider_handle_equal(
		snapshot->providers[0].provider, handles[0]) ||
	    !is_remote_chunk_provider_handle_equal(
		snapshot->providers[1].provider, handles[1]) ||
	    is_remote_chunk_provider_handle_equal(handles[0], handles[1]))
		failed = 1;
	is_remote_chunk_snapshot_release(snapshot);
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_module_creation_validates_roster_atomically(void)
{
	struct is_remote_chunk_provider_config providers[2] = {
		{ .identifier = "provider-a", .placement_weight = 100 },
		{ .identifier = "provider-a", .placement_weight = 200 },
	};
	struct is_remote_chunk_config config = remote_only_two_provider_config;
	struct is_remote_chunk_provider_handle handles[2] = {
		{ .opaque = { 11, 12 } },
		{ .opaque = { 21, 22 } },
	};
	struct is_remote_chunk_module *module = NULL;
	int failed = 0;

	config.providers = providers;
	if (is_remote_chunk_module_create(&config, handles, 2, &module) !=
		-EINVAL || module || handles[0].opaque[0] != 11 ||
	    handles[1].opaque[1] != 22)
		failed = 1;
	providers[1].identifier[0] = '\0';
	if (is_remote_chunk_module_create(&config, handles, 2, &module) !=
		-EINVAL || module)
		failed = 1;
	strcpy(providers[1].identifier, "provider-b");
	providers[1].placement_weight = 0;
	if (is_remote_chunk_module_create(&config, handles, 2, &module) !=
		-ERANGE || module)
		failed = 1;
	providers[1].placement_weight = 200;
	config.placement_sample_size = 3;
	if (is_remote_chunk_module_create(&config, handles, 2, &module) !=
		-ERANGE || module)
		failed = 1;
	config.placement_sample_size = 2;
	config.providers = NULL;
	if (is_remote_chunk_module_create(&config, handles, 2, &module) !=
		-EINVAL || module)
		failed = 1;
	config.providers = providers;
	if (is_remote_chunk_module_create(&config, handles, 1, &module) !=
		-ENOSPC || module || handles[0].opaque[0] != 11 ||
	    handles[1].opaque[1] != 22)
		failed = 1;
	config.provider_count = IS_REMOTE_CHUNK_MAX_PROVIDERS + 1U;
	if (is_remote_chunk_module_create(&config, handles, 2, &module) !=
		-EINVAL || module)
		failed = 1;
	config.provider_count = 2;
	memset(providers[1].identifier, 'a',
		sizeof(providers[1].identifier));
	if (is_remote_chunk_module_create(&config, handles, 2, &module) !=
		-EINVAL || module)
		failed = 1;
	strcpy(providers[1].identifier, "provider-b");
	config.placement_sample_size = 0;
	if (is_remote_chunk_module_create(&config, handles, 2, &module) !=
		-ERANGE || module)
		failed = 1;
	config.placement_sample_size = 2;
	providers[1].placement_weight =
		IS_REMOTE_CHUNK_PLACEMENT_WEIGHT_MAX + 1U;
	if (is_remote_chunk_module_create(&config, handles, 2, &module) !=
		-ERANGE || module)
		failed = 1;
	providers[1].placement_weight = 200;
	config.mode = (enum is_remote_chunk_mode)0;
	if (is_remote_chunk_module_create(&config, handles, 2, &module) !=
		-EINVAL || module)
		failed = 1;
	config.mode = IS_REMOTE_CHUNK_MODE_REMOTE_ONLY;
	config.chunk_count = 0;
	if (is_remote_chunk_module_create(&config, handles, 2, &module) !=
		-EINVAL || module)
		failed = 1;
	config.chunk_count = 4;
	config.hot_policy.threshold = 0;
	if (is_remote_chunk_module_create(&config, handles, 2, &module) !=
		-ERANGE || module)
		failed = 1;
	return failed;
}

static int test_empty_provider_roster_is_backed_only(void)
{
	struct is_remote_chunk_config remote_only = backed_empty_config;
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_snapshot *snapshot = NULL;
	struct is_remote_chunk_mapping_request request = { 0 };
	bool mapping_needed = false;
	int failed = 0;

	remote_only.mode = IS_REMOTE_CHUNK_MODE_REMOTE_ONLY;
	if (is_remote_chunk_module_create(&remote_only, NULL, 0, &module) !=
		-EINVAL || module)
		failed = 1;
	if (is_remote_chunk_module_create(&backed_empty_config, NULL, 0,
		&module))
		return 1;
	if (is_remote_chunk_note_activity(module, 0, 1,
		IS_REMOTE_CHUNK_ACTIVITY_WRITE, &mapping_needed) ||
	    is_remote_chunk_note_activity(module, 0, 1,
		IS_REMOTE_CHUNK_ACTIVITY_WRITE, &mapping_needed) ||
	    !mapping_needed ||
	    is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_NO_ELIGIBLE_CAPACITY ||
	    is_remote_chunk_snapshot_take(module, &snapshot) ||
	    snapshot->provider_count != 0 ||
	    snapshot->placement_sample_size != 2 ||
	    snapshot->placement_seed != 7)
		failed = 1;
	is_remote_chunk_snapshot_release(snapshot);
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_provider_observations_are_ordered_and_diagnostic(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_provider_observation current = {
		.sequence = 10,
		.available_chunks = 7,
		.exclusion = IS_REMOTE_CHUNK_PROVIDER_EXCLUDE_NONE,
		.placement_eligible = true,
	};
	struct is_remote_chunk_provider_observation invalid = current;
	struct is_remote_chunk_provider_observation stale = current;
	struct is_remote_chunk_provider_observation conflict = current;
	struct is_remote_chunk_snapshot *snapshot = NULL;
	int failed = 0;

	if (create_single_provider_module(&backed_config, &module, &provider) ||
	    map_backed_chunk(module, provider, 0, 20))
		return 1;
	invalid.placement_eligible = false;
	if (is_remote_chunk_provider_observe(module, provider, &invalid) !=
		IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_INVALID)
		failed = 1;
	if (is_remote_chunk_provider_observe(module, provider, &current) !=
		IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_APPLIED)
		failed = 1;
	stale.sequence = 9;
	stale.available_chunks = 99;
	if (is_remote_chunk_provider_observe(module, provider, &stale) !=
		IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_STALE ||
	    is_remote_chunk_provider_observe(module, provider, &current) !=
		IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_DUPLICATE)
		failed = 1;
	conflict.available_chunks = 6;
	if (is_remote_chunk_provider_observe(module, provider, &conflict) !=
		IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_CONFLICT)
		failed = 1;
	if (is_remote_chunk_snapshot_take(module, &snapshot) ||
	    snapshot->provider_count != 1 ||
	    strcmp(snapshot->providers[0].identifier, "provider-a") ||
	    !snapshot->providers[0].has_observation ||
	    snapshot->providers[0].observation_sequence != 10 ||
	    snapshot->providers[0].reported_available_chunks != 7 ||
	    snapshot->providers[0].observation_assigned_chunks != 1 ||
	    snapshot->providers[0].assigned_chunks != 1 ||
	    !snapshot->providers[0].placement_eligible ||
	    snapshot->providers[0].exclusion !=
		IS_REMOTE_CHUNK_PROVIDER_EXCLUDE_NONE)
		failed = 1;
	is_remote_chunk_snapshot_release(snapshot);
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_next_mapping_reports_idle_capacity_and_active_claim(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_mapping_request request = { 0 };
	struct is_remote_chunk_mapping_request blocked = { 0 };
	struct is_remote_chunk_provider_observation unavailable = {
		.sequence = 1,
		.available_chunks = 0,
		.exclusion = IS_REMOTE_CHUNK_PROVIDER_EXCLUDE_ZERO_CAPACITY,
		.placement_eligible = false,
	};
	struct is_remote_chunk_provider_observation available = {
		.sequence = 2,
		.available_chunks = 2,
		.exclusion = IS_REMOTE_CHUNK_PROVIDER_EXCLUDE_NONE,
		.placement_eligible = true,
	};
	bool mapping_needed = false;
	int failed = 0;

	if (is_remote_chunk_next_mapping(NULL, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_INVALID_INPUT)
		failed = 1;
	if (create_single_provider_module(&backed_config, &module, &provider))
		return 1;
	if (is_remote_chunk_next_mapping(module, NULL) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_INVALID_INPUT)
		failed = 1;
	if (is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_NO_HOT_RANGE ||
	    is_remote_chunk_note_activity(module, 2, 1,
		IS_REMOTE_CHUNK_ACTIVITY_WRITE, &mapping_needed) ||
	    is_remote_chunk_note_activity(module, 2, 1,
		IS_REMOTE_CHUNK_ACTIVITY_WRITE, &mapping_needed) ||
	    !mapping_needed ||
	    is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_PENDING_PROVIDER_FACTS ||
	    is_remote_chunk_provider_observe(module, provider, &unavailable) !=
		IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_APPLIED ||
	    is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_NO_ELIGIBLE_CAPACITY ||
	    is_remote_chunk_provider_observe(module, provider, &available) !=
		IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_APPLIED)
		failed = 1;
	if (is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST ||
	    !is_remote_chunk_provider_handle_equal(request.provider, provider) ||
	    request.pool != IS_REMOTE_CHUNK_MAPPING_POOL_OPPORTUNISTIC ||
	    request.logical_chunk != 2 ||
	    is_remote_chunk_next_mapping(module, &blocked) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_CLAIM_ACTIVE ||
	    is_remote_chunk_mapping_abort(module, &request.claim) ||
	    is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST ||
	    request.logical_chunk != 2 ||
	    is_remote_chunk_mapping_abort(module, &request.claim))
		failed = 1;
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_deterministic_placement_prefers_sampled_capacity(void)
{
	const struct is_remote_chunk_config config = {
		.mode = IS_REMOTE_CHUNK_MODE_BACKED,
		.chunk_count = 1,
		.hot_policy = {
			.threshold = TEST_THRESHOLD,
			.read_weight = TEST_READ_WEIGHT,
			.write_weight = TEST_WRITE_WEIGHT,
		},
		.providers = three_providers,
		.provider_count = 3,
		.placement_sample_size = 2,
		.placement_seed = 7,
	};
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle providers[3];
	struct is_remote_chunk_mapping_request request = { 0 };
	int failed = 0;

	if (is_remote_chunk_module_create(&config, providers, 3, &module) ||
	    observe_provider_available(module, providers[0], 5) ||
	    observe_provider_available(module, providers[1], 5) ||
	    observe_provider_available(module, providers[2], 9) ||
	    make_chunk_hot(module, 0))
		return 1;
	if (is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST ||
	    !is_remote_chunk_provider_handle_equal(request.provider,
		providers[2]) || request.logical_chunk != 0 ||
	    is_remote_chunk_mapping_abort(module, &request.claim))
		failed = 1;
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_weights_bias_sampling_with_d_equals_one(void)
{
	struct is_remote_chunk_provider_config providers[] = {
		{ .identifier = "provider-a", .placement_weight = 900 },
		{ .identifier = "provider-b", .placement_weight = 100 },
	};
	unsigned int first_wins = 0;
	unsigned int second_wins = 0;
	unsigned int trial;

	for (trial = 0; trial < 200; trial++) {
		struct is_remote_chunk_config config = backed_two_provider_config;
		struct is_remote_chunk_module *module = NULL;
		struct is_remote_chunk_provider_handle handles[2];
		struct is_remote_chunk_mapping_request request = { 0 };

		config.providers = providers;
		config.placement_sample_size = 1;
		config.placement_seed = 1000ULL + trial;
		if (is_remote_chunk_module_create(&config, handles, 2, &module) ||
		    observe_provider_available(module, handles[0], 4) ||
		    observe_provider_available(module, handles[1], 4) ||
		    make_chunk_hot(module, 0) ||
		    is_remote_chunk_next_mapping(module, &request) !=
			IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST)
			return 1;
		if (is_remote_chunk_provider_handle_equal(request.provider,
			handles[0]))
			first_wins++;
		else if (is_remote_chunk_provider_handle_equal(request.provider,
			handles[1]))
			second_wins++;
		else
			return 1;
		if (is_remote_chunk_mapping_abort(module, &request.claim) ||
		    is_remote_chunk_module_destroy(module))
			return 1;
	}
	if (first_wins < 150 || !second_wins) {
		fprintf(stderr, "weight bias unexpected: first=%u second=%u\n",
			first_wins, second_wins);
		return 1;
	}
	return 0;
}

static int test_exclusions_clamping_and_stable_ties(void)
{
	const struct is_remote_chunk_config config = {
		.mode = IS_REMOTE_CHUNK_MODE_BACKED,
		.chunk_count = 1,
		.hot_policy = {
			.threshold = TEST_THRESHOLD,
			.read_weight = TEST_READ_WEIGHT,
			.write_weight = TEST_WRITE_WEIGHT,
		},
		.providers = three_providers,
		.provider_count = 3,
		.placement_sample_size = 3,
		.placement_seed = 7,
	};
	struct is_remote_chunk_provider_observation excluded = {
		.sequence = 1,
		.available_chunks = 10,
		.exclusion = IS_REMOTE_CHUNK_PROVIDER_EXCLUDE_UNHEALTHY,
		.placement_eligible = false,
	};
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle providers[3];
	struct is_remote_chunk_mapping_request request = { 0 };
	int failed = 0;

	if (is_remote_chunk_module_create(&config, providers, 3, &module) ||
	    is_remote_chunk_provider_observe(module, providers[0], &excluded) !=
		IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_APPLIED ||
	    observe_provider_available(module, providers[1], 4) ||
	    observe_provider_available(module, providers[2], 4) ||
	    make_chunk_hot(module, 0))
		return 1;
	if (is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST ||
	    !is_remote_chunk_provider_handle_equal(request.provider,
		providers[1]) ||
	    is_remote_chunk_mapping_abort(module, &request.claim))
		failed = 1;
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_effective_capacity_tracks_assignments_and_abort(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle first;
	struct is_remote_chunk_provider_handle second;
	struct is_remote_chunk_mapping_request request = { 0 };
	struct is_remote_chunk_mapping_grant grant;
	int failed = 0;

	if (create_two_provider_module(&backed_two_provider_config, &module,
		&first, &second) || observe_provider_available(module, first, 2) ||
	    observe_provider_available(module, second, 2) ||
	    make_chunk_hot(module, 0) || make_chunk_hot(module, 1) ||
	    make_chunk_hot(module, 2))
		return 1;
	if (is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST ||
	    !is_remote_chunk_provider_handle_equal(request.provider, first))
		return 1;
	grant = mapping_grant(request.logical_chunk, 80);
	if (is_remote_chunk_mapping_commit(module, &request.claim,
		request.provider, &grant, 1))
		return 1;
	if (is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST ||
	    !is_remote_chunk_provider_handle_equal(request.provider, second) ||
	    is_remote_chunk_mapping_abort(module, &request.claim))
		failed = 1;
	if (is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST ||
	    !is_remote_chunk_provider_handle_equal(request.provider, second))
		failed = 1;
	grant = mapping_grant(request.logical_chunk, 81);
	if (is_remote_chunk_mapping_commit(module, &request.claim,
		request.provider, &grant, 1))
		failed = 1;
	if (is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST ||
	    !is_remote_chunk_provider_handle_equal(request.provider, first) ||
	    is_remote_chunk_mapping_abort(module, &request.claim))
		failed = 1;
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_observation_during_claim_tracks_reserved_baseline(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_mapping_request request = { 0 };
	struct is_remote_chunk_provider_observation observed_claim = {
		.sequence = 2,
		.available_chunks = 1,
		.exclusion = IS_REMOTE_CHUNK_PROVIDER_EXCLUDE_NONE,
		.placement_eligible = true,
	};
	struct is_remote_chunk_mapping_grant grant;
	int failed = 0;

	if (create_single_provider_module(&backed_config, &module, &provider) ||
	    observe_provider_available(module, provider, 2) ||
	    make_chunk_hot(module, 0) || make_chunk_hot(module, 1) ||
	    make_chunk_hot(module, 2) ||
	    is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST)
		return 1;
	if (is_remote_chunk_provider_observe(module, provider, &observed_claim) !=
		IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_APPLIED ||
	    is_remote_chunk_mapping_abort(module, &request.claim) ||
	    is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST)
		failed = 1;
	grant = mapping_grant(request.logical_chunk, 91);
	if (!failed && is_remote_chunk_mapping_commit(module, &request.claim,
		request.provider, &grant, 1))
		failed = 1;
	if (!failed && is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST)
		failed = 1;
	grant = mapping_grant(request.logical_chunk, 92);
	if (!failed && is_remote_chunk_mapping_commit(module, &request.claim,
		request.provider, &grant, 1))
		failed = 1;
	if (!failed && is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_NO_ELIGIBLE_CAPACITY)
		failed = 1;
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_remote_only_waits_for_complete_provider_facts(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle first;
	struct is_remote_chunk_provider_handle second;
	struct is_remote_chunk_mapping_request request = { 0 };
	struct is_remote_chunk_provider_failure_facts terminal_facts;
	int failed = 0;

	if (create_two_provider_module(&remote_only_two_provider_config, &module,
		&first, &second) || observe_provider_available(module, first, 4))
		return 1;
	if (is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_PENDING_PROVIDER_FACTS ||
	    is_remote_chunk_provider_failed(module, second, &terminal_facts) ||
	    terminal_facts.affected_chunks ||
	    is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST ||
	    request.pool != IS_REMOTE_CHUNK_MAPPING_POOL_COMMITTED ||
	    request.logical_chunk != 0 ||
	    !is_remote_chunk_provider_handle_equal(request.provider, first) ||
	    is_remote_chunk_mapping_abort(module, &request.claim))
		failed = 1;
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_remote_only_rejects_insufficient_capacity_before_claim(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle first;
	struct is_remote_chunk_provider_handle second;
	struct is_remote_chunk_mapping_request request = { 0 };
	int failed = 0;

	if (create_two_provider_module(&remote_only_two_provider_config, &module,
		&first, &second) || observe_provider_available(module, first, 1) ||
	    observe_provider_available(module, second, 2))
		return 1;
	if (is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_INSUFFICIENT_CAPACITY)
		failed = 1;
	failed |= expect_snapshot(module, 0, 0, 0, 0, 0,
		"insufficient Committed Pool capacity");
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_remote_only_progresses_in_order_after_abort(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_mapping_request request = { 0 };
	struct is_remote_chunk_mapping_request blocked = { 0 };
	unsigned int logical_chunk;
	int failed = 0;

	if (create_single_provider_module(&remote_only_config, &module, &provider) ||
	    observe_provider_available(module, provider, 4) ||
	    is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST)
		return 1;
	if (request.pool != IS_REMOTE_CHUNK_MAPPING_POOL_COMMITTED ||
	    request.logical_chunk != 0 ||
	    is_remote_chunk_next_mapping(module, &blocked) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_CLAIM_ACTIVE ||
	    is_remote_chunk_mapping_abort(module, &request.claim) ||
	    is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST ||
	    request.logical_chunk != 0)
		failed = 1;
	if (!failed && commit_mapping_request(module, &request, 20))
		failed = 1;
	for (logical_chunk = 1; !failed && logical_chunk < 4; logical_chunk++) {
		request = (struct is_remote_chunk_mapping_request) { 0 };
		if (is_remote_chunk_next_mapping(module, &request) !=
			IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST ||
		    request.pool != IS_REMOTE_CHUNK_MAPPING_POOL_COMMITTED ||
		    request.logical_chunk != logical_chunk ||
		    commit_mapping_request(module, &request, 20 + logical_chunk))
			failed = 1;
	}
	if (!failed && is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_COMPLETE)
		failed = 1;
	failed |= expect_snapshot(module, 4, 4, 0, 0, 4,
		"complete Remote-Only commitment");
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_remote_only_reports_no_eligible_provider_after_progress(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_mapping_request request = { 0 };
	int failed = 0;

	if (create_single_provider_module(&remote_only_config, &module, &provider) ||
	    observe_provider_available(module, provider, 4) ||
	    is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST ||
	    commit_mapping_request(module, &request, 20) ||
	    observe_provider_available(module, provider, 0))
		return 1;
	if (is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_NO_ELIGIBLE_CAPACITY)
		failed = 1;
	failed |= expect_snapshot(module, 1, 1, 0, 0, 1,
		"unavailable Committed Pool Provider");
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_remote_only_uses_deterministic_multi_provider_selection(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle first;
	struct is_remote_chunk_provider_handle second;
	struct is_remote_chunk_mapping_request request = { 0 };
	int failed = 0;

	if (create_two_provider_module(&remote_only_two_provider_config, &module,
		&first, &second) || observe_provider_available(module, first, 2) ||
	    observe_provider_available(module, second, 4))
		return 1;
	if (is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST ||
	    !is_remote_chunk_provider_handle_equal(request.provider, second) ||
	    request.logical_chunk != 0 ||
	    request.pool != IS_REMOTE_CHUNK_MAPPING_POOL_COMMITTED ||
	    is_remote_chunk_mapping_abort(module, &request.claim))
		failed = 1;
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

#ifdef IS_REMOTE_CHUNK_ALLOCATION_TEST
static int test_placement_lifecycle_does_not_allocate(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_module *remote_module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_provider_handle remote_provider;
	struct is_remote_chunk_provider_observation observation = {
		.sequence = 1,
		.available_chunks = 2,
		.exclusion = IS_REMOTE_CHUNK_PROVIDER_EXCLUDE_NONE,
		.placement_eligible = true,
	};
	struct is_remote_chunk_provider_observation remote_observation = {
		.sequence = 1,
		.available_chunks = 4,
		.exclusion = IS_REMOTE_CHUNK_PROVIDER_EXCLUDE_NONE,
		.placement_eligible = true,
	};
	struct is_remote_chunk_mapping_request request = { 0 };
	struct is_remote_chunk_mapping_request remote_request = { 0 };
	struct is_remote_chunk_mapping_grant grant;
	int failed = 0;

	if (create_single_provider_module(&backed_config, &module, &provider) ||
	    create_single_provider_module(&remote_only_config, &remote_module,
		&remote_provider) || make_chunk_hot(module, 0))
		return 1;
	tracked_allocations = 0;
	track_allocations = true;
	if (is_remote_chunk_provider_observe(module, provider, &observation) !=
		IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_APPLIED ||
	    is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST ||
	    is_remote_chunk_mapping_abort(module, &request.claim) ||
	    is_remote_chunk_next_mapping(module, &request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST ||
	    is_remote_chunk_provider_observe(remote_module, remote_provider,
		&remote_observation) !=
		IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_APPLIED ||
	    is_remote_chunk_next_mapping(remote_module, &remote_request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST ||
	    is_remote_chunk_mapping_abort(remote_module, &remote_request.claim) ||
	    is_remote_chunk_next_mapping(remote_module, &remote_request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST)
		failed = 1;
	grant = mapping_grant(request.logical_chunk, 90);
	if (!failed && is_remote_chunk_mapping_commit(module, &request.claim,
		request.provider, &grant, 1))
		failed = 1;
	grant = mapping_grant(remote_request.logical_chunk, 91);
	if (!failed && is_remote_chunk_mapping_commit(remote_module,
		&remote_request.claim, remote_request.provider, &grant, 1))
		failed = 1;
	track_allocations = false;
	if (tracked_allocations)
		failed = 1;
	failed |= is_remote_chunk_module_destroy(module) != 0;
	failed |= is_remote_chunk_module_destroy(remote_module) != 0;
	return failed;
}
#else
static int test_placement_lifecycle_does_not_allocate(void)
{
	return 0;
}
#endif

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

	if (create_single_provider_module(&remote_only_config, &module, &provider) ||
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

static int test_mapping_claim_is_module_wide(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_mapping_claim first =
		IS_REMOTE_CHUNK_MAPPING_CLAIM_INIT;
	struct is_remote_chunk_mapping_claim second =
		IS_REMOTE_CHUNK_MAPPING_CLAIM_INIT;
	const unsigned int first_chunk = 0;
	const unsigned int second_chunk = 1;
	int failed = 0;

	if (create_single_provider_module(&remote_only_config, &module, &provider) ||
	    is_remote_chunk_mapping_begin_explicit(module, provider, &first_chunk,
		1, &first))
		return 1;
	if (is_remote_chunk_mapping_begin_explicit(module, provider, &second_chunk,
		1, &second) != -EBUSY ||
	    is_remote_chunk_mapping_abort(module, &first))
		failed = 1;
	failed |= expect_snapshot(module, 0, 0, 0, 0, 0,
		"module-wide mapping claim");
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

	if (create_single_provider_module(&remote_only_config, &module, &provider) ||
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

	if (create_two_provider_module(&remote_only_two_provider_config, &module,
		&first_provider, &second_provider) ||
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

	if (create_single_provider_module(&remote_only_config, &module, &provider) ||
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
	struct is_remote_chunk_provider_handle first_copy;
	struct is_remote_chunk_provider_handle next_epoch;
	struct is_remote_chunk_provider_handle other_module;
	struct is_remote_chunk_provider_observation observation = {
		.sequence = 1,
		.available_chunks = 1,
		.exclusion = IS_REMOTE_CHUNK_PROVIDER_EXCLUDE_NONE,
		.placement_eligible = true,
	};
	struct is_remote_chunk_mapping_claim claim =
		IS_REMOTE_CHUNK_MAPPING_CLAIM_INIT;
	const unsigned int logical_chunk = 0;
	int failed = 0;

	if (create_two_provider_module(&remote_only_two_provider_config,
		&first_module, &first, &next_epoch) ||
	    create_single_provider_module(&remote_only_config, &second_module,
		&other_module))
		return 1;
	first_copy = first;
	if (!is_remote_chunk_provider_handle_equal(first, first_copy) ||
	    is_remote_chunk_provider_handle_equal(first, next_epoch) ||
	    is_remote_chunk_provider_handle_equal(first, other_module))
		failed = 1;
	if (is_remote_chunk_provider_observe(first_module, other_module,
		&observation) != IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_STALE_EPOCH)
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

	if (create_single_provider_module(&remote_only_config, &module, &provider))
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
	struct is_remote_chunk_hot_policy policy_snapshot = { 0 };
	unsigned int logical_chunk = 99;
	bool mapping_needed = true;
	int failed = 0;

	if (create_single_provider_module(&backed_config, &module, &provider))
		return 1;
	if (is_remote_chunk_hot_policy_set(module, &policy, &mapping_needed) ||
	    mapping_needed ||
	    is_remote_chunk_hot_policy_snapshot(module, &policy_snapshot) ||
	    policy_snapshot.threshold != policy.threshold ||
	    policy_snapshot.read_weight != policy.read_weight ||
	    policy_snapshot.write_weight != policy.write_weight)
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
	if (begin_backed_mapping(module, provider, &claim, &logical_chunk) ||
	    logical_chunk != 2)
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
	if (begin_backed_mapping(module, provider, &claim, &logical_chunk) ||
	    logical_chunk != 1 || is_remote_chunk_mapping_abort(module, &claim))
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

static void *run_next_mapping(void *context)
{
	struct concurrent_next_mapping *event = context;

	test_gate_wait(event->start);
	event->result = is_remote_chunk_next_mapping(event->module,
		&event->request);
	return NULL;
}

static int test_concurrent_next_mapping_calls_have_one_winner(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct concurrent_next_mapping events[2] = { 0 };
	struct test_gate start;
	pthread_t threads[2];
	unsigned int index;
	unsigned int requested = 0;
	unsigned int active = 0;
	int failed = 0;

	if (create_single_provider_module(&backed_config, &module, &provider) ||
	    observe_provider_available(module, provider, 1) ||
	    make_chunk_hot(module, 0) || test_gate_init(&start))
		return 1;
	for (index = 0; index < 2; index++) {
		events[index].module = module;
		events[index].start = &start;
		events[index].result = IS_REMOTE_CHUNK_NEXT_MAPPING_INVALID_INPUT;
		if (pthread_create(&threads[index], NULL, run_next_mapping,
			&events[index]))
			return 1;
	}
	test_gate_open(&start);
	for (index = 0; index < 2; index++) {
		if (pthread_join(threads[index], NULL))
			failed = 1;
	}
	for (index = 0; index < 2; index++) {
		requested += events[index].result ==
			IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST;
		active += events[index].result ==
			IS_REMOTE_CHUNK_NEXT_MAPPING_CLAIM_ACTIVE;
		if (events[index].result == IS_REMOTE_CHUNK_NEXT_MAPPING_REQUEST &&
		    is_remote_chunk_mapping_abort(module, &events[index].request.claim))
			failed = 1;
	}
	test_gate_destroy(&start);
	if (requested != 1 || active != 1)
		failed = 1;
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static void *run_provider_observation(void *context)
{
	struct concurrent_observation *event = context;

	test_gate_wait(event->start);
	event->result = is_remote_chunk_provider_observe(event->module,
		event->provider, &event->observation);
	return NULL;
}

static int run_concurrent_observations(
	struct concurrent_observation events[2])
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
		events[index].result = IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_INVALID;
		if (pthread_create(&threads[index], NULL, run_provider_observation,
			&events[index])) {
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

static int test_concurrent_provider_observations_are_atomic(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct concurrent_observation events[2];
	struct is_remote_chunk_snapshot *snapshot = NULL;
	unsigned int applied;
	unsigned int duplicate;
	unsigned int conflict;
	unsigned int index;
	int failed = 0;

	if (create_single_provider_module(&backed_config, &module, &provider))
		return 1;
	for (index = 0; index < 2; index++) {
		events[index] = (struct concurrent_observation) {
			.module = module,
			.provider = provider,
			.observation = {
				.sequence = 1,
				.available_chunks = 5,
				.exclusion =
					IS_REMOTE_CHUNK_PROVIDER_EXCLUDE_NONE,
				.placement_eligible = true,
			},
		};
	}
	if (run_concurrent_observations(events))
		failed = 1;
	applied = 0;
	duplicate = 0;
	for (index = 0; index < 2; index++) {
		applied += events[index].result ==
			IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_APPLIED;
		duplicate += events[index].result ==
			IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_DUPLICATE;
	}
	if (applied != 1 || duplicate != 1)
		failed = 1;

	events[0].observation.sequence = 2;
	events[0].observation.available_chunks = 3;
	events[1].observation.sequence = 2;
	events[1].observation.available_chunks = 0;
	events[1].observation.exclusion =
		IS_REMOTE_CHUNK_PROVIDER_EXCLUDE_ZERO_CAPACITY;
	events[1].observation.placement_eligible = false;
	if (run_concurrent_observations(events))
		failed = 1;
	applied = 0;
	conflict = 0;
	for (index = 0; index < 2; index++) {
		applied += events[index].result ==
			IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_APPLIED;
		conflict += events[index].result ==
			IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_CONFLICT;
	}
	if (applied != 1 || conflict != 1 ||
	    is_remote_chunk_snapshot_take(module, &snapshot) ||
	    snapshot->providers[0].observation_sequence != 2 ||
	    !((snapshot->providers[0].reported_available_chunks == 3 &&
	       snapshot->providers[0].placement_eligible &&
	       snapshot->providers[0].exclusion ==
		IS_REMOTE_CHUNK_PROVIDER_EXCLUDE_NONE) ||
	      (snapshot->providers[0].reported_available_chunks == 0 &&
	       !snapshot->providers[0].placement_eligible &&
	       snapshot->providers[0].exclusion ==
		IS_REMOTE_CHUNK_PROVIDER_EXCLUDE_ZERO_CAPACITY)))
		failed = 1;
	is_remote_chunk_snapshot_release(snapshot);
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
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

	if (create_single_provider_module(&remote_only_config, &module, &provider) ||
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

	if (create_single_provider_module(&remote_only_config, &module, &provider) ||
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

	if (create_single_provider_module(&remote_only_config, &module, &provider) ||
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

	if (create_single_provider_module(&remote_only_config, &module, &provider) ||
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
	    snapshot->active_io_leases != 0 || snapshot->invariant_count != 4 ||
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

	if (create_single_provider_module(&remote_only_config, &module, &provider) ||
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

	if (create_single_provider_module(&remote_only_config, &module, &provider) ||
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

	if (create_single_provider_module(&remote_only_config, &module, &provider) ||
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

static int test_provider_activity_query_and_eviction_are_atomic(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_eviction_claim claim =
		IS_REMOTE_CHUNK_EVICTION_CLAIM_INIT;
	const unsigned int provider_chunks[] = { 21, 20 };
	const unsigned int duplicate_chunks[] = { 20, 20 };
	const unsigned int malformed_chunks[] = { 20, 99 };
	struct is_remote_chunk_provider_activity activity[2] = {
		{ .provider_chunk = 999, .activity = 999 },
		{ .provider_chunk = 999, .activity = 999 },
	};
	struct is_remote_chunk_snapshot *snapshot = NULL;
	struct is_remote_chunk_mapping_request mapping_request = { 0 };
	int failed = 0;

	if (create_single_provider_module(&backed_config, &module, &provider) ||
	    map_backed_chunk(module, provider, 0, 20) ||
	    map_backed_chunk(module, provider, 1, 21))
		return 1;
	if (is_remote_chunk_provider_activity_query(module, provider,
		provider_chunks, 2, activity) ||
	    activity[0].provider_chunk != 21 || activity[0].activity != 8 ||
	    activity[1].provider_chunk != 20 || activity[1].activity != 8)
		failed = 1;
	if (is_remote_chunk_provider_activity_query(module, provider,
		duplicate_chunks, 2, activity) != -EINVAL ||
	    activity[0].provider_chunk != 21 || activity[0].activity != 8 ||
	    activity[1].provider_chunk != 20 || activity[1].activity != 8)
		failed = 1;
	if (is_remote_chunk_provider_activity_query(module, provider,
		malformed_chunks, 2, activity) != -ENOENT ||
	    activity[0].provider_chunk != 21 || activity[0].activity != 8 ||
	    activity[1].provider_chunk != 20 || activity[1].activity != 8)
		failed = 1;
	if (is_remote_chunk_eviction_begin(module, provider, duplicate_chunks, 2,
		&claim) != -EINVAL ||
	    is_remote_chunk_eviction_begin(module, provider, malformed_chunks, 2,
		&claim) != -ENOENT)
		failed = 1;
	failed |= expect_snapshot(module, 2, 2, 0, 0, 2,
		"malformed eviction batch");
	if (is_remote_chunk_eviction_begin(module, provider, provider_chunks, 2,
		&claim))
		failed = 1;
	if (is_remote_chunk_snapshot_take(module, &snapshot) ||
	    snapshot->assigned_chunks != 2 || snapshot->usable_chunks != 0 ||
	    snapshot->evicting_chunks != 2 ||
	    snapshot->active_eviction_claims != 1 ||
	    snapshot->placement_count != 2 || snapshot->placements[0].usable ||
	    snapshot->placements[1].usable)
		failed = 1;
	is_remote_chunk_snapshot_release(snapshot);
	if (is_remote_chunk_eviction_wait(module, &claim,
		monotonic_deadline_after_ms(100)) ||
	    is_remote_chunk_eviction_finish(module, &claim))
		failed = 1;
	failed |= expect_snapshot(module, 0, 0, 0, 0, 0,
		"finished eviction batch");
	if (is_remote_chunk_next_mapping(module, &mapping_request) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_NO_HOT_RANGE)
		failed = 1;
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_eviction_wait_times_out_without_choosing_policy(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_eviction_claim claim =
		IS_REMOTE_CHUNK_EVICTION_CLAIM_INIT;
	struct is_remote_chunk_io_lease lease = IS_REMOTE_CHUNK_IO_LEASE_INIT;
	struct is_remote_chunk_io_lease rejected = IS_REMOTE_CHUNK_IO_LEASE_INIT;
	struct is_remote_chunk_io_request request = {
		.direction = IS_REMOTE_CHUNK_IO_WRITE,
		.sector = 8,
		.bytes = 4096,
	};
	struct is_remote_chunk_transport_mapping mapping;
	struct is_remote_chunk_snapshot *snapshot = NULL;
	enum is_remote_chunk_io_resolve_result result;
	const unsigned int provider_chunk = 20;
	int failed = 0;

	if (create_single_provider_module(&backed_config, &module, &provider) ||
	    map_backed_chunk(module, provider, 0, provider_chunk) ||
	    is_remote_chunk_io_lease_acquire(module, &request, &lease, &mapping) ||
	    is_remote_chunk_eviction_begin(module, provider, &provider_chunk, 1,
		&claim))
		return 1;
	if (is_remote_chunk_io_lease_acquire(module, &request, &rejected,
		&mapping) != -ENXIO ||
	    is_remote_chunk_eviction_wait(module, &claim,
		monotonic_deadline_after_ms(1)) != -ETIMEDOUT ||
	    is_remote_chunk_module_destroy(module) != -EBUSY ||
	    is_remote_chunk_eviction_finish(module, &claim) != -EBUSY)
		failed = 1;
	if (is_remote_chunk_snapshot_take(module, &snapshot) ||
	    snapshot->assigned_chunks != 1 || snapshot->usable_chunks != 0 ||
	    snapshot->evicting_chunks != 1 || snapshot->active_io_leases != 1 ||
	    snapshot->invariant_count != 2 ||
	    snapshot->latest_invariant !=
		IS_REMOTE_CHUNK_INVARIANT_EVICTION_FINISH_ACTIVE_LEASES)
		failed = 1;
	is_remote_chunk_snapshot_release(snapshot);
	if (is_remote_chunk_io_lease_resolve(module, &lease,
		IS_REMOTE_CHUNK_IO_SUCCESS, &result) ||
	    is_remote_chunk_io_lease_release(module, &lease) ||
	    is_remote_chunk_eviction_wait(module, &claim,
		monotonic_deadline_after_ms(100)) ||
	    is_remote_chunk_eviction_finish(module, &claim))
		failed = 1;
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_remote_only_rejects_provider_eviction(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_eviction_claim claim =
		IS_REMOTE_CHUNK_EVICTION_CLAIM_INIT;
	const unsigned int provider_chunk = 20;
	int failed = 0;

	if (create_single_provider_module(&remote_only_config, &module, &provider) ||
	    map_remote_only_chunk(module, provider, 0, provider_chunk))
		return 1;
	if (is_remote_chunk_eviction_begin(module, provider, &provider_chunk, 1,
		&claim) != -EOPNOTSUPP)
		failed = 1;
	failed |= expect_snapshot(module, 1, 1, 0, 0, 1,
		"Committed Remote Chunk eviction");
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

struct concurrent_provider_failure {
	struct is_remote_chunk_module *module;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_provider_failure_facts facts;
	struct test_gate *start;
	int status;
};

struct concurrent_eviction_begin {
	struct is_remote_chunk_module *module;
	struct is_remote_chunk_provider_handle provider;
	unsigned int provider_chunk;
	struct is_remote_chunk_eviction_claim claim;
	struct test_gate *start;
	int status;
};

static void *run_provider_failure(void *context)
{
	struct concurrent_provider_failure *event = context;

	test_gate_wait(event->start);
	event->status = is_remote_chunk_provider_failed(event->module,
		event->provider, &event->facts);
	return NULL;
}

static void *run_eviction_begin(void *context)
{
	struct concurrent_eviction_begin *event = context;

	test_gate_wait(event->start);
	event->status = is_remote_chunk_eviction_begin(event->module,
		event->provider, &event->provider_chunk, 1, &event->claim);
	return NULL;
}

static int test_provider_failure_invalidates_atomically_and_preserves_activity(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle failed_provider;
	struct is_remote_chunk_provider_handle replacement_provider;
	struct is_remote_chunk_eviction_claim eviction_claim =
		IS_REMOTE_CHUNK_EVICTION_CLAIM_INIT;
	struct is_remote_chunk_mapping_claim mapping_claim =
		IS_REMOTE_CHUNK_MAPPING_CLAIM_INIT;
	struct is_remote_chunk_mapping_claim replacement_claim =
		IS_REMOTE_CHUNK_MAPPING_CLAIM_INIT;
	struct is_remote_chunk_mapping_grant replacement_grant =
		mapping_grant(0, 30);
	struct is_remote_chunk_provider_failure_facts facts;
	struct is_remote_chunk_snapshot *snapshot = NULL;
	struct is_remote_chunk_io_lease lease = IS_REMOTE_CHUNK_IO_LEASE_INIT;
	struct is_remote_chunk_io_request request = {
		.direction = IS_REMOTE_CHUNK_IO_WRITE,
		.sector = 8,
		.bytes = 4096,
	};
	struct is_remote_chunk_transport_mapping mapping;
	enum is_remote_chunk_io_resolve_result result;
	const unsigned int evicted_provider_chunk = 21;
	unsigned int logical_chunk = ~0U;
	bool mapping_needed = false;
	int failed = 0;

	if (create_two_provider_module(&backed_two_provider_config, &module,
		&failed_provider, &replacement_provider) ||
	    map_backed_chunk(module, failed_provider, 0, 20) ||
	    map_backed_chunk(module, failed_provider, 1, 21) ||
	    is_remote_chunk_io_lease_acquire(module, &request, &lease, &mapping) ||
	    is_remote_chunk_eviction_begin(module, failed_provider,
		&evicted_provider_chunk, 1, &eviction_claim) ||
	    is_remote_chunk_note_activity(module, 2, 1,
		IS_REMOTE_CHUNK_ACTIVITY_WRITE, &mapping_needed) ||
	    is_remote_chunk_note_activity(module, 2, 1,
		IS_REMOTE_CHUNK_ACTIVITY_WRITE, &mapping_needed) || !mapping_needed ||
	    begin_backed_mapping(module, failed_provider, &mapping_claim,
		&logical_chunk) || logical_chunk != 2)
		return 1;
	if (is_remote_chunk_provider_failed(module, failed_provider, &facts) ||
	    facts.affected_chunks != 3 || facts.assigned_chunks != 2 ||
	    facts.usable_chunks != 1 || facts.mapping_chunks != 1 ||
	    facts.evicting_chunks != 1 || facts.active_io_leases != 1)
		failed = 1;
	if (is_remote_chunk_snapshot_take(module, &snapshot) ||
	    snapshot->assigned_chunks != 0 || snapshot->usable_chunks != 0 ||
	    snapshot->mapping_chunks != 0 || snapshot->evicting_chunks != 0 ||
	    snapshot->active_mapping_claims != 0 ||
	    snapshot->active_eviction_claims != 0 ||
	    snapshot->active_io_leases != 1 || snapshot->hot_ranges != 3 ||
	    snapshot->mapping_candidates != 3)
		failed = 1;
	is_remote_chunk_snapshot_release(snapshot);
	if (is_remote_chunk_mapping_abort(module, &mapping_claim) != -ESTALE ||
	    is_remote_chunk_eviction_finish(module, &eviction_claim) != -ESTALE)
		failed = 1;
	logical_chunk = ~0U;
	if (begin_backed_mapping(module, replacement_provider,
		&replacement_claim, &logical_chunk) || logical_chunk != 0 ||
	    is_remote_chunk_mapping_commit(module, &replacement_claim,
		replacement_provider, &replacement_grant, 1))
		failed = 1;
	if (is_remote_chunk_io_lease_resolve(module, &lease,
		IS_REMOTE_CHUNK_IO_SUCCESS, &result) ||
	    result != IS_REMOTE_CHUNK_IO_RESOLVE_STALE ||
	    is_remote_chunk_io_lease_release(module, &lease))
		failed = 1;
	failed |= expect_read_admission(module, 8, 4096, -ENODATA);
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_remote_only_provider_failure_returns_policy_neutral_facts(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_provider_failure_facts facts;
	struct is_remote_chunk_provider_observation observation = {
		.sequence = 1,
		.available_chunks = 1,
		.exclusion = IS_REMOTE_CHUNK_PROVIDER_EXCLUDE_NONE,
		.placement_eligible = true,
	};
	int failed = 0;

	if (create_single_provider_module(&remote_only_config, &module, &provider) ||
	    map_remote_only_chunk(module, provider, 0, 20) ||
	    map_remote_only_chunk(module, provider, 1, 21))
		return 1;
	if (is_remote_chunk_provider_failed(module, provider, &facts) ||
	    facts.affected_chunks != 2 || facts.assigned_chunks != 2 ||
	    facts.usable_chunks != 2 || facts.mapping_chunks ||
	    facts.evicting_chunks || facts.active_io_leases)
		failed = 1;
	if (is_remote_chunk_provider_observe(module, provider, &observation) !=
		IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_STALE_EPOCH)
		failed = 1;
	failed |= expect_snapshot(module, 0, 0, 0, 0, 0,
		"Remote-Only Provider failure");
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_concurrent_provider_failure_and_eviction_are_atomic(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct concurrent_provider_failure failure;
	struct concurrent_eviction_begin eviction;
	struct test_gate start;
	pthread_t failure_thread;
	pthread_t eviction_thread;
	int failed = 0;

	if (create_single_provider_module(&backed_config, &module, &provider) ||
	    map_backed_chunk(module, provider, 0, 20) || test_gate_init(&start))
		return 1;
	failure = (struct concurrent_provider_failure) {
		.module = module,
		.provider = provider,
		.start = &start,
		.status = -999,
	};
	eviction = (struct concurrent_eviction_begin) {
		.module = module,
		.provider = provider,
		.provider_chunk = 20,
		.claim = IS_REMOTE_CHUNK_EVICTION_CLAIM_INIT,
		.start = &start,
		.status = -999,
	};
	if (pthread_create(&failure_thread, NULL, run_provider_failure, &failure) ||
	    pthread_create(&eviction_thread, NULL, run_eviction_begin, &eviction))
		return 1;
	test_gate_open(&start);
	(void)pthread_join(failure_thread, NULL);
	(void)pthread_join(eviction_thread, NULL);
	test_gate_destroy(&start);
	if (failure.status || failure.facts.affected_chunks != 1 ||
	    failure.facts.assigned_chunks != 1 ||
	    (eviction.status != 0 && eviction.status != -ENOENT &&
	     eviction.status != -ESTALE))
		failed = 1;
	if (!eviction.status &&
	    is_remote_chunk_eviction_finish(module, &eviction.claim) != -ESTALE)
		failed = 1;
	failed |= expect_snapshot(module, 0, 0, 0, 0, 0,
		"concurrent failure and eviction");
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

struct concurrent_eviction_wait {
	struct is_remote_chunk_module *module;
	const struct is_remote_chunk_eviction_claim *claim;
	struct test_gate *start;
	int status;
};

static void *run_eviction_wait(void *context)
{
	struct concurrent_eviction_wait *event = context;

	test_gate_wait(event->start);
	event->status = is_remote_chunk_eviction_wait(event->module, event->claim,
		monotonic_deadline_after_ms(1000));
	return NULL;
}

static int test_concurrent_lease_release_wakes_eviction_wait(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_eviction_claim claim =
		IS_REMOTE_CHUNK_EVICTION_CLAIM_INIT;
	struct is_remote_chunk_io_lease lease = IS_REMOTE_CHUNK_IO_LEASE_INIT;
	struct is_remote_chunk_io_request request = {
		.direction = IS_REMOTE_CHUNK_IO_WRITE,
		.sector = 8,
		.bytes = 4096,
	};
	struct is_remote_chunk_transport_mapping mapping;
	enum is_remote_chunk_io_resolve_result result;
	struct concurrent_eviction_wait wait_event;
	struct concurrent_lease_event release_event;
	struct test_gate start;
	pthread_t wait_thread;
	pthread_t release_thread;
	const unsigned int provider_chunk = 20;
	int failed = 0;

	if (create_single_provider_module(&backed_config, &module, &provider) ||
	    map_backed_chunk(module, provider, 0, provider_chunk) ||
	    is_remote_chunk_io_lease_acquire(module, &request, &lease, &mapping) ||
	    is_remote_chunk_eviction_begin(module, provider, &provider_chunk, 1,
		&claim) ||
	    is_remote_chunk_io_lease_resolve(module, &lease,
		IS_REMOTE_CHUNK_IO_SUCCESS, &result) || test_gate_init(&start))
		return 1;
	wait_event = (struct concurrent_eviction_wait) {
		.module = module,
		.claim = &claim,
		.start = &start,
		.status = -999,
	};
	release_event = (struct concurrent_lease_event) {
		.module = module,
		.lease = &lease,
		.start = &start,
		.status = -999,
	};
	if (pthread_create(&wait_thread, NULL, run_eviction_wait, &wait_event) ||
	    pthread_create(&release_thread, NULL, run_lease_release,
		&release_event))
		return 1;
	test_gate_open(&start);
	(void)pthread_join(wait_thread, NULL);
	(void)pthread_join(release_thread, NULL);
	test_gate_destroy(&start);
	if (wait_event.status || release_event.status ||
	    is_remote_chunk_eviction_finish(module, &claim))
		failed = 1;
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_concurrent_completion_and_provider_failure_are_generation_safe(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_io_lease lease = IS_REMOTE_CHUNK_IO_LEASE_INIT;
	struct is_remote_chunk_io_request request = {
		.direction = IS_REMOTE_CHUNK_IO_WRITE,
		.sector = 8,
		.bytes = 4096,
	};
	struct is_remote_chunk_transport_mapping mapping;
	struct concurrent_lease_event completion;
	struct concurrent_provider_failure failure;
	struct is_remote_chunk_snapshot *snapshot = NULL;
	struct test_gate start;
	pthread_t completion_thread;
	pthread_t failure_thread;
	int failed = 0;

	if (create_single_provider_module(&backed_config, &module, &provider) ||
	    map_backed_chunk(module, provider, 0, 20) ||
	    is_remote_chunk_io_lease_acquire(module, &request, &lease, &mapping) ||
	    test_gate_init(&start))
		return 1;
	completion = (struct concurrent_lease_event) {
		.module = module,
		.lease = &lease,
		.start = &start,
		.result = IS_REMOTE_CHUNK_IO_RESOLVE_NONE,
		.status = -999,
	};
	failure = (struct concurrent_provider_failure) {
		.module = module,
		.provider = provider,
		.start = &start,
		.status = -999,
	};
	if (pthread_create(&completion_thread, NULL, run_lease_resolve,
		&completion) ||
	    pthread_create(&failure_thread, NULL, run_provider_failure, &failure))
		return 1;
	test_gate_open(&start);
	(void)pthread_join(completion_thread, NULL);
	(void)pthread_join(failure_thread, NULL);
	test_gate_destroy(&start);
	if (completion.status ||
	    (completion.result != IS_REMOTE_CHUNK_IO_RESOLVE_CURRENT &&
	     completion.result != IS_REMOTE_CHUNK_IO_RESOLVE_STALE) ||
	    failure.status || failure.facts.affected_chunks != 1 ||
	    failure.facts.active_io_leases != 1 ||
	    is_remote_chunk_io_lease_release(module, &lease))
		failed = 1;
	if (is_remote_chunk_snapshot_take(module, &snapshot) ||
	    snapshot->assigned_chunks || snapshot->usable_chunks ||
	    snapshot->active_io_leases || snapshot->invariant_count)
		failed = 1;
	is_remote_chunk_snapshot_release(snapshot);
	failed |= is_remote_chunk_module_destroy(module) != 0;
	return failed;
}

static int test_quiesce_rejects_new_ownership_while_accepted_work_retires(void)
{
	struct is_remote_chunk_module *module = NULL;
	struct is_remote_chunk_provider_handle provider;
	struct is_remote_chunk_provider_observation rejected_observation = {
		.sequence = 1,
		.available_chunks = 1,
		.exclusion = IS_REMOTE_CHUNK_PROVIDER_EXCLUDE_NONE,
		.placement_eligible = true,
	};
	struct is_remote_chunk_mapping_claim mapping_claim =
		IS_REMOTE_CHUNK_MAPPING_CLAIM_INIT;
	struct is_remote_chunk_mapping_request rejected_mapping = { 0 };
	struct is_remote_chunk_io_lease lease = IS_REMOTE_CHUNK_IO_LEASE_INIT;
	struct is_remote_chunk_io_lease rejected_lease =
		IS_REMOTE_CHUNK_IO_LEASE_INIT;
	struct is_remote_chunk_io_request request = {
		.direction = IS_REMOTE_CHUNK_IO_WRITE,
		.sector = 8,
		.bytes = 4096,
	};
	struct is_remote_chunk_transport_mapping mapping;
	struct is_remote_chunk_snapshot *snapshot = NULL;
	struct is_remote_chunk_hot_policy policy = backed_config.hot_policy;
	enum is_remote_chunk_io_resolve_result result;
	unsigned int logical_chunk = ~0U;
	bool mapping_needed = false;
	int failed = 0;

	if (create_single_provider_module(&backed_config, &module, &provider) ||
	    map_backed_chunk(module, provider, 0, 20) ||
	    is_remote_chunk_io_lease_acquire(module, &request, &lease, &mapping) ||
	    is_remote_chunk_note_activity(module, 1, 1,
		IS_REMOTE_CHUNK_ACTIVITY_WRITE, &mapping_needed) ||
	    is_remote_chunk_note_activity(module, 1, 1,
		IS_REMOTE_CHUNK_ACTIVITY_WRITE, &mapping_needed) ||
	    begin_backed_mapping(module, provider, &mapping_claim,
		&logical_chunk) || logical_chunk != 1)
		return 1;
	if (is_remote_chunk_module_quiesce(module) ||
	    is_remote_chunk_module_quiesce(module) != -EALREADY ||
	    is_remote_chunk_provider_observe(module, provider,
		&rejected_observation) !=
		IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_SHUTDOWN ||
	    is_remote_chunk_hot_policy_set(module, &policy, &mapping_needed) !=
		-ESHUTDOWN ||
	    is_remote_chunk_note_activity(module, 2, 1,
		IS_REMOTE_CHUNK_ACTIVITY_WRITE, &mapping_needed) != -ESHUTDOWN ||
	    is_remote_chunk_next_mapping(module, &rejected_mapping) !=
		IS_REMOTE_CHUNK_NEXT_MAPPING_SHUTDOWN ||
	    is_remote_chunk_io_lease_acquire(module, &request, &rejected_lease,
		&mapping) != -ESHUTDOWN)
		failed = 1;
	if (is_remote_chunk_snapshot_take(module, &snapshot) ||
	    !snapshot->quiescing || snapshot->active_mapping_claims != 1 ||
	    snapshot->active_io_leases != 1 || snapshot->invariant_count != 1 ||
	    snapshot->latest_invariant !=
		IS_REMOTE_CHUNK_INVARIANT_MODULE_DUPLICATE_QUIESCE)
		failed = 1;
	is_remote_chunk_snapshot_release(snapshot);
	{
		struct is_remote_chunk_mapping_grant grant =
			mapping_grant(1, 21);

		if (is_remote_chunk_mapping_commit(module, &mapping_claim, provider,
			&grant, 1) != -ESHUTDOWN)
			failed = 1;
	}
	if (is_remote_chunk_module_destroy(module) != -EBUSY ||
	    is_remote_chunk_mapping_abort(module, &mapping_claim) ||
	    is_remote_chunk_io_lease_resolve(module, &lease,
		IS_REMOTE_CHUNK_IO_SUCCESS, &result) ||
	    is_remote_chunk_io_lease_release(module, &lease) ||
	    is_remote_chunk_module_destroy(module))
		failed = 1;
	return failed;
}

static int run_certification_contracts(void)
{
	int atomic_batches = test_malformed_commit_does_not_partially_map() |
		test_provider_activity_query_and_eviction_are_atomic();
	int eviction_drain = test_eviction_wait_times_out_without_choosing_policy() |
		test_concurrent_lease_release_wakes_eviction_wait();
	int generation_safety =
		test_provider_failure_invalidates_atomically_and_preserves_activity() |
		test_concurrent_completion_and_provider_failure_are_generation_safe();
	int remote_only = test_remote_only_rejects_provider_eviction() |
		test_remote_only_provider_failure_returns_policy_neutral_facts();
	int failed = atomic_batches | eviction_drain | generation_safety |
		remote_only;

	printf("{\"schema_version\":1,\"kind\":"
	       "\"infiniswap.remote-chunk-certification\","
	       "\"status\":\"%s\",\"checks\":{"
	       "\"atomic_batches\":\"%s\","
	       "\"eviction_drain\":\"%s\","
	       "\"generation_safety\":\"%s\","
	       "\"remote_only\":\"%s\"}}\n",
	       failed ? "failed" : "passed",
	       atomic_batches ? "failed" : "passed",
	       eviction_drain ? "failed" : "passed",
	       generation_safety ? "failed" : "passed",
	       remote_only ? "failed" : "passed");
	return failed;
}

int main(int argc, char **argv)
{
	int failed;

	if (argc == 2 && strcmp(argv[1], "--certification-report") == 0)
		return run_certification_contracts();
	if (argc != 1) {
		fprintf(stderr, "usage: %s [--certification-report]\n", argv[0]);
		return 2;
	}
	failed = test_module_creation_copies_complete_provider_roster() |
		test_module_creation_validates_roster_atomically() |
		test_empty_provider_roster_is_backed_only() |
		test_provider_observations_are_ordered_and_diagnostic() |
		test_concurrent_provider_observations_are_atomic() |
		test_concurrent_next_mapping_calls_have_one_winner() |
		test_next_mapping_reports_idle_capacity_and_active_claim() |
		test_deterministic_placement_prefers_sampled_capacity() |
		test_weights_bias_sampling_with_d_equals_one() |
		test_exclusions_clamping_and_stable_ties() |
		test_effective_capacity_tracks_assignments_and_abort() |
		test_observation_during_claim_tracks_reserved_baseline() |
		test_remote_only_waits_for_complete_provider_facts() |
		test_remote_only_rejects_insufficient_capacity_before_claim() |
		test_remote_only_progresses_in_order_after_abort() |
		test_remote_only_reports_no_eligible_provider_after_progress() |
		test_remote_only_uses_deterministic_multi_provider_selection() |
		test_placement_lifecycle_does_not_allocate() |
		test_explicit_mapping_commit_is_atomic() |
		test_mapping_claim_is_module_wide() |
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
		test_concurrent_independent_lease_lifetimes() |
		test_provider_activity_query_and_eviction_are_atomic() |
		test_eviction_wait_times_out_without_choosing_policy() |
		test_remote_only_rejects_provider_eviction() |
		test_provider_failure_invalidates_atomically_and_preserves_activity() |
		test_remote_only_provider_failure_returns_policy_neutral_facts() |
		test_concurrent_provider_failure_and_eviction_are_atomic() |
		test_concurrent_lease_release_wakes_eviction_wait() |
		test_concurrent_completion_and_provider_failure_are_generation_safe() |
		test_quiesce_rejects_new_ownership_while_accepted_work_retires();

	if (failed)
		fprintf(stderr, "Remote Chunk public-interface tests failed\n");
	return failed;
}
