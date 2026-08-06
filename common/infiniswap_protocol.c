/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#include "infiniswap_protocol.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#define IS_PROTOCOL_HELLO_FIXED_SIZE (8U + IS_PROTOCOL_NONCE_SIZE)
#define IS_PROTOCOL_CHALLENGE_SIZE (4U + IS_PROTOCOL_NONCE_SIZE)
#define IS_PROTOCOL_ACCEPT_SIZE 4U
#define IS_PROTOCOL_STATUS_SIZE 16U
#define IS_PROTOCOL_CHUNK_REQUEST_SIZE 12U
#define IS_PROTOCOL_CHUNK_LIST_HEADER_SIZE 4U
#define IS_PROTOCOL_CHUNK_SIZE 24U
#define IS_PROTOCOL_CHUNK_ID_SIZE 4U
#define IS_PROTOCOL_ACTIVITY_SIZE 12U
#define IS_PROTOCOL_ERROR_SIZE 8U

static void is_put_u16(is_protocol_u8 *out, is_protocol_u16 value)
{
  out[0] = (is_protocol_u8)(value >> 8);
  out[1] = (is_protocol_u8)value;
}

static void is_put_u32(is_protocol_u8 *out, is_protocol_u32 value)
{
  out[0] = (is_protocol_u8)(value >> 24);
  out[1] = (is_protocol_u8)(value >> 16);
  out[2] = (is_protocol_u8)(value >> 8);
  out[3] = (is_protocol_u8)value;
}

static void is_put_u64(is_protocol_u8 *out, is_protocol_u64 value)
{
  is_put_u32(out, (is_protocol_u32)(value >> 32));
  is_put_u32(out + 4, (is_protocol_u32)value);
}

static is_protocol_u16 is_get_u16(const is_protocol_u8 *in)
{
  return (is_protocol_u16)(((is_protocol_u16)in[0] << 8) |
                           (is_protocol_u16)in[1]);
}

static is_protocol_u32 is_get_u32(const is_protocol_u8 *in)
{
  return ((is_protocol_u32)in[0] << 24) |
         ((is_protocol_u32)in[1] << 16) |
         ((is_protocol_u32)in[2] << 8) |
         (is_protocol_u32)in[3];
}

static is_protocol_u64 is_get_u64(const is_protocol_u8 *in)
{
  return ((is_protocol_u64)is_get_u32(in) << 32) |
         (is_protocol_u64)is_get_u32(in + 4);
}

static size_t is_bounded_string_length(const char *value, size_t maximum)
{
  size_t length;

  for (length = 0; length <= maximum; length++) {
    if (value[length] == '\0')
      return length;
  }
  return maximum + 1U;
}

static int is_identifier_valid(const char *value, size_t length)
{
  size_t index;

  if (length == 0)
    return 0;
  for (index = 0; index < length; index++) {
    const char c = value[index];

    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
      return 0;
  }
  return 1;
}

static int is_type_valid(is_protocol_u16 type)
{
  return type >= IS_PROTOCOL_MSG_HELLO && type <= IS_PROTOCOL_MSG_ERROR;
}

static int is_flags_valid(is_protocol_u16 type, is_protocol_u16 flags)
{
  switch (type) {
  case IS_PROTOCOL_MSG_CHALLENGE:
  case IS_PROTOCOL_MSG_ACCEPT:
  case IS_PROTOCOL_MSG_STATUS_RESPONSE:
  case IS_PROTOCOL_MSG_CHUNK_GRANT:
  case IS_PROTOCOL_MSG_ACTIVITY:
    return flags == IS_PROTOCOL_FLAG_RESPONSE;
  case IS_PROTOCOL_MSG_RELEASE:
    return flags == 0 || flags == IS_PROTOCOL_FLAG_RESPONSE;
  case IS_PROTOCOL_MSG_ERROR:
    return flags == (IS_PROTOCOL_FLAG_RESPONSE | IS_PROTOCOL_FLAG_ERROR);
  default:
    return flags == 0;
  }
}

static int is_minor_valid(is_protocol_u16 minor)
{
  return minor == IS_PROTOCOL_MINOR_CURRENT ||
         minor == IS_PROTOCOL_MINOR_PREVIOUS ||
         minor == IS_PROTOCOL_MINOR_NEXT;
}

static int is_pool_valid(is_protocol_u8 pool)
{
  return pool == IS_PROTOCOL_POOL_OPPORTUNISTIC ||
         pool == IS_PROTOCOL_POOL_COMMITTED;
}

static int is_deadline_valid(is_protocol_u32 deadline_ms)
{
  return deadline_ms >= IS_PROTOCOL_FAILURE_DEADLINE_MIN_MS &&
         deadline_ms <= IS_PROTOCOL_FAILURE_DEADLINE_MAX_MS;
}

