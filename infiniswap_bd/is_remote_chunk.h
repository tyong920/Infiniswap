/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#ifndef INFINISWAP_REMOTE_CHUNK_H
#define INFINISWAP_REMOTE_CHUNK_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stddef.h>
#endif

#define IS_REMOTE_CHUNK_MAPPING_CLAIM_BYTES 64U
#define IS_REMOTE_CHUNK_IO_LEASE_BYTES 64U
#define IS_REMOTE_CHUNK_MAX_CHUNKS 128U
#define IS_REMOTE_CHUNK_HOT_WEIGHT_MAX 1000000U
#define IS_REMOTE_CHUNK_SECTOR_BYTES 512U
#define IS_REMOTE_CHUNK_SECTORS_PER_CHUNK (1ULL << 21)

struct is_remote_chunk_module;

enum is_remote_chunk_mode {
	IS_REMOTE_CHUNK_MODE_BACKED = 1,
	IS_REMOTE_CHUNK_MODE_REMOTE_ONLY,
};

enum is_remote_chunk_activity_kind {
	IS_REMOTE_CHUNK_ACTIVITY_READ = 1,
	IS_REMOTE_CHUNK_ACTIVITY_WRITE,
};

enum is_remote_chunk_io_direction {
	IS_REMOTE_CHUNK_IO_READ = 1,
	IS_REMOTE_CHUNK_IO_WRITE,
};

enum is_remote_chunk_io_outcome {
	IS_REMOTE_CHUNK_IO_SUCCESS = 1,
	IS_REMOTE_CHUNK_IO_FAILURE,
	IS_REMOTE_CHUNK_IO_CANCELLED,
	IS_REMOTE_CHUNK_IO_SUBMISSION_FAILURE,
};

enum is_remote_chunk_io_resolve_result {
	IS_REMOTE_CHUNK_IO_RESOLVE_NONE = 0,
	IS_REMOTE_CHUNK_IO_RESOLVE_CURRENT,
	IS_REMOTE_CHUNK_IO_RESOLVE_STALE,
};

enum is_remote_chunk_invariant_event {
	IS_REMOTE_CHUNK_INVARIANT_NONE = 0,
	IS_REMOTE_CHUNK_INVARIANT_LEASE_RELEASE_BEFORE_RESOLVE,
	IS_REMOTE_CHUNK_INVARIANT_LEASE_DUPLICATE_RESOLVE,
	IS_REMOTE_CHUNK_INVARIANT_LEASE_DUPLICATE_RELEASE,
};

struct is_remote_chunk_hot_policy {
	unsigned long long threshold;
	unsigned int read_weight;
	unsigned int write_weight;
};

struct is_remote_chunk_config {
	enum is_remote_chunk_mode mode;
	unsigned int chunk_count;
	struct is_remote_chunk_hot_policy hot_policy;
};

/* Stable, copyable identity for one Memory Provider connection epoch. */
struct is_remote_chunk_provider_handle {
	unsigned long long opaque[2];
};

/* Initialize caller-owned claim storage before its first mapping begin. */
struct is_remote_chunk_mapping_claim {
	union {
		void *pointer_alignment;
		unsigned long long integer_alignment;
		unsigned char bytes[IS_REMOTE_CHUNK_MAPPING_CLAIM_BYTES];
	} opaque;
};

#define IS_REMOTE_CHUNK_MAPPING_CLAIM_INIT { .opaque = { .bytes = { 0 } } }

/*
 * Lease storage is single-use and address-bound. It must not be copied, moved,
 * reset, or reused after acquisition.
 */
struct is_remote_chunk_io_lease {
	union {
		void *pointer_alignment;
		unsigned long long integer_alignment;
		unsigned char bytes[IS_REMOTE_CHUNK_IO_LEASE_BYTES];
	} opaque;
};

#define IS_REMOTE_CHUNK_IO_LEASE_INIT { .opaque = { .bytes = { 0 } } }

struct is_remote_chunk_io_request {
	enum is_remote_chunk_io_direction direction;
	unsigned long long sector;
	unsigned int bytes;
};

/* Immutable transport mapping captured for the accepted lease lifetime. */
struct is_remote_chunk_transport_mapping {
	struct is_remote_chunk_provider_handle provider;
	unsigned long long remote_address;
	unsigned int remote_key;
	unsigned int provider_chunk;
	unsigned int logical_chunk;
};

struct is_remote_chunk_mapping_grant {
	unsigned int logical_chunk;
	unsigned int provider_chunk;
	unsigned long long remote_address;
	unsigned int remote_key;
};

