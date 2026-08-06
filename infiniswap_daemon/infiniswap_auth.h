/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#ifndef INFINISWAP_AUTH_H
#define INFINISWAP_AUTH_H

#include "infiniswap_protocol.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define IS_AUTH_SECRET_MIN_SIZE 32U
#define IS_AUTH_SECRET_MAX_SIZE 64U
#define IS_AUTH_MAX_CONSUMERS 64U
#define IS_AUTH_MAX_KEY_OVERLAP_SECONDS (30U * 24U * 60U * 60U)

enum is_auth_result {
  IS_AUTH_OK = 0,
  IS_AUTH_INVALID_ARGUMENT,
  IS_AUTH_FULL,
  IS_AUTH_DUPLICATE,
  IS_AUTH_NOT_FOUND,
  IS_AUTH_REVOKED,
  IS_AUTH_FAILED,
  IS_AUTH_CRYPTO_ERROR,
  IS_AUTH_IO_ERROR,
  IS_AUTH_FORMAT_ERROR
};

struct is_auth_key {
  char key_id[IS_PROTOCOL_KEY_ID_MAX + 1U];
  uint8_t secret[IS_AUTH_SECRET_MAX_SIZE];
  size_t secret_size;
};

struct is_auth_credential {
  char consumer_id[IS_PROTOCOL_CONSUMER_ID_MAX + 1U];
  struct is_auth_key current;
  struct is_auth_key next;
  uint64_t next_valid_until_unix;
  uint32_t max_connections;
  uint32_t max_opportunistic_chunks;
  uint32_t max_committed_chunks;
  uint64_t authorization_version;
  int revoked;
};

typedef void (*is_auth_revocation_callback)(void *context);

struct is_auth_listener {
  char consumer_id[IS_PROTOCOL_CONSUMER_ID_MAX + 1U];
  is_auth_revocation_callback callback;
  void *context;
};

struct is_auth_registry {
  pthread_mutex_t lock;
  struct is_auth_credential entries[IS_AUTH_MAX_CONSUMERS];
  struct is_auth_listener listeners[IS_AUTH_MAX_CONSUMERS];
  size_t count;
  size_t listener_count;
  uint64_t next_authorization_version;
};

void is_auth_registry_init(struct is_auth_registry *registry);
void is_auth_registry_destroy(struct is_auth_registry *registry);

enum is_auth_result
is_auth_registry_add(struct is_auth_registry *registry,
                     const struct is_auth_credential *credential);

enum is_auth_result
is_auth_registry_load_file(struct is_auth_registry *registry,
                           const char *path);

enum is_auth_result
is_auth_registry_subscribe(struct is_auth_registry *registry,
                           const char *consumer_id,
                           uint64_t authorization_version,
                           is_auth_revocation_callback callback,
                           void *context);

void is_auth_registry_unsubscribe(struct is_auth_registry *registry,
                                  void *context);

enum is_auth_result
is_auth_registry_revoke(struct is_auth_registry *registry,
                        const char *consumer_id);

enum is_auth_result
is_auth_registry_get_limits(struct is_auth_registry *registry,
                            const char *consumer_id,
                            uint32_t *max_connections,
                            uint32_t *max_opportunistic_chunks,
                            uint32_t *max_committed_chunks);

enum is_auth_result
is_auth_registry_begin_authorized_operation(
    struct is_auth_registry *registry, const char *consumer_id,
    uint64_t authorization_version);

void is_auth_registry_end_authorized_operation(
    struct is_auth_registry *registry);

enum is_auth_result
is_auth_registry_validate_authorization(
    struct is_auth_registry *registry, const char *consumer_id,
    uint64_t authorization_version);

enum is_auth_result
is_auth_registry_is_revoked(struct is_auth_registry *registry,
                            const char *consumer_id);

enum is_auth_result
is_auth_compute_tag(const uint8_t *secret, size_t secret_size,
                    const uint8_t *hello_frame, size_t hello_size,
                    const uint8_t *challenge_frame, size_t challenge_size,
                    uint8_t tag[IS_PROTOCOL_AUTH_TAG_SIZE]);

enum is_auth_result
is_auth_registry_verify(struct is_auth_registry *registry,
                        const char *consumer_id, const char *key_id,
                        const uint8_t *hello_frame, size_t hello_size,
                        const uint8_t *challenge_frame,
                        size_t challenge_size,
                        const uint8_t tag[IS_PROTOCOL_AUTH_TAG_SIZE],
                        uint64_t *authenticated_until_unix,
                        uint64_t *authorization_version);

const char *is_auth_result_name(enum is_auth_result result);

#endif /* INFINISWAP_AUTH_H */
