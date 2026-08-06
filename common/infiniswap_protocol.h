/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#ifndef INFINISWAP_PROTOCOL_H
#define INFINISWAP_PROTOCOL_H

#ifdef __KERNEL__
#include <linux/stddef.h>
#include <linux/types.h>
typedef u8 is_protocol_u8;
typedef u16 is_protocol_u16;
typedef u32 is_protocol_u32;
typedef u64 is_protocol_u64;
#else
#include <stddef.h>
#include <stdint.h>
typedef uint8_t is_protocol_u8;
typedef uint16_t is_protocol_u16;
typedef uint32_t is_protocol_u32;
typedef uint64_t is_protocol_u64;
#endif

#define IS_PROTOCOL_MAGIC 0x49535750U
#define IS_PROTOCOL_MAJOR 1U
#ifndef IS_PROTOCOL_MINOR_CURRENT
#define IS_PROTOCOL_MINOR_CURRENT 1U
#endif
#if IS_PROTOCOL_MINOR_CURRENT > 0
#define IS_PROTOCOL_MINOR_PREVIOUS (IS_PROTOCOL_MINOR_CURRENT - 1U)
#else
#define IS_PROTOCOL_MINOR_PREVIOUS 0U
#endif
#define IS_PROTOCOL_MINOR_NEXT (IS_PROTOCOL_MINOR_CURRENT + 1U)
#define IS_PROTOCOL_HEADER_SIZE 48U
#define IS_PROTOCOL_MAX_FRAME_SIZE 4096U
#define IS_PROTOCOL_NONCE_SIZE 32U
#define IS_PROTOCOL_AUTH_TAG_SIZE 32U
#define IS_PROTOCOL_CONSUMER_ID_MAX 63U
#define IS_PROTOCOL_KEY_ID_MAX 31U
#define IS_PROTOCOL_MAX_CHUNKS_PER_FRAME 128U
#define IS_PROTOCOL_FLAG_RESPONSE (1U << 0)
#define IS_PROTOCOL_FLAG_ERROR (1U << 1)
#define IS_PROTOCOL_FLAGS_KNOWN                                             \
  (IS_PROTOCOL_FLAG_RESPONSE | IS_PROTOCOL_FLAG_ERROR)
#define IS_PROTOCOL_STATUS_HEALTHY (1U << 0)

#define IS_PROTOCOL_CAP_BACKED (1ULL << 0)
#define IS_PROTOCOL_CAP_REMOTE_ONLY (1ULL << 1)
#define IS_PROTOCOL_CAP_OPPORTUNISTIC_POOL (1ULL << 2)
#define IS_PROTOCOL_CAP_COMMITTED_POOL (1ULL << 3)
#define IS_PROTOCOL_CAP_FAILURE_DEADLINE (1ULL << 4)
#define IS_PROTOCOL_CAP_STATUS (1ULL << 5)
#define IS_PROTOCOL_CAP_AUTH_HMAC_SHA256 (1ULL << 6)
#define IS_PROTOCOL_CAP_KNOWN                                                \
  (IS_PROTOCOL_CAP_BACKED | IS_PROTOCOL_CAP_REMOTE_ONLY |                   \
   IS_PROTOCOL_CAP_OPPORTUNISTIC_POOL | IS_PROTOCOL_CAP_COMMITTED_POOL |    \
   IS_PROTOCOL_CAP_FAILURE_DEADLINE | IS_PROTOCOL_CAP_STATUS |              \
   IS_PROTOCOL_CAP_AUTH_HMAC_SHA256)

enum is_protocol_result {
  IS_PROTOCOL_OK = 0,
  IS_PROTOCOL_INVALID_ARGUMENT,
  IS_PROTOCOL_BUFFER_TOO_SMALL,
  IS_PROTOCOL_BAD_MAGIC,
  IS_PROTOCOL_UNSUPPORTED_VERSION,
  IS_PROTOCOL_OVERSIZED,
  IS_PROTOCOL_TRUNCATED,
  IS_PROTOCOL_TRAILING_DATA,
  IS_PROTOCOL_UNKNOWN_MESSAGE,
  IS_PROTOCOL_UNKNOWN_REQUIRED_CAPABILITY,
  IS_PROTOCOL_INVALID_CAPABILITIES,
  IS_PROTOCOL_INVALID_PAYLOAD,
  IS_PROTOCOL_INTEGER_OVERFLOW
};

enum is_protocol_message_type {
  IS_PROTOCOL_MSG_HELLO = 1,
  IS_PROTOCOL_MSG_CHALLENGE = 2,
  IS_PROTOCOL_MSG_AUTH = 3,
  IS_PROTOCOL_MSG_ACCEPT = 4,
  IS_PROTOCOL_MSG_STATUS_REQUEST = 5,
  IS_PROTOCOL_MSG_STATUS_RESPONSE = 6,
  IS_PROTOCOL_MSG_CHUNK_REQUEST = 7,
  IS_PROTOCOL_MSG_CHUNK_GRANT = 8,
  IS_PROTOCOL_MSG_EVICT = 9,
  IS_PROTOCOL_MSG_ACTIVITY = 10,
  IS_PROTOCOL_MSG_RELEASE = 11,
  IS_PROTOCOL_MSG_GOODBYE = 12,
  IS_PROTOCOL_MSG_ERROR = 13
};