struct is_remote_chunk_provider_snapshot {
	struct is_remote_chunk_provider_handle provider;
	unsigned int assigned_chunks;
	unsigned int usable_chunks;
};

struct is_remote_chunk_placement_snapshot {
	struct is_remote_chunk_provider_handle provider;
	unsigned int logical_chunk;
	bool usable;
};

/* Snapshot storage belongs to the module until snapshot_release. */
struct is_remote_chunk_snapshot {
	enum is_remote_chunk_mode mode;
	unsigned int chunk_count;
	unsigned int assigned_chunks;
	unsigned int usable_chunks;
	unsigned int mapping_chunks;
	unsigned int active_mapping_claims;
	unsigned int hot_ranges;
	unsigned int mapping_candidates;
	unsigned int active_io_leases;
	unsigned long long invariant_count;
	enum is_remote_chunk_invariant_event latest_invariant;
	struct is_remote_chunk_hot_policy hot_policy;
	unsigned int provider_count;
	struct is_remote_chunk_provider_snapshot *providers;
	unsigned int placement_count;
	struct is_remote_chunk_placement_snapshot *placements;
};

int is_remote_chunk_module_create(
	const struct is_remote_chunk_config *config,
	struct is_remote_chunk_module **module_out);
int is_remote_chunk_module_destroy(struct is_remote_chunk_module *module);

int is_remote_chunk_provider_handle_create(
	struct is_remote_chunk_module *module,
	struct is_remote_chunk_provider_handle *provider_out);
bool is_remote_chunk_provider_handle_equal(
	struct is_remote_chunk_provider_handle left,
	struct is_remote_chunk_provider_handle right);

int is_remote_chunk_hot_policy_set(
	struct is_remote_chunk_module *module,
	const struct is_remote_chunk_hot_policy *policy,
	bool *mapping_needed);
int is_remote_chunk_note_activity(
	struct is_remote_chunk_module *module, unsigned int logical_start,
	unsigned int chunk_count, enum is_remote_chunk_activity_kind kind,
	bool *mapping_needed);

/* Explicit batches reserve Committed Remote Chunks in Remote-Only Mode. */
int is_remote_chunk_mapping_begin_explicit(
	struct is_remote_chunk_module *module,
	struct is_remote_chunk_provider_handle provider,
	const unsigned int *logical_chunks, unsigned int chunk_count,
	struct is_remote_chunk_mapping_claim *claim_out);

/* Backed Mode selects the next unmapped Hot Range inside the module. */
int is_remote_chunk_mapping_begin_hot(
	struct is_remote_chunk_module *module,
	struct is_remote_chunk_provider_handle provider,
	struct is_remote_chunk_mapping_claim *claim_out,
	unsigned int *logical_chunk_out);

int is_remote_chunk_mapping_commit(
	struct is_remote_chunk_module *module,
	const struct is_remote_chunk_mapping_claim *claim,
	struct is_remote_chunk_provider_handle provider,
	const struct is_remote_chunk_mapping_grant *grants,
	unsigned int grant_count);
int is_remote_chunk_mapping_abort(
	struct is_remote_chunk_module *module,
	const struct is_remote_chunk_mapping_claim *claim);

/*
 * Lease operations do not sleep or allocate. Acquire atomically checks one
 * Remote Chunk and its sector validity. Invalid reads return -ENODATA;
 * unusable mappings return -ENXIO. Write validity is cleared before success.
 */
int is_remote_chunk_io_lease_acquire(
	struct is_remote_chunk_module *module,
	const struct is_remote_chunk_io_request *request,
	struct is_remote_chunk_io_lease *lease,
	struct is_remote_chunk_transport_mapping *mapping_out);
/* A stale result consumes resolve normally and is not an invariant event. */
int is_remote_chunk_io_lease_resolve(
	struct is_remote_chunk_module *module,
	struct is_remote_chunk_io_lease *lease,
	enum is_remote_chunk_io_outcome outcome,
	enum is_remote_chunk_io_resolve_result *result_out);
/* Release before resolve returns -EPERM without consuming the release right. */
int is_remote_chunk_io_lease_release(
	struct is_remote_chunk_module *module,
	struct is_remote_chunk_io_lease *lease);

int is_remote_chunk_snapshot_take(
	struct is_remote_chunk_module *module,
	struct is_remote_chunk_snapshot **snapshot_out);
void is_remote_chunk_snapshot_release(struct is_remote_chunk_snapshot *snapshot);

#endif /* INFINISWAP_REMOTE_CHUNK_H */
