/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#include "infiniswap_auth.h"

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/params.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static const uint8_t auth_domain[] =
    "Infiniswap control authentication v1";

static size_t bounded_length(const char *value, size_t maximum)
{
  size_t length;

  for (length = 0; length <= maximum; length++) {
    if (value[length] == '\0')
      return length;
  }
  return maximum + 1U;
}

static int identifier_valid(const char *value, size_t maximum)
{
  size_t index;
  size_t length = bounded_length(value, maximum);

  if (length == 0 || length > maximum)
    return 0;
  for (index = 0; index < length; index++) {
    const char c = value[index];

    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
      return 0;
  }
  return 1;
}

static int key_valid(const struct is_auth_key *key, int optional)
{
  if (optional && key->secret_size == 0 && key->key_id[0] == '\0')
    return 1;
  return identifier_valid(key->key_id, IS_PROTOCOL_KEY_ID_MAX) &&
         key->secret_size >= IS_AUTH_SECRET_MIN_SIZE &&
         key->secret_size <= IS_AUTH_SECRET_MAX_SIZE;
}

static int credential_valid(const struct is_auth_credential *credential)
{
  time_t now;

  if (!identifier_valid(credential->consumer_id,
                        IS_PROTOCOL_CONSUMER_ID_MAX) ||
      !key_valid(&credential->current, 0) ||
      !key_valid(&credential->next, 1))
    return 0;
  if (credential->next.secret_size != 0 &&
      (strcmp(credential->current.key_id, credential->next.key_id) == 0 ||
       credential->next_valid_until_unix == 0))
    return 0;
  if (credential->next.secret_size != 0) {
    now = time(NULL);
    if (now < 0 || credential->next_valid_until_unix <= (uint64_t)now ||
        credential->next_valid_until_unix - (uint64_t)now >
            IS_AUTH_MAX_KEY_OVERLAP_SECONDS)
      return 0;
  }
  if (credential->next.secret_size == 0 &&
      credential->next_valid_until_unix != 0)
    return 0;
  if (credential->max_connections == 0 ||
      credential->max_connections > IS_AUTH_MAX_CONSUMERS)
    return 0;
  return credential->max_opportunistic_chunks != 0 ||
         credential->max_committed_chunks != 0;
}

static uint64_t next_authorization_version(
    struct is_auth_registry *registry)
{
  registry->next_authorization_version++;
  if (registry->next_authorization_version == 0)
    registry->next_authorization_version++;
  return registry->next_authorization_version;
}

void is_auth_registry_init(struct is_auth_registry *registry)
{
  memset(registry, 0, sizeof(*registry));
  (void)pthread_mutex_init(&registry->lock, NULL);
}

void is_auth_registry_destroy(struct is_auth_registry *registry)
{
  if (!registry)
    return;
  OPENSSL_cleanse(registry->entries, sizeof(registry->entries));
  OPENSSL_cleanse(registry->listeners, sizeof(registry->listeners));
  registry->count = 0;
  registry->listener_count = 0;
  (void)pthread_mutex_destroy(&registry->lock);
}

enum is_auth_result
is_auth_registry_add(struct is_auth_registry *registry,
                     const struct is_auth_credential *credential)
{
  size_t index;
  enum is_auth_result result = IS_AUTH_OK;

  if (!registry || !credential || !credential_valid(credential))
    return IS_AUTH_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&registry->lock);
  if (registry->count == IS_AUTH_MAX_CONSUMERS) {
    result = IS_AUTH_FULL;
    goto out;
  }
  for (index = 0; index < registry->count; index++) {
    if (strcmp(registry->entries[index].consumer_id,
               credential->consumer_id) == 0) {
      result = IS_AUTH_DUPLICATE;
      goto out;
    }
  }
  registry->entries[registry->count] = *credential;
  registry->entries[registry->count].authorization_version =
      next_authorization_version(registry);
  registry->count++;
out:
  (void)pthread_mutex_unlock(&registry->lock);
  return result;
}

static struct is_auth_credential *
find_credential(struct is_auth_registry *registry, const char *consumer_id)
{
  size_t index;

  for (index = 0; index < registry->count; index++) {
    if (strcmp(registry->entries[index].consumer_id, consumer_id) == 0)
      return &registry->entries[index];
  }
  return NULL;
}

