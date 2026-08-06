/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#include "infiniswap_memory_manager.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct is_memory_chunk {
  void *address;
  void *registration;
  void *owner;
  struct is_memory_registration_adapter registration_adapter;
  enum is_memory_pool pool;
  int quarantined;
};

struct is_memory_connection {
  char consumer_id[IS_MEMORY_CONSUMER_ID_MAX + 1U];
  void *owner;
  uint32_t max_connections;
  uint32_t max_opportunistic_chunks;
  uint32_t max_committed_chunks;
  uint32_t assigned_opportunistic_chunks;
  uint32_t assigned_committed_chunks;
};

struct is_memory_manager {
  pthread_mutex_t lock;
  struct is_memory_manager_config config;
  struct is_memory_accounting_adapter accounting;
  struct is_memory_allocation_adapter allocation;
  struct is_memory_chunk *chunks;
  struct is_memory_connection *connections;
  size_t chunk_capacity;
  size_t connection_count;
  size_t connection_capacity;
  size_t page_size;
  uint32_t allocated_opportunistic_chunks;
  uint32_t assigned_opportunistic_chunks;
  uint32_t allocated_committed_chunks;
  uint32_t assigned_committed_chunks;
  uint32_t quarantined_chunks;
  uint64_t admissions;
  uint64_t rejections;
  uint64_t pressure_reclaims;
  enum is_memory_rejection_reason last_rejection;
  int healthy;
};

struct pending_chunk {
  size_t index;
  void *address;
  void *registration;
  int newly_allocated;
  int converted_from_opportunistic;
};

static int config_valid(const struct is_memory_manager_config *config)
{
  uint64_t capacity;

  if (!config)
    return 0;
  capacity = (uint64_t)config->max_opportunistic_gib +
             (uint64_t)config->max_committed_gib;
  return capacity != 0 && capacity <= IS_MEMORY_MAX_RUNTIME_CHUNKS;
}

static int consumer_id_valid(const char *consumer_id)
{
  size_t index;

  if (!consumer_id || !consumer_id[0])
    return 0;
  for (index = 0; index <= IS_MEMORY_CONSUMER_ID_MAX; index++) {
    char value = consumer_id[index];

    if (value == '\0')
      return 1;
    if (!((value >= 'a' && value <= 'z') ||
          (value >= 'A' && value <= 'Z') ||
          (value >= '0' && value <= '9') || value == '.' || value == '_' ||
          value == '-'))
      return 0;
  }
  return 0;
}

static struct is_memory_connection *find_connection(
    struct is_memory_manager *manager, void *owner)
{
  size_t index;

  for (index = 0; index < manager->connection_count; index++) {
    if (manager->connections[index].owner == owner)
      return &manager->connections[index];
  }
  return NULL;
}

static enum is_memory_result reject(
    struct is_memory_manager *manager,
    enum is_memory_result result,
    enum is_memory_rejection_reason reason,
    enum is_memory_rejection_reason *reported_reason)
{
  manager->last_rejection = reason;
  manager->rejections++;
  if (reported_reason)
    *reported_reason = reason;
  if (reason == IS_MEMORY_REJECTION_CLEANUP_FAILED)
    manager->healthy = 0;
  return result;
}

static uint32_t connection_count_for_consumer(
    const struct is_memory_manager *manager, const char *consumer_id)
{
  uint32_t count = 0;
  size_t index;

  for (index = 0; index < manager->connection_count; index++) {
    if (strcmp(manager->connections[index].consumer_id, consumer_id) == 0)
      count++;
  }
  return count;
}

static void assignments_for_consumer(
    const struct is_memory_manager *manager, const char *consumer_id,
    uint32_t *opportunistic_chunks, uint32_t *committed_chunks)
{
  size_t index;

  *opportunistic_chunks = 0;
  *committed_chunks = 0;
  for (index = 0; index < manager->connection_count; index++) {
    const struct is_memory_connection *connection =
        &manager->connections[index];

    if (strcmp(connection->consumer_id, consumer_id) != 0)
      continue;
    *opportunistic_chunks += connection->assigned_opportunistic_chunks;
    *committed_chunks += connection->assigned_committed_chunks;
  }
}

static int grow_connections(struct is_memory_manager *manager)
{
  struct is_memory_connection *connections;
  size_t capacity = manager->connection_capacity == 0
                        ? 4
                        : manager->connection_capacity * 2;

  if (capacity < manager->connection_capacity)
    return -1;
  connections = realloc(manager->connections,
                        capacity * sizeof(*manager->connections));
  if (!connections)
    return -1;
  manager->connections = connections;
  manager->connection_capacity = capacity;
  return 0;
}

