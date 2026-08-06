#include "infiniswap_auth.h"
#include "infiniswap_provider_session.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static void set_credential(struct is_auth_credential *credential)
{
  static const uint8_t current_secret[] =
      "current-secret-32-bytes-00000000";
  static const uint8_t next_secret[] =
      "next-secret-32-bytes-00000000000";

  memset(credential, 0, sizeof(*credential));
  strcpy(credential->consumer_id, "consumer-a");
  strcpy(credential->current.key_id, "key-current");
  memcpy(credential->current.secret, current_secret,
         sizeof(current_secret) - 1U);
  credential->current.secret_size = sizeof(current_secret) - 1U;
  strcpy(credential->next.key_id, "key-next");
  memcpy(credential->next.secret, next_secret, sizeof(next_secret) - 1U);
  credential->next.secret_size = sizeof(next_secret) - 1U;
  credential->next_valid_until_unix = (uint64_t)time(NULL) + 3600U;
  credential->max_opportunistic_chunks = 32;
  credential->max_committed_chunks = 16;
}

static void count_revocation(void *context)
{
  int *count = context;

  (*count)++;
}

static int test_hmac_rotation_and_revocation(void)
{
  static const uint8_t expected[] = {
    0x65, 0x92, 0x44, 0x53, 0x8d, 0x5f, 0x8f, 0x19,
    0x25, 0x30, 0xd5, 0x9b, 0x4b, 0x9f, 0xb5, 0xa4,
    0xed, 0x25, 0x6e, 0x0a, 0x08, 0x21, 0x21, 0x15,
    0x70, 0x0a, 0x62, 0x1d, 0x65, 0x04, 0x10, 0x20
  };
  static const uint8_t hello[] = "hello-frame";
  static const uint8_t challenge[] = "challenge-frame";
  struct is_auth_credential credential;
  struct is_auth_registry registry;
  uint64_t authenticated_until_unix = 0;
  uint64_t authorization_version = 0;
  uint8_t tag[IS_PROTOCOL_AUTH_TAG_SIZE];
  int revocations = 0;

  set_credential(&credential);
  is_auth_registry_init(&registry);
  if (is_auth_registry_add(&registry, &credential) != IS_AUTH_OK ||
      is_auth_compute_tag(credential.current.secret,
                          credential.current.secret_size,
                          hello, sizeof(hello) - 1U,
                          challenge, sizeof(challenge) - 1U,
                          tag) != IS_AUTH_OK ||
      memcmp(tag, expected, sizeof(expected)) != 0) {
    fprintf(stderr, "current PSK does not match the HMAC golden vector\n");
    return 1;
  }
  if (is_auth_registry_verify(&registry, "consumer-a", "key-current",
                              hello, sizeof(hello) - 1U,
                              challenge, sizeof(challenge) - 1U,
                              tag, &authenticated_until_unix,
                              &authorization_version) != IS_AUTH_OK ||
      authenticated_until_unix != 0 || authorization_version == 0) {
    fprintf(stderr, "current PSK was not accepted\n");
    return 1;
  }
  if (is_auth_compute_tag(credential.next.secret,
                          credential.next.secret_size,
                          hello, sizeof(hello) - 1U,
                          challenge, sizeof(challenge) - 1U,
                          tag) != IS_AUTH_OK ||
      is_auth_registry_verify(&registry, "consumer-a", "key-next",
                              hello, sizeof(hello) - 1U,
                              challenge, sizeof(challenge) - 1U,
                              tag, &authenticated_until_unix,
                              &authorization_version) != IS_AUTH_OK ||
      authenticated_until_unix != credential.next_valid_until_unix ||
      authorization_version == 0) {
    fprintf(stderr, "next PSK was not accepted during overlap\n");
    return 1;
  }
  tag[0] ^= 1U;
  if (is_auth_registry_verify(&registry, "consumer-a", "key-next",
                              hello, sizeof(hello) - 1U,
                              challenge, sizeof(challenge) - 1U,
                              tag, &authenticated_until_unix,
                              &authorization_version) !=
          IS_AUTH_FAILED ||
      authenticated_until_unix != 0 || authorization_version != 0) {
    fprintf(stderr, "an invalid authentication tag was accepted\n");
    return 1;
  }
  if (is_auth_registry_subscribe(
          &registry, "consumer-a",
          registry.entries[0].authorization_version,
          count_revocation, &revocations) != IS_AUTH_OK ||
      is_auth_registry_revoke(&registry, "consumer-a") != IS_AUTH_OK ||
      revocations != 1 ||
      is_auth_registry_verify(&registry, "consumer-a", "key-current",
                              hello, sizeof(hello) - 1U,
                              challenge, sizeof(challenge) - 1U,
                              expected, &authenticated_until_unix,
                              &authorization_version) !=
          IS_AUTH_REVOKED) {
    fprintf(stderr, "revocation was not immediate\n");
    return 1;
  }
  is_auth_registry_destroy(&registry);
  return 0;
}