static enum is_protocol_result
is_validate_header(const struct is_protocol_header *header)
{
  if (header->major != IS_PROTOCOL_MAJOR || !is_minor_valid(header->minor))
    return IS_PROTOCOL_UNSUPPORTED_VERSION;
  if (!is_type_valid(header->type))
    return IS_PROTOCOL_UNKNOWN_MESSAGE;
  if (header->required_capabilities & ~IS_PROTOCOL_CAP_KNOWN)
    return IS_PROTOCOL_UNKNOWN_REQUIRED_CAPABILITY;
  if (header->required_capabilities & ~header->capabilities)
    return IS_PROTOCOL_INVALID_CAPABILITIES;
  if (header->request_id == 0 ||
      (header->flags & ~IS_PROTOCOL_FLAGS_KNOWN) != 0 ||
      !is_flags_valid(header->type, header->flags))
    return IS_PROTOCOL_INVALID_PAYLOAD;
  if ((header->type == IS_PROTOCOL_MSG_HELLO ||
       header->type == IS_PROTOCOL_MSG_CHALLENGE ||
       header->type == IS_PROTOCOL_MSG_AUTH) &&
      header->session_id != 0)
    return IS_PROTOCOL_INVALID_PAYLOAD;
  if (header->type > IS_PROTOCOL_MSG_AUTH &&
      header->type != IS_PROTOCOL_MSG_ERROR && header->session_id == 0)
    return IS_PROTOCOL_INVALID_PAYLOAD;
  return IS_PROTOCOL_OK;
}

static enum is_protocol_result
is_validate_hello(const struct is_protocol_hello *hello, size_t *consumer_length,
                  size_t *key_length)
{
  *consumer_length = is_bounded_string_length(
      hello->consumer_id, IS_PROTOCOL_CONSUMER_ID_MAX);
  *key_length = is_bounded_string_length(hello->key_id,
                                         IS_PROTOCOL_KEY_ID_MAX);
  if (*consumer_length > IS_PROTOCOL_CONSUMER_ID_MAX ||
      *key_length > IS_PROTOCOL_KEY_ID_MAX ||
      !is_identifier_valid(hello->consumer_id, *consumer_length) ||
      !is_identifier_valid(hello->key_id, *key_length))
    return IS_PROTOCOL_INVALID_PAYLOAD;
  if ((hello->mode != IS_PROTOCOL_MODE_BACKED &&
       hello->mode != IS_PROTOCOL_MODE_REMOTE_ONLY) ||
      !is_pool_valid(hello->pool) ||
      !is_deadline_valid(hello->failure_deadline_ms))
    return IS_PROTOCOL_INVALID_PAYLOAD;
  if ((hello->mode == IS_PROTOCOL_MODE_BACKED &&
       hello->pool != IS_PROTOCOL_POOL_OPPORTUNISTIC) ||
      (hello->mode == IS_PROTOCOL_MODE_REMOTE_ONLY &&
       hello->pool != IS_PROTOCOL_POOL_COMMITTED))
    return IS_PROTOCOL_INVALID_PAYLOAD;
  return IS_PROTOCOL_OK;
}

static enum is_protocol_result
is_validate_chunk_count(is_protocol_u16 chunk_count)
{
  if (chunk_count == 0 || chunk_count > IS_PROTOCOL_MAX_CHUNKS_PER_FRAME)
    return IS_PROTOCOL_INVALID_PAYLOAD;
  return IS_PROTOCOL_OK;
}

