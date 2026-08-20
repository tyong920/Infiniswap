/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#include "is_remote_chunk.h"

#ifdef __KERNEL__
#include <linux/atomic.h>
#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>

typedef spinlock_t is_remote_chunk_lock_t;
typedef unsigned long is_remote_chunk_lock_flags_t;

static atomic64_t is_remote_chunk_next_module_identity = ATOMIC64_INIT(0);

static int is_remote_chunk_lock_init(is_remote_chunk_lock_t *lock)
{
	spin_lock_init(lock);
	return 0;
}

static void is_remote_chunk_lock(is_remote_chunk_lock_t *lock,
				 is_remote_chunk_lock_flags_t *flags)
{
	spin_lock_irqsave(lock, *flags);
}

static void is_remote_chunk_unlock(is_remote_chunk_lock_t *lock,
				   is_remote_chunk_lock_flags_t *flags)
{
	spin_unlock_irqrestore(lock, *flags);
}

static void is_remote_chunk_lock_destroy(is_remote_chunk_lock_t *lock)
{
	(void)lock;
}

static void *is_remote_chunk_allocate(size_t size)
{
	return kzalloc(size, GFP_KERNEL);
}

static void *is_remote_chunk_allocate_array(size_t count, size_t size)
{
	return kcalloc(count, size, GFP_KERNEL);
}

static void is_remote_chunk_free(void *allocation)
{
	kfree(allocation);
}

static unsigned long long is_remote_chunk_allocate_module_identity(void)
{
	return (unsigned long long)atomic64_inc_return(
		&is_remote_chunk_next_module_identity);
}
#else
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef pthread_mutex_t is_remote_chunk_lock_t;
typedef int is_remote_chunk_lock_flags_t;

static atomic_ullong is_remote_chunk_next_module_identity =
	ATOMIC_VAR_INIT(0);

static int is_remote_chunk_lock_init(is_remote_chunk_lock_t *lock)
{
	return pthread_mutex_init(lock, NULL);
}

static void is_remote_chunk_lock(is_remote_chunk_lock_t *lock,
				 is_remote_chunk_lock_flags_t *flags)
{
	(void)flags;
	(void)pthread_mutex_lock(lock);
}

static void is_remote_chunk_unlock(is_remote_chunk_lock_t *lock,
				   is_remote_chunk_lock_flags_t *flags)
{
	(void)flags;
	(void)pthread_mutex_unlock(lock);
}

static void is_remote_chunk_lock_destroy(is_remote_chunk_lock_t *lock)
{
	(void)pthread_mutex_destroy(lock);
}

static void *is_remote_chunk_allocate(size_t size)
{
	return calloc(1, size);
}

static void *is_remote_chunk_allocate_array(size_t count, size_t size)
{
	if (size && count > SIZE_MAX / size)
		return NULL;
	return calloc(count, size);
}

static void is_remote_chunk_free(void *allocation)
{
	free(allocation);
}

static unsigned long long is_remote_chunk_allocate_module_identity(void)
{
	return atomic_fetch_add_explicit(&is_remote_chunk_next_module_identity, 1,
		memory_order_relaxed) + 1;
}
#endif

#define IS_REMOTE_CHUNK_CLAIM_MAGIC 0x49534348434c4149ULL
#define IS_REMOTE_CHUNK_ACTIVITY_MAX (~0ULL >> 1)
#define IS_REMOTE_CHUNK_ID_MAX (~0ULL)
#define IS_REMOTE_CHUNK_DIGEST_OFFSET 1469598103934665603ULL
#define IS_REMOTE_CHUNK_DIGEST_PRIME 1099511628211ULL

enum is_remote_chunk_state {
	IS_REMOTE_CHUNK_UNMAPPED = 0,
	IS_REMOTE_CHUNK_MAPPING,
	IS_REMOTE_CHUNK_MAPPED,
};

struct is_remote_chunk {
	enum is_remote_chunk_state state;
	struct is_remote_chunk_provider_handle provider;
	unsigned int provider_chunk;
	unsigned long long remote_address;
	unsigned int remote_key;
	unsigned long long activity;
	unsigned long long mapping_generation;
	unsigned long long mapping_claim_id;
	unsigned long long transition_generation;
};

