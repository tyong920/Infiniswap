/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#ifndef INFINISWAP_MEMORY_MANAGER_H
#define INFINISWAP_MEMORY_MANAGER_H

#include <stddef.h>
#include <stdint.h>

#define IS_MEMORY_CHUNK_BYTES UINT64_C(1073741824)
#define IS_MEMORY_MAX_RUNTIME_CHUNKS 128U
#define IS_MEMORY_CONSUMER_ID_MAX 63U

enum is_memory_result {
  IS_MEMORY_OK = 0,
  IS_MEMORY_INVALID_ARGUMENT,
  IS_MEMORY_CONFIG_INVALID,
  IS_MEMORY_ACCOUNTING_FAILED,
  IS_MEMORY_ALLOCATION_FAILED,
  IS_MEMORY_TOUCH_FAILED,
  IS_MEMORY_REGISTRATION_FAILED,
  IS_MEMORY_CLEANUP_FAILED,
  IS_MEMORY_CONNECTION_LIMIT,
  IS_MEMORY_QUOTA_EXCEEDED,
  IS_MEMORY_POOL_EXHAUSTED,
  IS_MEMORY_HOST_RESERVE,
  IS_MEMORY_NOT_CONNECTED,
  IS_MEMORY_NOT_FOUND,
  IS_MEMORY_COMMITTED_PROTECTED
};

enum is_memory_pool {
  IS_MEMORY_POOL_OPPORTUNISTIC = 1,
  IS_MEMORY_POOL_COMMITTED = 2
};

enum is_memory_release_cause {
  IS_MEMORY_RELEASE_CONSUMER = 1,
  IS_MEMORY_RELEASE_PRESSURE,
  IS_MEMORY_RELEASE_DISCONNECT,
  IS_MEMORY_RELEASE_SHUTDOWN
};

enum is_memory_rejection_reason {
  IS_MEMORY_REJECTION_NONE = 0,
  IS_MEMORY_REJECTION_INVALID_REQUEST,
  IS_MEMORY_REJECTION_CONNECTION_LIMIT,
  IS_MEMORY_REJECTION_CONSUMER_QUOTA,
  IS_MEMORY_REJECTION_POOL_LIMIT,
  IS_MEMORY_REJECTION_HOST_RESERVE,
  IS_MEMORY_REJECTION_ACCOUNTING_FAILED,
  IS_MEMORY_REJECTION_ALLOCATION_FAILED,
  IS_MEMORY_REJECTION_TOUCH_FAILED,
  IS_MEMORY_REJECTION_REGISTRATION_FAILED,
  IS_MEMORY_REJECTION_CLEANUP_FAILED,
  IS_MEMORY_REJECTION_COMMITTED_PROTECTED
};

struct is_memory_manager_config {
  uint32_t host_reserve_gib;
  uint32_t max_opportunistic_gib;
  uint32_t max_committed_gib;
};

struct is_memory_accounting_adapter {
  void *context;
  int (*read_available_bytes)(void *context, uint64_t *available_bytes);
};

struct is_memory_allocation_adapter {
  void *context;
  int (*allocate)(void *context, size_t alignment, size_t size,
                  void **address);
  int (*touch)(void *context, void *address, size_t size);
  void (*release)(void *context, void *address, size_t size);
};

struct is_memory_registration_adapter {
  void *context;
  void *(*register_chunk)(void *context, void *address, size_t size);
  int (*deregister_chunk)(void *context, void *registration);
};

struct is_memory_grant {
  uint32_t provider_chunk_id;
  void *address;
  void *registration;
  enum is_memory_pool pool;
};

struct is_memory_reconcile_result {
  uint32_t grown_opportunistic_chunks;
  uint32_t reclaimed_unassigned_chunks;
  uint32_t assigned_reclaim_needed;
};

struct is_memory_manager_status {
  uint32_t host_reserve_chunks;
  uint32_t max_opportunistic_chunks;
  uint32_t max_committed_chunks;
  uint32_t allocated_opportunistic_chunks;
  uint32_t assigned_opportunistic_chunks;
  uint32_t allocated_committed_chunks;
  uint32_t assigned_committed_chunks;
  uint32_t quarantined_chunks;
  uint32_t available_opportunistic_chunks;
  uint32_t available_committed_chunks;
  uint32_t active_connections;
  uint32_t consumer_connections;
  uint32_t consumer_assigned_opportunistic_chunks;
  uint32_t consumer_assigned_committed_chunks;
  uint64_t admissions;
  uint64_t rejections;
  uint64_t pressure_reclaims;
  enum is_memory_rejection_reason last_rejection;
  int healthy;
};

struct is_memory_manager;

struct is_memory_manager *is_memory_manager_create(
    const struct is_memory_manager_config *config,
    const struct is_memory_accounting_adapter *accounting,
    const struct is_memory_allocation_adapter *allocation);

enum is_memory_result
is_memory_manager_destroy(struct is_memory_manager *manager);

enum is_memory_result is_memory_manager_connect(
    struct is_memory_manager *manager, const char *consumer_id, void *owner,
    uint32_t max_connections, uint32_t max_opportunistic_chunks,
    uint32_t max_committed_chunks,
    enum is_memory_rejection_reason *rejection);

enum is_memory_result is_memory_manager_disconnect(
    struct is_memory_manager *manager, void *owner);

enum is_memory_result is_memory_manager_acquire(
    struct is_memory_manager *manager, void *owner, enum is_memory_pool pool,
    uint32_t chunk_count, enum is_memory_rejection_reason *rejection,
    const struct is_memory_registration_adapter *registration,
    struct is_memory_grant grants[], size_t grant_capacity);

enum is_memory_result is_memory_manager_release(
    struct is_memory_manager *manager, void *owner,
    const uint32_t provider_chunk_ids[], size_t chunk_count,
    enum is_memory_release_cause cause);

enum is_memory_result is_memory_manager_reconcile(
    struct is_memory_manager *manager,
    struct is_memory_reconcile_result *result);

enum is_memory_result is_memory_manager_get_status(
    struct is_memory_manager *manager, void *owner,
    struct is_memory_manager_status *status);

enum is_memory_result is_memory_manager_config_load(
    const char *path, struct is_memory_manager_config *config);

void is_memory_linux_accounting_adapter(
    struct is_memory_accounting_adapter *adapter);
void is_memory_posix_allocation_adapter(
    struct is_memory_allocation_adapter *adapter);

const char *is_memory_result_name(enum is_memory_result result);
const char *is_memory_rejection_name(
    enum is_memory_rejection_reason rejection);

#endif /* INFINISWAP_MEMORY_MANAGER_H */
