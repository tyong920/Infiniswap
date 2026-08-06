#include "infiniswap_protocol.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int test_hello_golden_vector(void)
{
  static const uint8_t expected[] = {
    0x49, 0x53, 0x57, 0x50, 0x00, 0x01, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x37,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x75,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40,
    0x0a, 0x05, 0x01, 0x01, 0x00, 0x00, 0x07, 0xd0,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    'c', 'o', 'n', 's', 'u', 'm', 'e', 'r', '-', 'a',
    'k', 'e', 'y', '-', 'a'
  };
  struct is_protocol_message decoded;
  struct is_protocol_message message;
  uint8_t encoded[IS_PROTOCOL_MAX_FRAME_SIZE];
  uint8_t previous[sizeof(expected)];
  size_t encoded_size = 0;
  uint16_t negotiated_minor = UINT16_MAX;
  uint64_t negotiated_capabilities = 0;
  size_t i;

  memset(&message, 0, sizeof(message));
  message.header.major = IS_PROTOCOL_MAJOR;
  message.header.minor = IS_PROTOCOL_MINOR_CURRENT;
  message.header.type = IS_PROTOCOL_MSG_HELLO;
  message.header.request_id = 1;
  message.header.capabilities = IS_PROTOCOL_CAP_BACKED |
                                IS_PROTOCOL_CAP_OPPORTUNISTIC_POOL |
                                IS_PROTOCOL_CAP_FAILURE_DEADLINE |
                                IS_PROTOCOL_CAP_STATUS |
                                IS_PROTOCOL_CAP_AUTH_HMAC_SHA256;
  message.header.required_capabilities = IS_PROTOCOL_CAP_AUTH_HMAC_SHA256;
  message.payload.hello.mode = IS_PROTOCOL_MODE_BACKED;
  message.payload.hello.pool = IS_PROTOCOL_POOL_OPPORTUNISTIC;
  message.payload.hello.failure_deadline_ms = 2000;
  strcpy(message.payload.hello.consumer_id, "consumer-a");
  strcpy(message.payload.hello.key_id, "key-a");
  for (i = 0; i < IS_PROTOCOL_NONCE_SIZE; i++)
    message.payload.hello.nonce[i] = (uint8_t)i;

  if (is_protocol_encode(&message, encoded, sizeof(encoded),
                         &encoded_size) != IS_PROTOCOL_OK) {
    fprintf(stderr, "HELLO did not encode\n");
    return 1;
  }
  if (encoded_size != sizeof(expected) ||
      memcmp(encoded, expected, sizeof(expected)) != 0) {
    fprintf(stderr, "HELLO does not match the golden vector\n");
    return 1;
  }
  if (is_protocol_decode(expected, sizeof(expected), &decoded) !=
      IS_PROTOCOL_OK) {
    fprintf(stderr, "HELLO golden vector did not decode\n");
    return 1;
  }
  if (decoded.header.request_id != 1 ||
      strcmp(decoded.payload.hello.consumer_id, "consumer-a") != 0 ||
      strcmp(decoded.payload.hello.key_id, "key-a") != 0 ||
      decoded.payload.hello.failure_deadline_ms != 2000) {
    fprintf(stderr, "HELLO golden vector decoded incorrectly\n");
    return 1;
  }

  memcpy(previous, expected, sizeof(previous));
  previous[7] = 0;
  if (is_protocol_decode(previous, sizeof(previous), &decoded) !=
          IS_PROTOCOL_OK ||
      is_protocol_negotiate(
          IS_PROTOCOL_MINOR_CURRENT, message.header.capabilities,
          message.header.required_capabilities, decoded.header.minor,
          decoded.header.capabilities, decoded.header.required_capabilities,
          &negotiated_minor, &negotiated_capabilities) != IS_PROTOCOL_OK ||
      negotiated_minor != IS_PROTOCOL_MINOR_PREVIOUS ||
      negotiated_capabilities != message.header.capabilities) {
    fprintf(stderr, "current codec rejected the frozen 1.0 HELLO vector\n");
    return 1;
  }

  return 0;
}