struct is_remote_chunk_module {
	is_remote_chunk_lock_t lock;
	struct is_remote_chunk *chunks;
	enum is_remote_chunk_mode mode;
	unsigned int chunk_count;
	struct is_remote_chunk_hot_policy hot_policy;
	unsigned long long identity;
	unsigned long long next_provider_serial;
	unsigned long long next_mapping_claim_id;
	unsigned long long next_transition_generation;
	unsigned long long next_mapping_generation;
	unsigned int active_mapping_claims;
	unsigned int assigned_chunks;
	unsigned int usable_chunks;
};

struct is_remote_chunk_mapping_claim_internal {
	unsigned long long magic;
	unsigned long long module_identity;
	struct is_remote_chunk_provider_handle provider;
	unsigned long long claim_id;
	unsigned long long transition_generation;
	unsigned long long batch_digest;
	unsigned int chunk_count;
	unsigned int reserved;
};

#ifdef __KERNEL__
static_assert(sizeof(struct is_remote_chunk_mapping_claim_internal) <=
	IS_REMOTE_CHUNK_MAPPING_CLAIM_BYTES);
#else
_Static_assert(sizeof(struct is_remote_chunk_mapping_claim_internal) <=
	IS_REMOTE_CHUNK_MAPPING_CLAIM_BYTES,
	"mapping claim storage is too small");
#endif

static bool is_remote_chunk_mode_valid(enum is_remote_chunk_mode mode)
{
	return mode == IS_REMOTE_CHUNK_MODE_BACKED ||
		mode == IS_REMOTE_CHUNK_MODE_REMOTE_ONLY;
}

static bool is_remote_chunk_hot_policy_valid(
	const struct is_remote_chunk_hot_policy *policy)
{
	return policy && policy->threshold &&
		policy->threshold <= IS_REMOTE_CHUNK_ACTIVITY_MAX &&
		policy->read_weight &&
		policy->read_weight <= IS_REMOTE_CHUNK_HOT_WEIGHT_MAX &&
		policy->write_weight &&
		policy->write_weight <= IS_REMOTE_CHUNK_HOT_WEIGHT_MAX;
}

bool is_remote_chunk_provider_handle_equal(
	struct is_remote_chunk_provider_handle left,
	struct is_remote_chunk_provider_handle right)
{
	return left.opaque[0] == right.opaque[0] &&
		left.opaque[1] == right.opaque[1];
}

static bool is_remote_chunk_provider_valid_locked(
	const struct is_remote_chunk_module *module,
	struct is_remote_chunk_provider_handle provider)
{
	return provider.opaque[0] == module->identity && provider.opaque[1] &&
		provider.opaque[1] <= module->next_provider_serial;
}

static unsigned long long is_remote_chunk_batch_digest(
	const bool *selected, unsigned int chunk_count)
{
	unsigned long long digest = IS_REMOTE_CHUNK_DIGEST_OFFSET;
	unsigned int index;

	for (index = 0; index < chunk_count; index++) {
		if (!selected[index])
			continue;
		digest ^= (unsigned long long)index + 1ULL;
		digest *= IS_REMOTE_CHUNK_DIGEST_PRIME;
	}
	return digest;
}

static void is_remote_chunk_claim_decode(
	const struct is_remote_chunk_mapping_claim *claim,
	struct is_remote_chunk_mapping_claim_internal *internal)
{
	memset(internal, 0, sizeof(*internal));
	if (claim)
		memcpy(internal, claim->opaque.bytes, sizeof(*internal));
}

static void is_remote_chunk_claim_encode(
	struct is_remote_chunk_mapping_claim *claim,
	const struct is_remote_chunk_mapping_claim_internal *internal)
{
	memset(claim, 0, sizeof(*claim));
	memcpy(claim->opaque.bytes, internal, sizeof(*internal));
}

