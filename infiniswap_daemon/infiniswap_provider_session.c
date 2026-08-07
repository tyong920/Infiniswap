/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#include "infiniswap_provider_session.h"

#include <string.h>
#include <time.h>

static uint16_t frame_type(const uint8_t *frame, size_t frame_size)
{
  if (frame_size < 10U)
    return IS_PROTOCOL_MSG_HELLO;
  return (uint16_t)(((uint16_t)frame[8] << 8) | frame[9]);
}

static uint64_t frame_request_id(const uint8_t *frame, size_t frame_size)
{
  uint64_t value = 0;
  size_t index;

  if (frame_size < 24U)
    return 1;
  for (index = 16; index < 24; index++)
    value = (value << 8) | frame[index];
  return value == 0 ? 1 : value;
}

static enum is_protocol_error_code
protocol_result_to_error(enum is_protocol_result result)
{
  switch (result) {
  case IS_PROTOCOL_UNSUPPORTED_VERSION:
    return IS_PROTOCOL_ERROR_VERSION;
  case IS_PROTOCOL_UNKNOWN_REQUIRED_CAPABILITY:
  case IS_PROTOCOL_INVALID_CAPABILITIES:
    return IS_PROTOCOL_ERROR_CAPABILITY;
  default:
    return IS_PROTOCOL_ERROR_MALFORMED;
  }
}

static int build_error(struct is_provider_session *session,
                       uint64_t request_id, uint16_t offending_type,
                       enum is_protocol_error_code code, uint8_t *response,
                       size_t response_capacity,
                       struct is_provider_session_outcome *outcome)
{
  struct is_protocol_message error;
  enum is_protocol_result result;

  memset(&error, 0, sizeof(error));
  error.header.major = IS_PROTOCOL_MAJOR;
  error.header.minor = session->negotiated_minor;
  error.header.type = IS_PROTOCOL_MSG_ERROR;
  error.header.flags = IS_PROTOCOL_FLAG_RESPONSE | IS_PROTOCOL_FLAG_ERROR;
  error.header.request_id = request_id == 0 ? 1 : request_id;
  error.header.session_id = session->state == IS_PROVIDER_SESSION_READY
                                ? session->session_id
                                : 0;
  error.header.capabilities = session->negotiated_capabilities;
  error.header.required_capabilities =
      session->required_capabilities & session->negotiated_capabilities;
  error.payload.error.code = code;
  error.payload.error.offending_type = offending_type;
  error.payload.error.retryable = code == IS_PROTOCOL_ERROR_RESOURCE;
  result = is_protocol_encode(&error, response, response_capacity,
                              &outcome->response_size);
  if (result != IS_PROTOCOL_OK)
    return -1;
  outcome->response_ready = 1;
  outcome->close_after_response = 1;
  session->state = IS_PROVIDER_SESSION_CLOSED;
  return 0;
}

void is_provider_session_init(
    struct is_provider_session *session, struct is_auth_registry *registry,
    uint16_t local_minor, uint64_t supported_capabilities,
    uint64_t required_capabilities,
    const uint8_t provider_nonce[IS_PROTOCOL_NONCE_SIZE], uint64_t session_id)
{
  memset(session, 0, sizeof(*session));
  session->state = IS_PROVIDER_SESSION_WAIT_HELLO;
  session->registry = registry;
  session->local_minor = local_minor;
  session->negotiated_minor = local_minor;
  session->supported_capabilities = supported_capabilities;
  session->required_capabilities = required_capabilities;
  session->negotiated_capabilities = supported_capabilities;
  session->session_id = session_id;
  memcpy(session->provider_nonce, provider_nonce, IS_PROTOCOL_NONCE_SIZE);
}

