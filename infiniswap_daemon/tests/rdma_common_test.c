#include "rdma-common.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

volatile sig_atomic_t running;

static unsigned int disconnect_calls;
static struct is_memory_manager *disconnected_manager;
static void *disconnected_owner;

enum is_memory_result __wrap_is_memory_manager_disconnect(
    struct is_memory_manager *manager, void *owner)
{
  disconnect_calls++;
  disconnected_manager = manager;
  disconnected_owner = owner;
  return IS_MEMORY_OK;
}

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

static int test_provider_capabilities(void)
{
  uint64_t required = IS_PROTOCOL_CAP_REMOTE_ONLY |
                      IS_PROTOCOL_CAP_COMMITTED_POOL |
                      IS_PROTOCOL_CAP_FAILURE_DEADLINE |
                      IS_PROTOCOL_CAP_STATUS |
                      IS_PROTOCOL_CAP_AUTH_HMAC_SHA256;

  if ((IS_PROVIDER_CAPABILITIES & required) != required ||
      (IS_PROVIDER_REQUIRED_CAPABILITIES &
       (IS_PROTOCOL_CAP_FAILURE_DEADLINE | IS_PROTOCOL_CAP_STATUS |
        IS_PROTOCOL_CAP_AUTH_HMAC_SHA256)) !=
          (IS_PROTOCOL_CAP_FAILURE_DEADLINE | IS_PROTOCOL_CAP_STATUS |
           IS_PROTOCOL_CAP_AUTH_HMAC_SHA256)) {
    fprintf(stderr, "Provider does not advertise required liveness support\n");
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

static int test_silent_connection_reclaims_memory_once(void)
{
  static int manager_token;
  struct is_memory_manager *manager =
      (struct is_memory_manager *)(void *)&manager_token;
  struct connection connection;

  memset(&connection, 0, sizeof(connection));
  connection.memory_connected = 1;
  rdma_session_init(&session, manager);
  disconnect_calls = 0;
  disconnected_manager = NULL;
  disconnected_owner = NULL;

  if (reclaim_connection_memory(&connection) != IS_MEMORY_OK ||
      connection.memory_connected || disconnect_calls != 1 ||
      disconnected_manager != manager || disconnected_owner != &connection) {
    fprintf(stderr, "silent connection did not reclaim its Remote Chunks\n");
    return 1;
  }
  if (reclaim_connection_memory(&connection) != IS_MEMORY_OK ||
      disconnect_calls != 1) {
    fprintf(stderr, "Remote Chunk reclamation was not idempotent\n");
    return 1;
  }
  return 0;
}

int main(void)
{
  return test_connection_params() == 0 &&
                 test_provider_capabilities() == 0 &&
                 test_exact_control_response_correlation() == 0 &&
                 test_silent_connection_reclaims_memory_once() == 0
             ? 0
             : 1;
}