static int is_remote_chunk_claim_validate_locked(
	const struct is_remote_chunk_module *module,
	const struct is_remote_chunk_mapping_claim_internal *claim)
{
	bool selected[IS_REMOTE_CHUNK_MAX_CHUNKS] = { false };
	unsigned int member_count = 0;
	unsigned int index;

	if (claim->magic != IS_REMOTE_CHUNK_CLAIM_MAGIC || !claim->claim_id ||
	    !claim->transition_generation || !claim->chunk_count)
		return -EINVAL;
	if (claim->module_identity != module->identity)
		return -ESTALE;
	if (!is_remote_chunk_provider_valid_locked(module, claim->provider))
		return -ESTALE;
	if (claim->chunk_count > module->chunk_count)
		return -EINVAL;

	for (index = 0; index < module->chunk_count; index++) {
		const struct is_remote_chunk *chunk = &module->chunks[index];

		if (chunk->mapping_claim_id != claim->claim_id)
			continue;
		if (chunk->state != IS_REMOTE_CHUNK_MAPPING ||
		    chunk->transition_generation != claim->transition_generation ||
		    !is_remote_chunk_provider_handle_equal(chunk->provider,
			claim->provider))
			return -ESTALE;
		selected[index] = true;
		member_count++;
	}
	if (member_count != claim->chunk_count ||
	    is_remote_chunk_batch_digest(selected, module->chunk_count) !=
		claim->batch_digest)
		return -ESTALE;
	return 0;
}

static bool is_remote_chunk_claim_active_locked(
	const struct is_remote_chunk_module *module,
	const struct is_remote_chunk_mapping_claim *claim)
{
	struct is_remote_chunk_mapping_claim_internal internal;

	is_remote_chunk_claim_decode(claim, &internal);
	return is_remote_chunk_claim_validate_locked(module, &internal) == 0;
}

static int is_remote_chunk_mapping_begin_locked(
	struct is_remote_chunk_module *module,
	struct is_remote_chunk_provider_handle provider,
	const unsigned int *logical_chunks, unsigned int chunk_count,
	struct is_remote_chunk_mapping_claim *claim_out)
{
	struct is_remote_chunk_mapping_claim_internal claim = { 0 };
	bool selected[IS_REMOTE_CHUNK_MAX_CHUNKS] = { false };
	unsigned int index;

	if (!is_remote_chunk_provider_valid_locked(module, provider))
		return -ESTALE;
	if (is_remote_chunk_claim_active_locked(module, claim_out))
		return -EBUSY;
	for (index = 0; index < chunk_count; index++) {
		unsigned int logical_chunk = logical_chunks[index];

		if (logical_chunk >= module->chunk_count)
			return -ERANGE;
		if (selected[logical_chunk])
			return -EINVAL;
		if (module->chunks[logical_chunk].state !=
		    IS_REMOTE_CHUNK_UNMAPPED)
			return -EBUSY;
		selected[logical_chunk] = true;
	}
	if (module->next_mapping_claim_id == IS_REMOTE_CHUNK_ID_MAX ||
	    module->next_transition_generation == IS_REMOTE_CHUNK_ID_MAX)
		return -EOVERFLOW;

	claim.magic = IS_REMOTE_CHUNK_CLAIM_MAGIC;
	claim.module_identity = module->identity;
	claim.provider = provider;
	claim.claim_id = ++module->next_mapping_claim_id;
	claim.transition_generation = ++module->next_transition_generation;
	claim.batch_digest = is_remote_chunk_batch_digest(selected,
		module->chunk_count);
	claim.chunk_count = chunk_count;
	for (index = 0; index < module->chunk_count; index++) {
		struct is_remote_chunk *chunk;

		if (!selected[index])
			continue;
		chunk = &module->chunks[index];
		chunk->state = IS_REMOTE_CHUNK_MAPPING;
		chunk->provider = provider;
		chunk->mapping_claim_id = claim.claim_id;
		chunk->transition_generation = claim.transition_generation;
	}
	module->active_mapping_claims++;
	is_remote_chunk_claim_encode(claim_out, &claim);
	return 0;
}

int is_remote_chunk_module_create(
	const struct is_remote_chunk_config *config,
	struct is_remote_chunk_module **module_out)
{
	struct is_remote_chunk_module *module;
	int status;

	if (!module_out)
		return -EINVAL;
	*module_out = NULL;
	if (!config || !is_remote_chunk_mode_valid(config->mode) ||
	    !config->chunk_count ||
	    config->chunk_count > IS_REMOTE_CHUNK_MAX_CHUNKS)
		return -EINVAL;
	if (!is_remote_chunk_hot_policy_valid(&config->hot_policy))
		return -ERANGE;