enum is_protocol_error_code {
  IS_PROTOCOL_ERROR_MALFORMED = 1,
  IS_PROTOCOL_ERROR_VERSION = 2,
  IS_PROTOCOL_ERROR_CAPABILITY = 3,
  IS_PROTOCOL_ERROR_OUT_OF_ORDER = 4,
  IS_PROTOCOL_ERROR_AUTHENTICATION = 5,
  IS_PROTOCOL_ERROR_REPLAY = 6,
  IS_PROTOCOL_ERROR_REVOKED = 7,
  IS_PROTOCOL_ERROR_RESOURCE = 8,
  IS_PROTOCOL_ERROR_INTERNAL = 9
};

enum is_protocol_mode {
  IS_PROTOCOL_MODE_BACKED = 1,
  IS_PROTOCOL_MODE_REMOTE_ONLY = 2
};

enum is_protocol_pool {
  IS_PROTOCOL_POOL_OPPORTUNISTIC = 1,
  IS_PROTOCOL_POOL_COMMITTED = 2
};

struct is_protocol_header {
  is_protocol_u16 major;
  is_protocol_u16 minor;
  is_protocol_u16 type;
  is_protocol_u16 flags;
  is_protocol_u64 request_id;
  is_protocol_u64 session_id;
  is_protocol_u64 capabilities;
  is_protocol_u64 required_capabilities;
};

struct is_protocol_hello {
  char consumer_id[IS_PROTOCOL_CONSUMER_ID_MAX + 1U];
  char key_id[IS_PROTOCOL_KEY_ID_MAX + 1U];
  is_protocol_u8 nonce[IS_PROTOCOL_NONCE_SIZE];
  is_protocol_u8 mode;
  is_protocol_u8 pool;
  is_protocol_u32 failure_deadline_ms;
};

struct is_protocol_challenge {
  is_protocol_u8 nonce[IS_PROTOCOL_NONCE_SIZE];
  is_protocol_u16 negotiated_minor;
};

struct is_protocol_auth {
  is_protocol_u8 tag[IS_PROTOCOL_AUTH_TAG_SIZE];
};

struct is_protocol_accept {
  is_protocol_u16 negotiated_minor;
};

struct is_protocol_status {
  is_protocol_u32 available_opportunistic_chunks;
  is_protocol_u32 available_committed_chunks;
  is_protocol_u32 provider_failure_deadline_ms;
  is_protocol_u32 flags;
};

struct is_protocol_chunk_request {
  is_protocol_u32 chunk_count;
  is_protocol_u32 logical_start;
  is_protocol_u8 pool;
};

struct is_protocol_chunk {
  is_protocol_u32 logical_chunk_id;
  is_protocol_u32 provider_chunk_id;
  is_protocol_u64 remote_address;
  is_protocol_u32 remote_key;
  is_protocol_u8 pool;
};

struct is_protocol_chunk_grant {
  is_protocol_u16 chunk_count;
  struct is_protocol_chunk chunks[IS_PROTOCOL_MAX_CHUNKS_PER_FRAME];
};

struct is_protocol_chunk_ids {
  is_protocol_u16 chunk_count;
  is_protocol_u32 chunk_ids[IS_PROTOCOL_MAX_CHUNKS_PER_FRAME];
};

struct is_protocol_chunk_activity {
  is_protocol_u32 provider_chunk_id;
  is_protocol_u64 activity;
};

struct is_protocol_activity {
  is_protocol_u16 chunk_count;
  struct is_protocol_chunk_activity
      chunks[IS_PROTOCOL_MAX_CHUNKS_PER_FRAME];
};

struct is_protocol_error {
  is_protocol_u16 code;
  is_protocol_u16 offending_type;
  is_protocol_u8 retryable;
};

struct is_protocol_message {
  struct is_protocol_header header;
  union {
    struct is_protocol_hello hello;
    struct is_protocol_challenge challenge;
    struct is_protocol_auth auth;
    struct is_protocol_accept accept;
    struct is_protocol_status status;
    struct is_protocol_chunk_request chunk_request;
    struct is_protocol_chunk_grant chunk_grant;
    struct is_protocol_chunk_ids chunk_ids;
    struct is_protocol_activity activity;
    struct is_protocol_error error;
  } payload;
};

enum is_protocol_result
is_protocol_encode(const struct is_protocol_message *message,
                   is_protocol_u8 *frame, size_t frame_capacity,
                   size_t *frame_size);

enum is_protocol_result
is_protocol_decode(const is_protocol_u8 *frame, size_t frame_size,
                   struct is_protocol_message *message);

enum is_protocol_result
is_protocol_negotiate(is_protocol_u16 local_minor,
                      is_protocol_u64 local_capabilities,
                      is_protocol_u64 local_required_capabilities,
                      is_protocol_u16 peer_minor,
                      is_protocol_u64 peer_capabilities,
                      is_protocol_u64 peer_required_capabilities,
                      is_protocol_u16 *negotiated_minor,
                      is_protocol_u64 *negotiated_capabilities);

const char *is_protocol_result_name(enum is_protocol_result result);

#endif /* INFINISWAP_PROTOCOL_H */