static void init_hello(struct is_protocol_message *hello, uint16_t minor)
{
  size_t index;

  memset(hello, 0, sizeof(*hello));
  hello->header.major = IS_PROTOCOL_MAJOR;
  hello->header.minor = minor;
  hello->header.type = IS_PROTOCOL_MSG_HELLO;
  hello->header.request_id = 1;
  hello->header.capabilities = IS_PROTOCOL_CAP_KNOWN;
  hello->header.required_capabilities = IS_PROTOCOL_CAP_AUTH_HMAC_SHA256;
  strcpy(hello->payload.hello.consumer_id, "consumer-a");
  strcpy(hello->payload.hello.key_id, "key-current");
  hello->payload.hello.mode = IS_PROTOCOL_MODE_BACKED;
  hello->payload.hello.pool = IS_PROTOCOL_POOL_OPPORTUNISTIC;
  hello->payload.hello.failure_deadline_ms = 2000;
  for (index = 0; index < IS_PROTOCOL_NONCE_SIZE; index++)
    hello->payload.hello.nonce[index] = (uint8_t)index;
}

static int encode_message(const struct is_protocol_message *message,
                          uint8_t *frame, size_t *frame_size)
{
  enum is_protocol_result result = is_protocol_encode(
      message, frame, IS_PROTOCOL_MAX_FRAME_SIZE, frame_size);

  if (result == IS_PROTOCOL_OK)
    return 0;
  fprintf(stderr, "test message failed to encode: %s\n",
          is_protocol_result_name(result));
  return 1;
}

static int authenticate_session_with_key(
    struct is_provider_session *session,
    const struct is_auth_key *key, uint16_t hello_minor)
{
  struct is_provider_session_outcome outcome;
  struct is_protocol_message challenge;
  struct is_protocol_message auth;
  uint8_t hello_frame[IS_PROTOCOL_MAX_FRAME_SIZE];
  uint8_t auth_frame[IS_PROTOCOL_MAX_FRAME_SIZE];
  uint8_t response[IS_PROTOCOL_MAX_FRAME_SIZE];
  size_t hello_size = 0;
  size_t auth_size = 0;
  struct is_protocol_message hello;

  init_hello(&hello, hello_minor);
  strcpy(hello.payload.hello.key_id, key->key_id);
  if (encode_message(&hello, hello_frame, &hello_size) ||
      is_provider_session_handle(session, hello_frame, hello_size,
                                 response, sizeof(response), &outcome) != 0 ||
      !outcome.response_ready || outcome.close_after_response ||
      is_protocol_decode(response, outcome.response_size, &challenge) !=
          IS_PROTOCOL_OK ||
      challenge.header.type != IS_PROTOCOL_MSG_CHALLENGE)
    return 1;

  memset(&auth, 0, sizeof(auth));
  auth.header.major = IS_PROTOCOL_MAJOR;
  auth.header.minor = challenge.payload.challenge.negotiated_minor;
  auth.header.type = IS_PROTOCOL_MSG_AUTH;
  auth.header.request_id = 2;
  auth.header.capabilities = challenge.header.capabilities;
  auth.header.required_capabilities = IS_PROTOCOL_CAP_AUTH_HMAC_SHA256;
  if (is_auth_compute_tag(key->secret, key->secret_size,
                          hello_frame, hello_size,
                          response, outcome.response_size,
                          auth.payload.auth.tag) != IS_AUTH_OK ||
      encode_message(&auth, auth_frame, &auth_size) ||
      is_provider_session_handle(session, auth_frame, auth_size,
                                 response, sizeof(response), &outcome) != 0 ||
      !outcome.response_ready || outcome.close_after_response ||
      is_protocol_decode(response, outcome.response_size, &challenge) !=
          IS_PROTOCOL_OK ||
      challenge.header.type != IS_PROTOCOL_MSG_ACCEPT)
    return 1;
  return 0;
}