	module = is_remote_chunk_allocate(sizeof(*module));
	if (!module)
		return -ENOMEM;
	module->chunks = is_remote_chunk_allocate_array(config->chunk_count,
		sizeof(*module->chunks));
	if (!module->chunks) {
		is_remote_chunk_free(module);
		return -ENOMEM;
	}
	status = is_remote_chunk_lock_init(&module->lock);
	if (status) {
		is_remote_chunk_free(module->chunks);
		is_remote_chunk_free(module);
		return status < 0 ? status : -status;
	}
	module->identity = is_remote_chunk_allocate_module_identity();
	if (!module->identity ||
	    module->identity == IS_REMOTE_CHUNK_ID_MAX) {
		is_remote_chunk_lock_destroy(&module->lock);
		is_remote_chunk_free(module->chunks);
		is_remote_chunk_free(module);
		return -EOVERFLOW;
	}
	module->mode = config->mode;
	module->chunk_count = config->chunk_count;
	module->hot_policy = config->hot_policy;
	*module_out = module;
	return 0;
}

int is_remote_chunk_module_destroy(struct is_remote_chunk_module *module)
{
	is_remote_chunk_lock_flags_t flags = 0;

	if (!module)
		return -EINVAL;
	is_remote_chunk_lock(&module->lock, &flags);
	if (module->active_mapping_claims) {
		is_remote_chunk_unlock(&module->lock, &flags);
		return -EBUSY;
	}
	is_remote_chunk_unlock(&module->lock, &flags);
	is_remote_chunk_lock_destroy(&module->lock);
	is_remote_chunk_free(module->chunks);
	is_remote_chunk_free(module);
	return 0;
}

int is_remote_chunk_provider_handle_create(
	struct is_remote_chunk_module *module,
	struct is_remote_chunk_provider_handle *provider_out)
{
	is_remote_chunk_lock_flags_t flags = 0;

	if (!module || !provider_out)
		return -EINVAL;
	is_remote_chunk_lock(&module->lock, &flags);
	if (module->next_provider_serial == IS_REMOTE_CHUNK_ID_MAX) {
		is_remote_chunk_unlock(&module->lock, &flags);
		return -EOVERFLOW;
	}
	provider_out->opaque[0] = module->identity;
	provider_out->opaque[1] = ++module->next_provider_serial;
	is_remote_chunk_unlock(&module->lock, &flags);
	return 0;
}

int is_remote_chunk_hot_policy_set(
	struct is_remote_chunk_module *module,
	const struct is_remote_chunk_hot_policy *policy,
	bool *mapping_needed)
{
	is_remote_chunk_lock_flags_t flags = 0;
	bool needed = false;
	unsigned int index;

	if (!module || !mapping_needed)
		return -EINVAL;
	*mapping_needed = false;
	if (!is_remote_chunk_hot_policy_valid(policy))
		return -ERANGE;
	is_remote_chunk_lock(&module->lock, &flags);
	module->hot_policy = *policy;
	if (module->mode == IS_REMOTE_CHUNK_MODE_BACKED) {
		for (index = 0; index < module->chunk_count; index++) {
			const struct is_remote_chunk *chunk = &module->chunks[index];

			if (chunk->state == IS_REMOTE_CHUNK_UNMAPPED &&
			    chunk->activity >= policy->threshold) {
				needed = true;
				break;
			}
		}
	}
	is_remote_chunk_unlock(&module->lock, &flags);
	*mapping_needed = needed;
	return 0;
}