static void notify_revocation(struct is_auth_registry *registry,
                              const char *consumer_id)
{
  size_t index;

  for (index = 0; index < registry->listener_count; index++) {
    struct is_auth_listener *listener = &registry->listeners[index];

    if (strcmp(listener->consumer_id, consumer_id) == 0)
      listener->callback(listener->context);
  }
}

enum is_auth_result
is_auth_registry_subscribe(struct is_auth_registry *registry,
                           const char *consumer_id,
                           uint64_t authorization_version,
                           is_auth_revocation_callback callback,
                           void *context)
{
  struct is_auth_credential *credential;
  struct is_auth_listener *listener;
  size_t index;
  enum is_auth_result result = IS_AUTH_OK;

  if (!registry || !consumer_id || !callback || !context)
    return IS_AUTH_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&registry->lock);
  credential = find_credential(registry, consumer_id);
  if (!credential)
    result = IS_AUTH_NOT_FOUND;
  else if (credential->revoked)
    result = IS_AUTH_REVOKED;
  else if (authorization_version == 0 ||
           credential->authorization_version != authorization_version)
    result = IS_AUTH_FAILED;
  else if (registry->listener_count == IS_AUTH_MAX_CONSUMERS)
    result = IS_AUTH_FULL;
  for (index = 0; result == IS_AUTH_OK &&
                  index < registry->listener_count; index++) {
    if (registry->listeners[index].context == context)
      result = IS_AUTH_DUPLICATE;
  }
  if (result == IS_AUTH_OK) {
    listener = &registry->listeners[registry->listener_count++];
    strcpy(listener->consumer_id, consumer_id);
    listener->callback = callback;
    listener->context = context;
  }
  (void)pthread_mutex_unlock(&registry->lock);
  return result;
}

void is_auth_registry_unsubscribe(struct is_auth_registry *registry,
                                  void *context)
{
  size_t index;

  if (!registry || !context)
    return;
  (void)pthread_mutex_lock(&registry->lock);
  for (index = 0; index < registry->listener_count; index++) {
    if (registry->listeners[index].context != context)
      continue;
    registry->listener_count--;
    if (index != registry->listener_count)
      registry->listeners[index] =
          registry->listeners[registry->listener_count];
    memset(&registry->listeners[registry->listener_count], 0,
           sizeof(registry->listeners[0]));
    break;
  }
  (void)pthread_mutex_unlock(&registry->lock);
}

enum is_auth_result
is_auth_registry_revoke(struct is_auth_registry *registry,
                        const char *consumer_id)
{
  struct is_auth_credential *credential;
  enum is_auth_result result;

  if (!registry || !consumer_id)
    return IS_AUTH_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&registry->lock);
  credential = find_credential(registry, consumer_id);
  if (!credential) {
    result = IS_AUTH_NOT_FOUND;
  } else {
    credential->revoked = 1;
    credential->authorization_version =
        next_authorization_version(registry);
    notify_revocation(registry, consumer_id);
    result = IS_AUTH_OK;
  }
  (void)pthread_mutex_unlock(&registry->lock);
  return result;
}

enum is_auth_result
is_auth_registry_get_limits(struct is_auth_registry *registry,
                            const char *consumer_id,
                            uint32_t *max_connections,
                            uint32_t *max_opportunistic_chunks,
                            uint32_t *max_committed_chunks)
{
  struct is_auth_credential *credential;
  enum is_auth_result result;

  if (!registry || !consumer_id || !max_connections ||
      !max_opportunistic_chunks || !max_committed_chunks)
    return IS_AUTH_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&registry->lock);
  credential = find_credential(registry, consumer_id);
  if (!credential)
    result = IS_AUTH_NOT_FOUND;
  else if (credential->revoked)
    result = IS_AUTH_REVOKED;
  else {
    *max_connections = credential->max_connections;
    *max_opportunistic_chunks = credential->max_opportunistic_chunks;
    *max_committed_chunks = credential->max_committed_chunks;
    result = IS_AUTH_OK;
  }
  (void)pthread_mutex_unlock(&registry->lock);
  return result;
}

static enum is_auth_result validate_authorization_locked(
    struct is_auth_registry *registry, const char *consumer_id,
    uint64_t authorization_version)
{
  struct is_auth_credential *credential =
      find_credential(registry, consumer_id);

  if (!credential)
    return IS_AUTH_NOT_FOUND;
  if (credential->revoked)
    return IS_AUTH_REVOKED;
  if (credential->authorization_version != authorization_version)
    return IS_AUTH_FAILED;
  return IS_AUTH_OK;
}