static enum is_protocol_result
is_payload_size(const struct is_protocol_message *message, size_t *payload_size,
                size_t *consumer_length, size_t *key_length)
{
  enum is_protocol_result result;
  size_t index;

  *consumer_length = 0;
  *key_length = 0;
  switch (message->header.type) {
  case IS_PROTOCOL_MSG_HELLO:
    result = is_validate_hello(&message->payload.hello, consumer_length,
                               key_length);
    if (result != IS_PROTOCOL_OK)
      return result;
    *payload_size = IS_PROTOCOL_HELLO_FIXED_SIZE + *consumer_length +
                    *key_length;
    return IS_PROTOCOL_OK;
  case IS_PROTOCOL_MSG_CHALLENGE:
    if (!is_minor_valid(message->payload.challenge.negotiated_minor))
      return IS_PROTOCOL_INVALID_PAYLOAD;
    *payload_size = IS_PROTOCOL_CHALLENGE_SIZE;
    return IS_PROTOCOL_OK;
  case IS_PROTOCOL_MSG_AUTH:
    *payload_size = IS_PROTOCOL_AUTH_TAG_SIZE;
    return IS_PROTOCOL_OK;
  case IS_PROTOCOL_MSG_ACCEPT:
    if (!is_minor_valid(message->payload.accept.negotiated_minor))
      return IS_PROTOCOL_INVALID_PAYLOAD;
    *payload_size = IS_PROTOCOL_ACCEPT_SIZE;
    return IS_PROTOCOL_OK;
  case IS_PROTOCOL_MSG_STATUS_REQUEST:
  case IS_PROTOCOL_MSG_GOODBYE:
    *payload_size = 0;
    return IS_PROTOCOL_OK;
  case IS_PROTOCOL_MSG_STATUS_RESPONSE:
    if (!is_deadline_valid(
            message->payload.status.provider_failure_deadline_ms))
      return IS_PROTOCOL_INVALID_PAYLOAD;
    *payload_size = IS_PROTOCOL_STATUS_SIZE;
    return IS_PROTOCOL_OK;
  case IS_PROTOCOL_MSG_CHUNK_REQUEST:
    if (message->payload.chunk_request.chunk_count == 0 ||
        message->payload.chunk_request.chunk_count >
            IS_PROTOCOL_MAX_CHUNKS_PER_FRAME ||
        message->payload.chunk_request.chunk_count - 1U >
            (is_protocol_u32)~message->payload.chunk_request.logical_start ||
        !is_pool_valid(message->payload.chunk_request.pool))
      return IS_PROTOCOL_INVALID_PAYLOAD;
    *payload_size = IS_PROTOCOL_CHUNK_REQUEST_SIZE;
    return IS_PROTOCOL_OK;
  case IS_PROTOCOL_MSG_CHUNK_GRANT:
    result = is_validate_chunk_count(
        message->payload.chunk_grant.chunk_count);
    if (result != IS_PROTOCOL_OK)
      return result;
    for (index = 0; index < message->payload.chunk_grant.chunk_count;
         index++) {
      const struct is_protocol_chunk *chunk =
          &message->payload.chunk_grant.chunks[index];
      size_t previous;

      if (!is_pool_valid(chunk->pool) || chunk->remote_address == 0 ||
          chunk->remote_key == 0)
        return IS_PROTOCOL_INVALID_PAYLOAD;
      for (previous = 0; previous < index; previous++) {
        const struct is_protocol_chunk *seen =
            &message->payload.chunk_grant.chunks[previous];

        if (seen->logical_chunk_id == chunk->logical_chunk_id ||
            seen->provider_chunk_id == chunk->provider_chunk_id)
          return IS_PROTOCOL_INVALID_PAYLOAD;
      }
    }
    *payload_size = IS_PROTOCOL_CHUNK_LIST_HEADER_SIZE +
                    IS_PROTOCOL_CHUNK_SIZE *
                        message->payload.chunk_grant.chunk_count;
    return IS_PROTOCOL_OK;
  case IS_PROTOCOL_MSG_EVICT:
  case IS_PROTOCOL_MSG_RELEASE:
    result = is_validate_chunk_count(message->payload.chunk_ids.chunk_count);
    if (result != IS_PROTOCOL_OK)
      return result;
    for (index = 0; index < message->payload.chunk_ids.chunk_count; index++) {
      size_t previous;

      for (previous = 0; previous < index; previous++) {
        if (message->payload.chunk_ids.chunk_ids[previous] ==
            message->payload.chunk_ids.chunk_ids[index])
          return IS_PROTOCOL_INVALID_PAYLOAD;
      }
    }
    *payload_size = IS_PROTOCOL_CHUNK_LIST_HEADER_SIZE +
                    IS_PROTOCOL_CHUNK_ID_SIZE *
                        message->payload.chunk_ids.chunk_count;
    return IS_PROTOCOL_OK;
  case IS_PROTOCOL_MSG_ACTIVITY:
    result = is_validate_chunk_count(message->payload.activity.chunk_count);
    if (result != IS_PROTOCOL_OK)
      return result;
    for (index = 0; index < message->payload.activity.chunk_count; index++) {
      size_t previous;

      for (previous = 0; previous < index; previous++) {
        if (message->payload.activity.chunks[previous].provider_chunk_id ==
            message->payload.activity.chunks[index].provider_chunk_id)
          return IS_PROTOCOL_INVALID_PAYLOAD;
      }
    }
    *payload_size = IS_PROTOCOL_CHUNK_LIST_HEADER_SIZE +
                    IS_PROTOCOL_ACTIVITY_SIZE *
                        message->payload.activity.chunk_count;
    return IS_PROTOCOL_OK;
  case IS_PROTOCOL_MSG_ERROR:
    if (message->payload.error.code < IS_PROTOCOL_ERROR_MALFORMED ||
        message->payload.error.code > IS_PROTOCOL_ERROR_INTERNAL ||
        message->payload.error.retryable > 1)
      return IS_PROTOCOL_INVALID_PAYLOAD;
    *payload_size = IS_PROTOCOL_ERROR_SIZE;
    return IS_PROTOCOL_OK;
  default:
    return IS_PROTOCOL_UNKNOWN_MESSAGE;
  }
}