static int authenticate_session(struct is_provider_session *session,
                                const struct is_auth_credential *credential,
                                uint16_t hello_minor)
{
  return authenticate_session_with_key(
      session, &credential->current, hello_minor);
}

static int test_handshake_order_replay_and_minor_compatibility(void)
{
  struct is_auth_credential credential;
  struct is_auth_registry registry;
  struct is_provider_session session;
  struct is_provider_session_outcome outcome;
  struct is_protocol_message request;
  struct is_protocol_message response_message;
  uint8_t provider_nonce[IS_PROTOCOL_NONCE_SIZE];
  uint8_t request_frame[IS_PROTOCOL_MAX_FRAME_SIZE];
  uint8_t response[IS_PROTOCOL_MAX_FRAME_SIZE];
  size_t request_size = 0;
  uint16_t negotiated_minor = UINT16_MAX;
  uint64_t negotiated_capabilities = 0;
  size_t index;

  set_credential(&credential);
  is_auth_registry_init(&registry);
  if (is_auth_registry_add(&registry, &credential) != IS_AUTH_OK)
    return 1;
  for (index = 0; index < sizeof(provider_nonce); index++)
    provider_nonce[index] = (uint8_t)(0x80 + index);

  is_provider_session_init(&session, &registry,
                           IS_PROTOCOL_MINOR_CURRENT,
                           IS_PROTOCOL_CAP_KNOWN,
                           IS_PROTOCOL_CAP_AUTH_HMAC_SHA256,
                           provider_nonce, UINT64_C(0x1122334455667788));
  if (authenticate_session(&session, &credential, IS_PROTOCOL_MINOR_CURRENT)) {
    fprintf(stderr, "valid handshake did not authenticate\n");
    return 1;
  }

  memset(&request, 0, sizeof(request));
  request.header.major = IS_PROTOCOL_MAJOR;
  request.header.minor = IS_PROTOCOL_MINOR_CURRENT;
  request.header.type = IS_PROTOCOL_MSG_STATUS_REQUEST;
  request.header.request_id = 3;
  request.header.session_id = UINT64_C(0x1122334455667788);
  request.header.capabilities = IS_PROTOCOL_CAP_KNOWN;
  request.header.required_capabilities = IS_PROTOCOL_CAP_AUTH_HMAC_SHA256;
  if (encode_message(&request, request_frame, &request_size) ||
      is_provider_session_handle(&session, request_frame, request_size,
                                 response, sizeof(response), &outcome) != 0 ||
      !outcome.request_ready || outcome.response_ready)
    return 1;

  if (is_provider_session_handle(&session, request_frame, request_size,
                                 response, sizeof(response), &outcome) != 0 ||
      !outcome.response_ready || !outcome.close_after_response ||
      is_protocol_decode(response, outcome.response_size,
                         &response_message) != IS_PROTOCOL_OK ||
      response_message.payload.error.code != IS_PROTOCOL_ERROR_REPLAY) {
    fprintf(stderr, "replayed request was not rejected explicitly\n");
    return 1;
  }

  if (is_protocol_negotiate(IS_PROTOCOL_MINOR_CURRENT,
                            IS_PROTOCOL_CAP_KNOWN,
                            IS_PROTOCOL_CAP_AUTH_HMAC_SHA256,
                            IS_PROTOCOL_MINOR_PREVIOUS,
                            IS_PROTOCOL_CAP_KNOWN,
                            IS_PROTOCOL_CAP_AUTH_HMAC_SHA256,
                            &negotiated_minor,
                            &negotiated_capabilities) != IS_PROTOCOL_OK ||
      negotiated_minor != IS_PROTOCOL_MINOR_PREVIOUS ||
      is_protocol_negotiate(IS_PROTOCOL_MINOR_PREVIOUS,
                            IS_PROTOCOL_CAP_KNOWN,
                            IS_PROTOCOL_CAP_AUTH_HMAC_SHA256,
                            IS_PROTOCOL_MINOR_CURRENT,
                            IS_PROTOCOL_CAP_KNOWN,
                            IS_PROTOCOL_CAP_AUTH_HMAC_SHA256,
                            &negotiated_minor,
                            &negotiated_capabilities) != IS_PROTOCOL_OK ||
      negotiated_minor != IS_PROTOCOL_MINOR_PREVIOUS) {
    fprintf(stderr, "current/previous minor negotiation is not bidirectional\n");
    return 1;
  }

  is_provider_session_init(&session, &registry,
                           IS_PROTOCOL_MINOR_CURRENT,
                           IS_PROTOCOL_CAP_KNOWN,
                           IS_PROTOCOL_CAP_AUTH_HMAC_SHA256,
                           provider_nonce, 21);
  if (authenticate_session(&session, &credential,
                           IS_PROTOCOL_MINOR_PREVIOUS) ||
      session.negotiated_minor != IS_PROTOCOL_MINOR_PREVIOUS) {
    fprintf(stderr, "current Provider rejected previous Consumer messages\n");
    return 1;
  }
  is_provider_session_init(&session, &registry,
                           IS_PROTOCOL_MINOR_PREVIOUS,
                           IS_PROTOCOL_CAP_KNOWN,
                           IS_PROTOCOL_CAP_AUTH_HMAC_SHA256,
                           provider_nonce, 22);
  if (authenticate_session(&session, &credential,
                           IS_PROTOCOL_MINOR_CURRENT) ||
      session.negotiated_minor != IS_PROTOCOL_MINOR_PREVIOUS) {
    fprintf(stderr, "previous Provider rejected current Consumer messages\n");
    return 1;
  }

  is_provider_session_init(&session, &registry,
                           IS_PROTOCOL_MINOR_CURRENT,
                           IS_PROTOCOL_CAP_KNOWN,
                           IS_PROTOCOL_CAP_AUTH_HMAC_SHA256,
                           provider_nonce, 24);
  if (authenticate_session_with_key(&session, &credential.next,
                                    IS_PROTOCOL_MINOR_CURRENT) ||
      session.authenticated_until_unix !=
          credential.next_valid_until_unix) {
    fprintf(stderr, "next-key session did not retain its expiry\n");
    return 1;
  }
  session.authenticated_until_unix = (uint64_t)time(NULL);
  memset(&request, 0, sizeof(request));
  request.header.major = IS_PROTOCOL_MAJOR;
  request.header.minor = session.negotiated_minor;
  request.header.type = IS_PROTOCOL_MSG_STATUS_REQUEST;
  request.header.request_id = 3;
  request.header.session_id = session.session_id;
  request.header.capabilities = session.negotiated_capabilities;
  request.header.required_capabilities = session.required_capabilities;
  if (encode_message(&request, request_frame, &request_size) ||
      is_provider_session_handle(&session, request_frame, request_size,
                                 response, sizeof(response), &outcome) != 0 ||
      !outcome.response_ready || !outcome.close_after_response ||
      is_protocol_decode(response, outcome.response_size,
                         &response_message) != IS_PROTOCOL_OK ||
      response_message.payload.error.code !=
          IS_PROTOCOL_ERROR_AUTHENTICATION) {
    fprintf(stderr, "expired next-key session remained authenticated\n");
    return 1;
  }

  is_provider_session_init(&session, &registry,
                           IS_PROTOCOL_MINOR_CURRENT,
                           IS_PROTOCOL_CAP_KNOWN,
                           IS_PROTOCOL_CAP_AUTH_HMAC_SHA256,
                           provider_nonce, 23);
  init_hello(&request, IS_PROTOCOL_MINOR_CURRENT);
  if (encode_message(&request, request_frame, &request_size))
    return 1;
  request_frame[10] = 0;
  request_frame[11] = IS_PROTOCOL_FLAG_RESPONSE;
  if (is_provider_session_handle(&session, request_frame, request_size,
                                 response, sizeof(response), &outcome) != 0 ||
      !outcome.response_ready || !outcome.close_after_response ||
      is_protocol_decode(response, outcome.response_size,
                         &response_message) != IS_PROTOCOL_OK ||
      response_message.payload.error.code != IS_PROTOCOL_ERROR_MALFORMED) {
    fprintf(stderr, "response-marked HELLO was not rejected\n");
    return 1;
  }
  is_auth_registry_destroy(&registry);
  return 0;
}