static void put_expected_u16(uint8_t *output, uint16_t value)
{
  output[0] = (uint8_t)(value >> 8);
  output[1] = (uint8_t)value;
}

static void put_expected_u32(uint8_t *output, uint32_t value)
{
  output[0] = (uint8_t)(value >> 24);
  output[1] = (uint8_t)(value >> 16);
  output[2] = (uint8_t)(value >> 8);
  output[3] = (uint8_t)value;
}

static void put_expected_u64(uint8_t *output, uint64_t value)
{
  put_expected_u32(output, (uint32_t)(value >> 32));
  put_expected_u32(output + 4, (uint32_t)value);
}

static int assert_payload_golden(struct is_protocol_message *message,
                                 const uint8_t *expected,
                                 size_t expected_size,
                                 const char *name)
{
  struct is_protocol_message decoded;
  uint8_t encoded[IS_PROTOCOL_MAX_FRAME_SIZE];
  uint8_t expected_header[IS_PROTOCOL_HEADER_SIZE] = {0};
  uint8_t reencoded[IS_PROTOCOL_MAX_FRAME_SIZE];
  size_t encoded_size = 0;
  size_t reencoded_size = 0;

  if (is_protocol_encode(message, encoded, sizeof(encoded), &encoded_size) !=
      IS_PROTOCOL_OK) {
    fprintf(stderr, "%s did not encode\n", name);
    return 1;
  }
  expected_header[0] = 0x49;
  expected_header[1] = 0x53;
  expected_header[2] = 0x57;
  expected_header[3] = 0x50;
  put_expected_u16(expected_header + 4, message->header.major);
  put_expected_u16(expected_header + 6, message->header.minor);
  put_expected_u16(expected_header + 8, message->header.type);
  put_expected_u16(expected_header + 10, message->header.flags);
  put_expected_u32(expected_header + 12, (uint32_t)expected_size);
  put_expected_u64(expected_header + 16, message->header.request_id);
  put_expected_u64(expected_header + 24, message->header.session_id);
  put_expected_u64(expected_header + 32, message->header.capabilities);
  put_expected_u64(expected_header + 40,
                   message->header.required_capabilities);
  if (encoded_size != IS_PROTOCOL_HEADER_SIZE + expected_size ||
      memcmp(encoded, expected_header, sizeof(expected_header)) != 0 ||
      (expected_size != 0 &&
       memcmp(encoded + IS_PROTOCOL_HEADER_SIZE, expected,
              expected_size) != 0)) {
    fprintf(stderr, "%s does not match its golden payload\n", name);
    return 1;
  }
  if (is_protocol_decode(encoded, encoded_size, &decoded) != IS_PROTOCOL_OK ||
      is_protocol_encode(&decoded, reencoded, sizeof(reencoded),
                         &reencoded_size) != IS_PROTOCOL_OK ||
      reencoded_size != encoded_size ||
      memcmp(reencoded, encoded, encoded_size) != 0) {
    fprintf(stderr, "%s did not round-trip canonically\n", name);
    return 1;
  }
  return 0;
}

static void init_message(struct is_protocol_message *message,
                         enum is_protocol_message_type type)
{
  memset(message, 0, sizeof(*message));
  message->header.major = IS_PROTOCOL_MAJOR;
  message->header.minor = IS_PROTOCOL_MINOR_CURRENT;
  message->header.type = type;
  if (type == IS_PROTOCOL_MSG_CHALLENGE ||
      type == IS_PROTOCOL_MSG_ACCEPT ||
      type == IS_PROTOCOL_MSG_STATUS_RESPONSE ||
      type == IS_PROTOCOL_MSG_CHUNK_GRANT ||
      type == IS_PROTOCOL_MSG_ACTIVITY ||
      type == IS_PROTOCOL_MSG_ERROR)
    message->header.flags |= IS_PROTOCOL_FLAG_RESPONSE;
  if (type == IS_PROTOCOL_MSG_ERROR)
    message->header.flags |= IS_PROTOCOL_FLAG_ERROR;
  message->header.request_id = UINT64_C(0x0102030405060708);
  message->header.session_id = UINT64_C(0x1112131415161718);
  message->header.capabilities = IS_PROTOCOL_CAP_KNOWN;
  message->header.required_capabilities = IS_PROTOCOL_CAP_AUTH_HMAC_SHA256;
}