struct is_memory_manager *is_memory_manager_create(
    const struct is_memory_manager_config *config,
    const struct is_memory_accounting_adapter *accounting,
    const struct is_memory_allocation_adapter *allocation)
{
  struct is_memory_manager *manager;
  long page_size;

  if (!config_valid(config) || !accounting ||
      !accounting->read_available_bytes || !allocation ||
      !allocation->allocate || !allocation->touch || !allocation->release)
    return NULL;
  manager = calloc(1, sizeof(*manager));
  if (!manager)
    return NULL;
  manager->chunk_capacity = (size_t)config->max_opportunistic_gib +
                            (size_t)config->max_committed_gib;
  manager->chunks = calloc(manager->chunk_capacity, sizeof(*manager->chunks));
  if (!manager->chunks) {
    free(manager);
    return NULL;
  }
  if (pthread_mutex_init(&manager->lock, NULL) != 0) {
    free(manager->chunks);
    free(manager);
    return NULL;
  }
  manager->config = *config;
  manager->accounting = *accounting;
  manager->allocation = *allocation;
  manager->healthy = 1;
  page_size = sysconf(_SC_PAGESIZE);
  manager->page_size = page_size > 0 ? (size_t)page_size : 4096U;
  return manager;
}

static enum is_memory_result release_chunk_locked(
    struct is_memory_manager *manager, struct is_memory_connection *connection,
    struct is_memory_chunk *chunk, enum is_memory_release_cause cause)
{
  enum is_memory_pool pool = chunk->pool;

  if (chunk->registration &&
      chunk->registration_adapter.deregister_chunk(
          chunk->registration_adapter.context, chunk->registration) != 0)
    return reject(manager, IS_MEMORY_CLEANUP_FAILED,
                  IS_MEMORY_REJECTION_CLEANUP_FAILED, NULL);
  chunk->registration = NULL;
  chunk->owner = NULL;
  memset(&chunk->registration_adapter, 0,
         sizeof(chunk->registration_adapter));
  if (pool == IS_MEMORY_POOL_OPPORTUNISTIC) {
    manager->assigned_opportunistic_chunks--;
    connection->assigned_opportunistic_chunks--;
  } else {
    manager->assigned_committed_chunks--;
    connection->assigned_committed_chunks--;
  }

  if (cause == IS_MEMORY_RELEASE_PRESSURE ||
      pool == IS_MEMORY_POOL_COMMITTED) {
    manager->allocation.release(manager->allocation.context, chunk->address,
                                (size_t)IS_MEMORY_CHUNK_BYTES);
    chunk->address = NULL;
    chunk->pool = 0;
    if (pool == IS_MEMORY_POOL_OPPORTUNISTIC) {
      manager->allocated_opportunistic_chunks--;
      if (cause == IS_MEMORY_RELEASE_PRESSURE)
        manager->pressure_reclaims++;
    } else {
      manager->allocated_committed_chunks--;
    }
  }
  return IS_MEMORY_OK;
}

enum is_memory_result is_memory_manager_destroy(
    struct is_memory_manager *manager)
{
  size_t index;
  int cleanup_failed = 0;

  if (!manager)
    return IS_MEMORY_INVALID_ARGUMENT;
  pthread_mutex_lock(&manager->lock);
  for (index = 0; index < manager->chunk_capacity; index++) {
    struct is_memory_chunk *chunk = &manager->chunks[index];

    if (chunk->registration &&
        chunk->registration_adapter.deregister_chunk(
            chunk->registration_adapter.context,
            chunk->registration) != 0) {
      cleanup_failed = 1;
      continue;
    }
    chunk->registration = NULL;
    chunk->owner = NULL;
  }
  if (cleanup_failed) {
    manager->healthy = 0;
    manager->last_rejection = IS_MEMORY_REJECTION_CLEANUP_FAILED;
    manager->rejections++;
    pthread_mutex_unlock(&manager->lock);
    return IS_MEMORY_CLEANUP_FAILED;
  }
  for (index = 0; index < manager->chunk_capacity; index++) {
    if (manager->chunks[index].address)
      manager->allocation.release(manager->allocation.context,
                                  manager->chunks[index].address,
                                  (size_t)IS_MEMORY_CHUNK_BYTES);
  }
  free(manager->connections);
  free(manager->chunks);
  pthread_mutex_unlock(&manager->lock);
  pthread_mutex_destroy(&manager->lock);
  free(manager);
  return IS_MEMORY_OK;
}

enum is_memory_result is_memory_manager_connect(
    struct is_memory_manager *manager, const char *consumer_id, void *owner,
    uint32_t max_connections, uint32_t max_opportunistic_chunks,
    uint32_t max_committed_chunks,
    enum is_memory_rejection_reason *rejection)
{
  struct is_memory_connection *connection;
  enum is_memory_result result = IS_MEMORY_OK;