static void is_encode_header(const struct is_protocol_header *header,
                             size_t payload_size, is_protocol_u8 *frame)
{
  is_put_u32(frame, IS_PROTOCOL_MAGIC);
  is_put_u16(frame + 4, header->major);
  is_put_u16(frame + 6, header->minor);
  is_put_u16(frame + 8, header->type);
  is_put_u16(frame + 10, header->flags);
  is_put_u32(frame + 12, (is_protocol_u32)payload_size);
  is_put_u64(frame + 16, header->request_id);
  is_put_u64(frame + 24, header->session_id);
  is_put_u64(frame + 32, header->capabilities);
  is_put_u64(frame + 40, header->required_capabilities);
}

static void is_encode_chunk_ids(const struct is_protocol_chunk_ids *chunk_ids,
                                is_protocol_u8 *payload)
{
  size_t index;

  is_put_u16(payload, chunk_ids->chunk_count);
  is_put_u16(payload + 2, 0);
  for (index = 0; index < chunk_ids->chunk_count; index++)
    is_put_u32(payload + IS_PROTOCOL_CHUNK_LIST_HEADER_SIZE +
                   index * IS_PROTOCOL_CHUNK_ID_SIZE,
               chunk_ids->chunk_ids[index]);
}

static void is_encode_payload(const struct is_protocol_message *message,
                              size_t consumer_length, size_t key_length,
                              is_protocol_u8 *payload)
{
  size_t index;

  switch (message->header.type) {
  case IS_PROTOCOL_MSG_HELLO:
    payload[0] = (is_protocol_u8)consumer_length;
    payload[1] = (is_protocol_u8)key_length;
    payload[2] = message->payload.hello.mode;
    payload[3] = message->payload.hello.pool;
    is_put_u32(payload + 4, message->payload.hello.failure_deadline_ms);
    memcpy(payload + 8, message->payload.hello.nonce,
           IS_PROTOCOL_NONCE_SIZE);
    memcpy(payload + IS_PROTOCOL_HELLO_FIXED_SIZE,
           message->payload.hello.consumer_id, consumer_length);
    memcpy(payload + IS_PROTOCOL_HELLO_FIXED_SIZE + consumer_length,
           message->payload.hello.key_id, key_length);
    break;
  case IS_PROTOCOL_MSG_CHALLENGE:
    memcpy(payload, message->payload.challenge.nonce,
           IS_PROTOCOL_NONCE_SIZE);
    is_put_u16(payload + IS_PROTOCOL_NONCE_SIZE,
               message->payload.challenge.negotiated_minor);
    is_put_u16(payload + IS_PROTOCOL_NONCE_SIZE + 2U, 0);
    break;
  case IS_PROTOCOL_MSG_AUTH:
    memcpy(payload, message->payload.auth.tag, IS_PROTOCOL_AUTH_TAG_SIZE);
    break;
  case IS_PROTOCOL_MSG_ACCEPT:
    is_put_u16(payload, message->payload.accept.negotiated_minor);
    is_put_u16(payload + 2, 0);
    break;
  case IS_PROTOCOL_MSG_STATUS_RESPONSE:
    is_put_u32(payload,
               message->payload.status.available_opportunistic_chunks);
    is_put_u32(payload + 4,
               message->payload.status.available_committed_chunks);
    is_put_u32(payload + 8,
               message->payload.status.provider_failure_deadline_ms);
    is_put_u32(payload + 12, message->payload.status.flags);
    break;
  case IS_PROTOCOL_MSG_CHUNK_REQUEST:
    is_put_u32(payload, message->payload.chunk_request.chunk_count);
    is_put_u32(payload + 4, message->payload.chunk_request.logical_start);
    payload[8] = message->payload.chunk_request.pool;
    payload[9] = 0;
    payload[10] = 0;
    payload[11] = 0;
    break;
  case IS_PROTOCOL_MSG_CHUNK_GRANT:
    is_put_u16(payload, message->payload.chunk_grant.chunk_count);
    is_put_u16(payload + 2, 0);
    for (index = 0; index < message->payload.chunk_grant.chunk_count;
         index++) {
      const struct is_protocol_chunk *chunk =
          &message->payload.chunk_grant.chunks[index];
      is_protocol_u8 *entry = payload + IS_PROTOCOL_CHUNK_LIST_HEADER_SIZE +
                              index * IS_PROTOCOL_CHUNK_SIZE;

      is_put_u32(entry, chunk->logical_chunk_id);
      is_put_u32(entry + 4, chunk->provider_chunk_id);
      is_put_u64(entry + 8, chunk->remote_address);
      is_put_u32(entry + 16, chunk->remote_key);
      entry[20] = chunk->pool;
      entry[21] = 0;
      entry[22] = 0;
      entry[23] = 0;
    }
    break;
  case IS_PROTOCOL_MSG_EVICT:
  case IS_PROTOCOL_MSG_RELEASE:
    is_encode_chunk_ids(&message->payload.chunk_ids, payload);
    break;
  case IS_PROTOCOL_MSG_ACTIVITY:
    is_put_u16(payload, message->payload.activity.chunk_count);
    is_put_u16(payload + 2, 0);
    for (index = 0; index < message->payload.activity.chunk_count; index++) {
      const struct is_protocol_chunk_activity *activity =
          &message->payload.activity.chunks[index];
      is_protocol_u8 *entry = payload + IS_PROTOCOL_CHUNK_LIST_HEADER_SIZE +
                              index * IS_PROTOCOL_ACTIVITY_SIZE;

      is_put_u32(entry, activity->provider_chunk_id);
      is_put_u64(entry + 4, activity->activity);
    }
    break;
  case IS_PROTOCOL_MSG_ERROR:
    is_put_u16(payload, message->payload.error.code);
    is_put_u16(payload + 2, message->payload.error.offending_type);
    payload[4] = message->payload.error.retryable;
    payload[5] = 0;
    payload[6] = 0;
    payload[7] = 0;
    break;
  case IS_PROTOCOL_MSG_STATUS_REQUEST:
  case IS_PROTOCOL_MSG_GOODBYE:
  default:
    break;
  }
}

