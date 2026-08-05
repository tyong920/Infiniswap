#include "rdma-common.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

long page_size;
int running;

static int test_connection_params(void)
{
  struct rdma_conn_param params;

  memset(&params, 0xa5, sizeof(params));
  build_params(&params);

  if (params.initiator_depth != 1 || params.responder_resources != 1 ||
      params.rnr_retry_count != 7) {
    fprintf(stderr, "build_params did not initialize the RDMA connection policy\n");
    return 1;
  }

  return 0;
}

static int test_host_to_network_u64(void)
{
  const uint64_t host = UINT64_C(0x0123456789abcdef);
  const unsigned char expected[] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef
  };
  const uint64_t network = infiniswap_htonll(host);

  if (memcmp(&network, expected, sizeof(expected)) == 0)
    return 0;

  fprintf(stderr, "infiniswap_htonll did not produce network byte order\n");
  return 1;
}

int main(void)
{
  int failures = 0;

  failures += test_connection_params();
  failures += test_host_to_network_u64();

  return failures == 0 ? 0 : 1;
}