  if (rejection)
    *rejection = IS_MEMORY_REJECTION_NONE;
  if (!manager || !consumer_id_valid(consumer_id) || !owner ||
      max_connections == 0 ||
      (max_opportunistic_chunks == 0 && max_committed_chunks == 0))
    return IS_MEMORY_INVALID_ARGUMENT;
  pthread_mutex_lock(&manager->lock);
  if (find_connection(manager, owner)) {
    result = reject(manager, IS_MEMORY_INVALID_ARGUMENT,
                    IS_MEMORY_REJECTION_INVALID_REQUEST, rejection);
    goto out;
  }
  if (connection_count_for_consumer(manager, consumer_id) >=
      max_connections) {
    result = reject(manager, IS_MEMORY_CONNECTION_LIMIT,
                    IS_MEMORY_REJECTION_CONNECTION_LIMIT, rejection);
    goto out;
  }
  {
    size_t index;

    for (index = 0; index < manager->connection_count; index++) {
      const struct is_memory_connection *existing =
          &manager->connections[index];

      if (strcmp(existing->consumer_id, consumer_id) == 0 &&
          (existing->max_connections != max_connections ||
           existing->max_opportunistic_chunks !=
               max_opportunistic_chunks ||
           existing->max_committed_chunks != max_committed_chunks)) {
        result = reject(manager, IS_MEMORY_INVALID_ARGUMENT,
                        IS_MEMORY_REJECTION_INVALID_REQUEST, rejection);
        goto out;
      }
    }
  }
  if (manager->connection_count == manager->connection_capacity &&
      grow_connections(manager) != 0) {
    result = reject(manager, IS_MEMORY_ALLOCATION_FAILED,
                    IS_MEMORY_REJECTION_ALLOCATION_FAILED, rejection);
    goto out;
  }
  connection = &manager->connections[manager->connection_count++];
  memset(connection, 0, sizeof(*connection));
  strcpy(connection->consumer_id, consumer_id);
  connection->owner = owner;
  connection->max_connections = max_connections;
  connection->max_opportunistic_chunks = max_opportunistic_chunks;
  connection->max_committed_chunks = max_committed_chunks;
out:
  pthread_mutex_unlock(&manager->lock);
  return result;
}

static enum is_memory_result disconnect_locked(
    struct is_memory_manager *manager, size_t connection_index)
{
  struct is_memory_connection *connection =
      &manager->connections[connection_index];
  size_t index;

  for (index = 0; index < manager->chunk_capacity; index++) {
    struct is_memory_chunk *chunk = &manager->chunks[index];
    enum is_memory_result result;

    if (chunk->owner != connection->owner)
      continue;
    result = release_chunk_locked(manager, connection, chunk,
                                  IS_MEMORY_RELEASE_DISCONNECT);
    if (result != IS_MEMORY_OK)
      return result;
  }
  manager->connection_count--;
  if (connection_index != manager->connection_count)
    manager->connections[connection_index] =
        manager->connections[manager->connection_count];
  memset(&manager->connections[manager->connection_count], 0,
         sizeof(manager->connections[0]));
  return IS_MEMORY_OK;
}

enum is_memory_result is_memory_manager_disconnect(
    struct is_memory_manager *manager, void *owner)
{
  size_t index;
  enum is_memory_result result = IS_MEMORY_NOT_CONNECTED;

  if (!manager || !owner)
    return IS_MEMORY_INVALID_ARGUMENT;
  pthread_mutex_lock(&manager->lock);
  for (index = 0; index < manager->connection_count; index++) {
    if (manager->connections[index].owner != owner)
      continue;
    result = disconnect_locked(manager, index);
    break;
  }
  pthread_mutex_unlock(&manager->lock);
  return result;
}

static enum is_memory_result validate_acquire_locked(
    struct is_memory_manager *manager,
    struct is_memory_connection *connection, enum is_memory_pool pool,
    uint32_t chunk_count, enum is_memory_rejection_reason *rejection)
{
  uint32_t assigned_opportunistic;
  uint32_t assigned_committed;
  uint32_t assigned;
  uint32_t quota;

  assignments_for_consumer(manager, connection->consumer_id,
                           &assigned_opportunistic, &assigned_committed);
  if (pool == IS_MEMORY_POOL_OPPORTUNISTIC) {
    assigned = assigned_opportunistic;
    quota = connection->max_opportunistic_chunks;
  } else if (pool == IS_MEMORY_POOL_COMMITTED) {
    assigned = assigned_committed;
    quota = connection->max_committed_chunks;
  } else {
    return reject(manager, IS_MEMORY_INVALID_ARGUMENT,
                  IS_MEMORY_REJECTION_INVALID_REQUEST, rejection);
  }
  if (assigned > quota || chunk_count > quota - assigned)
    return reject(manager, IS_MEMORY_QUOTA_EXCEEDED,
                  IS_MEMORY_REJECTION_CONSUMER_QUOTA, rejection);
  return IS_MEMORY_OK;
}

static void rollback_pending(
    struct is_memory_manager *manager, struct pending_chunk pending[],
    uint32_t count, enum is_memory_pool requested_pool,
    const struct is_memory_registration_adapter *registration,
    int *cleanup_failed)
{
  uint32_t index;

  for (index = 0; index < count; index++) {
    if (!pending[index].registration)
      continue;
    if (registration->deregister_chunk(registration->context,
                                       pending[index].registration) != 0) {
      struct is_memory_chunk *chunk =
          &manager->chunks[pending[index].index];

      *cleanup_failed = 1;
      if (pending[index].newly_allocated) {
        chunk->address = pending[index].address;
        chunk->pool = requested_pool;
        if (requested_pool == IS_MEMORY_POOL_OPPORTUNISTIC)
          manager->allocated_opportunistic_chunks++;
        else
          manager->allocated_committed_chunks++;
      }
      chunk->registration = pending[index].registration;
      chunk->registration_adapter = *registration;
      chunk->quarantined = 1;
      manager->quarantined_chunks++;
    } else {
      pending[index].registration = NULL;
    }
  }
  for (index = 0; index < count; index++) {
    if (!pending[index].newly_allocated ||
        !pending[index].address || pending[index].registration)
      continue;
    manager->allocation.release(manager->allocation.context,
                                pending[index].address,
                                (size_t)IS_MEMORY_CHUNK_BYTES);
    pending[index].address = NULL;
  }
}