enum is_auth_result
is_auth_registry_begin_authorized_operation(
    struct is_auth_registry *registry, const char *consumer_id,
    uint64_t authorization_version)
{
  enum is_auth_result result;

  if (!registry || !consumer_id || authorization_version == 0)
    return IS_AUTH_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&registry->lock);
  result = validate_authorization_locked(
      registry, consumer_id, authorization_version);
  if (result != IS_AUTH_OK)
    (void)pthread_mutex_unlock(&registry->lock);
  return result;
}

void is_auth_registry_end_authorized_operation(
    struct is_auth_registry *registry)
{
  if (registry)
    (void)pthread_mutex_unlock(&registry->lock);
}

enum is_auth_result
is_auth_registry_validate_authorization(
    struct is_auth_registry *registry, const char *consumer_id,
    uint64_t authorization_version)
{
  enum is_auth_result result;

  if (!registry || !consumer_id || authorization_version == 0)
    return IS_AUTH_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&registry->lock);
  result = validate_authorization_locked(
      registry, consumer_id, authorization_version);
  (void)pthread_mutex_unlock(&registry->lock);
  return result;
}

enum is_auth_result
is_auth_registry_is_revoked(struct is_auth_registry *registry,
                            const char *consumer_id)
{
  struct is_auth_credential *credential;
  enum is_auth_result result;

  if (!registry || !consumer_id)
    return IS_AUTH_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&registry->lock);
  credential = find_credential(registry, consumer_id);
  if (!credential)
    result = IS_AUTH_NOT_FOUND;
  else if (credential->revoked)
    result = IS_AUTH_REVOKED;
  else
    result = IS_AUTH_OK;
  (void)pthread_mutex_unlock(&registry->lock);
  return result;
}

enum is_auth_result
is_auth_compute_tag(const uint8_t *secret, size_t secret_size,
                    const uint8_t *hello_frame, size_t hello_size,
                    const uint8_t *challenge_frame, size_t challenge_size,
                    uint8_t tag[IS_PROTOCOL_AUTH_TAG_SIZE])
{
  EVP_MAC *mac = NULL;
  EVP_MAC_CTX *context = NULL;
  OSSL_PARAM parameters[2];
  size_t output_size = 0;
  enum is_auth_result result = IS_AUTH_CRYPTO_ERROR;

  if (!secret || secret_size < IS_AUTH_SECRET_MIN_SIZE ||
      secret_size > IS_AUTH_SECRET_MAX_SIZE || !hello_frame ||
      hello_size == 0 || hello_size > IS_PROTOCOL_MAX_FRAME_SIZE ||
      !challenge_frame || challenge_size == 0 ||
      challenge_size > IS_PROTOCOL_MAX_FRAME_SIZE || !tag)
    return IS_AUTH_INVALID_ARGUMENT;

  mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
  if (!mac)
    goto out;
  context = EVP_MAC_CTX_new(mac);
  if (!context)
    goto out;
  parameters[0] = OSSL_PARAM_construct_utf8_string(
      OSSL_MAC_PARAM_DIGEST, (char *)"SHA256", 0);
  parameters[1] = OSSL_PARAM_construct_end();
  if (EVP_MAC_init(context, secret, secret_size, parameters) != 1 ||
      EVP_MAC_update(context, auth_domain, sizeof(auth_domain) - 1U) != 1 ||
      EVP_MAC_update(context, hello_frame, hello_size) != 1 ||
      EVP_MAC_update(context, challenge_frame, challenge_size) != 1 ||
      EVP_MAC_final(context, tag, &output_size,
                    IS_PROTOCOL_AUTH_TAG_SIZE) != 1 ||
      output_size != IS_PROTOCOL_AUTH_TAG_SIZE)
    goto out;
  result = IS_AUTH_OK;
out:
  EVP_MAC_CTX_free(context);
  EVP_MAC_free(mac);
  return result;
}

