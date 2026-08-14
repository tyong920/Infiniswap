/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#ifndef INFINISWAP_OBSERVABILITY_H
#define INFINISWAP_OBSERVABILITY_H

#include "infiniswap_memory_manager.h"
#include "infiniswap_protocol.h"

#include <stddef.h>
#include <stdint.h>

#define IS_PROVIDER_OBSERVABILITY_MAX_CONSUMERS 64U
#define IS_PROVIDER_OBSERVABILITY_ID_MAX 63U

enum is_observability_result {
  IS_OBSERVABILITY_OK = 0,
  IS_OBSERVABILITY_INVALID,
  IS_OBSERVABILITY_NO_SPACE
};

enum is_provider_consumer_state {
  IS_PROVIDER_CONSUMER_CONNECTING = 0,
  IS_PROVIDER_CONSUMER_READY,
  IS_PROVIDER_CONSUMER_DEGRADED,
  IS_PROVIDER_CONSUMER_CLOSED
};

struct is_provider_consumer_snapshot {
  char consumer_id[IS_PROVIDER_OBSERVABILITY_ID_MAX + 1U];
  enum is_provider_consumer_state state;
  uint8_t mode;
  uint8_t pool;
  uint32_t failure_deadline_ms;
  uint32_t connection_count;
  uint32_t assigned_opportunistic_chunks;
  uint32_t assigned_committed_chunks;
  uint32_t inflight_control_requests;
  uint16_t last_error_code;
};

struct is_provider_observability_snapshot {
  char provider_id[IS_PROVIDER_OBSERVABILITY_ID_MAX + 1U];
  int healthy;
  uint32_t host_reserve_chunks;
  uint32_t max_opportunistic_chunks;
  uint32_t max_committed_chunks;
  uint32_t allocated_opportunistic_chunks;
  uint32_t assigned_opportunistic_chunks;
  uint32_t allocated_committed_chunks;
  uint32_t assigned_committed_chunks;
  uint32_t available_opportunistic_chunks;
  uint32_t available_committed_chunks;
  uint32_t quarantined_chunks;
  uint32_t active_connections;
  uint64_t admissions_total;
  uint64_t admission_rejections_total;
  uint64_t pressure_reclaims_total;
  uint64_t authentication_failures_total;
  uint64_t deadline_expiries_total;
  uint64_t connections_total;
  uint64_t disconnections_total;
  uint64_t control_errors_total;
  enum is_memory_rejection_reason last_rejection;
  size_t consumer_count;
  struct is_provider_consumer_snapshot
      consumers[IS_PROVIDER_OBSERVABILITY_MAX_CONSUMERS];
};

enum is_observability_result is_provider_observability_render_status(
    const struct is_provider_observability_snapshot *snapshot, char *output,
    size_t output_size);

enum is_observability_result is_provider_observability_render_metrics(
    const struct is_provider_observability_snapshot *snapshot, char *output,
    size_t output_size);

#endif /* INFINISWAP_OBSERVABILITY_H */