static int contains_bytes(const uint8_t *haystack, size_t haystack_size,
                          const uint8_t *needle, size_t needle_size)
{
  size_t offset;

  if (needle_size == 0 || needle_size > haystack_size)
    return 0;
  for (offset = 0; offset <= haystack_size - needle_size; offset++) {
    if (memcmp(haystack + offset, needle, needle_size) == 0)
      return 1;
  }
  return 0;
}

static int test_out_of_order_and_failed_authentication(void)
{
  struct is_auth_credential credential;
  struct is_auth_registry registry;
  struct is_provider_session session;
  struct is_provider_session_outcome outcome;
  struct is_protocol_message request;
  struct is_protocol_message response_message;
  uint8_t provider_nonce[IS_PROTOCOL_NONCE_SIZE] = {0};
  uint8_t request_frame[IS_PROTOCOL_MAX_FRAME_SIZE];
  uint8_t response[IS_PROTOCOL_MAX_FRAME_SIZE];
  size_t request_size = 0;

  set_credential(&credential);
  is_auth_registry_init(&registry);
  if (is_auth_registry_add(&registry, &credential) != IS_AUTH_OK)
    return 1;
  is_provider_session_init(&session, &registry,
                           IS_PROTOCOL_MINOR_CURRENT,
                           IS_PROTOCOL_CAP_KNOWN,
                           IS_PROTOCOL_CAP_AUTH_HMAC_SHA256,
                           provider_nonce, 7);

  memset(&request, 0, sizeof(request));
  request.header.major = IS_PROTOCOL_MAJOR;
  request.header.minor = IS_PROTOCOL_MINOR_CURRENT;
  request.header.type = IS_PROTOCOL_MSG_CHUNK_REQUEST;
  request.header.request_id = 1;
  request.header.session_id = 7;
  request.header.capabilities = IS_PROTOCOL_CAP_KNOWN;
  request.header.required_capabilities = IS_PROTOCOL_CAP_AUTH_HMAC_SHA256;
  request.payload.chunk_request.chunk_count = 1;
  request.payload.chunk_request.pool = IS_PROTOCOL_POOL_OPPORTUNISTIC;
  if (encode_message(&request, request_frame, &request_size) ||
      is_provider_session_handle(&session, request_frame, request_size,
                                 response, sizeof(response), &outcome) != 0 ||
      !outcome.close_after_response ||
      is_protocol_decode(response, outcome.response_size,
                         &response_message) != IS_PROTOCOL_OK ||
      response_message.payload.error.code !=
          IS_PROTOCOL_ERROR_OUT_OF_ORDER) {
    fprintf(stderr, "out-of-order request was not rejected explicitly\n");
    return 1;
  }

  is_provider_session_init(&session, &registry,
                           IS_PROTOCOL_MINOR_CURRENT,
                           IS_PROTOCOL_CAP_KNOWN,
                           IS_PROTOCOL_CAP_AUTH_HMAC_SHA256,
                           provider_nonce, 7);
  init_hello(&request, IS_PROTOCOL_MINOR_CURRENT);
  if (encode_message(&request, request_frame, &request_size) ||
      is_provider_session_handle(&session, request_frame, request_size,
                                 response, sizeof(response), &outcome) != 0)
    return 1;
  memset(&request, 0, sizeof(request));
  request.header.major = IS_PROTOCOL_MAJOR;
  request.header.minor = IS_PROTOCOL_MINOR_CURRENT;
  request.header.type = IS_PROTOCOL_MSG_AUTH;
  request.header.request_id = 2;
  request.header.capabilities = IS_PROTOCOL_CAP_KNOWN;
  request.header.required_capabilities = IS_PROTOCOL_CAP_AUTH_HMAC_SHA256;
  memset(request.payload.auth.tag, 0xa5, IS_PROTOCOL_AUTH_TAG_SIZE);
  if (encode_message(&request, request_frame, &request_size) ||
      is_provider_session_handle(&session, request_frame, request_size,
                                 response, sizeof(response), &outcome) != 0 ||
      !outcome.close_after_response ||
      is_protocol_decode(response, outcome.response_size,
                         &response_message) != IS_PROTOCOL_OK ||
      response_message.payload.error.code !=
          IS_PROTOCOL_ERROR_AUTHENTICATION ||
      contains_bytes(response, outcome.response_size,
                     credential.current.secret,
                     credential.current.secret_size) ||
      contains_bytes(response, outcome.response_size,
                     request.payload.auth.tag,
                     IS_PROTOCOL_AUTH_TAG_SIZE)) {
    fprintf(stderr, "authentication failure was not explicit and secret-free\n");
    return 1;
  }
  return 0;
}

