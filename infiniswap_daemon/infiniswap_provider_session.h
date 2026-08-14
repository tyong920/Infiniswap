/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#ifndef INFINISWAP_PROVIDER_SESSION_H
#define INFINISWAP_PROVIDER_SESSION_H

#include "infiniswap_auth.h"

#include <stddef.h>
#include <stdint.h>

enum is_provider_session_state {
  IS_PROVIDER_SESSION_WAIT_HELLO = 0,
  IS_PROVIDER_SESSION_WAIT_AUTH,
  IS_PROVIDER_SESSION_READY,
  IS_PROVIDER_SESSION_CLOSED
};

struct is_provider_session {
  enum is_provider_session_state state;
  struct is_auth_registry *registry;
  uint16_t local_minor;
  uint16_t negotiated_minor;
  uint64_t supported_capabilities;
  uint64_t required_capabilities;
  uint64_t negotiated_capabilities;
  uint64_t session_id;
  uint64_t last_request_id;
  uint64_t authenticated_until_unix;
  uint64_t authorization_version;
  uint32_t failure_deadline_ms;
  uint16_t last_error_code;
  uint8_t selected_mode;
  uint8_t selected_pool;
  uint8_t provider_nonce[IS_PROTOCOL_NONCE_SIZE];
  char consumer_id[IS_PROTOCOL_CONSUMER_ID_MAX + 1U];
  char key_id[IS_PROTOCOL_KEY_ID_MAX + 1U];
  uint8_t hello_frame[IS_PROTOCOL_MAX_FRAME_SIZE];
  size_t hello_size;
  uint8_t challenge_frame[IS_PROTOCOL_MAX_FRAME_SIZE];
  size_t challenge_size;
};

struct is_provider_session_outcome {
  int response_ready;
  int request_ready;
  int close_after_response;
  size_t response_size;
  struct is_protocol_message request;
};

void is_provider_session_init(
    struct is_provider_session *session, struct is_auth_registry *registry,
    uint16_t local_minor, uint64_t supported_capabilities,
    uint64_t required_capabilities,
    const uint8_t provider_nonce[IS_PROTOCOL_NONCE_SIZE], uint64_t session_id);

int is_provider_session_handle(
    struct is_provider_session *session, const uint8_t *frame,
    size_t frame_size, uint8_t *response, size_t response_capacity,
    struct is_provider_session_outcome *outcome);

int is_provider_session_fail(
    struct is_provider_session *session, uint64_t request_id,
    uint16_t offending_type, enum is_protocol_error_code code,
    uint8_t *response, size_t response_capacity, size_t *response_size);

#endif /* INFINISWAP_PROVIDER_SESSION_H */
