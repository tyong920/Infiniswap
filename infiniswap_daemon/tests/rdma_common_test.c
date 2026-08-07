#include "rdma-common.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

volatile sig_atomic_t running;

static int test_connection_params(void)
{
  struct rdma_conn_param params;

  memset(&params, 0xa5, sizeof(params));
  build_params(&params);

  if (params.initiator_depth != 1 || params.responder_resources != 1 ||
      params.rnr_retry_count != 6) {
    fprintf(stderr, "build_params did not initialize the RDMA connection policy\n");
    return 1;
  }

  return 0;
}

static int test_remote_only_provider_capabilities(void)
{
  uint64_t required = IS_PROTOCOL_CAP_REMOTE_ONLY |
                      IS_PROTOCOL_CAP_COMMITTED_POOL |
                      IS_PROTOCOL_CAP_FAILURE_DEADLINE |
                      IS_PROTOCOL_CAP_STATUS |
                      IS_PROTOCOL_CAP_AUTH_HMAC_SHA256;

  if ((IS_PROVIDER_CAPABILITIES & required) != required) {
    fprintf(stderr, "Provider does not advertise Remote-Only admission\n");
    return 1;
  }
  return 0;
}

static int test_exact_control_response_correlation(void)
{
  uint8_t expected[MAX_MR_SIZE_GB] = {0};
  uint32_t exact[] = {1, 3};
  uint32_t partial[] = {1};
  uint32_t duplicate[] = {1, 1};
  uint32_t unexpected[] = {1, 4};

  expected[1] = 1;
  expected[3] = 1;
  if (!control_chunk_set_matches(expected, 2, exact, 2) ||
      control_chunk_set_matches(expected, 2, partial, 1) ||
      control_chunk_set_matches(expected, 2, duplicate, 2) ||
      control_chunk_set_matches(expected, 2, unexpected, 2)) {
    fprintf(stderr, "Provider response correlation accepted a wrong chunk set\n");
    return 1;
  }
  return 0;
}

int main(void)
{
  return test_connection_params() == 0 &&
                 test_remote_only_provider_capabilities() == 0 &&
                 test_exact_control_response_correlation() == 0
             ? 0
             : 1;
}
