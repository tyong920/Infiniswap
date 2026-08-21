/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#include "is_remote_chunk.h"

#define IS_REMOTE_CHUNK_ID_MAX (~0ULL)

#ifdef __KERNEL__
#include <linux/atomic.h>
#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>

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

static unsigned long *is_remote_chunk_validity_allocate(size_t word_count)
{
	return kvcalloc(word_count, sizeof(unsigned long), GFP_KERNEL);
}

static void is_remote_chunk_validity_free(unsigned long *validity)
{
	kvfree(validity);
}

static unsigned long long is_remote_chunk_allocate_module_identity(void)
{
	s64 previous = atomic64_read(&is_remote_chunk_next_module_identity);

	for (;;) {
		unsigned long long previous_value = (unsigned long long)previous;
		unsigned long long next_value;
		s64 observed;

		if (previous_value >= IS_REMOTE_CHUNK_ID_MAX - 1ULL)
			return 0;
		next_value = previous_value + 1ULL;
		observed = atomic64_cmpxchg(&is_remote_chunk_next_module_identity,
			previous, (s64)next_value);
		if (observed == previous)
			return next_value;
		previous = observed;
	}
}
#else
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static unsigned long *is_remote_chunk_validity_allocate(size_t word_count)
{
	if (word_count > SIZE_MAX / sizeof(unsigned long))
		return NULL;
	return calloc(word_count, sizeof(unsigned long));
}

static void is_remote_chunk_validity_free(unsigned long *validity)
{
	free(validity);
}

static unsigned long long is_remote_chunk_allocate_module_identity(void)
{
	unsigned long long previous = atomic_load_explicit(
		&is_remote_chunk_next_module_identity, memory_order_relaxed);

	for (;;) {
		unsigned long long next;

		if (previous >= IS_REMOTE_CHUNK_ID_MAX - 1ULL)
			return 0;
		next = previous + 1ULL;
		if (atomic_compare_exchange_weak_explicit(
			&is_remote_chunk_next_module_identity, &previous, next,
			memory_order_relaxed, memory_order_relaxed))
			return next;
	}
}
#endif

#define IS_REMOTE_CHUNK_CLAIM_MAGIC 0x49534348434c4149ULL
#define IS_REMOTE_CHUNK_EVICTION_MAGIC 0x4953434845564943ULL
#define IS_REMOTE_CHUNK_LEASE_MAGIC 0x495343484c454153ULL
#define IS_REMOTE_CHUNK_ACTIVITY_MAX (~0ULL >> 1)
#define IS_REMOTE_CHUNK_DIGEST_OFFSET 1469598103934665603ULL
#define IS_REMOTE_CHUNK_DIGEST_PRIME 1099511628211ULL
#define IS_REMOTE_CHUNK_BITS_PER_WORD (sizeof(unsigned long) * 8U)
#define IS_REMOTE_CHUNK_LEASE_RESOLVED (1U << 0)
#define IS_REMOTE_CHUNK_LEASE_RELEASED (1U << 1)
#define IS_REMOTE_CHUNK_WAIT_SLICE_NS 1000000000ULL

enum is_remote_chunk_state {
	IS_REMOTE_CHUNK_UNMAPPED = 0,
	IS_REMOTE_CHUNK_MAPPING,
	IS_REMOTE_CHUNK_MAPPED,
	IS_REMOTE_CHUNK_EVICTING,
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
	unsigned long long eviction_claim_id;
	unsigned long long transition_generation;
	unsigned int active_io_leases;
};

struct is_remote_chunk_provider {
	struct is_remote_chunk_provider_handle handle;
	char identifier[IS_REMOTE_CHUNK_PROVIDER_ID_MAX + 1U];
	unsigned int placement_weight;
	unsigned int reported_available_chunks;
	unsigned int observation_assigned_chunks;
	unsigned long long observation_sequence;
	enum is_remote_chunk_provider_exclusion exclusion;
	unsigned char placement_eligible;
	unsigned char has_observation;
	unsigned char failed;
};