int is_remote_chunk_note_activity(
	struct is_remote_chunk_module *module, unsigned int logical_start,
	unsigned int chunk_count, enum is_remote_chunk_activity_kind kind,
	bool *mapping_needed)
{
	is_remote_chunk_lock_flags_t flags = 0;
	unsigned int weight;
	bool needed = false;
	unsigned int index;

	if (!module || !mapping_needed || !chunk_count ||
	    (kind != IS_REMOTE_CHUNK_ACTIVITY_READ &&
	     kind != IS_REMOTE_CHUNK_ACTIVITY_WRITE))
		return -EINVAL;
	*mapping_needed = false;
	if (logical_start >= module->chunk_count ||
	    chunk_count > module->chunk_count - logical_start)
		return -ERANGE;

	is_remote_chunk_lock(&module->lock, &flags);
	if (module->mode != IS_REMOTE_CHUNK_MODE_BACKED) {
		is_remote_chunk_unlock(&module->lock, &flags);
		return -EOPNOTSUPP;
	}
	weight = kind == IS_REMOTE_CHUNK_ACTIVITY_WRITE ?
		module->hot_policy.write_weight : module->hot_policy.read_weight;
	for (index = logical_start; index < logical_start + chunk_count; index++) {
		struct is_remote_chunk *chunk = &module->chunks[index];

		if (chunk->activity > IS_REMOTE_CHUNK_ACTIVITY_MAX - weight)
			chunk->activity = IS_REMOTE_CHUNK_ACTIVITY_MAX;
		else
			chunk->activity += weight;
		if (chunk->state == IS_REMOTE_CHUNK_UNMAPPED &&
		    chunk->activity >= module->hot_policy.threshold)
			needed = true;
	}
	is_remote_chunk_unlock(&module->lock, &flags);
	*mapping_needed = needed;
	return 0;
}

int is_remote_chunk_mapping_begin_explicit(
	struct is_remote_chunk_module *module,
	struct is_remote_chunk_provider_handle provider,
	const unsigned int *logical_chunks, unsigned int chunk_count,
	struct is_remote_chunk_mapping_claim *claim_out)
{
	is_remote_chunk_lock_flags_t flags = 0;
	int status;

	if (!module || !logical_chunks || !chunk_count || !claim_out ||
	    chunk_count > module->chunk_count)
		return -EINVAL;
	is_remote_chunk_lock(&module->lock, &flags);
	if (module->mode != IS_REMOTE_CHUNK_MODE_REMOTE_ONLY) {
		is_remote_chunk_unlock(&module->lock, &flags);
		return -EOPNOTSUPP;
	}
	status = is_remote_chunk_mapping_begin_locked(module, provider,
		logical_chunks, chunk_count, claim_out);
	is_remote_chunk_unlock(&module->lock, &flags);
	return status;
}

int is_remote_chunk_mapping_begin_hot(
	struct is_remote_chunk_module *module,
	struct is_remote_chunk_provider_handle provider,
	struct is_remote_chunk_mapping_claim *claim_out,
	unsigned int *logical_chunk_out)
{
	is_remote_chunk_lock_flags_t flags = 0;
	unsigned int logical_chunk;
	int status = -ENOENT;

	if (!module || !claim_out || !logical_chunk_out)
		return -EINVAL;
	is_remote_chunk_lock(&module->lock, &flags);
	if (module->mode != IS_REMOTE_CHUNK_MODE_BACKED) {
		is_remote_chunk_unlock(&module->lock, &flags);
		return -EOPNOTSUPP;
	}
	for (logical_chunk = 0; logical_chunk < module->chunk_count;
	     logical_chunk++) {
		const struct is_remote_chunk *chunk =
			&module->chunks[logical_chunk];

		if (chunk->state == IS_REMOTE_CHUNK_UNMAPPED &&
		    chunk->activity >= module->hot_policy.threshold) {
			status = is_remote_chunk_mapping_begin_locked(module, provider,
				&logical_chunk, 1, claim_out);
			break;
		}
	}
	is_remote_chunk_unlock(&module->lock, &flags);
	if (!status)
		*logical_chunk_out = logical_chunk;
	return status;
}