static int test_pool_and_cumulative_quota_authorization(void)
{
  struct is_auth_credential credential;
  struct is_auth_registry registry;
  struct is_provider_session session;
  struct is_provider_session_outcome outcome;
  struct is_protocol_message request;
  struct is_protocol_message error;
  uint8_t nonce[IS_PROTOCOL_NONCE_SIZE] = {0};
  uint8_t frame[IS_PROTOCOL_MAX_FRAME_SIZE];
  uint8_t response[IS_PROTOCOL_MAX_FRAME_SIZE];
  size_t frame_size = 0;

  set_credential(&credential);
  is_auth_registry_init(&registry);
  if (is_auth_registry_add(&registry, &credential) != IS_AUTH_OK)
    return 1;
  is_provider_session_init(&session, &registry,
                           IS_PROTOCOL_MINOR_CURRENT,
                           IS_PROTOCOL_CAP_KNOWN,
                           IS_PROTOCOL_CAP_AUTH_HMAC_SHA256,
                           nonce, 98);
  init_hello(&request, IS_PROTOCOL_MINOR_CURRENT);
  request.payload.hello.mode = IS_PROTOCOL_MODE_REMOTE_ONLY;
  request.payload.hello.pool = IS_PROTOCOL_POOL_COMMITTED;
  if (encode_message(&request, frame, &frame_size) ||
      is_provider_session_handle(&session, frame, frame_size,
                                 response, sizeof(response), &outcome) != 0 ||
      !outcome.response_ready || outcome.close_after_response ||
      session.selected_mode != IS_PROTOCOL_MODE_REMOTE_ONLY ||
      session.selected_pool != IS_PROTOCOL_POOL_COMMITTED ||
      !(session.negotiated_capabilities & IS_PROTOCOL_CAP_REMOTE_ONLY) ||
      !(session.negotiated_capabilities & IS_PROTOCOL_CAP_COMMITTED_POOL)) {
    fprintf(stderr, "Remote-Only/Committed negotiation failed\n");
    return 1;
  }
  is_provider_session_init(&session, &registry,
                           IS_PROTOCOL_MINOR_CURRENT,
                           IS_PROTOCOL_CAP_KNOWN,
                           IS_PROTOCOL_CAP_AUTH_HMAC_SHA256,
                           nonce, 99);
  if (authenticate_session(&session, &credential, IS_PROTOCOL_MINOR_CURRENT))
    return 1;

  memset(&request, 0, sizeof(request));
  request.header.major = IS_PROTOCOL_MAJOR;
  request.header.minor = IS_PROTOCOL_MINOR_CURRENT;
  request.header.type = IS_PROTOCOL_MSG_CHUNK_REQUEST;
  request.header.request_id = 3;
  request.header.session_id = 99;
  request.header.capabilities = IS_PROTOCOL_CAP_KNOWN;
  request.header.required_capabilities = IS_PROTOCOL_CAP_AUTH_HMAC_SHA256;
  request.payload.chunk_request.chunk_count = 20;
  request.payload.chunk_request.pool = IS_PROTOCOL_POOL_OPPORTUNISTIC;
  if (encode_message(&request, frame, &frame_size) ||
      is_provider_session_handle(&session, frame, frame_size,
                                 response, sizeof(response), &outcome) != 0 ||
      !outcome.request_ready)
    return 1;

  request.header.request_id = 4;
  if (encode_message(&request, frame, &frame_size) ||
      is_provider_session_handle(&session, frame, frame_size,
                                 response, sizeof(response), &outcome) != 0 ||
      !outcome.response_ready || !outcome.close_after_response ||
      is_protocol_decode(response, outcome.response_size, &error) !=
          IS_PROTOCOL_OK ||
      error.payload.error.code != IS_PROTOCOL_ERROR_RESOURCE) {
    fprintf(stderr, "cumulative quota was bypassed\n");
    return 1;
  }

  is_provider_session_init(&session, &registry,
                           IS_PROTOCOL_MINOR_CURRENT,
                           IS_PROTOCOL_CAP_KNOWN,
                           IS_PROTOCOL_CAP_AUTH_HMAC_SHA256,
                           nonce, 100);
  if (authenticate_session(&session, &credential, IS_PROTOCOL_MINOR_CURRENT))
    return 1;
  request.header.request_id = 3;
  request.header.session_id = 100;
  request.payload.chunk_request.chunk_count = 1;
  request.payload.chunk_request.pool = IS_PROTOCOL_POOL_COMMITTED;
  if (encode_message(&request, frame, &frame_size) ||
      is_provider_session_handle(&session, frame, frame_size,
                                 response, sizeof(response), &outcome) != 0 ||
      !outcome.response_ready ||
      is_protocol_decode(response, outcome.response_size, &error) !=
          IS_PROTOCOL_OK ||
      error.payload.error.code != IS_PROTOCOL_ERROR_CAPABILITY) {
    fprintf(stderr, "HELLO-selected pool was not enforced\n");
    return 1;
  }

  is_provider_session_init(&session, &registry,
                           IS_PROTOCOL_MINOR_CURRENT,
                           IS_PROTOCOL_CAP_KNOWN,
                           IS_PROTOCOL_CAP_AUTH_HMAC_SHA256,
                           nonce, 101);
  if (authenticate_session(&session, &credential, IS_PROTOCOL_MINOR_CURRENT))
    return 1;
  pthread_mutex_lock(&registry.lock);
  registry.entries[0].authorization_version++;
  pthread_mutex_unlock(&registry.lock);
  memset(&request, 0, sizeof(request));
  request.header.major = IS_PROTOCOL_MAJOR;
  request.header.minor = IS_PROTOCOL_MINOR_CURRENT;
  request.header.type = IS_PROTOCOL_MSG_STATUS_REQUEST;
  request.header.request_id = 3;
  request.header.session_id = 101;
  request.header.capabilities = IS_PROTOCOL_CAP_KNOWN;
  request.header.required_capabilities = IS_PROTOCOL_CAP_AUTH_HMAC_SHA256;
  if (encode_message(&request, frame, &frame_size) ||
      is_provider_session_handle(&session, frame, frame_size,
                                 response, sizeof(response), &outcome) != 0 ||
      !outcome.response_ready || !outcome.close_after_response ||
      is_protocol_decode(response, outcome.response_size, &error) !=
          IS_PROTOCOL_OK ||
      error.payload.error.code != IS_PROTOCOL_ERROR_AUTHENTICATION) {
    fprintf(stderr, "stale authorization processed a request\n");
    return 1;
  }
  is_auth_registry_destroy(&registry);
  return 0;
}

