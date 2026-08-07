/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#include "infiniswap_consumer_liveness.h"

#include <stdint.h>

void is_consumer_liveness_init(
    struct is_consumer_liveness *liveness, uint32_t failure_deadline_ms,
    uint64_t authenticated_until_unix, uint64_t now_monotonic_ms)
{
  liveness->last_activity_monotonic_ms = now_monotonic_ms;
  liveness->authenticated_until_unix = authenticated_until_unix;
  liveness->failure_deadline_ms = failure_deadline_ms;
}

void is_consumer_liveness_refresh(
    struct is_consumer_liveness *liveness, uint64_t now_monotonic_ms)
{
  if (now_monotonic_ms > liveness->last_activity_monotonic_ms)
    liveness->last_activity_monotonic_ms = now_monotonic_ms;
}

enum is_consumer_liveness_result is_consumer_liveness_check(
    const struct is_consumer_liveness *liveness, uint64_t now_monotonic_ms,
    uint64_t now_unix, uint64_t *wait_ms)
{
  uint64_t elapsed_ms;
  uint64_t remaining_ms;

  if (liveness->authenticated_until_unix != 0 &&
      now_unix >= liveness->authenticated_until_unix)
    return IS_CONSUMER_LIVENESS_AUTH_EXPIRED;
  if (now_monotonic_ms < liveness->last_activity_monotonic_ms)
    return IS_CONSUMER_LIVENESS_SILENT;
  elapsed_ms = now_monotonic_ms - liveness->last_activity_monotonic_ms;
  if (elapsed_ms >= liveness->failure_deadline_ms)
    return IS_CONSUMER_LIVENESS_SILENT;

  remaining_ms = liveness->failure_deadline_ms - elapsed_ms;
  if (liveness->authenticated_until_unix != 0) {
    uint64_t remaining_auth_seconds =
        liveness->authenticated_until_unix - now_unix;
    uint64_t remaining_auth_ms =
        remaining_auth_seconds > UINT64_MAX / UINT64_C(1000)
            ? UINT64_MAX
            : remaining_auth_seconds * UINT64_C(1000);

    if (remaining_auth_ms < remaining_ms)
      remaining_ms = remaining_auth_ms;
  }
  *wait_ms = remaining_ms;
  return IS_CONSUMER_LIVENESS_ACTIVE;
}