enum is_auth_result
is_auth_registry_verify(struct is_auth_registry *registry,
                        const char *consumer_id, const char *key_id,
                        const uint8_t *hello_frame, size_t hello_size,
                        const uint8_t *challenge_frame,
                        size_t challenge_size,
                        const uint8_t tag[IS_PROTOCOL_AUTH_TAG_SIZE],
                        uint64_t *authenticated_until_unix,
                        uint64_t *authorization_version)
{
  struct is_auth_credential *credential;
  const struct is_auth_key *key = NULL;
  time_t now;
  uint8_t expected[IS_PROTOCOL_AUTH_TAG_SIZE];
  enum is_auth_result result;

  if (!registry || !consumer_id || !key_id || !tag ||
      !authenticated_until_unix || !authorization_version)
    return IS_AUTH_INVALID_ARGUMENT;
  *authenticated_until_unix = 0;
  *authorization_version = 0;
  (void)pthread_mutex_lock(&registry->lock);
  credential = find_credential(registry, consumer_id);
  if (!credential) {
    result = IS_AUTH_NOT_FOUND;
    goto out;
  }
  if (credential->revoked) {
    result = IS_AUTH_REVOKED;
    goto out;
  }
  if (strcmp(credential->current.key_id, key_id) == 0) {
    key = &credential->current;
  } else {
    now = time(NULL);
    if (now >= 0 && credential->next.secret_size != 0 &&
        credential->next_valid_until_unix > (uint64_t)now &&
        strcmp(credential->next.key_id, key_id) == 0) {
      key = &credential->next;
      *authenticated_until_unix = credential->next_valid_until_unix;
    }
  }
  if (!key) {
    result = IS_AUTH_FAILED;
    goto out;
  }
  result = is_auth_compute_tag(key->secret, key->secret_size,
                               hello_frame, hello_size,
                               challenge_frame, challenge_size, expected);
  if (result != IS_AUTH_OK)
    goto out;
  if (CRYPTO_memcmp(expected, tag, sizeof(expected)) != 0) {
    result = IS_AUTH_FAILED;
    goto out;
  }
  *authorization_version = credential->authorization_version;
  result = IS_AUTH_OK;
out:
  if (result != IS_AUTH_OK) {
    *authenticated_until_unix = 0;
    *authorization_version = 0;
  }
  OPENSSL_cleanse(expected, sizeof(expected));
  (void)pthread_mutex_unlock(&registry->lock);
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

static int hex_value(char value)
{
  if (value >= '0' && value <= '9')
    return value - '0';
  if (value >= 'a' && value <= 'f')
    return value - 'a' + 10;
  if (value >= 'A' && value <= 'F')
    return value - 'A' + 10;
  return -1;
}

static int decode_secret(const char *hex, struct is_auth_key *key)
{
  size_t hex_size = strlen(hex);
  size_t index;

  if (hex_size < IS_AUTH_SECRET_MIN_SIZE * 2U ||
      hex_size > IS_AUTH_SECRET_MAX_SIZE * 2U || (hex_size & 1U) != 0)
    return 0;
  key->secret_size = hex_size / 2U;
  for (index = 0; index < key->secret_size; index++) {
    int high = hex_value(hex[index * 2U]);
    int low = hex_value(hex[index * 2U + 1U]);

    if (high < 0 || low < 0) {
      OPENSSL_cleanse(key->secret, sizeof(key->secret));
      key->secret_size = 0;
      return 0;
    }
    key->secret[index] = (uint8_t)((high << 4) | low);
  }
  return 1;
}

static int parse_u64(const char *value, uint64_t *parsed)
{
  char *end = NULL;
  unsigned long long number;

  if (!value[0] || value[0] == '-' || value[0] == '+')
    return 0;
  errno = 0;
  number = strtoull(value, &end, 10);
  if (errno != 0 || !end || *end != '\0')
    return 0;
  *parsed = (uint64_t)number;
  return 1;
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

static int append_credential(struct is_auth_credential entries[],
                             size_t *count,
                             const struct is_auth_credential *credential)
{
  size_t index;

  if (!credential_valid(credential) || *count == IS_AUTH_MAX_CONSUMERS)
    return 0;
  for (index = 0; index < *count; index++) {
    if (strcmp(entries[index].consumer_id, credential->consumer_id) == 0)
      return 0;
  }
  entries[(*count)++] = *credential;
  return 1;
}

static int start_consumer_section(const char *line,
                                  struct is_auth_credential *credential)
{
  static const char prefix[] = "[consumer:";
  size_t line_size = strlen(line);
  size_t id_size;

  if (strncmp(line, prefix, sizeof(prefix) - 1U) != 0 ||
      line_size <= sizeof(prefix) || line[line_size - 1U] != ']')
    return 0;
  id_size = line_size - (sizeof(prefix) - 1U) - 1U;
  if (id_size == 0 || id_size > IS_PROTOCOL_CONSUMER_ID_MAX)
    return 0;
  memset(credential, 0, sizeof(*credential));
  memcpy(credential->consumer_id, line + sizeof(prefix) - 1U, id_size);
  return identifier_valid(credential->consumer_id,
                          IS_PROTOCOL_CONSUMER_ID_MAX);
}

static int parse_credential_value(struct is_auth_credential *credential,
                                  uint32_t *seen, const char *key,
                                  const char *value)
{
  uint32_t field;

  if (strcmp(key, "current_key_id") == 0)
    field = 1U << 0;
  else if (strcmp(key, "current_psk_hex") == 0)
    field = 1U << 1;
  else if (strcmp(key, "next_key_id") == 0)
    field = 1U << 2;
  else if (strcmp(key, "next_psk_hex") == 0)
    field = 1U << 3;
  else if (strcmp(key, "next_valid_until_unix") == 0)
    field = 1U << 4;
  else if (strcmp(key, "max_connections") == 0)
    field = 1U << 5;
  else if (strcmp(key, "max_opportunistic_gib") == 0 ||
           strcmp(key, "max_opportunistic_chunks") == 0)
    field = 1U << 6;
  else if (strcmp(key, "max_committed_gib") == 0 ||
           strcmp(key, "max_committed_chunks") == 0)
    field = 1U << 7;
  else if (strcmp(key, "revoked") == 0)
    field = 1U << 8;
  else
    return 0;
  if (*seen & field)
    return 0;
  *seen |= field;

  if (field == (1U << 0)) {
    if (strlen(value) > IS_PROTOCOL_KEY_ID_MAX)
      return 0;
    strcpy(credential->current.key_id, value);
    return 1;
  }
  if (field == (1U << 1))
    return decode_secret(value, &credential->current);
  if (field == (1U << 2)) {
    if (strlen(value) > IS_PROTOCOL_KEY_ID_MAX)
      return 0;
    strcpy(credential->next.key_id, value);
    return 1;
  }
  if (field == (1U << 3))
    return decode_secret(value, &credential->next);
  if (field == (1U << 4))
    return parse_u64(value, &credential->next_valid_until_unix);
  if (field == (1U << 5))
    return parse_u32(value, &credential->max_connections);
  if (field == (1U << 6))
    return parse_u32(value, &credential->max_opportunistic_chunks);
  if (field == (1U << 7))
    return parse_u32(value, &credential->max_committed_chunks);
  if (strcmp(value, "true") == 0)
    credential->revoked = 1;
  else if (strcmp(value, "false") == 0)
    credential->revoked = 0;
  else
    return 0;
  return 1;
}

static const struct is_auth_credential *find_entry(
    const struct is_auth_credential entries[], size_t count,
    const char *consumer_id)
{
  size_t index;

  for (index = 0; index < count; index++) {
    if (strcmp(entries[index].consumer_id, consumer_id) == 0)
      return &entries[index];
  }
  return NULL;
}

static int key_equal(const struct is_auth_key *left,
                     const struct is_auth_key *right)
{
  return left->secret_size == right->secret_size &&
         strcmp(left->key_id, right->key_id) == 0 &&
         CRYPTO_memcmp(left->secret, right->secret,
                       left->secret_size) == 0;
}

static int authorization_equal(const struct is_auth_credential *left,
                               const struct is_auth_credential *right)
{
  return left && right && !right->revoked &&
         key_equal(&left->current, &right->current) &&
         key_equal(&left->next, &right->next) &&
         left->next_valid_until_unix == right->next_valid_until_unix &&
         left->max_connections == right->max_connections &&
         left->max_opportunistic_chunks == right->max_opportunistic_chunks &&
         left->max_committed_chunks == right->max_committed_chunks;
}

enum is_auth_result
is_auth_registry_load_file(struct is_auth_registry *registry,
                           const char *path)
{
  struct is_auth_credential entries[IS_AUTH_MAX_CONSUMERS];
  struct is_auth_credential current;
  struct stat metadata;
  FILE *file = NULL;
  char line_buffer[1024];
  size_t count = 0;
  size_t entry_count;
  size_t entry_index;
  size_t listener_index;
  int descriptor = -1;
  int in_consumer = 0;
  int version_seen = 0;
  uint32_t fields_seen = 0;
  uint8_t notify_listener[IS_AUTH_MAX_CONSUMERS] = {0};
  enum is_auth_result result = IS_AUTH_FORMAT_ERROR;

  if (!registry || !path)
    return IS_AUTH_INVALID_ARGUMENT;
  memset(entries, 0, sizeof(entries));
  memset(&current, 0, sizeof(current));
  descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0 || fstat(descriptor, &metadata) != 0 ||
      !S_ISREG(metadata.st_mode) || metadata.st_uid != geteuid() ||
      (metadata.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    result = IS_AUTH_IO_ERROR;
    goto out;
  }
  file = fdopen(descriptor, "r");
  if (!file) {
    result = IS_AUTH_IO_ERROR;
    goto out;
  }
  descriptor = -1;

  while (fgets(line_buffer, sizeof(line_buffer), file)) {
    char *line = trim(line_buffer);
    char *separator;
    char *key;
    char *value;

    if (!line[0] || line[0] == '#' || line[0] == ';')
      continue;
    if (line[0] == '[') {
      if (in_consumer && !append_credential(entries, &count, &current))
        goto out;
      if (!start_consumer_section(line, &current))
        goto out;
      fields_seen = 0;
      in_consumer = 1;
      continue;
    }
    separator = strchr(line, '=');
    if (!separator)
      goto out;
    *separator = '\0';
    key = trim(line);
    value = trim(separator + 1);
    if (!in_consumer) {
      if (strcmp(key, "version") != 0 || strcmp(value, "1") != 0 ||
          version_seen)
        goto out;
      version_seen = 1;
    } else if (!parse_credential_value(&current, &fields_seen, key, value)) {
      goto out;
    }
  }
  if (ferror(file) || !version_seen)
    goto out;
  if (in_consumer && !append_credential(entries, &count, &current))
    goto out;

  entry_count = count;
  (void)pthread_mutex_lock(&registry->lock);
  for (listener_index = 0;
       listener_index < registry->listener_count; listener_index++) {
    struct is_auth_listener *listener =
        &registry->listeners[listener_index];
    const struct is_auth_credential *old_credential =
        find_credential(registry, listener->consumer_id);
    const struct is_auth_credential *new_credential =
        find_entry(entries, entry_count, listener->consumer_id);

    notify_listener[listener_index] =
        !authorization_equal(old_credential, new_credential);
  }
  for (entry_index = 0; entry_index < entry_count; entry_index++) {
    const struct is_auth_credential *old_credential =
        find_credential(registry, entries[entry_index].consumer_id);

    if (authorization_equal(old_credential, &entries[entry_index]))
      entries[entry_index].authorization_version =
          old_credential->authorization_version;
    else
      entries[entry_index].authorization_version =
          next_authorization_version(registry);
  }
  OPENSSL_cleanse(registry->entries, sizeof(registry->entries));
  memcpy(registry->entries, entries, entry_count * sizeof(entries[0]));
  registry->count = entry_count;
  for (listener_index = 0;
       listener_index < registry->listener_count; listener_index++) {
    if (notify_listener[listener_index])
      registry->listeners[listener_index].callback(
          registry->listeners[listener_index].context);
  }
  (void)pthread_mutex_unlock(&registry->lock);
  result = IS_AUTH_OK;
out:
  if (file)
    (void)fclose(file);
  else if (descriptor >= 0)
    (void)close(descriptor);
  OPENSSL_cleanse(&current, sizeof(current));
  OPENSSL_cleanse(entries, sizeof(entries));
  return result;
}

const char *is_auth_result_name(enum is_auth_result result)
{
  static const char *const names[] = {
    "ok", "invalid argument", "registry full", "duplicate identity",
    "identity not found", "identity revoked", "authentication failed",
    "cryptographic failure", "configuration I/O failure",
    "configuration format failure"
  };

  if ((unsigned int)result >= sizeof(names) / sizeof(names[0]))
    return "unknown authentication result";
  return names[result];
}
