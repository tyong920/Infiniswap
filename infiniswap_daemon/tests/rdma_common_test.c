#include "rdma-common.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int running;

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
                 test_exact_control_response_correlation() == 0
             ? 0
             : 1;
}