static int hello_capabilities_valid(const struct is_protocol_message *hello,
                                    uint64_t negotiated_capabilities)
{
  uint64_t mode_capability;
  uint64_t pool_capability;

  if ((hello->payload.hello.mode == IS_PROTOCOL_MODE_BACKED &&
       hello->payload.hello.pool != IS_PROTOCOL_POOL_OPPORTUNISTIC) ||
      (hello->payload.hello.mode == IS_PROTOCOL_MODE_REMOTE_ONLY &&
       hello->payload.hello.pool != IS_PROTOCOL_POOL_COMMITTED))
    return 0;
  mode_capability = hello->payload.hello.mode == IS_PROTOCOL_MODE_BACKED
                        ? IS_PROTOCOL_CAP_BACKED
                        : IS_PROTOCOL_CAP_REMOTE_ONLY;
  pool_capability =
      hello->payload.hello.pool == IS_PROTOCOL_POOL_OPPORTUNISTIC
          ? IS_PROTOCOL_CAP_OPPORTUNISTIC_POOL
          : IS_PROTOCOL_CAP_COMMITTED_POOL;
  return (negotiated_capabilities & mode_capability) != 0 &&
         (negotiated_capabilities & pool_capability) != 0 &&
         (negotiated_capabilities & IS_PROTOCOL_CAP_FAILURE_DEADLINE) != 0 &&
         (negotiated_capabilities & IS_PROTOCOL_CAP_AUTH_HMAC_SHA256) != 0;
}

static int handle_hello(struct is_provider_session *session,
                        const struct is_protocol_message *hello,
                        const uint8_t *frame, size_t frame_size,
                        uint8_t *response, size_t response_capacity,
                        struct is_provider_session_outcome *outcome)
{
  struct is_protocol_message challenge;
  enum is_protocol_result result;

  result = is_protocol_negotiate(
      session->local_minor, session->supported_capabilities,
      session->required_capabilities, hello->header.minor,
      hello->header.capabilities, hello->header.required_capabilities,
      &session->negotiated_minor, &session->negotiated_capabilities);
  if (result != IS_PROTOCOL_OK ||
      !hello_capabilities_valid(hello, session->negotiated_capabilities))
    return build_error(session, hello->header.request_id,
                       IS_PROTOCOL_MSG_HELLO,
                       IS_PROTOCOL_ERROR_CAPABILITY, response,
                       response_capacity, outcome);

  memcpy(session->hello_frame, frame, frame_size);
  session->hello_size = frame_size;
  strcpy(session->consumer_id, hello->payload.hello.consumer_id);
  strcpy(session->key_id, hello->payload.hello.key_id);
  session->selected_mode = hello->payload.hello.mode;
  session->selected_pool = hello->payload.hello.pool;
  session->failure_deadline_ms =
      hello->payload.hello.failure_deadline_ms;
  session->last_request_id = hello->header.request_id;

  memset(&challenge, 0, sizeof(challenge));
  challenge.header.major = IS_PROTOCOL_MAJOR;
  challenge.header.minor = session->negotiated_minor;
  challenge.header.type = IS_PROTOCOL_MSG_CHALLENGE;
  challenge.header.flags = IS_PROTOCOL_FLAG_RESPONSE;
  challenge.header.request_id = hello->header.request_id;
  challenge.header.capabilities = session->negotiated_capabilities;
  challenge.header.required_capabilities = session->required_capabilities;
  challenge.payload.challenge.negotiated_minor = session->negotiated_minor;
  memcpy(challenge.payload.challenge.nonce, session->provider_nonce,
         IS_PROTOCOL_NONCE_SIZE);
  result = is_protocol_encode(&challenge, response, response_capacity,
                              &outcome->response_size);
  if (result != IS_PROTOCOL_OK)
    return -1;
  memcpy(session->challenge_frame, response, outcome->response_size);
  session->challenge_size = outcome->response_size;
  session->state = IS_PROVIDER_SESSION_WAIT_AUTH;
  outcome->response_ready = 1;
  return 0;
}

static enum is_protocol_error_code auth_error_code(enum is_auth_result result)
{
  return result == IS_AUTH_REVOKED ? IS_PROTOCOL_ERROR_REVOKED
                                   : IS_PROTOCOL_ERROR_AUTHENTICATION;
}

static int handle_auth(struct is_provider_session *session,
                       const struct is_protocol_message *auth,
                       uint8_t *response, size_t response_capacity,
                       struct is_provider_session_outcome *outcome)
{
  struct is_protocol_message accept;
  enum is_auth_result auth_result;
  enum is_protocol_result protocol_result;