static int test_all_message_golden_vectors(void)
{
  static const uint8_t challenge[] = {
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
    0x00, 0x01, 0x00, 0x00
  };
  static const uint8_t auth[IS_PROTOCOL_AUTH_TAG_SIZE] = {
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf
  };
  static const uint8_t accept[] = {0x00, 0x01, 0x00, 0x00};
  static const uint8_t status[] = {
    0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x22,
    0x00, 0x00, 0x07, 0xd0, 0x00, 0x00, 0x00, 0x03
  };
  static const uint8_t chunk_request[] = {
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x07,
    0x01, 0x00, 0x00, 0x00
  };
  static const uint8_t chunk_grant[] = {
    0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x09,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0x89, 0xab, 0xcd, 0xef, 0x01, 0x00, 0x00, 0x00
  };
  static const uint8_t chunk_ids[] = {
    0x00, 0x02, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x0a
  };
  static const uint8_t activity[] = {
    0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x09,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
  };
  static const uint8_t error[] = {
    0x00, 0x05, 0x00, 0x07, 0x01, 0x00, 0x00, 0x00
  };
  struct is_protocol_message message;
  size_t index;
  int failures = 0;

  init_message(&message, IS_PROTOCOL_MSG_CHALLENGE);
  message.header.session_id = 0;
  message.payload.challenge.negotiated_minor = IS_PROTOCOL_MINOR_CURRENT;
  for (index = 0; index < IS_PROTOCOL_NONCE_SIZE; index++)
    message.payload.challenge.nonce[index] = (uint8_t)(0x20 + index);
  failures += assert_payload_golden(&message, challenge, sizeof(challenge),
                                    "CHALLENGE");

  init_message(&message, IS_PROTOCOL_MSG_AUTH);
  message.header.session_id = 0;
  memcpy(message.payload.auth.tag, auth, sizeof(auth));
  failures += assert_payload_golden(&message, auth, sizeof(auth), "AUTH");

  init_message(&message, IS_PROTOCOL_MSG_ACCEPT);
  message.payload.accept.negotiated_minor = IS_PROTOCOL_MINOR_CURRENT;
  failures += assert_payload_golden(&message, accept, sizeof(accept),
                                    "ACCEPT");

  init_message(&message, IS_PROTOCOL_MSG_STATUS_REQUEST);
  failures += assert_payload_golden(&message, NULL, 0, "STATUS_REQUEST");

  init_message(&message, IS_PROTOCOL_MSG_STATUS_RESPONSE);
  message.payload.status.available_opportunistic_chunks = 0x11;
  message.payload.status.available_committed_chunks = 0x22;
  message.payload.status.provider_failure_deadline_ms = 2000;
  message.payload.status.flags = 3;
  failures += assert_payload_golden(&message, status, sizeof(status),
                                    "STATUS_RESPONSE");

  init_message(&message, IS_PROTOCOL_MSG_CHUNK_REQUEST);
  message.payload.chunk_request.chunk_count = 2;
  message.payload.chunk_request.logical_start = 7;
  message.payload.chunk_request.pool = IS_PROTOCOL_POOL_OPPORTUNISTIC;
  failures += assert_payload_golden(&message, chunk_request,
                                    sizeof(chunk_request), "CHUNK_REQUEST");

  init_message(&message, IS_PROTOCOL_MSG_CHUNK_GRANT);
  message.payload.chunk_grant.chunk_count = 1;
  message.payload.chunk_grant.chunks[0].logical_chunk_id = 7;
  message.payload.chunk_grant.chunks[0].provider_chunk_id = 9;
  message.payload.chunk_grant.chunks[0].remote_address =
      UINT64_C(0x0123456789abcdef);
  message.payload.chunk_grant.chunks[0].remote_key = UINT32_C(0x89abcdef);
  message.payload.chunk_grant.chunks[0].pool =
      IS_PROTOCOL_POOL_OPPORTUNISTIC;
  failures += assert_payload_golden(&message, chunk_grant,
                                    sizeof(chunk_grant), "CHUNK_GRANT");

  init_message(&message, IS_PROTOCOL_MSG_EVICT);
  message.payload.chunk_ids.chunk_count = 2;
  message.payload.chunk_ids.chunk_ids[0] = 9;
  message.payload.chunk_ids.chunk_ids[1] = 10;
  failures += assert_payload_golden(&message, chunk_ids, sizeof(chunk_ids),
                                    "EVICT");

  init_message(&message, IS_PROTOCOL_MSG_ACTIVITY);
  message.payload.activity.chunk_count = 1;
  message.payload.activity.chunks[0].provider_chunk_id = 9;
  message.payload.activity.chunks[0].activity =
      UINT64_C(0x0102030405060708);
  failures += assert_payload_golden(&message, activity, sizeof(activity),
                                    "ACTIVITY");

  init_message(&message, IS_PROTOCOL_MSG_RELEASE);
  message.payload.chunk_ids.chunk_count = 2;
  message.payload.chunk_ids.chunk_ids[0] = 9;
  message.payload.chunk_ids.chunk_ids[1] = 10;
  failures += assert_payload_golden(&message, chunk_ids, sizeof(chunk_ids),
                                    "RELEASE");

  init_message(&message, IS_PROTOCOL_MSG_GOODBYE);
  failures += assert_payload_golden(&message, NULL, 0, "GOODBYE");

  init_message(&message, IS_PROTOCOL_MSG_ERROR);
  message.payload.error.code = IS_PROTOCOL_ERROR_AUTHENTICATION;
  message.payload.error.offending_type = IS_PROTOCOL_MSG_CHUNK_REQUEST;
  message.payload.error.retryable = 1;
  failures += assert_payload_golden(&message, error, sizeof(error), "ERROR");

  return failures;
}