static int pending_contains(const struct pending_chunk pending[],
                            uint32_t count, size_t chunk_index)
{
  uint32_t index;

  for (index = 0; index < count; index++) {
    if (pending[index].index == chunk_index)
      return 1;
  }
  return 0;
}

enum is_memory_result is_memory_manager_acquire(
    struct is_memory_manager *manager, void *owner, enum is_memory_pool pool,
    uint32_t chunk_count, enum is_memory_rejection_reason *rejection,
    const struct is_memory_registration_adapter *registration,
    struct is_memory_grant grants[], size_t grant_capacity)
{
  struct is_memory_connection *connection;
  struct pending_chunk *pending = NULL;
  uint64_t available_bytes;
  uint64_t available_chunks;
  uint64_t required_chunks;
  uint32_t selected = 0;
  uint32_t newly_allocated = 0;
  uint32_t same_pool = 0;
  uint32_t index;
  int cleanup_failed = 0;
  enum is_memory_result result = IS_MEMORY_OK;

  if (rejection)
    *rejection = IS_MEMORY_REJECTION_NONE;
  if (!manager || !owner || chunk_count == 0 || !registration ||
      !registration->register_chunk || !registration->deregister_chunk ||
      !grants || grant_capacity < chunk_count)
    return IS_MEMORY_INVALID_ARGUMENT;
  pending = calloc(chunk_count, sizeof(*pending));
  if (!pending)
    return IS_MEMORY_ALLOCATION_FAILED;

  pthread_mutex_lock(&manager->lock);
  connection = find_connection(manager, owner);
  if (!connection) {
    result = IS_MEMORY_NOT_CONNECTED;
    goto out;
  }
  result = validate_acquire_locked(manager, connection, pool, chunk_count,
                                   rejection);
  if (result != IS_MEMORY_OK)
    goto out;

  for (index = 0; index < manager->chunk_capacity && selected < chunk_count;
       index++) {
    struct is_memory_chunk *chunk = &manager->chunks[index];

    if (!chunk->address || chunk->owner || chunk->registration ||
        chunk->quarantined || chunk->pool != pool)
      continue;
    pending[selected].index = index;
    pending[selected].address = chunk->address;
    selected++;
    same_pool++;
  }
  if (pool == IS_MEMORY_POOL_COMMITTED) {
    for (index = 0;
         index < manager->chunk_capacity && selected < chunk_count; index++) {
      struct is_memory_chunk *chunk = &manager->chunks[index];

      if (!chunk->address || chunk->owner || chunk->registration ||
          chunk->quarantined ||
          chunk->pool != IS_MEMORY_POOL_OPPORTUNISTIC)
        continue;
      pending[selected].index = index;
      pending[selected].address = chunk->address;
      pending[selected].converted_from_opportunistic = 1;
      selected++;
    }
  }
  for (index = 0; index < manager->chunk_capacity && selected < chunk_count;
       index++) {
    struct is_memory_chunk *chunk = &manager->chunks[index];

    if (chunk->address || pending_contains(pending, selected, index))
      continue;
    pending[selected].index = index;
    pending[selected].newly_allocated = 1;
    selected++;
    newly_allocated++;
  }
  if (selected != chunk_count) {
    result = reject(manager, IS_MEMORY_POOL_EXHAUSTED,
                    IS_MEMORY_REJECTION_POOL_LIMIT, rejection);
    goto out;
  }
  if (pool == IS_MEMORY_POOL_OPPORTUNISTIC) {
    uint32_t additional = chunk_count - same_pool;

    if (manager->allocated_opportunistic_chunks >
            manager->config.max_opportunistic_gib ||
        additional > manager->config.max_opportunistic_gib -
                         manager->allocated_opportunistic_chunks) {
      result = reject(manager, IS_MEMORY_POOL_EXHAUSTED,
                      IS_MEMORY_REJECTION_POOL_LIMIT, rejection);
      goto out;
    }
  } else {
    uint32_t additional = chunk_count - same_pool;

    if (manager->allocated_committed_chunks >
            manager->config.max_committed_gib ||
        additional > manager->config.max_committed_gib -
                         manager->allocated_committed_chunks) {
      result = reject(manager, IS_MEMORY_POOL_EXHAUSTED,
                      IS_MEMORY_REJECTION_POOL_LIMIT, rejection);
      goto out;
    }
  }

  if (newly_allocated != 0) {
    if (manager->accounting.read_available_bytes(
            manager->accounting.context, &available_bytes) != 0) {
      result = reject(manager, IS_MEMORY_ACCOUNTING_FAILED,
                      IS_MEMORY_REJECTION_ACCOUNTING_FAILED, rejection);
      goto out;
    }
    available_chunks = available_bytes / IS_MEMORY_CHUNK_BYTES;
    required_chunks = (uint64_t)manager->config.host_reserve_gib +
                      newly_allocated;
    if (available_chunks < required_chunks) {
      result = reject(manager, IS_MEMORY_HOST_RESERVE,
                      IS_MEMORY_REJECTION_HOST_RESERVE, rejection);
      goto out;
    }
  }

