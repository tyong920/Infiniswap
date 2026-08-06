/*
 * Infiniswap, remote memory paging over RDMA
 * Copyright 2017 University of Michigan, Ann Arbor
 * GPLv2 License
 */
#ifndef RDMA_COMMON_H
#define RDMA_COMMON_H

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <semaphore.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <linux/kernel.h>

#include "infiniswap_provider_session.h"

#define TEST_NZ(x) do { if ( (x)) die("error: " #x " failed (returned non-zero)." ); } while (0)
#define TEST_Z(x)  do { if (!(x)) die("error: " #x " failed (returned zero/null)."); } while (0)

#define CQ_QP_BUSY 1
#define CQ_QP_IDLE 0
#define CQ_QP_DOWN 2


#ifdef USER_MAX_CLIENT
  #define MAX_CLIENT	USER_MAX_CLIENT
#else
  #define MAX_CLIENT	1
#endif

#define EXTRA_CHUNK_NUM 2
#define PROVIDER_HANDSHAKE_TIMEOUT_MS 5000U


#ifdef USER_MAX_REMOTE_MEMORY
  #define MAX_FREE_MEM_GB USER_MAX_REMOTE_MEMORY //for local memory management
  #define MAX_MR_SIZE_GB MAX_FREE_MEM_GB //for msg passing
#else
  #define MAX_FREE_MEM_GB 32 //for local memory management
  #define MAX_MR_SIZE_GB 32 //for msg passing
#endif


#define ONE_MB 1048576
#define ONE_GB 1073741824

#ifdef USER_REMOTE_MEMORY_EVICT
  #define FREE_MEM_EVICT_THRESHOLD USER_REMOTE_MEMORY_EVICT //in GB
#else
  #define FREE_MEM_EVICT_THRESHOLD 8 //in GB
#endif

#ifdef USER_REMOTE_MEMORY_EXPAND
  #define FREE_MEM_EXPAND_THRESHOLD USER_REMOTE_MEMORY_EXPAND //in GB
#else
  #define FREE_MEM_EXPAND_THRESHOLD 16 // in GB
#endif

#ifdef USER_EVICT_HIT_LIMIT
  #define MEM_EVICT_HIT_THRESHOLD USER_EVICT_HIT_LIMIT
#else
  #define MEM_EVICT_HIT_THRESHOLD 1 
#endif

#ifdef USER_EXPAND_HIT_LIMIT
  #define MEM_EXPAND_HIT_THRESHOLD USER_EXPAND_HIT_LIMIT
#else
  #define MEM_EXPAND_HIT_THRESHOLD 20
#endif

#ifdef USER_MEASURED_FREE_MEM_WEIGHT
  #define CURR_FREE_MEM_WEIGHT USER_MEASURED_FREE_MEM_WEIGHT
#else
  #define CURR_FREE_MEM_WEIGHT 0.7
#endif

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
  enum control_message_type type;
};

struct context {
  struct ibv_context *ctx;
  struct ibv_pd *pd;
  struct ibv_cq *cq;
  struct ibv_comp_channel *comp_channel;

  pthread_t cq_poller_thread;
};

struct atomic_t{
  int value;
  pthread_mutex_t mutex;
};

struct connection {

  struct rdma_session *sess;
  int conn_index; //conn index in sess->conns
  int sess_chunk_map[MAX_MR_SIZE_GB];
  int mapped_chunk_size;

  sem_t evict_sem;
  sem_t stop_sem;

  struct rdma_cm_id *id;
  struct ibv_qp *qp;

  int connected;

  struct ibv_mr *recv_mr;
  struct ibv_mr *send_mr;
  struct ibv_mr *rdma_remote_mr;

  struct ibv_mr peer_mr;

  uint8_t *recv_frame;
  uint8_t *send_frame;
  uint8_t *pending_send_frame;
  size_t send_frame_size;
  size_t pending_send_frame_size;
  struct control_message recv_message;
  struct control_message send_message;
  struct is_provider_session protocol_session;
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

  char *rdma_remote_region;
  //struct rdma_remote_mem rdma_remote;

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

  pthread_t free_mem_thread;
  long free_mem_gb;
  unsigned long rdma_buf_size;

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

#define CHUNK_MALLOCED 1
#define CHUNK_ALLOCATING 2
#define CHUNK_EMPTY	0
struct rdma_remote_mem{
  char* region_list[MAX_FREE_MEM_GB];
  struct ibv_mr* mr_list[MAX_FREE_MEM_GB]; 
  int size_gb; 
  int mapped_size;
  int conn_map[MAX_FREE_MEM_GB]; //chunk is used by which connection, or -1
  int malloc_map[MAX_FREE_MEM_GB];
  int conn_chunk_map[MAX_FREE_MEM_GB]; //session_chunk 
};

enum conn_state{
  CONN_IDLE,
  CONN_CONNECTED,
  CONN_MAPPED,
  CONN_FAILED
};

struct chunk_activity{
  uint64_t activity;
  int chunk_index;
};
struct rdma_session {
	struct connection* conns[MAX_CLIENT]; // need to init NULL
  enum conn_state conns_state[MAX_CLIENT];
	int conn_num;	

	struct rdma_remote_mem rdma_remote;		
  struct chunk_activity *evict_list;

};

extern struct rdma_session session;

typedef struct ibv_mr *(*remote_memory_register_fn)(
    struct ibv_pd *protection_domain, void *address, size_t length,
    int access);
typedef int (*remote_memory_deregister_fn)(struct ibv_mr *memory_region);

int register_remote_chunks(
    struct connection *conn, struct rdma_session *provider_session,
    struct ibv_pd *protection_domain, int requested_chunks,
    remote_memory_register_fn register_region,
    remote_memory_deregister_fn deregister_region);

int release_connection_remote_chunks(
    struct connection *conn, struct rdma_session *provider_session,
    remote_memory_deregister_fn deregister_region);

int control_chunk_set_matches(
    const uint8_t expected_chunks[MAX_MR_SIZE_GB],
    uint16_t expected_count, const uint32_t response_chunks[],
    uint16_t response_count);

void die(const char *reason);
void set_provider_auth_registry(struct is_auth_registry *registry);

int build_connection(struct rdma_cm_id *id);
void build_params(struct rdma_conn_param *params);
void destroy_connection(void *context);
void * get_serving_mem_region(void *context);
void on_connect(void *context);
void send_mr(void *context, int n);
int send_stop(void *context, int n);
void send_evict(void *context, int n);
void send_free_mem_size(void *context);
void rdma_session_init(struct rdma_session *sess);
void *free_mem(void *data);

#endif