static int expect_decode_result(uint8_t *frame, size_t size,
                                enum is_protocol_result expected,
                                const char *name)
{
  struct is_protocol_message message;
  enum is_protocol_result actual = is_protocol_decode(frame, size, &message);

  if (actual == expected)
    return 0;
  fprintf(stderr, "%s: expected %s, got %s\n", name,
          is_protocol_result_name(expected), is_protocol_result_name(actual));
  return 1;
}

static int test_decoder_rejects_malformed_frames(void)
{
  struct is_protocol_message message;
  struct is_protocol_message decoded;
  uint8_t frame[IS_PROTOCOL_MAX_FRAME_SIZE + 1U];
  size_t frame_size = 0;
  int failures = 0;

  init_message(&message, IS_PROTOCOL_MSG_STATUS_REQUEST);
  if (is_protocol_encode(&message, frame, sizeof(frame), &frame_size) !=
      IS_PROTOCOL_OK)
    return 1;

  failures += expect_decode_result(frame, IS_PROTOCOL_HEADER_SIZE - 1U,
                                   IS_PROTOCOL_TRUNCATED, "short header");
  frame[12] = 0x00;
  frame[13] = 0x00;
  frame[14] = 0x00;
  frame[15] = 0x01;
  failures += expect_decode_result(frame, frame_size, IS_PROTOCOL_TRUNCATED,
                                   "truncated payload");
  frame[12] = 0xff;
  frame[13] = 0xff;
  frame[14] = 0xff;
  frame[15] = 0xff;
  failures += expect_decode_result(frame, frame_size, IS_PROTOCOL_OVERSIZED,
                                   "overflow payload length");

  if (is_protocol_encode(&message, frame, sizeof(frame), &frame_size) !=
      IS_PROTOCOL_OK)
    return 1;
  frame[8] = 0x7f;
  frame[9] = 0xff;
  failures += expect_decode_result(frame, frame_size,
                                   IS_PROTOCOL_UNKNOWN_MESSAGE,
                                   "unknown message");
  frame[8] = 0x00;
  frame[9] = IS_PROTOCOL_MSG_STATUS_REQUEST;
  frame[32] |= 0x80;
  failures += expect_decode_result(frame, frame_size, IS_PROTOCOL_OK,
                                   "unknown optional capability");
  frame[32] &= 0x7f;
  frame[47] = 0x80;
  failures += expect_decode_result(frame, frame_size,
                                   IS_PROTOCOL_UNKNOWN_REQUIRED_CAPABILITY,
                                   "unknown required capability");
  frame[47] = IS_PROTOCOL_CAP_AUTH_HMAC_SHA256;
  frame[4] = 0x00;
  frame[5] = IS_PROTOCOL_MAJOR + 1U;
  failures += expect_decode_result(frame, frame_size,
                                   IS_PROTOCOL_UNSUPPORTED_VERSION,
                                   "cross-major frame");
  frame[4] = 0x00;
  frame[5] = IS_PROTOCOL_MAJOR;

  init_message(&message, IS_PROTOCOL_MSG_CHUNK_REQUEST);
  message.payload.chunk_request.chunk_count = 2;
  message.payload.chunk_request.logical_start = UINT32_MAX;
  message.payload.chunk_request.pool = IS_PROTOCOL_POOL_OPPORTUNISTIC;
  if (is_protocol_encode(&message, frame, sizeof(frame), &frame_size) !=
      IS_PROTOCOL_INVALID_PAYLOAD) {
    fprintf(stderr, "wrapping logical chunk range was accepted\n");
    failures++;
  }

  message.payload.chunk_request.chunk_count = 1;
  message.payload.chunk_request.logical_start = 0;
  message.payload.chunk_request.pool = IS_PROTOCOL_POOL_OPPORTUNISTIC;
  if (is_protocol_encode(&message, frame, sizeof(frame), &frame_size) !=
      IS_PROTOCOL_OK)
    return 1;
  frame[IS_PROTOCOL_HEADER_SIZE + 9U] = 1;
  failures += expect_decode_result(frame, frame_size,
                                   IS_PROTOCOL_INVALID_PAYLOAD,
                                   "nonzero reserved byte");

  init_message(&message, IS_PROTOCOL_MSG_EVICT);
  message.payload.chunk_ids.chunk_count = 1;
  message.payload.chunk_ids.chunk_ids[0] = 1;
  if (is_protocol_encode(&message, frame, sizeof(frame), &frame_size) !=
      IS_PROTOCOL_OK)
    return 1;
  frame[IS_PROTOCOL_HEADER_SIZE] = 0xff;
  frame[IS_PROTOCOL_HEADER_SIZE + 1U] = 0xff;
  failures += expect_decode_result(frame, frame_size,
                                   IS_PROTOCOL_INTEGER_OVERFLOW,
                                   "overflowing entry count");

  init_message(&message, IS_PROTOCOL_MSG_STATUS_REQUEST);
  if (is_protocol_encode(&message, frame, sizeof(frame), &frame_size) !=
      IS_PROTOCOL_OK)
    return 1;
  frame[frame_size] = 0;
  failures += expect_decode_result(frame, frame_size + 1U,
                                   IS_PROTOCOL_TRAILING_DATA,
                                   "trailing data");

  init_message(&message, IS_PROTOCOL_MSG_ERROR);
  message.payload.error.code = IS_PROTOCOL_ERROR_MALFORMED;
  message.payload.error.offending_type = 0x1234;
  if (is_protocol_encode(&message, frame, sizeof(frame), &frame_size) !=
          IS_PROTOCOL_OK ||
      is_protocol_decode(frame, frame_size, &decoded) != IS_PROTOCOL_OK ||
      decoded.payload.error.offending_type != 0x1234) {
    fprintf(stderr, "ERROR did not preserve an unknown offending type\n");
    failures++;
  }

  return failures;
}

