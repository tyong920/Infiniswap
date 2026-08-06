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

static struct ibv_mr fake_memory_regions[2];
static int fake_registration_calls;
static int fake_deregistration_calls;

static struct ibv_mr *fake_register_region(
    struct ibv_pd *protection_domain, void *address, size_t length,
    int access)
{
  struct ibv_mr *memory_region;

  (void)protection_domain;
  (void)length;
  (void)access;
  if (fake_registration_calls++ == 1)
    return NULL;
  memory_region = &fake_memory_regions[0];
  memset(memory_region, 0, sizeof(*memory_region));
  memory_region->addr = address;
  memory_region->rkey = 1;
  return memory_region;
}

static int fake_deregister_region(struct ibv_mr *memory_region)
{
  if (memory_region != &fake_memory_regions[0] &&
      memory_region != &fake_memory_regions[1])
    return -1;
  fake_deregistration_calls++;
  return 0;
}

static int test_partial_registration_rollback(void)
{
  struct connection conn;
  struct rdma_session provider_session;
  int index;

  memset(&conn, 0, sizeof(conn));
  memset(&provider_session, 0, sizeof(provider_session));
  conn.conn_index = 0;
  for (index = 0; index < MAX_FREE_MEM_GB; index++) {
    conn.sess_chunk_map[index] = -1;
    provider_session.rdma_remote.conn_map[index] = -1;
    provider_session.rdma_remote.conn_chunk_map[index] = -1;
  }
  provider_session.rdma_remote.malloc_map[0] = CHUNK_MALLOCED;
  provider_session.rdma_remote.malloc_map[1] = CHUNK_MALLOCED;
  provider_session.rdma_remote.region_list[0] = (char *)(uintptr_t)1;
  provider_session.rdma_remote.region_list[1] = (char *)(uintptr_t)2;
  fake_registration_calls = 0;
  fake_deregistration_calls = 0;

  if (register_remote_chunks(&conn, &provider_session, NULL, 2,
                             fake_register_region,
                             fake_deregister_region) != -1 ||
      fake_registration_calls != 2 || fake_deregistration_calls != 1 ||
      provider_session.rdma_remote.mr_list[0] != NULL ||
      provider_session.rdma_remote.conn_map[0] != -1 ||
      provider_session.rdma_remote.conn_chunk_map[0] != -1 ||
      conn.sess_chunk_map[0] != -1 || conn.send_message.rkey[0] != 0 ||
      conn.send_message.buf[0] != 0) {
    fprintf(stderr, "partial Remote Memory registration was not rolled back\n");
    return 1;
  }
  return 0;
}

static int test_full_connection_registration_cleanup(void)
{
  struct connection conn;
  struct rdma_session provider_session;
  int index;

  memset(&conn, 0, sizeof(conn));
  memset(&provider_session, 0, sizeof(provider_session));
  for (index = 0; index < MAX_FREE_MEM_GB; index++) {
    conn.sess_chunk_map[index] = -1;
    provider_session.rdma_remote.conn_map[index] = -1;
    provider_session.rdma_remote.conn_chunk_map[index] = -1;
  }
  conn.sess_chunk_map[0] = 0;
  conn.sess_chunk_map[1] = 1;
  conn.mapped_chunk_size = 2;
  provider_session.rdma_remote.mr_list[0] = &fake_memory_regions[0];
  provider_session.rdma_remote.mr_list[1] = &fake_memory_regions[1];
  provider_session.rdma_remote.conn_map[0] = 0;
  provider_session.rdma_remote.conn_map[1] = 0;
  provider_session.rdma_remote.mapped_size = 2;
  fake_deregistration_calls = 0;

  if (release_connection_remote_chunks(
          &conn, &provider_session, fake_deregister_region) != 0 ||
      fake_deregistration_calls != 2 || conn.mapped_chunk_size != 0 ||
      provider_session.rdma_remote.mapped_size != 0 ||
      provider_session.rdma_remote.mr_list[0] != NULL ||
      provider_session.rdma_remote.mr_list[1] != NULL ||
      conn.sess_chunk_map[0] != -1 || conn.sess_chunk_map[1] != -1 ||
      provider_session.rdma_remote.conn_map[0] != -1 ||
      provider_session.rdma_remote.conn_map[1] != -1) {
    fprintf(stderr, "connection teardown left Remote Memory registered\n");
    return 1;
  }
  return 0;
}

int main(void)
{
  return test_connection_params() == 0 &&
                 test_exact_control_response_correlation() == 0 &&
                 test_partial_registration_rollback() == 0 &&
                 test_full_connection_registration_cleanup() == 0
             ? 0
             : 1;
}