static int is_remote_chunk_mapping_grants_validate_locked(
	const struct is_remote_chunk_module *module,
	const struct is_remote_chunk_mapping_claim_internal *claim,
	const struct is_remote_chunk_mapping_grant *grants,
	unsigned int grant_count)
{
	bool selected[IS_REMOTE_CHUNK_MAX_CHUNKS] = { false };
	unsigned int index;

	for (index = 0; index < grant_count; index++) {
		const struct is_remote_chunk_mapping_grant *grant = &grants[index];
		const struct is_remote_chunk *chunk;
		unsigned int other;

		if (grant->logical_chunk >= module->chunk_count ||
		    !grant->remote_address || !grant->remote_key)
			return -EINVAL;
		if (selected[grant->logical_chunk])
			return -EINVAL;
		selected[grant->logical_chunk] = true;
		chunk = &module->chunks[grant->logical_chunk];
		if (chunk->state != IS_REMOTE_CHUNK_MAPPING ||
		    chunk->mapping_claim_id != claim->claim_id ||
		    chunk->transition_generation != claim->transition_generation ||
		    !is_remote_chunk_provider_handle_equal(chunk->provider,
			claim->provider))
			return -EINVAL;
		for (other = 0; other < index; other++) {
			if (grants[other].provider_chunk == grant->provider_chunk)
				return -EINVAL;
		}
		for (other = 0; other < module->chunk_count; other++) {
			const struct is_remote_chunk *mapped =
				&module->chunks[other];

			if (mapped->state == IS_REMOTE_CHUNK_MAPPED &&
			    mapped->provider_chunk == grant->provider_chunk &&
			    is_remote_chunk_provider_handle_equal(mapped->provider,
				claim->provider))
				return -EINVAL;
		}
	}
	if (is_remote_chunk_batch_digest(selected, module->chunk_count) !=
	    claim->batch_digest)
		return -EINVAL;
	return 0;
}

int is_remote_chunk_mapping_commit(
	struct is_remote_chunk_module *module,
	const struct is_remote_chunk_mapping_claim *claim_storage,
	struct is_remote_chunk_provider_handle provider,
	const struct is_remote_chunk_mapping_grant *grants,
	unsigned int grant_count)
{
	struct is_remote_chunk_mapping_claim_internal claim;
	is_remote_chunk_lock_flags_t flags = 0;
	unsigned int index;
	int status;

	if (!module || !claim_storage || !grants || !grant_count ||
	    grant_count > module->chunk_count)
		return -EINVAL;
	is_remote_chunk_claim_decode(claim_storage, &claim);
	is_remote_chunk_lock(&module->lock, &flags);
	status = is_remote_chunk_claim_validate_locked(module, &claim);
	if (status)
		goto out;
	if (!is_remote_chunk_provider_handle_equal(provider, claim.provider)) {
		status = -ESTALE;
		goto out;
	}
	if (grant_count != claim.chunk_count) {
		status = -EINVAL;
		goto out;
	}
	status = is_remote_chunk_mapping_grants_validate_locked(module, &claim,
		grants, grant_count);
	if (status)
		goto out;
	if (module->next_mapping_generation >
	    IS_REMOTE_CHUNK_ID_MAX - grant_count) {
		status = -EOVERFLOW;
		goto out;
	}

	for (index = 0; index < grant_count; index++) {
		const struct is_remote_chunk_mapping_grant *grant = &grants[index];
		struct is_remote_chunk *chunk =
			&module->chunks[grant->logical_chunk];

		chunk->provider_chunk = grant->provider_chunk;
		chunk->remote_address = grant->remote_address;
		chunk->remote_key = grant->remote_key;
		chunk->mapping_generation = ++module->next_mapping_generation;
		chunk->mapping_claim_id = 0;
		chunk->transition_generation = 0;
		chunk->state = IS_REMOTE_CHUNK_MAPPED;
	}
	module->assigned_chunks += grant_count;
	module->usable_chunks += grant_count;
	module->active_mapping_claims--;
	status = 0;
out:
	is_remote_chunk_unlock(&module->lock, &flags);
	return status;
}

int is_remote_chunk_mapping_abort(
	struct is_remote_chunk_module *module,
	const struct is_remote_chunk_mapping_claim *claim_storage)
{
	struct is_remote_chunk_mapping_claim_internal claim;
	is_remote_chunk_lock_flags_t flags = 0;
	unsigned int index;
	int status;

	if (!module || !claim_storage)
		return -EINVAL;
	is_remote_chunk_claim_decode(claim_storage, &claim);
	is_remote_chunk_lock(&module->lock, &flags);
	status = is_remote_chunk_claim_validate_locked(module, &claim);
	if (status)
		goto out;
	for (index = 0; index < module->chunk_count; index++) {
		struct is_remote_chunk *chunk = &module->chunks[index];

		if (chunk->mapping_claim_id != claim.claim_id)
			continue;
		memset(&chunk->provider, 0, sizeof(chunk->provider));
		chunk->mapping_claim_id = 0;
		chunk->transition_generation = 0;
		chunk->state = IS_REMOTE_CHUNK_UNMAPPED;
	}
	module->active_mapping_claims--;
	status = 0;
out:
	is_remote_chunk_unlock(&module->lock, &flags);
	return status;
}