static int test_adjacent_minor_build_compatibility(void)
{
  struct is_protocol_message message;
  struct is_protocol_message decoded;
  uint8_t frame[IS_PROTOCOL_MAX_FRAME_SIZE];
  size_t frame_size = 0;
  uint16_t negotiated_minor = UINT16_MAX;
  uint64_t negotiated_capabilities = 0;

  init_message(&message, IS_PROTOCOL_MSG_HELLO);
  strcpy(message.payload.hello.consumer_id, "consumer-a");
  strcpy(message.payload.hello.key_id, "key-a");
  message.payload.hello.mode = IS_PROTOCOL_MODE_BACKED;
  message.payload.hello.pool = IS_PROTOCOL_POOL_OPPORTUNISTIC;
  message.payload.hello.failure_deadline_ms = 2000;
  message.header.minor = IS_PROTOCOL_MINOR_PREVIOUS;
  message.header.session_id = 0;
  if (is_protocol_encode(&message, frame, sizeof(frame), &frame_size) !=
          IS_PROTOCOL_OK ||
      is_protocol_decode(frame, frame_size, &decoded) != IS_PROTOCOL_OK ||
      is_protocol_negotiate(
          IS_PROTOCOL_MINOR_CURRENT, IS_PROTOCOL_CAP_KNOWN,
          IS_PROTOCOL_CAP_AUTH_HMAC_SHA256, IS_PROTOCOL_MINOR_PREVIOUS,
          IS_PROTOCOL_CAP_KNOWN, IS_PROTOCOL_CAP_AUTH_HMAC_SHA256,
          &negotiated_minor, &negotiated_capabilities) != IS_PROTOCOL_OK ||
      negotiated_minor != IS_PROTOCOL_MINOR_PREVIOUS) {
    fprintf(stderr, "current codec could not negotiate the previous minor\n");
    return 1;
  }
  message.header.minor = IS_PROTOCOL_MINOR_NEXT;
  if (is_protocol_encode(&message, frame, sizeof(frame), &frame_size) !=
          IS_PROTOCOL_OK ||
      is_protocol_decode(frame, frame_size, &decoded) != IS_PROTOCOL_OK ||
      is_protocol_negotiate(
          IS_PROTOCOL_MINOR_CURRENT, IS_PROTOCOL_CAP_KNOWN,
          IS_PROTOCOL_CAP_AUTH_HMAC_SHA256, IS_PROTOCOL_MINOR_NEXT,
          IS_PROTOCOL_CAP_KNOWN, IS_PROTOCOL_CAP_AUTH_HMAC_SHA256,
          &negotiated_minor, &negotiated_capabilities) != IS_PROTOCOL_OK ||
      negotiated_minor != IS_PROTOCOL_MINOR_CURRENT) {
    fprintf(stderr, "an immediately succeeding minor could not negotiate\n");
    return 1;
  }
  message.header.minor = IS_PROTOCOL_MINOR_CURRENT + 2U;
  if (is_protocol_encode(&message, frame, sizeof(frame), &frame_size) !=
      IS_PROTOCOL_UNSUPPORTED_VERSION) {
    fprintf(stderr, "a non-adjacent minor was accepted\n");
    return 1;
  }
  return 0;
}

int main(void)
{
  int failures = 0;

  failures += test_hello_golden_vector();
  failures += test_all_message_golden_vectors();
  failures += test_decoder_rejects_malformed_frames();
  failures += test_adjacent_minor_build_compatibility();
  return failures == 0 ? 0 : 1;
}
