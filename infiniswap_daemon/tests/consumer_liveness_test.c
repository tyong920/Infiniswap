#include "infiniswap_consumer_liveness.h"

#include <stdint.h>
#include <stdio.h>

static int test_silent_consumer_expires_at_failure_deadline(void)
{
  struct is_consumer_liveness liveness;
  uint64_t wait_ms = 0;

  is_consumer_liveness_init(&liveness, 2000, 0, 10000);
  if (is_consumer_liveness_check(&liveness, 11999, 500, &wait_ms) !=
          IS_CONSUMER_LIVENESS_ACTIVE ||
      wait_ms != 1 ||
      is_consumer_liveness_check(&liveness, 12000, 500, &wait_ms) !=
          IS_CONSUMER_LIVENESS_SILENT) {
    fprintf(stderr, "silent Consumer did not expire at its failure deadline\n");
    return 1;
  }
  return 0;
}

static int test_authenticated_traffic_refreshes_failure_deadline(void)
{
  struct is_consumer_liveness liveness;
  uint64_t wait_ms = 0;

  is_consumer_liveness_init(&liveness, 2000, 0, 10000);
  is_consumer_liveness_refresh(&liveness, 11500);
  if (is_consumer_liveness_check(&liveness, 12000, 500, &wait_ms) !=
          IS_CONSUMER_LIVENESS_ACTIVE ||
      wait_ms != 1500 ||
      is_consumer_liveness_check(&liveness, 13500, 500, &wait_ms) !=
          IS_CONSUMER_LIVENESS_SILENT) {
    fprintf(stderr, "authenticated traffic did not refresh Consumer liveness\n");
    return 1;
  }
  return 0;
}

static int test_authorization_expiry_wins_when_earlier(void)
{
  struct is_consumer_liveness liveness;
  uint64_t wait_ms = 0;

  is_consumer_liveness_init(&liveness, 2000, 501, 10000);
  if (is_consumer_liveness_check(&liveness, 10000, 500, &wait_ms) !=
          IS_CONSUMER_LIVENESS_ACTIVE ||
      wait_ms != 1000 ||
      is_consumer_liveness_check(&liveness, 11000, 501, &wait_ms) !=
          IS_CONSUMER_LIVENESS_AUTH_EXPIRED) {
    fprintf(stderr, "authorization expiry was not enforced first\n");
    return 1;
  }
  return 0;
}

int main(void)
{
  return test_silent_consumer_expires_at_failure_deadline() == 0 &&
                 test_authenticated_traffic_refreshes_failure_deadline() == 0 &&
                 test_authorization_expiry_wins_when_earlier() == 0
             ? 0
             : 1;
}