struct is_remote_chunk_module {
	is_remote_chunk_lock_t lock;
#ifdef __KERNEL__
	wait_queue_head_t lease_wait;
#else
	pthread_cond_t lease_changed;
#endif
	struct is_remote_chunk *chunks;
	struct is_remote_chunk_provider *providers;
	unsigned int *placement_candidate_indices;
	unsigned int *placement_sample_indices;
	unsigned long *valid_sectors;
	enum is_remote_chunk_mode mode;
	unsigned int chunk_count;
	unsigned int provider_count;
	unsigned int placement_sample_size;
	unsigned long long placement_seed;
	struct is_remote_chunk_hot_policy hot_policy;
	unsigned long long identity;
	unsigned long long next_mapping_claim_id;
	unsigned long long next_eviction_claim_id;
	unsigned long long next_transition_generation;
	unsigned long long next_mapping_generation;
	unsigned long long next_io_lease_id;
	unsigned long long invariant_count;
	enum is_remote_chunk_invariant_event latest_invariant;
	unsigned int active_mapping_claims;
	unsigned int active_eviction_claims;
	unsigned int active_eviction_waits;
	unsigned int active_io_leases;
	unsigned int assigned_chunks;
	unsigned int usable_chunks;
	unsigned char quiescing;
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

struct is_remote_chunk_eviction_claim_internal {
	unsigned long long magic;
	unsigned long long module_identity;
	struct is_remote_chunk_provider_handle provider;
	unsigned long long claim_id;
	unsigned long long transition_generation;
	unsigned long long batch_digest;
	unsigned int chunk_count;
	unsigned int reserved;
};

struct is_remote_chunk_io_lease_internal {
	unsigned long long magic;
	unsigned long long module_identity;
	unsigned long long storage_identity;
	unsigned long long lease_id;
	unsigned long long mapping_generation;
	unsigned long long sector;
	unsigned int sector_count;
	unsigned int logical_chunk;
	unsigned int direction;
	unsigned int state;
};

static int is_remote_chunk_wait_init(struct is_remote_chunk_module *module)
{
#ifdef __KERNEL__
	init_waitqueue_head(&module->lease_wait);
	return 0;
#else
	int status = pthread_cond_init(&module->lease_changed, NULL);

	return status ? -status : 0;
#endif
}

static void is_remote_chunk_wait_destroy(struct is_remote_chunk_module *module)
{
#ifdef __KERNEL__
	(void)module;
#else
	(void)pthread_cond_destroy(&module->lease_changed);
#endif
}

#ifdef __KERNEL__
static_assert(sizeof(struct is_remote_chunk_mapping_claim_internal) <=
	IS_REMOTE_CHUNK_MAPPING_CLAIM_BYTES);
static_assert(sizeof(struct is_remote_chunk_eviction_claim_internal) <=
	IS_REMOTE_CHUNK_EVICTION_CLAIM_BYTES);
static_assert(sizeof(struct is_remote_chunk_io_lease_internal) <=
	IS_REMOTE_CHUNK_IO_LEASE_BYTES);
#else
_Static_assert(sizeof(struct is_remote_chunk_mapping_claim_internal) <=
	IS_REMOTE_CHUNK_MAPPING_CLAIM_BYTES,
	"mapping claim storage is too small");
_Static_assert(sizeof(struct is_remote_chunk_eviction_claim_internal) <=
	IS_REMOTE_CHUNK_EVICTION_CLAIM_BYTES,
	"eviction claim storage is too small");
_Static_assert(sizeof(struct is_remote_chunk_io_lease_internal) <=
	IS_REMOTE_CHUNK_IO_LEASE_BYTES,
	"I/O lease storage is too small");
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

static bool is_remote_chunk_identifier_character_valid(unsigned char character,
	bool first)
{
	bool alpha_numeric = (character >= 'a' && character <= 'z') ||
		(character >= 'A' && character <= 'Z') ||
		(character >= '0' && character <= '9');

	return alpha_numeric || (!first &&
		(character == '.' || character == '_' || character == '-'));
}

static bool is_remote_chunk_provider_identifier_valid(const char *identifier)
{
	unsigned int index;

	if (!identifier || !identifier[0])
		return false;
	for (index = 0; index <= IS_REMOTE_CHUNK_PROVIDER_ID_MAX; index++) {
		unsigned char character = (unsigned char)identifier[index];

		if (!character)
			return true;
		if (!is_remote_chunk_identifier_character_valid(character,
			index == 0))
			return false;
	}
	return false;
}

static int is_remote_chunk_roster_validate(
	const struct is_remote_chunk_config *config)
{
	unsigned int index;

	if (config->provider_count > IS_REMOTE_CHUNK_MAX_PROVIDERS ||
	    (config->provider_count && !config->providers))
		return -EINVAL;
	if (!config->placement_sample_size ||
	    config->placement_sample_size > IS_REMOTE_CHUNK_MAX_PROVIDERS)
		return -ERANGE;
	if (!config->provider_count) {
		if (config->mode == IS_REMOTE_CHUNK_MODE_REMOTE_ONLY)
			return -EINVAL;
		return 0;
	}
	if (config->placement_sample_size > config->provider_count)
		return -ERANGE;
	for (index = 0; index < config->provider_count; index++) {
		const struct is_remote_chunk_provider_config *provider =
			&config->providers[index];
		unsigned int previous;

		if (!is_remote_chunk_provider_identifier_valid(provider->identifier))
			return -EINVAL;
		if (provider->placement_weight <
				IS_REMOTE_CHUNK_PLACEMENT_WEIGHT_MIN ||
		    provider->placement_weight >
				IS_REMOTE_CHUNK_PLACEMENT_WEIGHT_MAX)
			return -ERANGE;
		for (previous = 0; previous < index; previous++) {
			if (!strcmp(config->providers[previous].identifier,
				provider->identifier))
				return -EINVAL;
		}
	}
	return 0;
}

bool is_remote_chunk_provider_handle_equal(
	struct is_remote_chunk_provider_handle left,
	struct is_remote_chunk_provider_handle right)
{
	return left.opaque[0] == right.opaque[0] &&
		left.opaque[1] == right.opaque[1];
}

static int is_remote_chunk_provider_index_locked(
	const struct is_remote_chunk_module *module,
	struct is_remote_chunk_provider_handle provider,
	unsigned int *provider_index_out)
{
	unsigned int index;

	for (index = 0; index < module->provider_count; index++) {
		const struct is_remote_chunk_provider *candidate =
			&module->providers[index];

		if (!is_remote_chunk_provider_handle_equal(candidate->handle,
			provider))
			continue;
		if (candidate->failed)
			return -ESTALE;
		if (provider_index_out)
			*provider_index_out = index;
		return 0;
	}
	return -ESTALE;
}

static bool is_remote_chunk_provider_valid_locked(
	const struct is_remote_chunk_module *module,
	struct is_remote_chunk_provider_handle provider)
{
	return is_remote_chunk_provider_index_locked(module, provider,
		NULL) == 0;
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

static void is_remote_chunk_eviction_claim_decode(
	const struct is_remote_chunk_eviction_claim *claim,
	struct is_remote_chunk_eviction_claim_internal *internal)
{
	memset(internal, 0, sizeof(*internal));
	if (claim)
		memcpy(internal, claim->opaque.bytes, sizeof(*internal));
}

static void is_remote_chunk_eviction_claim_encode(
	struct is_remote_chunk_eviction_claim *claim,
	const struct is_remote_chunk_eviction_claim_internal *internal)
{
	memset(claim, 0, sizeof(*claim));
	memcpy(claim->opaque.bytes, internal, sizeof(*internal));
}

static void is_remote_chunk_lease_decode(
	const struct is_remote_chunk_io_lease *lease,
	struct is_remote_chunk_io_lease_internal *internal)
{
	memset(internal, 0, sizeof(*internal));
	if (lease)
		memcpy(internal, lease->opaque.bytes, sizeof(*internal));
}

static void is_remote_chunk_lease_encode(
	struct is_remote_chunk_io_lease *lease,
	const struct is_remote_chunk_io_lease_internal *internal)
{
	memset(lease, 0, sizeof(*lease));
	memcpy(lease->opaque.bytes, internal, sizeof(*internal));
}

static unsigned long long is_remote_chunk_lease_storage_identity(
	const struct is_remote_chunk_io_lease *lease)
{
	return (unsigned long long)(unsigned long)lease;
}

static void is_remote_chunk_wake_lease_waiters_locked(
	struct is_remote_chunk_module *module)
{
#ifdef __KERNEL__
	wake_up_all(&module->lease_wait);
#else
	(void)pthread_cond_broadcast(&module->lease_changed);
#endif
}

static unsigned long long is_remote_chunk_monotonic_ns(void)
{
#ifdef __KERNEL__
	return ktime_get_ns();
#else
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return IS_REMOTE_CHUNK_ID_MAX;
	return (unsigned long long)now.tv_sec * 1000000000ULL +
		(unsigned long long)now.tv_nsec;
#endif
}

static unsigned long is_remote_chunk_low_bits(unsigned int bit_count)
{
	if (bit_count >= IS_REMOTE_CHUNK_BITS_PER_WORD)
		return ~0UL;
	return (1UL << bit_count) - 1UL;
}

static unsigned long is_remote_chunk_word_range_mask(unsigned int first_bit,
	unsigned int last_bit)
{
	return is_remote_chunk_low_bits(last_bit) &
		~is_remote_chunk_low_bits(first_bit);
}

enum is_remote_chunk_validity_operation {
	IS_REMOTE_CHUNK_VALIDITY_TEST = 0,
	IS_REMOTE_CHUNK_VALIDITY_CLEAR,
	IS_REMOTE_CHUNK_VALIDITY_SET,
};

static bool is_remote_chunk_validity_apply(unsigned long *validity,
	unsigned long long first_sector, unsigned int sector_count,
	enum is_remote_chunk_validity_operation operation)
{
	unsigned long long last_sector = first_sector + sector_count;
	unsigned long long first_word =
		first_sector / IS_REMOTE_CHUNK_BITS_PER_WORD;
	unsigned long long last_word =
		(last_sector - 1ULL) / IS_REMOTE_CHUNK_BITS_PER_WORD;
	unsigned long long word;