static int write_config(char path[], const char *contents, mode_t mode)
{
  int descriptor = mkstemp(path);
  size_t size = strlen(contents);

  if (descriptor < 0 || fchmod(descriptor, mode) != 0 ||
      write(descriptor, contents, size) != (ssize_t)size ||
      close(descriptor) != 0)
    return 1;
  return 0;
}

static int test_allowlist_loader(void)
{
  static const char valid_config_template[] =
      "version = 1\n"
      "[consumer:consumer-a]\n"
      "current_key_id = key-current\n"
      "current_psk_hex = "
      "63757272656e742d7365637265742d33322d62797465732d3030303030303030\n"
      "next_key_id = key-next\n"
      "next_psk_hex = "
      "6e6578742d7365637265742d33322d62797465732d3030303030303030303030\n"
      "next_valid_until_unix = %llu\n"
      "max_opportunistic_chunks = 32\n"
      "max_committed_chunks = 16\n"
      "revoked = false\n"
      "[consumer:consumer-b]\n"
      "current_key_id = key-b\n"
      "current_psk_hex = "
      "3031323334353637383961626364656630313233343536373839616263646566\n"
      "max_opportunistic_chunks = 1\n"
      "max_committed_chunks = 0\n"
      "revoked = true\n";
  static const char invalid_config[] =
      "version = 1\n"
      "[consumer:consumer-a]\n"
      "current_key_id = key-current\n"
      "current_psk_hex = not-a-secret\n"
      "max_opportunistic_chunks = 32\n"
      "max_committed_chunks = 0\n"
      "revoked = false\n";
  static const char empty_config[] = "version = 1\n";
  struct is_auth_registry registry;
  char valid_config[2048];
  char changed_config[2048];
  char valid_path[] = "/tmp/infiniswap-allowlist-valid-XXXXXX";
  char invalid_path[] = "/tmp/infiniswap-allowlist-invalid-XXXXXX";
  char exposed_path[] = "/tmp/infiniswap-allowlist-exposed-XXXXXX";
  char changed_path[] = "/tmp/infiniswap-allowlist-changed-XXXXXX";
  char empty_path[] = "/tmp/infiniswap-allowlist-empty-XXXXXX";
  uint64_t original_authorization_version = 0;
  int revocations = 0;
  int failure = 0;

  if (snprintf(valid_config, sizeof(valid_config), valid_config_template,
               (unsigned long long)time(NULL) + 3600U) < 0)
    return 1;
  strcpy(changed_config, valid_config);
  memcpy(strstr(changed_config, "key-current"), "key-rotate1",
         strlen("key-rotate1"));
  is_auth_registry_init(&registry);
  if (write_config(valid_path, valid_config, S_IRUSR | S_IWUSR) ||
      is_auth_registry_load_file(&registry, valid_path) != IS_AUTH_OK ||
      registry.count != 2 ||
      is_auth_registry_is_revoked(&registry, "consumer-a") != IS_AUTH_OK ||
      is_auth_registry_is_revoked(&registry, "consumer-b") !=
          IS_AUTH_REVOKED) {
    fprintf(stderr, "valid allowlist did not load\n");
    failure = 1;
    goto out;
  }
  original_authorization_version =
      registry.entries[0].authorization_version;
  if (is_auth_registry_subscribe(
          &registry, "consumer-a",
          registry.entries[0].authorization_version,
          count_revocation, &revocations) != IS_AUTH_OK ||
      is_auth_registry_load_file(&registry, valid_path) != IS_AUTH_OK ||
      revocations != 0) {
    fprintf(stderr, "unchanged reload disconnected an active identity\n");
    failure = 1;
    goto out;
  }
  if (write_config(invalid_path, invalid_config, S_IRUSR | S_IWUSR) ||
      is_auth_registry_load_file(&registry, invalid_path) !=
          IS_AUTH_FORMAT_ERROR ||
      registry.count != 2) {
    fprintf(stderr, "malformed reload replaced the active allowlist\n");
    failure = 1;
    goto out;
  }
  if (write_config(exposed_path, valid_config,
                   S_IRUSR | S_IWUSR | S_IRGRP) ||
      is_auth_registry_load_file(&registry, exposed_path) !=
          IS_AUTH_IO_ERROR) {
    fprintf(stderr, "group-readable PSKs were accepted\n");
    failure = 1;
    goto out;
  }
  if (write_config(changed_path, changed_config, S_IRUSR | S_IWUSR) ||
      is_auth_registry_load_file(&registry, changed_path) != IS_AUTH_OK ||
      revocations != 1) {
    fprintf(stderr, "key rotation did not disconnect an active identity\n");
    failure = 1;
  } else {
    is_auth_registry_unsubscribe(&registry, &revocations);
    if (is_auth_registry_subscribe(
            &registry, "consumer-a", original_authorization_version,
            count_revocation, &revocations) != IS_AUTH_FAILED) {
      fprintf(stderr, "stale authentication subscribed after key rotation\n");
      failure = 1;
    } else if (is_auth_registry_subscribe(
                   &registry, "consumer-a",
                   registry.entries[0].authorization_version,
                   count_revocation, &revocations) != IS_AUTH_OK ||
               write_config(empty_path, empty_config,
                            S_IRUSR | S_IWUSR) ||
               is_auth_registry_load_file(&registry, empty_path) !=
                   IS_AUTH_OK ||
               registry.count != 0 || revocations != 2) {
      fprintf(stderr, "removing the final identity did not revoke it\n");
      failure = 1;
    }
  }
out:
  unlink(valid_path);
  unlink(invalid_path);
  unlink(exposed_path);
  unlink(changed_path);
  unlink(empty_path);
  is_auth_registry_destroy(&registry);
  return failure;
}

int main(void)
{
  int failures = 0;

  failures += test_hmac_rotation_and_revocation();
  failures += test_handshake_order_replay_and_minor_compatibility();
  failures += test_out_of_order_and_failed_authentication();
  failures += test_pool_and_cumulative_quota_authorization();
  failures += test_allowlist_loader();
  return failures == 0 ? 0 : 1;
}