  for (index = 0; index < chunk_count; index++) {
    if (!pending[index].newly_allocated)
      continue;
    if (manager->allocation.allocate(manager->allocation.context,
                                     manager->page_size,
                                     (size_t)IS_MEMORY_CHUNK_BYTES,
                                     &pending[index].address) != 0) {
      result = reject(manager, IS_MEMORY_ALLOCATION_FAILED,
                      IS_MEMORY_REJECTION_ALLOCATION_FAILED, rejection);
      rollback_pending(manager, pending, chunk_count, pool, registration,
                       &cleanup_failed);
      goto rollback_out;
    }
    if (manager->allocation.touch(manager->allocation.context,
                                  pending[index].address,
                                  (size_t)IS_MEMORY_CHUNK_BYTES) != 0) {
      result = reject(manager, IS_MEMORY_TOUCH_FAILED,
                      IS_MEMORY_REJECTION_TOUCH_FAILED, rejection);
      rollback_pending(manager, pending, chunk_count, pool, registration,
                       &cleanup_failed);
      goto rollback_out;
    }
  }
  for (index = 0; index < chunk_count; index++) {
    pending[index].registration = registration->register_chunk(
        registration->context, pending[index].address,
        (size_t)IS_MEMORY_CHUNK_BYTES);
    if (!pending[index].registration) {
      result = reject(manager, IS_MEMORY_REGISTRATION_FAILED,
                      IS_MEMORY_REJECTION_REGISTRATION_FAILED, rejection);
      rollback_pending(manager, pending, chunk_count, pool, registration,
                       &cleanup_failed);
      goto rollback_out;
    }
  }

  for (index = 0; index < chunk_count; index++) {
    struct is_memory_chunk *chunk = &manager->chunks[pending[index].index];

    if (pending[index].newly_allocated)
      chunk->address = pending[index].address;
    if (pending[index].converted_from_opportunistic) {
      manager->allocated_opportunistic_chunks--;
      manager->allocated_committed_chunks++;
    } else if (pending[index].newly_allocated) {
      if (pool == IS_MEMORY_POOL_OPPORTUNISTIC)
        manager->allocated_opportunistic_chunks++;
      else
        manager->allocated_committed_chunks++;
    }
    chunk->pool = pool;
    chunk->owner = owner;
    chunk->registration = pending[index].registration;
    chunk->registration_adapter = *registration;
    grants[index].provider_chunk_id = (uint32_t)pending[index].index;
    grants[index].address = chunk->address;
    grants[index].registration = chunk->registration;
    grants[index].pool = pool;
  }
  if (pool == IS_MEMORY_POOL_OPPORTUNISTIC) {
    manager->assigned_opportunistic_chunks += chunk_count;
    connection->assigned_opportunistic_chunks += chunk_count;
  } else {
    manager->assigned_committed_chunks += chunk_count;
    connection->assigned_committed_chunks += chunk_count;
  }
  manager->admissions += chunk_count;
  goto out;

rollback_out:
  if (cleanup_failed)
    result = reject(manager, IS_MEMORY_CLEANUP_FAILED,
                    IS_MEMORY_REJECTION_CLEANUP_FAILED, rejection);
out:
  pthread_mutex_unlock(&manager->lock);
  free(pending);
  return result;
}

enum is_memory_result is_memory_manager_release(
    struct is_memory_manager *manager, void *owner,
    const uint32_t provider_chunk_ids[], size_t chunk_count,
    enum is_memory_release_cause cause)
{
  struct is_memory_connection *connection;
  size_t index;
  size_t other;
  enum is_memory_result result = IS_MEMORY_OK;

  if (!manager || !owner || !provider_chunk_ids || chunk_count == 0 ||
      (cause != IS_MEMORY_RELEASE_CONSUMER &&
       cause != IS_MEMORY_RELEASE_PRESSURE &&
       cause != IS_MEMORY_RELEASE_DISCONNECT &&
       cause != IS_MEMORY_RELEASE_SHUTDOWN))
    return IS_MEMORY_INVALID_ARGUMENT;
  pthread_mutex_lock(&manager->lock);
  connection = find_connection(manager, owner);
  if (!connection) {
    result = IS_MEMORY_NOT_CONNECTED;
    goto out;
  }
  for (index = 0; index < chunk_count; index++) {
    uint32_t id = provider_chunk_ids[index];

    if (id >= manager->chunk_capacity ||
        manager->chunks[id].owner != owner) {
      result = IS_MEMORY_NOT_FOUND;
      goto out;
    }
    if (cause == IS_MEMORY_RELEASE_PRESSURE &&
        manager->chunks[id].pool == IS_MEMORY_POOL_COMMITTED) {
      result = reject(manager, IS_MEMORY_COMMITTED_PROTECTED,
                      IS_MEMORY_REJECTION_COMMITTED_PROTECTED, NULL);
      goto out;
    }
    for (other = 0; other < index; other++) {
      if (provider_chunk_ids[other] == id) {
        result = IS_MEMORY_INVALID_ARGUMENT;
        goto out;
      }
    }
  }
  for (index = 0; index < chunk_count; index++) {
    result = release_chunk_locked(manager, connection,
                                  &manager->chunks[provider_chunk_ids[index]],
                                  cause);
    if (result != IS_MEMORY_OK)
      break;
  }
out:
  pthread_mutex_unlock(&manager->lock);
  return result;
}