  if (auth->header.request_id <= session->last_request_id)
    return build_error(session, auth->header.request_id,
                       IS_PROTOCOL_MSG_AUTH, IS_PROTOCOL_ERROR_REPLAY,
                       response, response_capacity, outcome);
  if (auth->header.minor != session->negotiated_minor ||
      auth->header.capabilities != session->negotiated_capabilities)
    return build_error(session, auth->header.request_id,
                       IS_PROTOCOL_MSG_AUTH, IS_PROTOCOL_ERROR_CAPABILITY,
                       response, response_capacity, outcome);

  auth_result = is_auth_registry_verify(
      session->registry, session->consumer_id, session->key_id,
      session->hello_frame, session->hello_size,
      session->challenge_frame, session->challenge_size,
      auth->payload.auth.tag, &session->authenticated_until_unix,
      &session->authorization_version);
  if (auth_result != IS_AUTH_OK)
    return build_error(session, auth->header.request_id,
                       IS_PROTOCOL_MSG_AUTH, auth_error_code(auth_result),
                       response, response_capacity, outcome);

  session->last_request_id = auth->header.request_id;
  session->state = IS_PROVIDER_SESSION_READY;
  memset(&accept, 0, sizeof(accept));
  accept.header.major = IS_PROTOCOL_MAJOR;
  accept.header.minor = session->negotiated_minor;
  accept.header.type = IS_PROTOCOL_MSG_ACCEPT;
  accept.header.flags = IS_PROTOCOL_FLAG_RESPONSE;
  accept.header.request_id = auth->header.request_id;
  accept.header.session_id = session->session_id;
  accept.header.capabilities = session->negotiated_capabilities;
  accept.header.required_capabilities = session->required_capabilities;
  accept.payload.accept.negotiated_minor = session->negotiated_minor;
  protocol_result = is_protocol_encode(&accept, response, response_capacity,
                                       &outcome->response_size);
  if (protocol_result != IS_PROTOCOL_OK)
    return -1;
  outcome->response_ready = 1;
  return 0;
}

static int authenticated_request_allowed(
    const struct is_protocol_message *message, int response)
{
  if (response)
    return message->header.type == IS_PROTOCOL_MSG_ACTIVITY ||
           message->header.type == IS_PROTOCOL_MSG_RELEASE;
  switch (message->header.type) {
  case IS_PROTOCOL_MSG_STATUS_REQUEST:
  case IS_PROTOCOL_MSG_CHUNK_REQUEST:
  case IS_PROTOCOL_MSG_GOODBYE:
    return 1;
  default:
    return 0;
  }
}

static int handle_authenticated(struct is_provider_session *session,
                                const struct is_protocol_message *message,
                                uint8_t *response,
                                size_t response_capacity,
                                struct is_provider_session_outcome *outcome)
{
  enum is_auth_result revoke_result;
  time_t now;
  int response_message =
      (message->header.flags & IS_PROTOCOL_FLAG_RESPONSE) != 0;

  if (!response_message &&
      message->header.request_id <= session->last_request_id)
    return build_error(session, message->header.request_id,
                       message->header.type, IS_PROTOCOL_ERROR_REPLAY,
                       response, response_capacity, outcome);
  if (message->header.session_id != session->session_id ||
      message->header.minor != session->negotiated_minor ||
      message->header.capabilities != session->negotiated_capabilities)
    return build_error(session, message->header.request_id,
                       message->header.type,
                       IS_PROTOCOL_ERROR_CAPABILITY, response,
                       response_capacity, outcome);
  if (session->authenticated_until_unix != 0) {
    now = time(NULL);
    if (now < 0 || (uint64_t)now >= session->authenticated_until_unix)
      return build_error(session, message->header.request_id,
                         message->header.type,
                         IS_PROTOCOL_ERROR_AUTHENTICATION, response,
                         response_capacity, outcome);
  }
  revoke_result = is_auth_registry_validate_authorization(
      session->registry, session->consumer_id,
      session->authorization_version);
  if (revoke_result != IS_AUTH_OK)
    return build_error(session, message->header.request_id,
                       message->header.type,
                       revoke_result == IS_AUTH_REVOKED
                           ? IS_PROTOCOL_ERROR_REVOKED
                           : IS_PROTOCOL_ERROR_AUTHENTICATION,
                       response, response_capacity, outcome);
  if (!authenticated_request_allowed(message, response_message))
    return build_error(session, message->header.request_id,
                       message->header.type,
                       IS_PROTOCOL_ERROR_OUT_OF_ORDER, response,
                       response_capacity, outcome);
  if (message->header.type == IS_PROTOCOL_MSG_STATUS_REQUEST &&
      !(session->negotiated_capabilities & IS_PROTOCOL_CAP_STATUS))
    return build_error(session, message->header.request_id,
                       message->header.type,
                       IS_PROTOCOL_ERROR_CAPABILITY, response,
                       response_capacity, outcome);

