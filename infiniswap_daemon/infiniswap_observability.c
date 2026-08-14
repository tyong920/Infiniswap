/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#include "infiniswap_observability.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct output_buffer {
  char *data;
  size_t capacity;
  size_t length;
  int failed;
};

static void append(struct output_buffer *buffer, const char *format, ...)
{
  va_list arguments;
  int written;

  if (buffer->failed)
    return;
  va_start(arguments, format);
  written = vsnprintf(buffer->data + buffer->length,
                      buffer->capacity - buffer->length, format, arguments);
  va_end(arguments);
  if (written < 0 || (size_t)written >= buffer->capacity - buffer->length) {
    buffer->failed = 1;
    return;
  }
  buffer->length += (size_t)written;
}

static int identifier_valid(const char *value)
{
  size_t index;

  if (!value || !value[0])
    return 0;
  for (index = 0; index <= IS_PROVIDER_OBSERVABILITY_ID_MAX; index++) {
    char character = value[index];

    if (character == '\0')
      return 1;
    if (!((character >= 'a' && character <= 'z') ||
          (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9') || character == '.' ||
          character == '_' || character == '-'))
      return 0;
  }
  return 0;
}

static const char *consumer_state_name(enum is_provider_consumer_state state)
{
  switch (state) {
  case IS_PROVIDER_CONSUMER_CONNECTING:
    return "connecting";
  case IS_PROVIDER_CONSUMER_READY:
    return "ready";
  case IS_PROVIDER_CONSUMER_DEGRADED:
    return "degraded";
  case IS_PROVIDER_CONSUMER_CLOSED:
    return "closed";
  default:
    return "unknown";
  }
}

static const char *mode_name(uint8_t mode)
{
  if (mode == IS_PROTOCOL_MODE_BACKED)
    return "backed";
  if (mode == IS_PROTOCOL_MODE_REMOTE_ONLY)
    return "remote-only";
  return "unknown";
}

static const char *pool_name(uint8_t pool)
{
  if (pool == IS_PROTOCOL_POOL_OPPORTUNISTIC)
    return "opportunistic";
  if (pool == IS_PROTOCOL_POOL_COMMITTED)
    return "committed";
  return "unknown";
}

static int snapshot_valid(
    const struct is_provider_observability_snapshot *snapshot)
{
  size_t index;

  if (!snapshot || !identifier_valid(snapshot->provider_id) ||
      snapshot->consumer_count > IS_PROVIDER_OBSERVABILITY_MAX_CONSUMERS)
    return 0;
  for (index = 0; index < snapshot->consumer_count; index++) {
    if (!identifier_valid(snapshot->consumers[index].consumer_id))
      return 0;
  }
  return 1;
}

static void append_pool_json(struct output_buffer *buffer, const char *name,
                             uint32_t maximum, uint32_t allocated,
                             uint32_t assigned, uint32_t available)
{
  append(buffer,
         "{\"name\":\"%s\",\"max_chunks\":%u,\"allocated_chunks\":%u,"
         "\"assigned_chunks\":%u,\"available_chunks\":%u}",
         name, maximum, allocated, assigned, available);
}

enum is_observability_result is_provider_observability_render_status(
    const struct is_provider_observability_snapshot *snapshot, char *output,
    size_t output_size)
{
  struct output_buffer buffer = {output, output_size, 0, 0};
  size_t index;

  if (!output || output_size == 0 || !snapshot_valid(snapshot))
    return IS_OBSERVABILITY_INVALID;
  output[0] = '\0';
  append(&buffer,
         "{\"schema_version\":1,\"kind\":\"infiniswap.provider-status\","
         "\"provider\":{\"provider_id\":\"%s\",\"state\":\"%s\","
         "\"active_connections\":%u},\"pools\":[",
         snapshot->provider_id, snapshot->healthy ? "healthy" : "degraded",
         snapshot->active_connections);
  append_pool_json(&buffer, "opportunistic",
                   snapshot->max_opportunistic_chunks,
                   snapshot->allocated_opportunistic_chunks,
                   snapshot->assigned_opportunistic_chunks,
                   snapshot->available_opportunistic_chunks);
  append(&buffer, ",");
  append_pool_json(&buffer, "committed", snapshot->max_committed_chunks,
                   snapshot->allocated_committed_chunks,
                   snapshot->assigned_committed_chunks,
                   snapshot->available_committed_chunks);
  append(&buffer,
         "],\"chunks\":{\"size_bytes\":%" PRIu64
         ",\"host_reserve_chunks\":%u,\"allocated_chunks\":%u,"
         "\"assigned_chunks\":%u,\"quarantined_chunks\":%u},"
         "\"consumers\":[",
         IS_MEMORY_CHUNK_BYTES, snapshot->host_reserve_chunks,
         snapshot->allocated_opportunistic_chunks +
             snapshot->allocated_committed_chunks,
         snapshot->assigned_opportunistic_chunks +
             snapshot->assigned_committed_chunks,
         snapshot->quarantined_chunks);
  for (index = 0; index < snapshot->consumer_count; index++) {
    const struct is_provider_consumer_snapshot *consumer =
        &snapshot->consumers[index];

    append(&buffer,
           "%s{\"consumer_id\":\"%s\",\"state\":\"%s\","
           "\"mode\":\"%s\",\"pool\":\"%s\","
           "\"failure_deadline_ms\":%u,\"connection_count\":%u,"
           "\"assigned_opportunistic_chunks\":%u,"
           "\"assigned_committed_chunks\":%u,"
           "\"inflight_control_requests\":%u,\"last_error_code\":%u}",
           index ? "," : "", consumer->consumer_id,
           consumer_state_name(consumer->state), mode_name(consumer->mode),
           pool_name(consumer->pool), consumer->failure_deadline_ms,
           consumer->connection_count,
           consumer->assigned_opportunistic_chunks,
           consumer->assigned_committed_chunks,
           consumer->inflight_control_requests, consumer->last_error_code);
  }
  append(&buffer,
         "],\"metrics\":{\"admissions_total\":%" PRIu64
         ",\"admission_rejections_total\":%" PRIu64
         ",\"pressure_reclaims_total\":%" PRIu64
         ",\"authentication_failures_total\":%" PRIu64
         ",\"deadline_expiries_total\":%" PRIu64
         ",\"connections_total\":%" PRIu64
         ",\"disconnections_total\":%" PRIu64
         ",\"control_errors_total\":%" PRIu64 "},",
         snapshot->admissions_total, snapshot->admission_rejections_total,
         snapshot->pressure_reclaims_total,
         snapshot->authentication_failures_total,
         snapshot->deadline_expiries_total, snapshot->connections_total,
         snapshot->disconnections_total, snapshot->control_errors_total);
  if (snapshot->last_rejection == IS_MEMORY_REJECTION_NONE)
    append(&buffer, "\"last_error\":null}\n");
  else
    append(&buffer, "\"last_error\":{\"code\":\"%s\"}}\n",
           is_memory_rejection_name(snapshot->last_rejection));
  return buffer.failed ? IS_OBSERVABILITY_NO_SPACE : IS_OBSERVABILITY_OK;
}

static void append_gauge(struct output_buffer *buffer, const char *name,
                         const char *labels, uint64_t value)
{
  append(buffer, "%s%s %" PRIu64 "\n", name, labels, value);
}

static void append_counter(struct output_buffer *buffer, const char *name,
                           const char *provider, uint64_t value)
{
  append(buffer, "# TYPE %s counter\n%s{provider=\"%s\"} %" PRIu64 "\n",
         name, name, provider, value);
}

enum is_observability_result is_provider_observability_render_metrics(
    const struct is_provider_observability_snapshot *snapshot, char *output,
    size_t output_size)
{
  struct output_buffer buffer = {output, output_size, 0, 0};
  char labels[256];
  size_t index;

  if (!output || output_size == 0 || !snapshot_valid(snapshot))
    return IS_OBSERVABILITY_INVALID;
  output[0] = '\0';
  append(&buffer,
         "# infiniswap_metrics_schema_version 1\n"
         "# TYPE infiniswap_provider_healthy gauge\n"
         "# TYPE infiniswap_provider_active_connections gauge\n"
         "# TYPE infiniswap_provider_pool_max_chunks gauge\n"
         "# TYPE infiniswap_provider_pool_allocated_chunks gauge\n"
         "# TYPE infiniswap_provider_pool_assigned_chunks gauge\n"
         "# TYPE infiniswap_provider_pool_available_chunks gauge\n"
         "# TYPE infiniswap_provider_quarantined_chunks gauge\n"
         "# TYPE infiniswap_provider_host_reserve_chunks gauge\n"
         "# TYPE infiniswap_provider_consumer_connected gauge\n"
         "# TYPE infiniswap_provider_consumer_failure_deadline_milliseconds gauge\n"
         "# TYPE infiniswap_provider_consumer_inflight_control_requests gauge\n"
         "# TYPE infiniswap_provider_consumer_assigned_chunks gauge\n");
  snprintf(labels, sizeof(labels), "{provider=\"%s\"}",
           snapshot->provider_id);
  append_gauge(&buffer, "infiniswap_provider_healthy", labels,
               snapshot->healthy ? 1U : 0U);
  append_gauge(&buffer, "infiniswap_provider_active_connections", labels,
               snapshot->active_connections);
  snprintf(labels, sizeof(labels),
           "{provider=\"%s\",pool=\"opportunistic\"}",
           snapshot->provider_id);
  append_gauge(&buffer, "infiniswap_provider_pool_max_chunks", labels,
               snapshot->max_opportunistic_chunks);
  append_gauge(&buffer, "infiniswap_provider_pool_allocated_chunks", labels,
               snapshot->allocated_opportunistic_chunks);
  append_gauge(&buffer, "infiniswap_provider_pool_assigned_chunks", labels,
               snapshot->assigned_opportunistic_chunks);
  append_gauge(&buffer, "infiniswap_provider_pool_available_chunks", labels,
               snapshot->available_opportunistic_chunks);
  snprintf(labels, sizeof(labels),
           "{provider=\"%s\",pool=\"committed\"}", snapshot->provider_id);
  append_gauge(&buffer, "infiniswap_provider_pool_max_chunks", labels,
               snapshot->max_committed_chunks);
  append_gauge(&buffer, "infiniswap_provider_pool_allocated_chunks", labels,
               snapshot->allocated_committed_chunks);
  append_gauge(&buffer, "infiniswap_provider_pool_assigned_chunks", labels,
               snapshot->assigned_committed_chunks);
  append_gauge(&buffer, "infiniswap_provider_pool_available_chunks", labels,
               snapshot->available_committed_chunks);
  snprintf(labels, sizeof(labels), "{provider=\"%s\"}",
           snapshot->provider_id);
  append_gauge(&buffer, "infiniswap_provider_quarantined_chunks", labels,
               snapshot->quarantined_chunks);
  append_gauge(&buffer, "infiniswap_provider_host_reserve_chunks", labels,
               snapshot->host_reserve_chunks);
  for (index = 0; index < snapshot->consumer_count; index++) {
    const struct is_provider_consumer_snapshot *consumer =
        &snapshot->consumers[index];

    snprintf(labels, sizeof(labels),
             "{provider=\"%s\",consumer=\"%s\"}", snapshot->provider_id,
             consumer->consumer_id);
    append_gauge(&buffer, "infiniswap_provider_consumer_connected", labels,
                 consumer->state == IS_PROVIDER_CONSUMER_READY ? 1U : 0U);
    append_gauge(&buffer,
                 "infiniswap_provider_consumer_failure_deadline_milliseconds",
                 labels, consumer->failure_deadline_ms);
    append_gauge(&buffer, "infiniswap_provider_consumer_inflight_control_requests",
                 labels, consumer->inflight_control_requests);
    snprintf(labels, sizeof(labels),
             "{provider=\"%s\",consumer=\"%s\",pool=\"opportunistic\"}",
             snapshot->provider_id, consumer->consumer_id);
    append_gauge(&buffer, "infiniswap_provider_consumer_assigned_chunks", labels,
                 consumer->assigned_opportunistic_chunks);
    snprintf(labels, sizeof(labels),
             "{provider=\"%s\",consumer=\"%s\",pool=\"committed\"}",
             snapshot->provider_id, consumer->consumer_id);
    append_gauge(&buffer, "infiniswap_provider_consumer_assigned_chunks", labels,
                 consumer->assigned_committed_chunks);
  }
  append_counter(&buffer, "infiniswap_provider_admissions_total",
                 snapshot->provider_id, snapshot->admissions_total);
  append_counter(&buffer, "infiniswap_provider_admission_rejections_total",
                 snapshot->provider_id, snapshot->admission_rejections_total);
  append_counter(&buffer, "infiniswap_provider_pressure_reclaims_total",
                 snapshot->provider_id, snapshot->pressure_reclaims_total);
  append_counter(&buffer, "infiniswap_provider_authentication_failures_total",
                 snapshot->provider_id, snapshot->authentication_failures_total);
  append_counter(&buffer, "infiniswap_provider_deadline_expiries_total",
                 snapshot->provider_id, snapshot->deadline_expiries_total);
  append_counter(&buffer, "infiniswap_provider_connections_total",
                 snapshot->provider_id, snapshot->connections_total);
  append_counter(&buffer, "infiniswap_provider_disconnections_total",
                 snapshot->provider_id, snapshot->disconnections_total);
  append_counter(&buffer, "infiniswap_provider_control_errors_total",
                 snapshot->provider_id, snapshot->control_errors_total);
  append(&buffer, "# EOF\n");
  return buffer.failed ? IS_OBSERVABILITY_NO_SPACE : IS_OBSERVABILITY_OK;
}