enum is_protocol_result
is_protocol_encode(const struct is_protocol_message *message,
                   is_protocol_u8 *frame, size_t frame_capacity,
                   size_t *frame_size)
{
  enum is_protocol_result result;
  size_t consumer_length;
  size_t key_length;
  size_t payload_size;

  if (!message || !frame || !frame_size)
    return IS_PROTOCOL_INVALID_ARGUMENT;
  result = is_validate_header(&message->header);
  if (result != IS_PROTOCOL_OK)
    return result;
  result = is_payload_size(message, &payload_size, &consumer_length,
                           &key_length);
  if (result != IS_PROTOCOL_OK)
    return result;
  if (payload_size > IS_PROTOCOL_MAX_FRAME_SIZE - IS_PROTOCOL_HEADER_SIZE)
    return IS_PROTOCOL_OVERSIZED;
  if (frame_capacity < IS_PROTOCOL_HEADER_SIZE + payload_size)
    return IS_PROTOCOL_BUFFER_TOO_SMALL;

  is_encode_header(&message->header, payload_size, frame);
  is_encode_payload(message, consumer_length, key_length,
                    frame + IS_PROTOCOL_HEADER_SIZE);
  *frame_size = IS_PROTOCOL_HEADER_SIZE + payload_size;
  return IS_PROTOCOL_OK;
}

static int is_reserved_zero(const is_protocol_u8 *value, size_t size)
{
  size_t index;

  for (index = 0; index < size; index++) {
    if (value[index] != 0)
      return 0;
  }
  return 1;
}

static enum is_protocol_result
is_decode_header(const is_protocol_u8 *frame, size_t frame_size,
                 struct is_protocol_header *header, size_t *payload_size)
{
  if (frame_size < IS_PROTOCOL_HEADER_SIZE)
    return IS_PROTOCOL_TRUNCATED;
  if (is_get_u32(frame) != IS_PROTOCOL_MAGIC)
    return IS_PROTOCOL_BAD_MAGIC;

  header->major = is_get_u16(frame + 4);
  header->minor = is_get_u16(frame + 6);
  header->type = is_get_u16(frame + 8);
  header->flags = is_get_u16(frame + 10);
  *payload_size = is_get_u32(frame + 12);
  header->request_id = is_get_u64(frame + 16);
  header->session_id = is_get_u64(frame + 24);
  header->capabilities = is_get_u64(frame + 32);
  header->required_capabilities = is_get_u64(frame + 40);

  if (*payload_size > IS_PROTOCOL_MAX_FRAME_SIZE - IS_PROTOCOL_HEADER_SIZE)
    return IS_PROTOCOL_OVERSIZED;
  if (*payload_size > frame_size - IS_PROTOCOL_HEADER_SIZE)
    return IS_PROTOCOL_TRUNCATED;
  if (*payload_size < frame_size - IS_PROTOCOL_HEADER_SIZE)
    return IS_PROTOCOL_TRAILING_DATA;
  return is_validate_header(header);
}

static enum is_protocol_result
is_require_payload_size(size_t actual, size_t expected)
{
  return actual == expected ? IS_PROTOCOL_OK : IS_PROTOCOL_INVALID_PAYLOAD;
}

static enum is_protocol_result
is_counted_payload_size(size_t actual, is_protocol_u16 count,
                       size_t entry_size)
{
  size_t expected;

  if (count == 0)
    return IS_PROTOCOL_INVALID_PAYLOAD;
  if ((size_t)count >
      (IS_PROTOCOL_MAX_FRAME_SIZE - IS_PROTOCOL_CHUNK_LIST_HEADER_SIZE) /
          entry_size)
    return IS_PROTOCOL_INTEGER_OVERFLOW;
  if (count > IS_PROTOCOL_MAX_CHUNKS_PER_FRAME)
    return IS_PROTOCOL_INVALID_PAYLOAD;
  expected = IS_PROTOCOL_CHUNK_LIST_HEADER_SIZE + (size_t)count * entry_size;
  return is_require_payload_size(actual, expected);
}