  if (message->header.type == IS_PROTOCOL_MSG_CHUNK_REQUEST &&
      message->payload.chunk_request.pool != session->selected_pool)
    return build_error(session, message->header.request_id,
                       message->header.type,
                       IS_PROTOCOL_ERROR_CAPABILITY, response,
                       response_capacity, outcome);
  if (!response_message)
    session->last_request_id = message->header.request_id;
  outcome->request = *message;
  outcome->request_ready = 1;
  return 0;
}

int is_provider_session_fail(
    struct is_provider_session *session, uint64_t request_id,
    uint16_t offending_type, enum is_protocol_error_code code,
    uint8_t *response, size_t response_capacity, size_t *response_size)
{
  struct is_provider_session_outcome outcome;
  int result;

  if (!session || !response || !response_size)
    return -1;
  memset(&outcome, 0, sizeof(outcome));
  result = build_error(session, request_id, offending_type, code,
                       response, response_capacity, &outcome);
  if (result == 0)
    *response_size = outcome.response_size;
  return result;
}

int is_provider_session_handle(
    struct is_provider_session *session, const uint8_t *frame,
    size_t frame_size, uint8_t *response, size_t response_capacity,
    struct is_provider_session_outcome *outcome)
{
  struct is_protocol_message message;
  enum is_protocol_result result;

  if (!session || !frame || !response || !outcome || !session->registry ||
      session->session_id == 0 || !response_capacity)
    return -1;
  memset(outcome, 0, sizeof(*outcome));
  result = is_protocol_decode(frame, frame_size, &message);
  if (result != IS_PROTOCOL_OK) {
    if (frame_type(frame, frame_size) == IS_PROTOCOL_MSG_ERROR) {
      session->state = IS_PROVIDER_SESSION_CLOSED;
      outcome->close_after_response = 1;
      return 0;
    }
    return build_error(session, frame_request_id(frame, frame_size),
                       frame_type(frame, frame_size),
                       protocol_result_to_error(result), response,
                       response_capacity, outcome);
  }
  if (message.header.type == IS_PROTOCOL_MSG_ERROR) {
    if (session->state == IS_PROVIDER_SESSION_READY) {
      outcome->request = message;
      outcome->request_ready = 1;
    }
    outcome->close_after_response = 1;
    session->state = IS_PROVIDER_SESSION_CLOSED;
    return 0;
  }

  switch (session->state) {
  case IS_PROVIDER_SESSION_WAIT_HELLO:
    if (message.header.type != IS_PROTOCOL_MSG_HELLO ||
        message.header.flags != 0)
      return build_error(session, message.header.request_id,
                         message.header.type,
                         IS_PROTOCOL_ERROR_OUT_OF_ORDER, response,
                         response_capacity, outcome);
    return handle_hello(session, &message, frame, frame_size, response,
                        response_capacity, outcome);
  case IS_PROVIDER_SESSION_WAIT_AUTH:
    if (message.header.type != IS_PROTOCOL_MSG_AUTH ||
        message.header.flags != 0)
      return build_error(session, message.header.request_id,
                         message.header.type,
                         IS_PROTOCOL_ERROR_OUT_OF_ORDER, response,
                         response_capacity, outcome);
    return handle_auth(session, &message, response, response_capacity,
                       outcome);
  case IS_PROVIDER_SESSION_READY:
    return handle_authenticated(session, &message, response,
                                response_capacity, outcome);
  case IS_PROVIDER_SESSION_CLOSED:
  default:
    return build_error(session, message.header.request_id,
                       message.header.type,
                       IS_PROTOCOL_ERROR_OUT_OF_ORDER, response,
                       response_capacity, outcome);
  }
}