static enum is_memory_result allocate_unassigned_opportunistic_locked(
    struct is_memory_manager *manager, uint32_t count)
{
  struct pending_chunk *pending;
  uint32_t selected = 0;
  uint32_t index;
  enum is_memory_result result = IS_MEMORY_OK;

  if (count == 0)
    return IS_MEMORY_OK;
  pending = calloc(count, sizeof(*pending));
  if (!pending)
    return reject(manager, IS_MEMORY_ALLOCATION_FAILED,
                  IS_MEMORY_REJECTION_ALLOCATION_FAILED, NULL);
  for (index = 0; index < manager->chunk_capacity && selected < count;
       index++) {
    if (manager->chunks[index].address)
      continue;
    pending[selected].index = index;
    pending[selected].newly_allocated = 1;
    selected++;
  }
  if (selected != count) {
    result = reject(manager, IS_MEMORY_POOL_EXHAUSTED,
                    IS_MEMORY_REJECTION_POOL_LIMIT, NULL);
    goto out;
  }
  for (index = 0; index < count; index++) {
    if (manager->allocation.allocate(manager->allocation.context,
                                     manager->page_size,
                                     (size_t)IS_MEMORY_CHUNK_BYTES,
                                     &pending[index].address) != 0) {
      result = reject(manager, IS_MEMORY_ALLOCATION_FAILED,
                      IS_MEMORY_REJECTION_ALLOCATION_FAILED, NULL);
      break;
    }
    if (manager->allocation.touch(manager->allocation.context,
                                  pending[index].address,
                                  (size_t)IS_MEMORY_CHUNK_BYTES) != 0) {
      result = reject(manager, IS_MEMORY_TOUCH_FAILED,
                      IS_MEMORY_REJECTION_TOUCH_FAILED, NULL);
      break;
    }
  }
  if (result != IS_MEMORY_OK) {
    for (index = 0; index < count; index++) {
      if (pending[index].address)
        manager->allocation.release(manager->allocation.context,
                                    pending[index].address,
                                    (size_t)IS_MEMORY_CHUNK_BYTES);
    }
    goto out;
  }
  for (index = 0; index < count; index++) {
    struct is_memory_chunk *chunk = &manager->chunks[pending[index].index];

    chunk->address = pending[index].address;
    chunk->pool = IS_MEMORY_POOL_OPPORTUNISTIC;
  }
  manager->allocated_opportunistic_chunks += count;
out:
  free(pending);
  return result;
}

enum is_memory_result is_memory_manager_reconcile(
    struct is_memory_manager *manager,
    struct is_memory_reconcile_result *result)
{
  uint64_t available_bytes;
  uint64_t effective_chunks;
  uint32_t target;
  uint32_t desired_allocated;
  uint32_t reclaim;
  uint32_t index;
  enum is_memory_result operation_result = IS_MEMORY_OK;

  if (!manager || !result)
    return IS_MEMORY_INVALID_ARGUMENT;
  memset(result, 0, sizeof(*result));
  pthread_mutex_lock(&manager->lock);
  if (manager->accounting.read_available_bytes(
          manager->accounting.context, &available_bytes) != 0) {
    operation_result = reject(manager, IS_MEMORY_ACCOUNTING_FAILED,
                              IS_MEMORY_REJECTION_ACCOUNTING_FAILED, NULL);
    goto out;
  }
  effective_chunks = available_bytes / IS_MEMORY_CHUNK_BYTES;
  effective_chunks += manager->allocated_opportunistic_chunks;
  if (effective_chunks <= manager->config.host_reserve_gib)
    target = 0;
  else if (effective_chunks - manager->config.host_reserve_gib >=
           manager->config.max_opportunistic_gib)
    target = manager->config.max_opportunistic_gib;
  else
    target = (uint32_t)(effective_chunks -
                        manager->config.host_reserve_gib);

  if (target > manager->allocated_opportunistic_chunks) {
    uint32_t growth = target - manager->allocated_opportunistic_chunks;

    operation_result =
        allocate_unassigned_opportunistic_locked(manager, growth);
    if (operation_result == IS_MEMORY_OK)
      result->grown_opportunistic_chunks = growth;
    goto out;
  }
  desired_allocated = target;
  if (desired_allocated < manager->assigned_opportunistic_chunks)
    desired_allocated = manager->assigned_opportunistic_chunks;
  reclaim = manager->allocated_opportunistic_chunks - desired_allocated;
  for (index = 0; index < manager->chunk_capacity && reclaim != 0; index++) {
    struct is_memory_chunk *chunk = &manager->chunks[index];

    if (!chunk->address || chunk->owner || chunk->quarantined ||
        chunk->pool != IS_MEMORY_POOL_OPPORTUNISTIC)
      continue;
    manager->allocation.release(manager->allocation.context, chunk->address,
                                (size_t)IS_MEMORY_CHUNK_BYTES);
    memset(chunk, 0, sizeof(*chunk));
    manager->allocated_opportunistic_chunks--;
    result->reclaimed_unassigned_chunks++;
    manager->pressure_reclaims++;
    reclaim--;
  }
  if (manager->assigned_opportunistic_chunks > target)
    result->assigned_reclaim_needed =
        manager->assigned_opportunistic_chunks - target;
out:
  pthread_mutex_unlock(&manager->lock);
  return operation_result;
}