static enum is_protocol_result
is_decode_hello(const is_protocol_u8 *payload, size_t payload_size,
                struct is_protocol_hello *hello)
{
  size_t consumer_length;
  size_t key_length;

  if (payload_size < IS_PROTOCOL_HELLO_FIXED_SIZE)
    return IS_PROTOCOL_INVALID_PAYLOAD;
  consumer_length = payload[0];
  key_length = payload[1];
  if (consumer_length == 0 || consumer_length > IS_PROTOCOL_CONSUMER_ID_MAX ||
      key_length == 0 || key_length > IS_PROTOCOL_KEY_ID_MAX ||
      consumer_length + key_length !=
          payload_size - IS_PROTOCOL_HELLO_FIXED_SIZE)
    return IS_PROTOCOL_INVALID_PAYLOAD;

  hello->mode = payload[2];
  hello->pool = payload[3];
  hello->failure_deadline_ms = is_get_u32(payload + 4);
  memcpy(hello->nonce, payload + 8, IS_PROTOCOL_NONCE_SIZE);
  memcpy(hello->consumer_id, payload + IS_PROTOCOL_HELLO_FIXED_SIZE,
         consumer_length);
  memcpy(hello->key_id,
         payload + IS_PROTOCOL_HELLO_FIXED_SIZE + consumer_length,
         key_length);
  return is_validate_hello(hello, &consumer_length, &key_length);
}

static enum is_protocol_result
is_decode_chunk_ids(const is_protocol_u8 *payload, size_t payload_size,
                    struct is_protocol_chunk_ids *chunk_ids)
{
  enum is_protocol_result result;
  size_t index;

  if (payload_size < IS_PROTOCOL_CHUNK_LIST_HEADER_SIZE)
    return IS_PROTOCOL_INVALID_PAYLOAD;
  chunk_ids->chunk_count = is_get_u16(payload);
  if (!is_reserved_zero(payload + 2, 2U))
    return IS_PROTOCOL_INVALID_PAYLOAD;
  result = is_counted_payload_size(payload_size, chunk_ids->chunk_count,
                                   IS_PROTOCOL_CHUNK_ID_SIZE);
  if (result != IS_PROTOCOL_OK)
    return result;
  for (index = 0; index < chunk_ids->chunk_count; index++) {
    size_t previous;

    chunk_ids->chunk_ids[index] = is_get_u32(
        payload + IS_PROTOCOL_CHUNK_LIST_HEADER_SIZE +
        index * IS_PROTOCOL_CHUNK_ID_SIZE);
    for (previous = 0; previous < index; previous++) {
      if (chunk_ids->chunk_ids[previous] == chunk_ids->chunk_ids[index])
        return IS_PROTOCOL_INVALID_PAYLOAD;
    }
  }
  return IS_PROTOCOL_OK;
}

static enum is_protocol_result
is_decode_payload(const is_protocol_u8 *payload, size_t payload_size,
                  struct is_protocol_message *message)
{
  enum is_protocol_result result;
  size_t index;