static unsigned int is_remote_chunk_snapshot_provider_index(
	struct is_remote_chunk_snapshot *snapshot,
	struct is_remote_chunk_provider_handle provider)
{
	unsigned int index;

	for (index = 0; index < snapshot->provider_count; index++) {
		if (is_remote_chunk_provider_handle_equal(
			snapshot->providers[index].provider, provider))
			return index;
	}
	snapshot->providers[snapshot->provider_count].provider = provider;
	return snapshot->provider_count++;
}

int is_remote_chunk_snapshot_take(
	struct is_remote_chunk_module *module,
	struct is_remote_chunk_snapshot **snapshot_out)
{
	struct is_remote_chunk_snapshot *snapshot;
	is_remote_chunk_lock_flags_t flags = 0;
	unsigned int derived_assigned = 0;
	unsigned int derived_usable = 0;
	unsigned int index;

	if (!module || !snapshot_out)
		return -EINVAL;
	*snapshot_out = NULL;
	snapshot = is_remote_chunk_allocate(sizeof(*snapshot));
	if (!snapshot)
		return -ENOMEM;
	snapshot->providers = is_remote_chunk_allocate_array(module->chunk_count,
		sizeof(*snapshot->providers));
	snapshot->placements = is_remote_chunk_allocate_array(module->chunk_count,
		sizeof(*snapshot->placements));
	if (!snapshot->providers || !snapshot->placements) {
		is_remote_chunk_snapshot_release(snapshot);
		return -ENOMEM;
	}

	is_remote_chunk_lock(&module->lock, &flags);
	snapshot->mode = module->mode;
	snapshot->chunk_count = module->chunk_count;
	snapshot->assigned_chunks = module->assigned_chunks;
	snapshot->usable_chunks = module->usable_chunks;
	snapshot->active_mapping_claims = module->active_mapping_claims;
	snapshot->hot_policy = module->hot_policy;
	for (index = 0; index < module->chunk_count; index++) {
		const struct is_remote_chunk *chunk = &module->chunks[index];

		if (chunk->activity >= module->hot_policy.threshold) {
			snapshot->hot_ranges++;
			if (chunk->state == IS_REMOTE_CHUNK_UNMAPPED)
				snapshot->mapping_candidates++;
		}
		if (chunk->state == IS_REMOTE_CHUNK_MAPPING) {
			snapshot->mapping_chunks++;
		} else if (chunk->state == IS_REMOTE_CHUNK_MAPPED) {
			struct is_remote_chunk_placement_snapshot *placement =
				&snapshot->placements[snapshot->placement_count++];
			unsigned int provider_index =
				is_remote_chunk_snapshot_provider_index(snapshot,
					chunk->provider);

			placement->provider = chunk->provider;
			placement->logical_chunk = index;
			placement->usable = true;
			snapshot->providers[provider_index].assigned_chunks++;
			snapshot->providers[provider_index].usable_chunks++;
			derived_assigned++;
			derived_usable++;
		}
	}
	if (derived_assigned != module->assigned_chunks ||
	    derived_usable != module->usable_chunks) {
		is_remote_chunk_unlock(&module->lock, &flags);
		is_remote_chunk_snapshot_release(snapshot);
		return -EINVAL;
	}
	is_remote_chunk_unlock(&module->lock, &flags);
	*snapshot_out = snapshot;
	return 0;
}

void is_remote_chunk_snapshot_release(struct is_remote_chunk_snapshot *snapshot)
{
	if (!snapshot)
		return;
	is_remote_chunk_free(snapshot->providers);
	is_remote_chunk_free(snapshot->placements);
	is_remote_chunk_free(snapshot);
}
