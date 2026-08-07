/*
 * Infiniswap, remote memory paging over RDMA
 * Copyright 2017 University of Michigan, Ann Arbor
 * GPLv2 License
 */
#ifndef RDMA_COMMON_H
#define RDMA_COMMON_H

#include <arpa/inet.h>
#include <ctype.h>
#include <linux/kernel.h>
#include <netdb.h>
#include <netinet/in.h>
#include <rdma/rdma_cma.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "infiniswap_consumer_liveness.h"
#include "infiniswap_memory_manager.h"
#include "infiniswap_provider_session.h"

#define TEST_NZ(x)                                                            \
  do {                                                                        \
    if ((x))                                                                  \
      die("error: " #x " failed (returned non-zero).");                       \
  } while (0)
#define TEST_Z(x)                                                             \
  do {                                                                        \
    if (!(x))                                                                 \
      die("error: " #x " failed (returned zero/null).");                      \
  } while (0)

#define CQ_QP_BUSY 1
#define CQ_QP_IDLE 0
#define CQ_QP_DOWN 2
#define MAX_CLIENT 64
#define MAX_MR_SIZE_GB 128
_Static_assert(MAX_CLIENT == IS_AUTH_MAX_CONSUMERS,
               "connection slots must match the authenticated identity limit");
_Static_assert(MAX_MR_SIZE_GB == IS_PROTOCOL_MAX_CHUNKS_PER_FRAME,
               "Remote Chunk slots must match the protocol limit");
_Static_assert((int)IS_MEMORY_POOL_OPPORTUNISTIC ==
                   (int)IS_PROTOCOL_POOL_OPPORTUNISTIC &&
               (int)IS_MEMORY_POOL_COMMITTED ==
                   (int)IS_PROTOCOL_POOL_COMMITTED,
               "memory-manager pools must match the control protocol");
#define IS_PROVIDER_CAPABILITIES                                           \
  (IS_PROTOCOL_CAP_BACKED | IS_PROTOCOL_CAP_REMOTE_ONLY |                 \
   IS_PROTOCOL_CAP_OPPORTUNISTIC_POOL | IS_PROTOCOL_CAP_COMMITTED_POOL |  \
   IS_PROTOCOL_CAP_FAILURE_DEADLINE | IS_PROTOCOL_CAP_STATUS |            \
   IS_PROTOCOL_CAP_AUTH_HMAC_SHA256)
#define IS_PROVIDER_REQUIRED_CAPABILITIES                                 \
  (IS_PROTOCOL_CAP_FAILURE_DEADLINE | IS_PROTOCOL_CAP_STATUS |            \
   IS_PROTOCOL_CAP_AUTH_HMAC_SHA256)
#define PROVIDER_HANDSHAKE_TIMEOUT_MS 5000U

enum mode {
  M_WRITE,
  M_READ
};

enum control_message_type {
  CONTROL_DONE = 1,
  CONTROL_INFO,
  CONTROL_FREE_SIZE,
  CONTROL_EVICT,
  CONTROL_ACTIVITY,
  CONTROL_RELEASE,
  CONTROL_PEER_ERROR,
  CONTROL_QUERY,
  CONTROL_BIND
};

struct control_message {
  uint64_t buf[MAX_MR_SIZE_GB];
  uint32_t rkey[MAX_MR_SIZE_GB];
  int size_gb;
  uint32_t committed_size_gb;
  uint32_t status_flags;
  enum control_message_type type;
};

struct context {
  struct ibv_context *ctx;
  struct ibv_pd *pd;
  struct ibv_cq *cq;
  struct ibv_comp_channel *comp_channel;
  pthread_t cq_poller_thread;
};

struct atomic_t {
  int value;
  pthread_mutex_t mutex;
};

struct rdma_session;

struct connection {
  struct rdma_session *sess;
  int conn_index;
  int sess_chunk_map[MAX_MR_SIZE_GB];
  int mapped_chunk_size;

  sem_t evict_sem;
  sem_t stop_sem;

  struct rdma_cm_id *id;
  struct ibv_qp *qp;
  int connected;

  struct ibv_mr *recv_mr;
  struct ibv_mr *send_mr;
  struct ibv_mr peer_mr;

  uint8_t *recv_frame;
  uint8_t *send_frame;
  uint8_t *pending_send_frame;
  size_t send_frame_size;
  size_t pending_send_frame_size;
  struct control_message recv_message;
  struct control_message send_message;
  struct is_provider_session protocol_session;
  struct is_consumer_liveness consumer_liveness;
  uint64_t active_request_id;
  uint64_t next_provider_request_id;
  uint64_t pending_evict_request_id;
  uint64_t pending_release_request_id;
  uint8_t pending_evict_chunks[MAX_MR_SIZE_GB];
  uint8_t pending_release_chunks[MAX_MR_SIZE_GB];
  uint8_t release_chunks[MAX_MR_SIZE_GB];
  uint16_t pending_evict_chunk_count;
  uint16_t pending_release_chunk_count;
  uint16_t release_chunk_count;
  uint32_t requested_logical_start;
  uint8_t requested_pool;
  int close_after_send;
  int repost_after_send;
  int pending_send;
  int pending_close_after_send;
  int pending_repost_after_send;
  int auth_subscribed;
  int memory_connected;

  struct atomic_t cq_qp_state;
  pthread_mutex_t send_lock;
  pthread_mutex_t control_lock;
  pthread_mutex_t lifetime_lock;
  pthread_cond_t lifetime_idle;
  pthread_t deadline_thread;
  unsigned int references;
  int handshake_complete;
  int send_inflight;
  int closing;

  enum {
    S_WAIT,
    S_BIND,
    S_DONE
  } server_state;

  enum {
    SS_INIT,
    SS_MR_SENT,
    SS_STOP_SENT,
    SS_DONE_SENT
  } send_state;

  enum {
    RS_INIT,
    RS_STOPPED_RECV,
    RS_DONE_RECV
  } recv_state;
};

enum conn_state {
  CONN_IDLE,
  CONN_CONNECTED,
  CONN_MAPPED,
  CONN_FAILED
};

struct chunk_activity {
  uint64_t activity;
  uint32_t provider_chunk_id;
  struct connection *connection;
};

struct rdma_session {
  struct connection *conns[MAX_CLIENT];
  enum conn_state conns_state[MAX_CLIENT];
  int conn_num;
  struct is_memory_manager *memory_manager;
};

extern struct rdma_session session;

int control_chunk_set_matches(
    const uint8_t expected_chunks[MAX_MR_SIZE_GB],
    uint16_t expected_count, const uint32_t response_chunks[],
    uint16_t response_count);

_Noreturn void die(const char *reason);
void set_provider_auth_registry(struct is_auth_registry *registry);

int build_connection(struct rdma_cm_id *id);
void build_params(struct rdma_conn_param *params);
void destroy_connection(void *context);
enum is_memory_result reclaim_connection_memory(struct connection *conn);
void on_connect(void *context);
void send_mr(void *context, int n);
int send_stop(void *context, int n);
void send_evict(void *context, int n);
void send_free_mem_size(void *context);
void rdma_session_init(struct rdma_session *provider_session,
                       struct is_memory_manager *memory_manager);
int provider_connection_count(void);
void disconnect_provider_connections(void);
void *free_mem(void *data);

#endif /* RDMA_COMMON_H */