	for (word = first_word; word <= last_word; word++) {
		unsigned long long word_first =
			word * IS_REMOTE_CHUNK_BITS_PER_WORD;
		unsigned int first_bit = first_sector > word_first ?
			(unsigned int)(first_sector - word_first) : 0;
		unsigned int last_bit = last_sector <
				word_first + IS_REMOTE_CHUNK_BITS_PER_WORD ?
			(unsigned int)(last_sector - word_first) :
			IS_REMOTE_CHUNK_BITS_PER_WORD;
		unsigned long mask = is_remote_chunk_word_range_mask(first_bit,
			last_bit);

		if (operation == IS_REMOTE_CHUNK_VALIDITY_TEST) {
			if ((validity[word] & mask) != mask)
				return false;
		} else if (operation == IS_REMOTE_CHUNK_VALIDITY_SET) {
			validity[word] |= mask;
		} else {
			validity[word] &= ~mask;
		}
	}
	return true;
}

static void is_remote_chunk_validity_update(unsigned long *validity,
	unsigned long long first_sector, unsigned int sector_count, bool valid)
{
	(void)is_remote_chunk_validity_apply(validity, first_sector, sector_count,
		valid ? IS_REMOTE_CHUNK_VALIDITY_SET :
		IS_REMOTE_CHUNK_VALIDITY_CLEAR);
}

static bool is_remote_chunk_validity_test(unsigned long *validity,
	unsigned long long first_sector, unsigned int sector_count)
{
	return is_remote_chunk_validity_apply(validity, first_sector, sector_count,
		IS_REMOTE_CHUNK_VALIDITY_TEST);
}

static int is_remote_chunk_lease_validate_locked(
	const struct is_remote_chunk_module *module,
	const struct is_remote_chunk_io_lease *lease,
	const struct is_remote_chunk_io_lease_internal *internal)
{
	if (internal->magic != IS_REMOTE_CHUNK_LEASE_MAGIC ||
	    !internal->lease_id || !internal->mapping_generation ||
	    !internal->sector_count ||
	    internal->storage_identity !=
		is_remote_chunk_lease_storage_identity(lease))
		return -EINVAL;
	if (internal->module_identity != module->identity)
		return -ESTALE;
	if (internal->lease_id > module->next_io_lease_id ||
	    internal->mapping_generation > module->next_mapping_generation ||
	    internal->logical_chunk >= module->chunk_count ||
	    internal->sector / IS_REMOTE_CHUNK_SECTORS_PER_CHUNK !=
		internal->logical_chunk ||
	    internal->sector_count > IS_REMOTE_CHUNK_SECTORS_PER_CHUNK -
		(internal->sector % IS_REMOTE_CHUNK_SECTORS_PER_CHUNK) ||
	    (internal->direction != IS_REMOTE_CHUNK_IO_READ &&
	     internal->direction != IS_REMOTE_CHUNK_IO_WRITE) ||
	    (internal->state & ~(IS_REMOTE_CHUNK_LEASE_RESOLVED |
		IS_REMOTE_CHUNK_LEASE_RELEASED)) ||
	    ((internal->state & IS_REMOTE_CHUNK_LEASE_RELEASED) &&
	     !(internal->state & IS_REMOTE_CHUNK_LEASE_RESOLVED)))
		return -EINVAL;
	return 0;
}

static void is_remote_chunk_note_invariant_locked(
	struct is_remote_chunk_module *module,
	enum is_remote_chunk_invariant_event event)
{
	if (module->invariant_count != IS_REMOTE_CHUNK_ID_MAX)
		module->invariant_count++;
	module->latest_invariant = event;
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

static int is_remote_chunk_eviction_claim_validate_locked(
	const struct is_remote_chunk_module *module,
	const struct is_remote_chunk_eviction_claim_internal *claim)
{
	bool selected[IS_REMOTE_CHUNK_MAX_CHUNKS] = { false };
	unsigned int member_count = 0;
	unsigned int index;

	if (claim->magic != IS_REMOTE_CHUNK_EVICTION_MAGIC || !claim->claim_id ||
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

		if (chunk->eviction_claim_id != claim->claim_id)
			continue;
		if (chunk->state != IS_REMOTE_CHUNK_EVICTING ||
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

static bool is_remote_chunk_eviction_claim_active_locked(
	const struct is_remote_chunk_module *module,
	const struct is_remote_chunk_eviction_claim *claim)
{
	struct is_remote_chunk_eviction_claim_internal internal;

	is_remote_chunk_eviction_claim_decode(claim, &internal);
	return is_remote_chunk_eviction_claim_validate_locked(module,
		&internal) == 0;
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

	if (module->quiescing)
		return -ESHUTDOWN;
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
	struct is_remote_chunk_provider_handle *provider_handles_out,
	unsigned int provider_handle_capacity,
	struct is_remote_chunk_module **module_out)
{
	struct is_remote_chunk_module *module;
	unsigned int index;
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
	status = is_remote_chunk_roster_validate(config);
	if (status)
		return status;
	if (config->provider_count &&
	    (!provider_handles_out ||
	     provider_handle_capacity < config->provider_count))
		return -ENOSPC;

	module = is_remote_chunk_allocate(sizeof(*module));
	if (!module)
		return -ENOMEM;
	module->chunks = is_remote_chunk_allocate_array(config->chunk_count,
		sizeof(*module->chunks));
	if (!module->chunks) {
		status = -ENOMEM;
		goto free_module;
	}
	if (config->provider_count) {
		module->providers = is_remote_chunk_allocate_array(
			config->provider_count, sizeof(*module->providers));
		module->placement_candidate_indices = is_remote_chunk_allocate_array(
			config->provider_count,
			sizeof(*module->placement_candidate_indices));
		module->placement_sample_indices = is_remote_chunk_allocate_array(
			config->provider_count, sizeof(*module->placement_sample_indices));
		if (!module->providers || !module->placement_candidate_indices ||
		    !module->placement_sample_indices) {
			status = -ENOMEM;
			goto free_provider_storage;
		}
	}
	module->valid_sectors = is_remote_chunk_validity_allocate(
		(size_t)config->chunk_count * IS_REMOTE_CHUNK_SECTORS_PER_CHUNK /
		IS_REMOTE_CHUNK_BITS_PER_WORD);
	if (!module->valid_sectors) {
		status = -ENOMEM;
		goto free_provider_storage;
	}
	status = is_remote_chunk_lock_init(&module->lock);
	if (status) {
		status = status < 0 ? status : -status;
		goto free_validity;
	}
	status = is_remote_chunk_wait_init(module);
	if (status)
		goto destroy_lock;
	module->identity = is_remote_chunk_allocate_module_identity();
	if (!module->identity || module->identity == IS_REMOTE_CHUNK_ID_MAX) {
		status = -EOVERFLOW;
		goto destroy_wait;
	}
	module->mode = config->mode;
	module->chunk_count = config->chunk_count;
	module->provider_count = config->provider_count;
	module->placement_sample_size = config->placement_sample_size;
	module->placement_seed = config->placement_seed;
	module->hot_policy = config->hot_policy;
	for (index = 0; index < config->provider_count; index++) {
		struct is_remote_chunk_provider *provider =
			&module->providers[index];

		provider->handle.opaque[0] = module->identity;
		provider->handle.opaque[1] = (unsigned long long)index + 1ULL;
		strcpy(provider->identifier, config->providers[index].identifier);
		provider->placement_weight =
			config->providers[index].placement_weight;
	}

	/* Caller-visible state is the final step of the all-or-nothing create. */
	for (index = 0; index < config->provider_count; index++)
		provider_handles_out[index] = module->providers[index].handle;
	*module_out = module;
	return 0;

destroy_wait:
	is_remote_chunk_wait_destroy(module);
destroy_lock:
	is_remote_chunk_lock_destroy(&module->lock);
free_validity:
	is_remote_chunk_validity_free(module->valid_sectors);
free_provider_storage:
	is_remote_chunk_free(module->placement_sample_indices);
	is_remote_chunk_free(module->placement_candidate_indices);
	is_remote_chunk_free(module->providers);
	is_remote_chunk_free(module->chunks);
free_module:
	is_remote_chunk_free(module);
	return status;
}

int is_remote_chunk_module_quiesce(struct is_remote_chunk_module *module)
{
	is_remote_chunk_lock_flags_t flags = 0;
	int status = 0;

	if (!module)
		return -EINVAL;
	is_remote_chunk_lock(&module->lock, &flags);
	if (module->quiescing) {
		is_remote_chunk_note_invariant_locked(module,
			IS_REMOTE_CHUNK_INVARIANT_MODULE_DUPLICATE_QUIESCE);
		status = -EALREADY;
	} else {
		module->quiescing = 1;
	}
	is_remote_chunk_unlock(&module->lock, &flags);
	return status;
}

int is_remote_chunk_module_destroy(struct is_remote_chunk_module *module)
{
	is_remote_chunk_lock_flags_t flags = 0;

	if (!module)
		return -EINVAL;
	is_remote_chunk_lock(&module->lock, &flags);
	module->quiescing = 1;
	if (module->active_mapping_claims || module->active_eviction_claims ||
	    module->active_eviction_waits || module->active_io_leases) {
		is_remote_chunk_note_invariant_locked(module,
			IS_REMOTE_CHUNK_INVARIANT_MODULE_DESTROY_ACTIVE);
		is_remote_chunk_unlock(&module->lock, &flags);
		return -EBUSY;
	}
	is_remote_chunk_unlock(&module->lock, &flags);
	is_remote_chunk_wait_destroy(module);
	is_remote_chunk_lock_destroy(&module->lock);
	is_remote_chunk_validity_free(module->valid_sectors);
	is_remote_chunk_free(module->placement_sample_indices);
	is_remote_chunk_free(module->placement_candidate_indices);
	is_remote_chunk_free(module->providers);
	is_remote_chunk_free(module->chunks);
	is_remote_chunk_free(module);
	return 0;
}

static bool is_remote_chunk_provider_observation_valid(
	const struct is_remote_chunk_provider_observation *observation)
{
	if (!observation ||
	    observation->exclusion < IS_REMOTE_CHUNK_PROVIDER_EXCLUDE_NONE ||
	    observation->exclusion >
		IS_REMOTE_CHUNK_PROVIDER_EXCLUDE_ZERO_CAPACITY)
		return false;
	if (observation->placement_eligible)
		return observation->available_chunks > 0 &&
			observation->exclusion ==
				IS_REMOTE_CHUNK_PROVIDER_EXCLUDE_NONE;
	if (observation->exclusion == IS_REMOTE_CHUNK_PROVIDER_EXCLUDE_NONE)
		return false;
	if (observation->exclusion ==
		IS_REMOTE_CHUNK_PROVIDER_EXCLUDE_ZERO_CAPACITY)
		return observation->available_chunks == 0;
	return true;
}

static bool is_remote_chunk_provider_observation_equal(
	const struct is_remote_chunk_provider *provider,
	const struct is_remote_chunk_provider_observation *observation)
{
	return provider->observation_sequence == observation->sequence &&
		provider->reported_available_chunks ==
			observation->available_chunks &&
		provider->exclusion == observation->exclusion &&
		(provider->placement_eligible != 0) ==
			observation->placement_eligible;
}

enum is_remote_chunk_provider_observation_result
is_remote_chunk_provider_observe(
	struct is_remote_chunk_module *module,
	struct is_remote_chunk_provider_handle provider_handle,
	const struct is_remote_chunk_provider_observation *observation)
{
	is_remote_chunk_lock_flags_t flags = 0;
	struct is_remote_chunk_provider *provider;
	unsigned int provider_index;
	unsigned int assigned_chunks = 0;
	unsigned int index;
	enum is_remote_chunk_provider_observation_result result;

	if (!module || !is_remote_chunk_provider_observation_valid(observation))
		return IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_INVALID;
	is_remote_chunk_lock(&module->lock, &flags);
	if (module->quiescing) {
		result = IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_SHUTDOWN;
		goto out;
	}
	if (is_remote_chunk_provider_index_locked(module, provider_handle,
		&provider_index)) {
		result = IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_STALE_EPOCH;
		goto out;
	}
	provider = &module->providers[provider_index];
	if (provider->has_observation &&
	    observation->sequence < provider->observation_sequence) {
		result = IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_STALE;
		goto out;
	}
	if (provider->has_observation &&
	    observation->sequence == provider->observation_sequence) {
		result = is_remote_chunk_provider_observation_equal(provider,
			observation) ?
			IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_DUPLICATE :
			IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_CONFLICT;
		goto out;
	}
	for (index = 0; index < module->chunk_count; index++) {
		const struct is_remote_chunk *chunk = &module->chunks[index];

		if ((chunk->state == IS_REMOTE_CHUNK_MAPPED ||
		     chunk->state == IS_REMOTE_CHUNK_EVICTING) &&
		    is_remote_chunk_provider_handle_equal(chunk->provider,
			provider_handle))
			assigned_chunks++;
	}
	provider->reported_available_chunks = observation->available_chunks;
	provider->observation_assigned_chunks = assigned_chunks;
	provider->observation_sequence = observation->sequence;
	provider->exclusion = observation->exclusion;
	provider->placement_eligible = observation->placement_eligible;
	provider->has_observation = 1;
	result = IS_REMOTE_CHUNK_PROVIDER_OBSERVATION_APPLIED;
out:
	is_remote_chunk_unlock(&module->lock, &flags);
	return result;
}

int is_remote_chunk_hot_policy_snapshot(
	struct is_remote_chunk_module *module,
	struct is_remote_chunk_hot_policy *policy_out)
{
	is_remote_chunk_lock_flags_t flags = 0;

	if (!module || !policy_out)
		return -EINVAL;
	is_remote_chunk_lock(&module->lock, &flags);
	*policy_out = module->hot_policy;
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
	if (module->quiescing) {
		is_remote_chunk_unlock(&module->lock, &flags);
		return -ESHUTDOWN;
	}
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
	if (module->quiescing) {
		is_remote_chunk_unlock(&module->lock, &flags);
		return -ESHUTDOWN;
	}
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
	if (module->quiescing) {
		is_remote_chunk_unlock(&module->lock, &flags);
		return -ESHUTDOWN;
	}
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
		    !grant->remote_address || !grant->remote_key ||
		    grant->remote_address > IS_REMOTE_CHUNK_ID_MAX -
			(IS_REMOTE_CHUNK_SECTORS_PER_CHUNK *
			 IS_REMOTE_CHUNK_SECTOR_BYTES - 1ULL))
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
	if (module->quiescing) {
		status = -ESHUTDOWN;
		goto out;
	}
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
		is_remote_chunk_validity_update(module->valid_sectors,
			(unsigned long long)grant->logical_chunk *
				IS_REMOTE_CHUNK_SECTORS_PER_CHUNK,
			(unsigned int)IS_REMOTE_CHUNK_SECTORS_PER_CHUNK, false);
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

static int is_remote_chunk_find_provider_chunk_locked(
	const struct is_remote_chunk_module *module,
	struct is_remote_chunk_provider_handle provider,
	unsigned int provider_chunk, bool require_usable,
	unsigned int *logical_chunk_out)
{
	unsigned int index;

	for (index = 0; index < module->chunk_count; index++) {
		const struct is_remote_chunk *chunk = &module->chunks[index];
		bool assigned = chunk->state == IS_REMOTE_CHUNK_MAPPED ||
			chunk->state == IS_REMOTE_CHUNK_EVICTING;

		if (assigned && (!require_usable ||
			chunk->state == IS_REMOTE_CHUNK_MAPPED) &&
		    chunk->provider_chunk == provider_chunk &&
		    is_remote_chunk_provider_handle_equal(chunk->provider, provider)) {
			*logical_chunk_out = index;
			return 0;
		}
	}
	return -ENOENT;
}

int is_remote_chunk_provider_activity_query(
	struct is_remote_chunk_module *module,
	struct is_remote_chunk_provider_handle provider,
	const unsigned int *provider_chunks, unsigned int chunk_count,
	struct is_remote_chunk_provider_activity *activity_out)
{
	unsigned char logical_chunks[IS_REMOTE_CHUNK_MAX_CHUNKS];
	is_remote_chunk_lock_flags_t flags = 0;
	unsigned int index;
	int status = 0;

	if (!module || !provider_chunks || !chunk_count || !activity_out ||
	    chunk_count > module->chunk_count)
		return -EINVAL;
	is_remote_chunk_lock(&module->lock, &flags);
	if (module->mode != IS_REMOTE_CHUNK_MODE_BACKED) {
		status = -EOPNOTSUPP;
		goto out;
	}
	if (!is_remote_chunk_provider_valid_locked(module, provider)) {
		status = -ESTALE;
		goto out;
	}
	for (index = 0; index < chunk_count; index++) {
		unsigned int logical_chunk;
		unsigned int other;

		for (other = 0; other < index; other++) {
			if (provider_chunks[other] == provider_chunks[index]) {
				status = -EINVAL;
				goto out;
			}
		}
		status = is_remote_chunk_find_provider_chunk_locked(module, provider,
			provider_chunks[index], false, &logical_chunk);
		if (status)
			goto out;
		logical_chunks[index] = (unsigned char)logical_chunk;
	}
	for (index = 0; index < chunk_count; index++) {
		activity_out[index].provider_chunk = provider_chunks[index];
		activity_out[index].activity =
			module->chunks[logical_chunks[index]].activity;
	}
out:
	is_remote_chunk_unlock(&module->lock, &flags);
	return status;
}

int is_remote_chunk_eviction_begin(
	struct is_remote_chunk_module *module,
	struct is_remote_chunk_provider_handle provider,
	const unsigned int *provider_chunks, unsigned int chunk_count,
	struct is_remote_chunk_eviction_claim *claim_out)
{
	struct is_remote_chunk_eviction_claim_internal claim = { 0 };
	bool selected[IS_REMOTE_CHUNK_MAX_CHUNKS] = { false };
	is_remote_chunk_lock_flags_t flags = 0;
	unsigned int index;
	int status = 0;

	if (!module || !provider_chunks || !chunk_count || !claim_out ||
	    chunk_count > module->chunk_count)
		return -EINVAL;
	is_remote_chunk_lock(&module->lock, &flags);
	if (module->mode != IS_REMOTE_CHUNK_MODE_BACKED) {
		status = -EOPNOTSUPP;
		goto out;
	}
	if (module->quiescing) {
		status = -ESHUTDOWN;
		goto out;
	}
	if (!is_remote_chunk_provider_valid_locked(module, provider)) {
		status = -ESTALE;
		goto out;
	}
	if (is_remote_chunk_eviction_claim_active_locked(module, claim_out)) {
		status = -EBUSY;
		goto out;
	}
	for (index = 0; index < chunk_count; index++) {
		unsigned int logical_chunk;

		status = is_remote_chunk_find_provider_chunk_locked(module, provider,
			provider_chunks[index], true, &logical_chunk);
		if (status)
			goto out;
		if (selected[logical_chunk]) {
			status = -EINVAL;
			goto out;
		}
		selected[logical_chunk] = true;
	}
	if (module->next_eviction_claim_id == IS_REMOTE_CHUNK_ID_MAX ||
	    module->next_transition_generation == IS_REMOTE_CHUNK_ID_MAX ||
	    module->usable_chunks < chunk_count ||
	    module->active_eviction_claims == ~0U) {
		status = -EOVERFLOW;
		goto out;
	}

	claim.magic = IS_REMOTE_CHUNK_EVICTION_MAGIC;
	claim.module_identity = module->identity;
	claim.provider = provider;
	claim.claim_id = ++module->next_eviction_claim_id;
	claim.transition_generation = ++module->next_transition_generation;
	claim.batch_digest = is_remote_chunk_batch_digest(selected,
		module->chunk_count);
	claim.chunk_count = chunk_count;
	for (index = 0; index < module->chunk_count; index++) {
		struct is_remote_chunk *chunk;

		if (!selected[index])
			continue;
		chunk = &module->chunks[index];
		chunk->state = IS_REMOTE_CHUNK_EVICTING;
		chunk->eviction_claim_id = claim.claim_id;
		chunk->transition_generation = claim.transition_generation;
	}
	module->usable_chunks -= chunk_count;
	module->active_eviction_claims++;
	is_remote_chunk_eviction_claim_encode(claim_out, &claim);
out:
	is_remote_chunk_unlock(&module->lock, &flags);
	return status;
}

static bool is_remote_chunk_eviction_drained_locked(
	const struct is_remote_chunk_module *module,
	const struct is_remote_chunk_eviction_claim_internal *claim)
{
	unsigned int index;

	for (index = 0; index < module->chunk_count; index++) {
		const struct is_remote_chunk *chunk = &module->chunks[index];

		if (chunk->eviction_claim_id == claim->claim_id &&
		    chunk->active_io_leases)
			return false;
	}
	return true;
}

#ifdef __KERNEL__
static bool is_remote_chunk_eviction_wait_ready(
	struct is_remote_chunk_module *module,
	const struct is_remote_chunk_eviction_claim *claim_storage)
{
	struct is_remote_chunk_eviction_claim_internal claim;
	is_remote_chunk_lock_flags_t flags = 0;
	bool ready;

	is_remote_chunk_eviction_claim_decode(claim_storage, &claim);
	is_remote_chunk_lock(&module->lock, &flags);
	ready = is_remote_chunk_eviction_claim_validate_locked(module, &claim) != 0 ||
		is_remote_chunk_eviction_drained_locked(module, &claim);
	is_remote_chunk_unlock(&module->lock, &flags);
	return ready;
}
#endif

int is_remote_chunk_eviction_wait(
	struct is_remote_chunk_module *module,
	const struct is_remote_chunk_eviction_claim *claim_storage,
	unsigned long long deadline_monotonic_ns)
{
	struct is_remote_chunk_eviction_claim_internal claim;
	is_remote_chunk_lock_flags_t flags = 0;
	int status;

	if (!module || !claim_storage || !deadline_monotonic_ns)
		return -EINVAL;
	is_remote_chunk_eviction_claim_decode(claim_storage, &claim);
	is_remote_chunk_lock(&module->lock, &flags);
	status = is_remote_chunk_eviction_claim_validate_locked(module, &claim);
	if (status)
		goto out;
	if (module->active_eviction_waits == ~0U) {
		status = -EOVERFLOW;
		goto out;
	}
	module->active_eviction_waits++;
	for (;;) {
		unsigned long long now;

		status = is_remote_chunk_eviction_claim_validate_locked(module, &claim);
		if (status || is_remote_chunk_eviction_drained_locked(module, &claim))
			break;
		now = is_remote_chunk_monotonic_ns();
		if (now == IS_REMOTE_CHUNK_ID_MAX) {
			status = -EIO;
			break;
		}
		if (now >= deadline_monotonic_ns) {
			status = -ETIMEDOUT;
			break;
		}
#ifdef __KERNEL__
		{
			unsigned long long remaining_ns = deadline_monotonic_ns - now;
			unsigned long remaining;

			if (remaining_ns > IS_REMOTE_CHUNK_WAIT_SLICE_NS)
				remaining_ns = IS_REMOTE_CHUNK_WAIT_SLICE_NS;
			remaining = nsecs_to_jiffies(remaining_ns);

			if (!remaining)
				remaining = 1;
			is_remote_chunk_unlock(&module->lock, &flags);
			(void)wait_event_timeout(module->lease_wait,
				is_remote_chunk_eviction_wait_ready(module, claim_storage),
				remaining);
			is_remote_chunk_lock(&module->lock, &flags);
		}
#else
		{
			struct timespec realtime;
			unsigned long long remaining_ns = deadline_monotonic_ns - now;
			unsigned long long absolute_ns;
			int wait_status;

			if (remaining_ns > IS_REMOTE_CHUNK_WAIT_SLICE_NS)
				remaining_ns = IS_REMOTE_CHUNK_WAIT_SLICE_NS;
			if (clock_gettime(CLOCK_REALTIME, &realtime)) {
				status = -EIO;
				break;
			}
			absolute_ns = (unsigned long long)realtime.tv_nsec +
				remaining_ns;
			realtime.tv_sec += (time_t)(absolute_ns / 1000000000ULL);
			realtime.tv_nsec = (long)(absolute_ns % 1000000000ULL);
			wait_status = pthread_cond_timedwait(&module->lease_changed,
				&module->lock, &realtime);
			if (wait_status && wait_status != ETIMEDOUT) {
				status = -wait_status;
				break;
			}
		}
#endif
	}
	module->active_eviction_waits--;
out:
	is_remote_chunk_unlock(&module->lock, &flags);
	return status;
}

static void is_remote_chunk_reset_mapping_locked(
	struct is_remote_chunk_module *module, unsigned int logical_chunk,
	bool reset_activity)
{
	struct is_remote_chunk *chunk = &module->chunks[logical_chunk];

	is_remote_chunk_validity_update(module->valid_sectors,
		(unsigned long long)logical_chunk *
			IS_REMOTE_CHUNK_SECTORS_PER_CHUNK,
		(unsigned int)IS_REMOTE_CHUNK_SECTORS_PER_CHUNK, false);
	chunk->mapping_generation = ++module->next_mapping_generation;
	memset(&chunk->provider, 0, sizeof(chunk->provider));
	chunk->provider_chunk = 0;
	chunk->remote_address = 0;
	chunk->remote_key = 0;
	chunk->mapping_claim_id = 0;
	chunk->eviction_claim_id = 0;
	chunk->transition_generation = 0;
	if (reset_activity)
		chunk->activity = 0;
	chunk->state = IS_REMOTE_CHUNK_UNMAPPED;
}

int is_remote_chunk_eviction_finish(
	struct is_remote_chunk_module *module,
	const struct is_remote_chunk_eviction_claim *claim_storage)
{
	struct is_remote_chunk_eviction_claim_internal claim;
	is_remote_chunk_lock_flags_t flags = 0;
	unsigned int index;
	int status;

	if (!module || !claim_storage)
		return -EINVAL;
	is_remote_chunk_eviction_claim_decode(claim_storage, &claim);
	is_remote_chunk_lock(&module->lock, &flags);
	status = is_remote_chunk_eviction_claim_validate_locked(module, &claim);
	if (status)
		goto out;
	if (!is_remote_chunk_eviction_drained_locked(module, &claim)) {
		is_remote_chunk_note_invariant_locked(module,
			IS_REMOTE_CHUNK_INVARIANT_EVICTION_FINISH_ACTIVE_LEASES);
		status = -EBUSY;
		goto out;
	}
	if (module->next_mapping_generation >
	    IS_REMOTE_CHUNK_ID_MAX - claim.chunk_count ||
	    module->assigned_chunks < claim.chunk_count ||
	    !module->active_eviction_claims) {
		status = -EOVERFLOW;
		goto out;
	}
	for (index = 0; index < module->chunk_count; index++) {
		if (module->chunks[index].eviction_claim_id == claim.claim_id)
			is_remote_chunk_reset_mapping_locked(module, index, true);
	}
	module->assigned_chunks -= claim.chunk_count;
	module->active_eviction_claims--;
	is_remote_chunk_wake_lease_waiters_locked(module);
	status = 0;
out:
	is_remote_chunk_unlock(&module->lock, &flags);
	return status;
}

int is_remote_chunk_provider_failed(
	struct is_remote_chunk_module *module,
	struct is_remote_chunk_provider_handle provider,
	struct is_remote_chunk_provider_failure_facts *facts_out)
{
	struct is_remote_chunk_provider_failure_facts facts = { 0 };
	is_remote_chunk_lock_flags_t flags = 0;
	unsigned int mapping_claims = 0;
	unsigned int eviction_claims = 0;
	unsigned int provider_index;
	unsigned int index;
	int status = 0;

	if (!module || !facts_out)
		return -EINVAL;
	is_remote_chunk_lock(&module->lock, &flags);
	if (is_remote_chunk_provider_index_locked(module, provider,
		&provider_index)) {
		status = -ESTALE;
		goto out;
	}
	for (index = 0; index < module->chunk_count; index++) {
		const struct is_remote_chunk *chunk = &module->chunks[index];
		unsigned int previous;

		if (chunk->state == IS_REMOTE_CHUNK_UNMAPPED ||
		    !is_remote_chunk_provider_handle_equal(chunk->provider, provider))
			continue;
		facts.affected_chunks++;
		facts.active_io_leases += chunk->active_io_leases;
		/* Mapping and eviction claims are Provider-scoped batches. */
		if (chunk->state == IS_REMOTE_CHUNK_MAPPING) {
			facts.mapping_chunks++;
			for (previous = 0; previous < index; previous++) {
				if (module->chunks[previous].mapping_claim_id ==
				    chunk->mapping_claim_id)
					break;
			}
			mapping_claims += previous == index;
		} else {
			facts.assigned_chunks++;
			if (chunk->state == IS_REMOTE_CHUNK_MAPPED) {
				facts.usable_chunks++;
			} else {
				facts.evicting_chunks++;
				for (previous = 0; previous < index; previous++) {
					if (module->chunks[previous].eviction_claim_id ==
					    chunk->eviction_claim_id)
						break;
				}
				eviction_claims += previous == index;
			}
		}
	}
	if (module->next_mapping_generation >
	    IS_REMOTE_CHUNK_ID_MAX - facts.affected_chunks ||
	    module->assigned_chunks < facts.assigned_chunks ||
	    module->usable_chunks < facts.usable_chunks ||
	    module->active_mapping_claims < mapping_claims ||
	    module->active_eviction_claims < eviction_claims) {
		status = -EOVERFLOW;
		goto out;
	}
	for (index = 0; index < module->chunk_count; index++) {
		struct is_remote_chunk *chunk = &module->chunks[index];

		if (chunk->state != IS_REMOTE_CHUNK_UNMAPPED &&
		    is_remote_chunk_provider_handle_equal(chunk->provider, provider))
			is_remote_chunk_reset_mapping_locked(module, index, false);
	}
	module->assigned_chunks -= facts.assigned_chunks;
	module->usable_chunks -= facts.usable_chunks;
	module->active_mapping_claims -= mapping_claims;
	module->active_eviction_claims -= eviction_claims;
	module->providers[provider_index].failed = 1;
	module->providers[provider_index].placement_eligible = 0;
	module->providers[provider_index].exclusion =
		IS_REMOTE_CHUNK_PROVIDER_EXCLUDE_UNHEALTHY;
	is_remote_chunk_wake_lease_waiters_locked(module);
	*facts_out = facts;
out:
	is_remote_chunk_unlock(&module->lock, &flags);
	return status;
}

int is_remote_chunk_io_lease_acquire(
	struct is_remote_chunk_module *module,
	const struct is_remote_chunk_io_request *request,
	struct is_remote_chunk_io_lease *lease,
	struct is_remote_chunk_transport_mapping *mapping_out)
{
	struct is_remote_chunk_io_lease_internal previous;
	struct is_remote_chunk_io_lease_internal internal = { 0 };
	is_remote_chunk_lock_flags_t flags = 0;
	struct is_remote_chunk *chunk;
	unsigned long long last_sector;
	unsigned long long remote_offset;
	unsigned int sector_count;
	unsigned int logical_chunk;
	int status = 0;

	if (!module || !request || !lease || !mapping_out)
		return -EINVAL;
	memset(mapping_out, 0, sizeof(*mapping_out));
	if ((request->direction != IS_REMOTE_CHUNK_IO_READ &&
	     request->direction != IS_REMOTE_CHUNK_IO_WRITE) ||
	    !request->bytes || request->bytes % IS_REMOTE_CHUNK_SECTOR_BYTES)
		return -EINVAL;
	sector_count = request->bytes / IS_REMOTE_CHUNK_SECTOR_BYTES;
	if (request->sector > IS_REMOTE_CHUNK_ID_MAX - (sector_count - 1U))
		return -ERANGE;
	last_sector = request->sector + sector_count - 1U;
	logical_chunk = (unsigned int)(request->sector /
		IS_REMOTE_CHUNK_SECTORS_PER_CHUNK);
	if (logical_chunk >= module->chunk_count ||
	    last_sector / IS_REMOTE_CHUNK_SECTORS_PER_CHUNK != logical_chunk)
		return -ERANGE;

	is_remote_chunk_lock(&module->lock, &flags);
	if (module->quiescing) {
		status = -ESHUTDOWN;
		goto out;
	}
	is_remote_chunk_lease_decode(lease, &previous);
	if (previous.magic && previous.magic != IS_REMOTE_CHUNK_LEASE_MAGIC) {
		status = -EINVAL;
		goto out;
	}
	if (previous.magic == IS_REMOTE_CHUNK_LEASE_MAGIC &&
	    previous.storage_identity !=
		is_remote_chunk_lease_storage_identity(lease)) {
		status = -EINVAL;
		goto out;
	}
	if (previous.magic == IS_REMOTE_CHUNK_LEASE_MAGIC) {
		status = previous.state & IS_REMOTE_CHUNK_LEASE_RELEASED ?
			-EALREADY : -EBUSY;
		goto out;
	}
	chunk = &module->chunks[logical_chunk];
	if (chunk->state != IS_REMOTE_CHUNK_MAPPED) {
		status = -ENXIO;
		goto out;
	}
	if (request->direction == IS_REMOTE_CHUNK_IO_READ &&
	    !is_remote_chunk_validity_test(module->valid_sectors,
		request->sector, sector_count)) {
		status = -ENODATA;
		goto out;
	}
	if (module->next_io_lease_id == IS_REMOTE_CHUNK_ID_MAX ||
	    module->active_io_leases == ~0U ||
	    chunk->active_io_leases == ~0U) {
		status = -EOVERFLOW;
		goto out;
	}

	internal.magic = IS_REMOTE_CHUNK_LEASE_MAGIC;
	internal.module_identity = module->identity;
	internal.storage_identity =
		is_remote_chunk_lease_storage_identity(lease);
	internal.lease_id = ++module->next_io_lease_id;
	internal.mapping_generation = chunk->mapping_generation;
	internal.sector = request->sector;
	internal.sector_count = sector_count;
	internal.logical_chunk = logical_chunk;
	internal.direction = request->direction;
	if (request->direction == IS_REMOTE_CHUNK_IO_WRITE)
		is_remote_chunk_validity_update(module->valid_sectors,
			request->sector, sector_count, false);
	chunk->active_io_leases++;
	module->active_io_leases++;
	remote_offset = (request->sector %
		IS_REMOTE_CHUNK_SECTORS_PER_CHUNK) *
		IS_REMOTE_CHUNK_SECTOR_BYTES;
	mapping_out->provider = chunk->provider;
	mapping_out->remote_address = chunk->remote_address + remote_offset;
	mapping_out->remote_key = chunk->remote_key;
	mapping_out->provider_chunk = chunk->provider_chunk;
	mapping_out->logical_chunk = logical_chunk;
	is_remote_chunk_lease_encode(lease, &internal);
out:
	is_remote_chunk_unlock(&module->lock, &flags);
	return status;
}

int is_remote_chunk_io_lease_resolve(
	struct is_remote_chunk_module *module,
	struct is_remote_chunk_io_lease *lease,
	enum is_remote_chunk_io_outcome outcome,
	enum is_remote_chunk_io_resolve_result *result_out)
{
	struct is_remote_chunk_io_lease_internal internal;
	is_remote_chunk_lock_flags_t flags = 0;
	struct is_remote_chunk *chunk;
	bool mapping_current;
	int status;

	if (!module || !lease || !result_out ||
	    (outcome != IS_REMOTE_CHUNK_IO_SUCCESS &&
	     outcome != IS_REMOTE_CHUNK_IO_FAILURE &&
	     outcome != IS_REMOTE_CHUNK_IO_CANCELLED &&
	     outcome != IS_REMOTE_CHUNK_IO_SUBMISSION_FAILURE))
		return -EINVAL;
	*result_out = IS_REMOTE_CHUNK_IO_RESOLVE_NONE;
	is_remote_chunk_lock(&module->lock, &flags);
	is_remote_chunk_lease_decode(lease, &internal);
	status = is_remote_chunk_lease_validate_locked(module, lease, &internal);
	if (status)
		goto out;
	if (internal.state & IS_REMOTE_CHUNK_LEASE_RESOLVED) {
		is_remote_chunk_note_invariant_locked(module,
			IS_REMOTE_CHUNK_INVARIANT_LEASE_DUPLICATE_RESOLVE);
		status = -EALREADY;
		goto out;
	}

	internal.state |= IS_REMOTE_CHUNK_LEASE_RESOLVED;
	chunk = &module->chunks[internal.logical_chunk];
	mapping_current =
		chunk->mapping_generation == internal.mapping_generation;
	if (mapping_current && internal.direction == IS_REMOTE_CHUNK_IO_WRITE)
		is_remote_chunk_validity_update(module->valid_sectors,
			internal.sector, internal.sector_count,
			outcome == IS_REMOTE_CHUNK_IO_SUCCESS);
	*result_out = mapping_current ? IS_REMOTE_CHUNK_IO_RESOLVE_CURRENT :
		IS_REMOTE_CHUNK_IO_RESOLVE_STALE;
	is_remote_chunk_lease_encode(lease, &internal);
	status = 0;
out:
	is_remote_chunk_unlock(&module->lock, &flags);
	return status;
}

int is_remote_chunk_io_lease_release(
	struct is_remote_chunk_module *module,
	struct is_remote_chunk_io_lease *lease)
{
	struct is_remote_chunk_io_lease_internal internal;
	is_remote_chunk_lock_flags_t flags = 0;
	struct is_remote_chunk *chunk;
	int status;

	if (!module || !lease)
		return -EINVAL;
	is_remote_chunk_lock(&module->lock, &flags);
	is_remote_chunk_lease_decode(lease, &internal);
	status = is_remote_chunk_lease_validate_locked(module, lease, &internal);
	if (status)
		goto out;
	if (internal.state & IS_REMOTE_CHUNK_LEASE_RELEASED) {
		is_remote_chunk_note_invariant_locked(module,
			IS_REMOTE_CHUNK_INVARIANT_LEASE_DUPLICATE_RELEASE);
		status = -EALREADY;
		goto out;
	}
	if (!(internal.state & IS_REMOTE_CHUNK_LEASE_RESOLVED)) {
		is_remote_chunk_note_invariant_locked(module,
			IS_REMOTE_CHUNK_INVARIANT_LEASE_RELEASE_BEFORE_RESOLVE);
		status = -EPERM;
		goto out;
	}

	chunk = &module->chunks[internal.logical_chunk];
	if (!chunk->active_io_leases || !module->active_io_leases) {
		status = -EINVAL;
		goto out;
	}
	chunk->active_io_leases--;
	module->active_io_leases--;
	internal.state |= IS_REMOTE_CHUNK_LEASE_RELEASED;
	is_remote_chunk_lease_encode(lease, &internal);
	is_remote_chunk_wake_lease_waiters_locked(module);
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
	return snapshot->provider_count;
}

int is_remote_chunk_snapshot_take(
	struct is_remote_chunk_module *module,
	struct is_remote_chunk_snapshot **snapshot_out)
{
	struct is_remote_chunk_snapshot *snapshot;
	is_remote_chunk_lock_flags_t flags = 0;
	unsigned int derived_assigned = 0;
	unsigned int derived_usable = 0;
	unsigned int derived_active_io_leases = 0;
	unsigned int index;

	if (!module || !snapshot_out)
		return -EINVAL;
	*snapshot_out = NULL;
	snapshot = is_remote_chunk_allocate(sizeof(*snapshot));
	if (!snapshot)
		return -ENOMEM;
	snapshot->providers = is_remote_chunk_allocate_array(module->provider_count,
		sizeof(*snapshot->providers));
	snapshot->placements = is_remote_chunk_allocate_array(module->chunk_count,
		sizeof(*snapshot->placements));
	if ((module->provider_count && !snapshot->providers) ||
	    !snapshot->placements) {
		is_remote_chunk_snapshot_release(snapshot);
		return -ENOMEM;
	}

	is_remote_chunk_lock(&module->lock, &flags);
	snapshot->mode = module->mode;
	snapshot->chunk_count = module->chunk_count;
	snapshot->assigned_chunks = module->assigned_chunks;
	snapshot->usable_chunks = module->usable_chunks;
	snapshot->active_mapping_claims = module->active_mapping_claims;
	snapshot->active_eviction_claims = module->active_eviction_claims;
	snapshot->active_io_leases = module->active_io_leases;
	snapshot->invariant_count = module->invariant_count;
	snapshot->latest_invariant = module->latest_invariant;
	snapshot->quiescing = module->quiescing != 0;
	snapshot->hot_policy = module->hot_policy;
	snapshot->placement_sample_size = module->placement_sample_size;
	snapshot->placement_seed = module->placement_seed;
	snapshot->provider_count = module->provider_count;
	for (index = 0; index < module->provider_count; index++) {
		const struct is_remote_chunk_provider *provider =
			&module->providers[index];
		struct is_remote_chunk_provider_snapshot *provider_snapshot =
			&snapshot->providers[index];

		provider_snapshot->provider = provider->handle;
		strcpy(provider_snapshot->identifier, provider->identifier);
		provider_snapshot->placement_weight = provider->placement_weight;
		provider_snapshot->reported_available_chunks =
			provider->reported_available_chunks;
		provider_snapshot->observation_assigned_chunks =
			provider->observation_assigned_chunks;
		provider_snapshot->observation_sequence =
			provider->observation_sequence;
		provider_snapshot->exclusion = provider->exclusion;
		provider_snapshot->placement_eligible =
			provider->placement_eligible != 0;
		provider_snapshot->has_observation = provider->has_observation != 0;
		provider_snapshot->failed = provider->failed != 0;
	}
	for (index = 0; index < module->chunk_count; index++) {
		const struct is_remote_chunk *chunk = &module->chunks[index];

		derived_active_io_leases += chunk->active_io_leases;
		if (chunk->activity >= module->hot_policy.threshold) {
			snapshot->hot_ranges++;
			if (chunk->state == IS_REMOTE_CHUNK_UNMAPPED)
				snapshot->mapping_candidates++;
		}
		if (chunk->state == IS_REMOTE_CHUNK_MAPPING) {
			snapshot->mapping_chunks++;
		} else if (chunk->state == IS_REMOTE_CHUNK_MAPPED ||
			   chunk->state == IS_REMOTE_CHUNK_EVICTING) {
			struct is_remote_chunk_placement_snapshot *placement =
				&snapshot->placements[snapshot->placement_count++];
			unsigned int provider_index =
				is_remote_chunk_snapshot_provider_index(snapshot,
					chunk->provider);
			bool usable = chunk->state == IS_REMOTE_CHUNK_MAPPED;

			if (provider_index >= snapshot->provider_count) {
				is_remote_chunk_unlock(&module->lock, &flags);
				is_remote_chunk_snapshot_release(snapshot);
				return -EINVAL;
			}
			placement->provider = chunk->provider;
			placement->logical_chunk = index;
			placement->usable = usable;
			snapshot->providers[provider_index].assigned_chunks++;
			if (usable)
				snapshot->providers[provider_index].usable_chunks++;
			else
				snapshot->evicting_chunks++;
			derived_assigned++;
			derived_usable += usable;
		}
	}
	if (derived_assigned != module->assigned_chunks ||
	    derived_usable != module->usable_chunks ||
	    derived_active_io_leases != module->active_io_leases) {
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
