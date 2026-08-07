/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#ifndef INFINISWAP_CONSUMER_LIVENESS_H
#define INFINISWAP_CONSUMER_LIVENESS_H

#include <stdint.h>

enum is_consumer_liveness_result {
  IS_CONSUMER_LIVENESS_ACTIVE = 0,
  IS_CONSUMER_LIVENESS_SILENT,
  IS_CONSUMER_LIVENESS_AUTH_EXPIRED
};

struct is_consumer_liveness {
  uint64_t last_activity_monotonic_ms;
  uint64_t authenticated_until_unix;
  uint32_t failure_deadline_ms;
};

void is_consumer_liveness_init(
    struct is_consumer_liveness *liveness, uint32_t failure_deadline_ms,
    uint64_t authenticated_until_unix, uint64_t now_monotonic_ms);

void is_consumer_liveness_refresh(
    struct is_consumer_liveness *liveness, uint64_t now_monotonic_ms);

enum is_consumer_liveness_result is_consumer_liveness_check(
    const struct is_consumer_liveness *liveness, uint64_t now_monotonic_ms,
    uint64_t now_unix, uint64_t *wait_ms);

#endif /* INFINISWAP_CONSUMER_LIVENESS_H */