  switch (message->header.type) {
  case IS_PROTOCOL_MSG_HELLO:
    return is_decode_hello(payload, payload_size, &message->payload.hello);
  case IS_PROTOCOL_MSG_CHALLENGE:
    result = is_require_payload_size(payload_size,
                                     IS_PROTOCOL_CHALLENGE_SIZE);
    if (result != IS_PROTOCOL_OK)
      return result;
    memcpy(message->payload.challenge.nonce, payload,
           IS_PROTOCOL_NONCE_SIZE);
    if (!is_reserved_zero(payload + IS_PROTOCOL_NONCE_SIZE + 2U, 2U))
      return IS_PROTOCOL_INVALID_PAYLOAD;
    message->payload.challenge.negotiated_minor =
        is_get_u16(payload + IS_PROTOCOL_NONCE_SIZE);
    if (!is_minor_valid(message->payload.challenge.negotiated_minor))
      return IS_PROTOCOL_INVALID_PAYLOAD;
    return IS_PROTOCOL_OK;
  case IS_PROTOCOL_MSG_AUTH:
    result = is_require_payload_size(payload_size, IS_PROTOCOL_AUTH_TAG_SIZE);
    if (result == IS_PROTOCOL_OK)
      memcpy(message->payload.auth.tag, payload, IS_PROTOCOL_AUTH_TAG_SIZE);
    return result;
  case IS_PROTOCOL_MSG_ACCEPT:
    result = is_require_payload_size(payload_size, IS_PROTOCOL_ACCEPT_SIZE);
    if (result != IS_PROTOCOL_OK)
      return result;
    if (!is_reserved_zero(payload + 2, 2U))
      return IS_PROTOCOL_INVALID_PAYLOAD;
    message->payload.accept.negotiated_minor = is_get_u16(payload);
    return is_minor_valid(message->payload.accept.negotiated_minor)
               ? IS_PROTOCOL_OK
               : IS_PROTOCOL_INVALID_PAYLOAD;
  case IS_PROTOCOL_MSG_STATUS_REQUEST:
  case IS_PROTOCOL_MSG_GOODBYE:
    return is_require_payload_size(payload_size, 0);
  case IS_PROTOCOL_MSG_STATUS_RESPONSE:
    result = is_require_payload_size(payload_size, IS_PROTOCOL_STATUS_SIZE);
    if (result != IS_PROTOCOL_OK)
      return result;
    message->payload.status.available_opportunistic_chunks =
        is_get_u32(payload);
    message->payload.status.available_committed_chunks =
        is_get_u32(payload + 4);
    message->payload.status.provider_failure_deadline_ms =
        is_get_u32(payload + 8);
    message->payload.status.flags = is_get_u32(payload + 12);
    return is_deadline_valid(
               message->payload.status.provider_failure_deadline_ms)
               ? IS_PROTOCOL_OK
               : IS_PROTOCOL_INVALID_PAYLOAD;
  case IS_PROTOCOL_MSG_CHUNK_REQUEST:
    result = is_require_payload_size(payload_size,
                                     IS_PROTOCOL_CHUNK_REQUEST_SIZE);
    if (result != IS_PROTOCOL_OK)
      return result;
    message->payload.chunk_request.chunk_count = is_get_u32(payload);
    message->payload.chunk_request.logical_start = is_get_u32(payload + 4);
    message->payload.chunk_request.pool = payload[8];
    if (!is_reserved_zero(payload + 9, 3U))
      return IS_PROTOCOL_INVALID_PAYLOAD;
    if (message->payload.chunk_request.chunk_count == 0 ||
        message->payload.chunk_request.chunk_count >
            IS_PROTOCOL_MAX_CHUNKS_PER_FRAME ||
        message->payload.chunk_request.chunk_count - 1U >
            (is_protocol_u32)~message->payload.chunk_request.logical_start ||
        !is_pool_valid(message->payload.chunk_request.pool))
      return IS_PROTOCOL_INVALID_PAYLOAD;
    return IS_PROTOCOL_OK;
  case IS_PROTOCOL_MSG_CHUNK_GRANT:
    if (payload_size < IS_PROTOCOL_CHUNK_LIST_HEADER_SIZE)
      return IS_PROTOCOL_INVALID_PAYLOAD;
    message->payload.chunk_grant.chunk_count = is_get_u16(payload);
    if (!is_reserved_zero(payload + 2, 2U))
      return IS_PROTOCOL_INVALID_PAYLOAD;
    result = is_counted_payload_size(
        payload_size, message->payload.chunk_grant.chunk_count,
        IS_PROTOCOL_CHUNK_SIZE);
    if (result != IS_PROTOCOL_OK)
      return result;
    for (index = 0; index < message->payload.chunk_grant.chunk_count;
         index++) {
      struct is_protocol_chunk *chunk =
          &message->payload.chunk_grant.chunks[index];
      const is_protocol_u8 *entry =
          payload + IS_PROTOCOL_CHUNK_LIST_HEADER_SIZE +
          index * IS_PROTOCOL_CHUNK_SIZE;
      size_t previous;

      chunk->logical_chunk_id = is_get_u32(entry);
      chunk->provider_chunk_id = is_get_u32(entry + 4);
      chunk->remote_address = is_get_u64(entry + 8);
      chunk->remote_key = is_get_u32(entry + 16);
      chunk->pool = entry[20];
      if (!is_reserved_zero(entry + 21, 3U) ||
          !is_pool_valid(chunk->pool) || chunk->remote_address == 0 ||
          chunk->remote_key == 0)
        return IS_PROTOCOL_INVALID_PAYLOAD;
      for (previous = 0; previous < index; previous++) {
        const struct is_protocol_chunk *seen =
            &message->payload.chunk_grant.chunks[previous];

        if (seen->logical_chunk_id == chunk->logical_chunk_id ||
            seen->provider_chunk_id == chunk->provider_chunk_id)
          return IS_PROTOCOL_INVALID_PAYLOAD;
      }
    }
    return IS_PROTOCOL_OK;
  case IS_PROTOCOL_MSG_EVICT:
  case IS_PROTOCOL_MSG_RELEASE:
    return is_decode_chunk_ids(payload, payload_size,
                               &message->payload.chunk_ids);
  case IS_PROTOCOL_MSG_ACTIVITY:
    if (payload_size < IS_PROTOCOL_CHUNK_LIST_HEADER_SIZE)
      return IS_PROTOCOL_INVALID_PAYLOAD;
    message->payload.activity.chunk_count = is_get_u16(payload);
    if (!is_reserved_zero(payload + 2, 2U))
      return IS_PROTOCOL_INVALID_PAYLOAD;
    result = is_counted_payload_size(
        payload_size, message->payload.activity.chunk_count,
        IS_PROTOCOL_ACTIVITY_SIZE);
    if (result != IS_PROTOCOL_OK)
      return result;
    for (index = 0; index < message->payload.activity.chunk_count; index++) {
      struct is_protocol_chunk_activity *activity =
          &message->payload.activity.chunks[index];
      const is_protocol_u8 *entry =
          payload + IS_PROTOCOL_CHUNK_LIST_HEADER_SIZE +
          index * IS_PROTOCOL_ACTIVITY_SIZE;
      size_t previous;

      activity->provider_chunk_id = is_get_u32(entry);
      activity->activity = is_get_u64(entry + 4);
      for (previous = 0; previous < index; previous++) {
        if (message->payload.activity.chunks[previous].provider_chunk_id ==
            activity->provider_chunk_id)
          return IS_PROTOCOL_INVALID_PAYLOAD;
      }
    }
    return IS_PROTOCOL_OK;
  case IS_PROTOCOL_MSG_ERROR:
    result = is_require_payload_size(payload_size, IS_PROTOCOL_ERROR_SIZE);
    if (result != IS_PROTOCOL_OK)
      return result;
    message->payload.error.code = is_get_u16(payload);
    message->payload.error.offending_type = is_get_u16(payload + 2);
    message->payload.error.retryable = payload[4];
    if (!is_reserved_zero(payload + 5, 3U))
      return IS_PROTOCOL_INVALID_PAYLOAD;
    if (message->payload.error.code < IS_PROTOCOL_ERROR_MALFORMED ||
        message->payload.error.code > IS_PROTOCOL_ERROR_INTERNAL ||
        message->payload.error.retryable > 1)
      return IS_PROTOCOL_INVALID_PAYLOAD;
    return IS_PROTOCOL_OK;
  default:
    return IS_PROTOCOL_UNKNOWN_MESSAGE;
  }
}