enum is_memory_result is_memory_manager_get_status(
    struct is_memory_manager *manager, void *owner,
    struct is_memory_manager_status *status)
{
  struct is_memory_connection *connection = NULL;
  uint64_t available_bytes;
  uint64_t available_chunks;
  uint64_t safe_new;
  uint64_t unassigned_opportunistic;
  uint64_t quarantined_opportunistic = 0;
  uint64_t opportunistic_room;
  uint64_t committed_room;
  uint64_t available_opportunistic;
  uint64_t available_committed;
  size_t chunk_index;
  enum is_memory_result result = IS_MEMORY_OK;

  if (!manager || !status)
    return IS_MEMORY_INVALID_ARGUMENT;
  pthread_mutex_lock(&manager->lock);
  if (owner) {
    connection = find_connection(manager, owner);
    if (!connection) {
      result = IS_MEMORY_NOT_CONNECTED;
      goto out;
    }
  }
  if (manager->accounting.read_available_bytes(
          manager->accounting.context, &available_bytes) != 0) {
    result = IS_MEMORY_ACCOUNTING_FAILED;
    goto out;
  }
  memset(status, 0, sizeof(*status));
  available_chunks = available_bytes / IS_MEMORY_CHUNK_BYTES;
  safe_new = available_chunks > manager->config.host_reserve_gib
                 ? available_chunks - manager->config.host_reserve_gib
                 : 0;
  for (chunk_index = 0; chunk_index < manager->chunk_capacity;
       chunk_index++) {
    if (manager->chunks[chunk_index].quarantined &&
        manager->chunks[chunk_index].pool == IS_MEMORY_POOL_OPPORTUNISTIC)
      quarantined_opportunistic++;
  }
  unassigned_opportunistic = manager->allocated_opportunistic_chunks -
                             manager->assigned_opportunistic_chunks -
                             quarantined_opportunistic;
  opportunistic_room = manager->config.max_opportunistic_gib -
                       manager->allocated_opportunistic_chunks;
  committed_room = manager->config.max_committed_gib -
                   manager->allocated_committed_chunks;
  available_opportunistic = unassigned_opportunistic +
                            (safe_new < opportunistic_room
                                 ? safe_new
                                 : opportunistic_room);
  available_committed = unassigned_opportunistic + safe_new;
  if (available_committed > committed_room)
    available_committed = committed_room;

  status->host_reserve_chunks = manager->config.host_reserve_gib;
  status->max_opportunistic_chunks =
      manager->config.max_opportunistic_gib;
  status->max_committed_chunks = manager->config.max_committed_gib;
  status->allocated_opportunistic_chunks =
      manager->allocated_opportunistic_chunks;
  status->assigned_opportunistic_chunks =
      manager->assigned_opportunistic_chunks;
  status->allocated_committed_chunks = manager->allocated_committed_chunks;
  status->assigned_committed_chunks = manager->assigned_committed_chunks;
  status->quarantined_chunks = manager->quarantined_chunks;
  status->available_opportunistic_chunks =
      (uint32_t)available_opportunistic;
  status->available_committed_chunks = (uint32_t)available_committed;
  status->active_connections = (uint32_t)manager->connection_count;
  status->admissions = manager->admissions;
  status->rejections = manager->rejections;
  status->pressure_reclaims = manager->pressure_reclaims;
  status->last_rejection = manager->last_rejection;
  status->healthy = manager->healthy;
  if (connection) {
    uint32_t consumer_opportunistic;
    uint32_t consumer_committed;
    uint32_t remaining;

    assignments_for_consumer(manager, connection->consumer_id,
                             &consumer_opportunistic,
                             &consumer_committed);
    status->consumer_connections = connection_count_for_consumer(
        manager, connection->consumer_id);
    status->consumer_assigned_opportunistic_chunks =
        consumer_opportunistic;
    status->consumer_assigned_committed_chunks = consumer_committed;
    remaining = connection->max_opportunistic_chunks -
                consumer_opportunistic;
    if (status->available_opportunistic_chunks > remaining)
      status->available_opportunistic_chunks = remaining;
    remaining = connection->max_committed_chunks - consumer_committed;
    if (status->available_committed_chunks > remaining)
      status->available_committed_chunks = remaining;
  }
out:
  pthread_mutex_unlock(&manager->lock);
  return result;
}

static char *trim(char *value)
{
  char *end;

  while (*value == ' ' || *value == '\t')
    value++;
  end = value + strlen(value);
  while (end > value &&
         (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
          end[-1] == '\n'))
    end--;
  *end = '\0';
  return value;
}

static int parse_u32(const char *value, uint32_t *parsed)
{
  char *end = NULL;
  unsigned long number;

  if (!value[0] || value[0] == '-' || value[0] == '+')
    return 0;
  errno = 0;
  number = strtoul(value, &end, 10);
  if (errno != 0 || !end || *end != '\0' || number > UINT32_MAX)
    return 0;
  *parsed = (uint32_t)number;
  return 1;
}

enum is_memory_result is_memory_manager_config_load(
    const char *path, struct is_memory_manager_config *config)
{
  FILE *file;
  struct is_memory_manager_config parsed = {0};
  char buffer[256];
  uint32_t seen = 0;
  enum is_memory_result result = IS_MEMORY_CONFIG_INVALID;

  if (!path || !config)
    return IS_MEMORY_INVALID_ARGUMENT;
  file = fopen(path, "r");
  if (!file)
    return IS_MEMORY_CONFIG_INVALID;
  while (fgets(buffer, sizeof(buffer), file)) {
    char *line = trim(buffer);
    char *separator;
    char *key;
    char *value;
    uint32_t field;
    uint32_t number;

    if (!line[0] || line[0] == '#' || line[0] == ';')
      continue;
    separator = strchr(line, '=');
    if (!separator)
      goto out;
    *separator = '\0';
    key = trim(line);
    value = trim(separator + 1);
    if (strcmp(key, "version") == 0)
      field = 1U << 0;
    else if (strcmp(key, "host_reserve_gib") == 0)
      field = 1U << 1;
    else if (strcmp(key, "max_opportunistic_gib") == 0)
      field = 1U << 2;
    else if (strcmp(key, "max_committed_gib") == 0)
      field = 1U << 3;
    else
      goto out;
    if (seen & field)
      goto out;
    seen |= field;
    if (!parse_u32(value, &number))
      goto out;
    if (field == (1U << 0)) {
      if (number != 1)
        goto out;
    } else if (field == (1U << 1)) {
      parsed.host_reserve_gib = number;
    } else if (field == (1U << 2)) {
      parsed.max_opportunistic_gib = number;
    } else {
      parsed.max_committed_gib = number;
    }
  }
  if (ferror(file) || seen != 0x0fU || !config_valid(&parsed))
    goto out;
  *config = parsed;
  result = IS_MEMORY_OK;
out:
  fclose(file);
  return result;
}

static int linux_read_available_bytes(void *context, uint64_t *available_bytes)
{
  FILE *file;
  char buffer[256];

  (void)context;
  if (!available_bytes)
    return -1;
  file = fopen("/proc/meminfo", "r");
  if (!file)
    return -1;
  while (fgets(buffer, sizeof(buffer), file)) {
    unsigned long long available_kib;
    char unit[16];

    if (sscanf(buffer, "MemAvailable: %llu %15s",
               &available_kib, unit) != 2)
      continue;
    if (strcmp(unit, "kB") != 0 ||
        available_kib > UINT64_MAX / UINT64_C(1024)) {
      fclose(file);
      return -1;
    }
    *available_bytes = (uint64_t)available_kib * UINT64_C(1024);
    fclose(file);
    return 0;
  }
  fclose(file);
  return -1;
}

static int posix_allocate(void *context, size_t alignment, size_t size,
                          void **address)
{
  (void)context;
  return posix_memalign(address, alignment, size);
}

static int posix_touch(void *context, void *address, size_t size)
{
  (void)context;
  memset(address, 0, size);
  return 0;
}

static void posix_release(void *context, void *address, size_t size)
{
  (void)context;
  (void)size;
  free(address);
}

void is_memory_linux_accounting_adapter(
    struct is_memory_accounting_adapter *adapter)
{
  if (!adapter)
    return;
  adapter->context = NULL;
  adapter->read_available_bytes = linux_read_available_bytes;
}

void is_memory_posix_allocation_adapter(
    struct is_memory_allocation_adapter *adapter)
{
  if (!adapter)
    return;
  adapter->context = NULL;
  adapter->allocate = posix_allocate;
  adapter->touch = posix_touch;
  adapter->release = posix_release;
}

const char *is_memory_result_name(enum is_memory_result result)
{
  static const char *const names[] = {
    "ok",
    "invalid argument",
    "invalid configuration",
    "memory accounting failed",
    "chunk allocation failed",
    "chunk touch failed",
    "RDMA registration failed",
    "resource cleanup failed",
    "connection limit exceeded",
    "Consumer quota exceeded",
    "pool limit exceeded",
    "Host Reserve would be violated",
    "connection is not registered",
    "chunk assignment not found",
    "committed assignment is protected"
  };

  if ((unsigned int)result >= sizeof(names) / sizeof(names[0]))
    return "unknown memory manager result";
  return names[result];
}

const char *is_memory_rejection_name(
    enum is_memory_rejection_reason rejection)
{
  static const char *const names[] = {
    "none",
    "invalid_request",
    "connection_limit",
    "consumer_quota",
    "pool_limit",
    "host_reserve",
    "memory_accounting_failed",
    "allocation_failed",
    "touch_failed",
    "registration_failed",
    "cleanup_failed",
    "committed_protected"
  };

  if ((unsigned int)rejection >= sizeof(names) / sizeof(names[0]))
    return "unknown";
  return names[rejection];
}