enum is_protocol_result
is_protocol_decode(const is_protocol_u8 *frame, size_t frame_size,
                   struct is_protocol_message *message)
{
  enum is_protocol_result result;
  size_t payload_size;

  if (!frame || !message)
    return IS_PROTOCOL_INVALID_ARGUMENT;
  if (frame_size > IS_PROTOCOL_MAX_FRAME_SIZE)
    return IS_PROTOCOL_OVERSIZED;
  memset(message, 0, sizeof(*message));
  result = is_decode_header(frame, frame_size, &message->header,
                            &payload_size);
  if (result != IS_PROTOCOL_OK)
    return result;
  return is_decode_payload(frame + IS_PROTOCOL_HEADER_SIZE, payload_size,
                           message);
}

enum is_protocol_result
is_protocol_negotiate(is_protocol_u16 local_minor,
                      is_protocol_u64 local_capabilities,
                      is_protocol_u64 local_required_capabilities,
                      is_protocol_u16 peer_minor,
                      is_protocol_u64 peer_capabilities,
                      is_protocol_u64 peer_required_capabilities,
                      is_protocol_u16 *negotiated_minor,
                      is_protocol_u64 *negotiated_capabilities)
{
  is_protocol_u64 shared;

  if (!negotiated_minor || !negotiated_capabilities)
    return IS_PROTOCOL_INVALID_ARGUMENT;
  if (!is_minor_valid(local_minor) || !is_minor_valid(peer_minor) ||
      (local_minor > peer_minor
           ? local_minor - peer_minor
           : peer_minor - local_minor) > 1)
    return IS_PROTOCOL_UNSUPPORTED_VERSION;
  if ((local_required_capabilities | peer_required_capabilities) &
      ~IS_PROTOCOL_CAP_KNOWN)
    return IS_PROTOCOL_UNKNOWN_REQUIRED_CAPABILITY;
  if ((local_required_capabilities & ~local_capabilities) ||
      (peer_required_capabilities & ~peer_capabilities))
    return IS_PROTOCOL_INVALID_CAPABILITIES;

  shared = local_capabilities & peer_capabilities;
  if ((local_required_capabilities | peer_required_capabilities) & ~shared)
    return IS_PROTOCOL_UNKNOWN_REQUIRED_CAPABILITY;
  *negotiated_minor = local_minor < peer_minor ? local_minor : peer_minor;
  *negotiated_capabilities = shared;
  return IS_PROTOCOL_OK;
}

const char *is_protocol_result_name(enum is_protocol_result result)
{
  static const char *const names[] = {
    "ok", "invalid argument", "buffer too small", "bad magic",
    "unsupported version", "oversized frame", "truncated frame",
    "trailing data", "unknown message", "unknown required capability",
    "invalid capabilities", "invalid payload", "integer overflow"
  };

  if ((unsigned int)result >= sizeof(names) / sizeof(names[0]))
    return "unknown protocol result";
  return names[result];
}
