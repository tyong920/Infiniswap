/*
 * Infiniswap, remote memory paging over RDMA
 * Copyright 2017 University of Michigan, Ann Arbor
 * GPLv2 License
 */
#include "rdma-common.h"

#include <errno.h>
#include <openssl/rand.h>
#include <signal.h>
#include <time.h>

extern volatile sig_atomic_t running;

static void build_context(struct ibv_context *verbs);
static void build_qp_attr(struct ibv_qp_init_attr *qp_attr);
static void *monitor_connection_deadlines(void *context);
static void on_completion(struct ibv_wc *);
static void * poll_cq(void *);
static void post_receives(struct connection *conn);
static void prepare_connection_close(struct connection *conn);
static void register_memory(struct connection *conn);
static void send_message(struct connection *conn, int repost_receive);
static void wait_for_connection_references(struct connection *conn,
                                           unsigned int remaining);

struct rdma_session session;

static struct context *s_ctx = NULL;
static struct is_auth_registry *provider_auth_registry;
static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;

void set_provider_auth_registry(struct is_auth_registry *registry)
{
  provider_auth_registry = registry;
}

void atomic_init(struct atomic_t *m)
{
  pthread_mutex_init(&m->mutex,NULL);
  m->value = -1;
}
void atomic_set(struct atomic_t *m, int val)
{
  pthread_mutex_lock(&m->mutex);
  m->value = val;
  pthread_mutex_unlock(&m->mutex);
}
int atomic_read(struct atomic_t *m)
{
  int res;
  pthread_mutex_lock(&m->mutex);
  res = m->value;
  pthread_mutex_unlock(&m->mutex);

  return res;
}

static int connection_get_reference(struct connection *conn)
{
  int result = 0;

  pthread_mutex_lock(&conn->lifetime_lock);
  if (conn->closing)
    result = -1;
  else
    conn->references++;
  pthread_mutex_unlock(&conn->lifetime_lock);
  return result;
}

static void connection_put_reference(struct connection *conn)
{
  pthread_mutex_lock(&conn->lifetime_lock);
  if (conn->references > 0)
    conn->references--;
  if (conn->closing)
    pthread_cond_broadcast(&conn->lifetime_idle);
  pthread_mutex_unlock(&conn->lifetime_lock);
}

static void add_milliseconds(struct timespec *deadline,
                             uint32_t milliseconds)
{
  deadline->tv_sec += (time_t)(milliseconds / 1000U);
  deadline->tv_nsec += (long)(milliseconds % 1000U) * 1000000L;
  if (deadline->tv_nsec >= 1000000000L) {
    deadline->tv_sec++;
    deadline->tv_nsec -= 1000000000L;
  }
}

static int monotonic_now(struct timespec *now, uint64_t *milliseconds)
{
  if (clock_gettime(CLOCK_MONOTONIC, now) != 0 || now->tv_sec < 0 ||
      (uint64_t)now->tv_sec > UINT64_MAX / UINT64_C(1000))
    return -1;
  *milliseconds = (uint64_t)now->tv_sec * UINT64_C(1000) +
                  (uint64_t)now->tv_nsec / UINT64_C(1000000);
  return 0;
}

static int start_connection_liveness(struct connection *conn)
{
  struct timespec now;
  uint64_t now_ms;
  int result = 0;

  if (monotonic_now(&now, &now_ms) != 0)
    return -1;
  pthread_mutex_lock(&conn->lifetime_lock);
  if (conn->closing) {
    result = -1;
  } else {
    is_consumer_liveness_init(
        &conn->consumer_liveness,
        conn->protocol_session.failure_deadline_ms,
        conn->protocol_session.authenticated_until_unix, now_ms);
    conn->handshake_complete = 1;
    pthread_cond_broadcast(&conn->lifetime_idle);
  }
  pthread_mutex_unlock(&conn->lifetime_lock);
  return result;
}

static int refresh_connection_liveness(struct connection *conn)
{
  struct timespec now;
  uint64_t now_ms;
  int result = 0;

  if (monotonic_now(&now, &now_ms) != 0)
    return -1;
  pthread_mutex_lock(&conn->lifetime_lock);
  if (conn->closing || !conn->handshake_complete) {
    result = -1;
  } else {
    is_consumer_liveness_refresh(&conn->consumer_liveness, now_ms);
    pthread_cond_broadcast(&conn->lifetime_idle);
  }
  pthread_mutex_unlock(&conn->lifetime_lock);
  return result;
}

static void *monitor_connection_deadlines(void *context)
{
  struct connection *conn = context;
  struct timespec deadline;
  uint64_t now_ms;
  int result = 0;
  int disconnect = 0;

  if (monotonic_now(&deadline, &now_ms) != 0) {
    disconnect = 1;
    goto out;
  }
  add_milliseconds(&deadline, PROVIDER_HANDSHAKE_TIMEOUT_MS);
  pthread_mutex_lock(&conn->lifetime_lock);
  while (!conn->closing && !conn->handshake_complete && result == 0)
    result = pthread_cond_timedwait(&conn->lifetime_idle,
                                    &conn->lifetime_lock, &deadline);
  if (!conn->closing && !conn->handshake_complete)
    disconnect = 1;

  while (!conn->closing && !disconnect) {
    enum is_consumer_liveness_result liveness_result;
    uint64_t wait_ms;
    time_t now_unix = time(NULL);

    if (now_unix < 0 || monotonic_now(&deadline, &now_ms) != 0) {
      disconnect = 1;
      break;
    }
    liveness_result = is_consumer_liveness_check(
        &conn->consumer_liveness, now_ms, (uint64_t)now_unix, &wait_ms);
    if (liveness_result != IS_CONSUMER_LIVENESS_ACTIVE) {
      disconnect = 1;
      break;
    }
    add_milliseconds(&deadline, (uint32_t)wait_ms);
    result = pthread_cond_timedwait(&conn->lifetime_idle,
                                    &conn->lifetime_lock, &deadline);
    if (result != 0 && result != ETIMEDOUT)
      disconnect = 1;
  }
  pthread_mutex_unlock(&conn->lifetime_lock);
out:
  if (disconnect) {
    (void)rdma_disconnect(conn->id);
    prepare_connection_close(conn);
    wait_for_connection_references(conn, 1);
    if (reclaim_connection_memory(conn) != IS_MEMORY_OK)
      die("could not reclaim silent Consumer Remote Chunks");
  }
  connection_put_reference(conn);
  return NULL;
}

static int wait_for_control_response(struct connection *conn,
                                     sem_t *semaphore)
{
  struct timespec deadline;
  uint32_t timeout_ms = conn->protocol_session.failure_deadline_ms;
  int result;
  int closing;

  if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
    return -1;
  add_milliseconds(&deadline, timeout_ms);
  do {
    result = sem_clockwait(semaphore, CLOCK_MONOTONIC, &deadline);
  } while (result != 0 && errno == EINTR);
  pthread_mutex_lock(&conn->lifetime_lock);
  closing = conn->closing;
  pthread_mutex_unlock(&conn->lifetime_lock);
  if (result != 0 || closing) {
    if (!closing)
      rdma_disconnect(conn->id);
    return -1;
  }
  return 0;
}

static void deregister_memory(struct ibv_mr *memory_region)
{
  if (memory_region && ibv_dereg_mr(memory_region) != 0)
    die("could not deregister RDMA memory");
}

_Noreturn void die(const char *reason)
{
  fprintf(stderr, "%s\n", reason);
  exit(EXIT_FAILURE);
}

int build_connection(struct rdma_cm_id *id)
{
  int i;
  struct connection *conn;
  struct ibv_qp_init_attr qp_attr;
  pthread_condattr_t condition_attributes;
  uint8_t provider_nonce[IS_PROTOCOL_NONCE_SIZE];
  uint64_t session_id;

  pthread_mutex_lock(&session_lock);
  if (session.conn_num >= MAX_CLIENT) {
    pthread_mutex_unlock(&session_lock);
    return -1;
  }
  pthread_mutex_unlock(&session_lock);
  if (!provider_auth_registry ||
      RAND_bytes(provider_nonce, sizeof(provider_nonce)) != 1 ||
      RAND_bytes((unsigned char *)&session_id, sizeof(session_id)) != 1)
    die("could not initialize an authenticated control session");
  if (session_id == 0)
    session_id = 1;

  build_context(id->verbs);
  build_qp_attr(&qp_attr);

  TEST_NZ(rdma_create_qp(id, s_ctx->pd, &qp_attr));

  id->context = conn = (struct connection *)calloc(1, sizeof(*conn));
  TEST_Z(conn);

  conn->id = id;
  conn->qp = id->qp;

  conn->send_state = SS_INIT;
  conn->recv_state = RS_INIT;
  conn->server_state = S_WAIT;

  conn->connected = 0;
  atomic_init(&conn->cq_qp_state);
  atomic_set(&conn->cq_qp_state, CQ_QP_BUSY);
  pthread_mutex_init(&conn->send_lock, NULL);
  pthread_mutex_init(&conn->control_lock, NULL);
  pthread_mutex_init(&conn->lifetime_lock, NULL);
  TEST_NZ(pthread_condattr_init(&condition_attributes));
  TEST_NZ(pthread_condattr_setclock(&condition_attributes, CLOCK_MONOTONIC));
  TEST_NZ(pthread_cond_init(&conn->lifetime_idle, &condition_attributes));
  TEST_NZ(pthread_condattr_destroy(&condition_attributes));

  sem_init(&conn->stop_sem, 0, 0);
  sem_init(&conn->evict_sem, 0, 0);
  conn->sess = &session;
  for (i = 0; i < MAX_MR_SIZE_GB; i++) {
    conn->sess_chunk_map[i] = -1;
  }
  conn->mapped_chunk_size = 0;
  conn->next_provider_request_id = 1;
  is_provider_session_init(
      &conn->protocol_session, provider_auth_registry,
      IS_PROTOCOL_MINOR_CURRENT, IS_PROVIDER_CAPABILITIES,
      IS_PROVIDER_REQUIRED_CAPABILITIES, provider_nonce, session_id);
  //add to session
  pthread_mutex_lock(&session_lock);
  for (i=0; i<MAX_CLIENT; i++){
    if (session.conns_state[i] == CONN_IDLE){
      session.conns[i] = conn;
      session.conns_state[i] = CONN_CONNECTED;
      conn->conn_index = i;
      break;
    } 
  }
  session.conn_num += 1;
  pthread_mutex_unlock(&session_lock);

  if (connection_get_reference(conn) != 0)
    die("could not retain an unauthenticated connection");
  if (pthread_create(&conn->deadline_thread, NULL,
                     monitor_connection_deadlines, conn) != 0) {
    connection_put_reference(conn);
    die("could not start the authentication deadline");
  }
  register_memory(conn);
  post_receives(conn);
  return 0;
}

void build_context(struct ibv_context *verbs)
{
  if (s_ctx) {
    if (s_ctx->ctx != verbs)
      die("cannot handle events in more than one context.");

    return;
  }

  s_ctx = (struct context *)malloc(sizeof(struct context));

  s_ctx->ctx = verbs;

  TEST_Z(s_ctx->pd = ibv_alloc_pd(s_ctx->ctx));
  TEST_Z(s_ctx->comp_channel = ibv_create_comp_channel(s_ctx->ctx));
  TEST_Z(s_ctx->cq = ibv_create_cq(
      s_ctx->ctx, MAX_CLIENT * 4, NULL, s_ctx->comp_channel, 0));
  TEST_NZ(ibv_req_notify_cq(s_ctx->cq, 0));

  TEST_NZ(pthread_create(&s_ctx->cq_poller_thread, NULL, poll_cq, NULL));
}

void build_params(struct rdma_conn_param *params)
{
  memset(params, 0, sizeof(*params));

  params->initiator_depth = params->responder_resources = 1;
  params->rnr_retry_count = 6;
}

void build_qp_attr(struct ibv_qp_init_attr *qp_attr)
{
  memset(qp_attr, 0, sizeof(*qp_attr));

  qp_attr->send_cq = s_ctx->cq;
  qp_attr->recv_cq = s_ctx->cq;
  qp_attr->qp_type = IBV_QPT_RC;

  qp_attr->cap.max_send_wr = 10;
  qp_attr->cap.max_recv_wr = 100; //original 10
  qp_attr->cap.max_send_sge = 1;
  qp_attr->cap.max_recv_sge = 1;
}

static void prepare_connection_close(struct connection *conn)
{
  struct ibv_qp_attr qp_attr;
  struct ibv_qp_init_attr qp_init_attr;
  int prepare = 0;

  pthread_mutex_lock(&conn->lifetime_lock);
  if (!conn->closing) {
    conn->closing = 1;
    prepare = 1;
  }
  pthread_cond_broadcast(&conn->lifetime_idle);
  pthread_mutex_unlock(&conn->lifetime_lock);
  if (!prepare)
    return;

  sem_post(&conn->evict_sem);
  sem_post(&conn->stop_sem);
  memset(&qp_attr, 0, sizeof(qp_attr));
  memset(&qp_init_attr, 0, sizeof(qp_init_attr));
  if (ibv_query_qp(conn->qp, &qp_attr, IBV_QP_STATE, &qp_init_attr) != 0)
    die("could not inspect a disconnected control queue");
  if (qp_attr.qp_state != IBV_QPS_ERR) {
    qp_attr.qp_state = IBV_QPS_ERR;
    if (ibv_modify_qp(conn->qp, &qp_attr, IBV_QP_STATE) != 0)
      die("could not drain a disconnected control queue");
  }
}

static void wait_for_connection_references(struct connection *conn,
                                           unsigned int remaining)
{
  pthread_mutex_lock(&conn->lifetime_lock);
  while (conn->references > remaining)
    pthread_cond_wait(&conn->lifetime_idle, &conn->lifetime_lock);
  pthread_mutex_unlock(&conn->lifetime_lock);
}

enum is_memory_result reclaim_connection_memory(struct connection *conn)
{
  enum is_memory_result result;

  if (!conn || !conn->memory_connected)
    return IS_MEMORY_OK;
  result = is_memory_manager_disconnect(session.memory_manager, conn);
  if (result == IS_MEMORY_OK || result == IS_MEMORY_NOT_CONNECTED) {
    conn->memory_connected = 0;
    return IS_MEMORY_OK;
  }
  return result;
}

void destroy_connection(void *context)
{
  struct connection *conn = (struct connection *)context;

  prepare_connection_close(conn);
  wait_for_connection_references(conn, 0);
  TEST_NZ(pthread_join(conn->deadline_thread, NULL));
  if (conn->auth_subscribed) {
    is_auth_registry_unsubscribe(provider_auth_registry, conn);
    conn->auth_subscribed = 0;
  }
  rdma_destroy_qp(conn->id);

  deregister_memory(conn->send_mr);
  deregister_memory(conn->recv_mr);

  free(conn->send_frame);
  free(conn->pending_send_frame);
  free(conn->recv_frame);

  if (reclaim_connection_memory(conn) != IS_MEMORY_OK)
    die("could not release registered Remote Memory");

  pthread_mutex_lock(&session_lock);
  session.conns[conn->conn_index] = NULL;
  session.conns_state[conn->conn_index] = CONN_IDLE;
  session.conn_num -= 1;
  pthread_mutex_unlock(&session_lock);
  rdma_destroy_id(conn->id);
  sem_destroy(&conn->stop_sem);
  sem_destroy(&conn->evict_sem);
  pthread_mutex_destroy(&conn->cq_qp_state.mutex);
  pthread_mutex_destroy(&conn->send_lock);
  pthread_mutex_destroy(&conn->control_lock);
  pthread_cond_destroy(&conn->lifetime_idle);
  pthread_mutex_destroy(&conn->lifetime_lock);

  free(conn);
}

void rdma_session_init(struct rdma_session *provider_session,
                       struct is_memory_manager *memory_manager)
{
  int index;

  memset(provider_session, 0, sizeof(*provider_session));
  provider_session->memory_manager = memory_manager;
  for (index = 0; index < MAX_CLIENT; index++)
    provider_session->conns_state[index] = CONN_IDLE;
}

int provider_connection_count(void)
{
  int count;

  pthread_mutex_lock(&session_lock);
  count = session.conn_num;
  pthread_mutex_unlock(&session_lock);
  return count;
}

void disconnect_provider_connections(void)
{
  struct connection *connections[MAX_CLIENT] = {0};
  int count = 0;
  int index;

  pthread_mutex_lock(&session_lock);
  for (index = 0; index < MAX_CLIENT; index++) {
    struct connection *conn = session.conns[index];

    if (!conn || connection_get_reference(conn) != 0)
      continue;
    connections[count++] = conn;
  }
  pthread_mutex_unlock(&session_lock);
  for (index = 0; index < count; index++) {
    (void)rdma_disconnect(connections[index]->id);
    connection_put_reference(connections[index]);
  }
}

static int compare_activity(const void *left, const void *right)
{
  const struct chunk_activity *left_activity = left;
  const struct chunk_activity *right_activity = right;

  if (left_activity->activity < right_activity->activity)
    return -1;
  if (left_activity->activity > right_activity->activity)
    return 1;
  return 0;
}

static void evict_mem(uint32_t requested_chunks)
{
  struct connection *held_connections[MAX_CLIENT] = {0};
  struct chunk_activity *activities = NULL;
  uint32_t activity_count = 0;
  uint32_t selected_count;
  int held_count = 0;
  int index;

  pthread_mutex_lock(&session_lock);
  for (index = 0; index < MAX_CLIENT; index++) {
    struct connection *conn = session.conns[index];

    if (!conn || session.conns_state[index] != CONN_MAPPED ||
        conn->protocol_session.selected_pool !=
            IS_PROTOCOL_POOL_OPPORTUNISTIC ||
        connection_get_reference(conn) != 0)
      continue;
    held_connections[held_count++] = conn;
  }
  pthread_mutex_unlock(&session_lock);
  if (held_count == 0)
    goto out;

  activities = calloc(IS_MEMORY_MAX_RUNTIME_CHUNKS,
                      sizeof(*activities));
  if (!activities)
    goto out;
  activity_count = 0;
  for (index = 0; index < held_count; index++)
    send_evict(held_connections[index], held_connections[index]->mapped_chunk_size);
  for (index = 0; index < held_count; index++) {
    struct connection *conn = held_connections[index];
    int chunk_index;

    if (wait_for_control_response(conn, &conn->evict_sem) != 0)
      goto out;
    for (chunk_index = 0; chunk_index < MAX_MR_SIZE_GB; chunk_index++) {
      if (!conn->recv_message.rkey[chunk_index])
        continue;
      if (activity_count == IS_MEMORY_MAX_RUNTIME_CHUNKS)
        goto out;
      activities[activity_count].activity =
          conn->recv_message.buf[chunk_index];
      activities[activity_count].provider_chunk_id = (uint32_t)chunk_index;
      activities[activity_count].connection = conn;
      activity_count++;
    }
    post_receives(conn);
  }
  qsort(activities, activity_count, sizeof(*activities), compare_activity);
  selected_count = requested_chunks < activity_count
                       ? requested_chunks
                       : activity_count;
  for (index = 0; (uint32_t)index < selected_count; index++) {
    struct connection *conn = activities[index].connection;
    uint32_t chunk_id = activities[index].provider_chunk_id;

    pthread_mutex_lock(&conn->control_lock);
    if (!conn->release_chunks[chunk_id]) {
      conn->release_chunks[chunk_id] = 1;
      conn->release_chunk_count++;
    }
    pthread_mutex_unlock(&conn->control_lock);
  }
  for (index = 0; index < held_count; index++) {
    struct connection *conn = held_connections[index];

    if (conn->release_chunk_count != 0 &&
        send_stop(conn, conn->release_chunk_count) != 0)
      goto out;
  }
out:
  free(activities);
  for (index = 0; index < held_count; index++)
    connection_put_reference(held_connections[index]);
}

void *free_mem(void *data)
{
  struct rdma_session *provider_session = data;

  while (running) {
    struct is_memory_reconcile_result reconcile;
    enum is_memory_result result = is_memory_manager_reconcile(
        provider_session->memory_manager, &reconcile);

    if (result != IS_MEMORY_OK) {
      fprintf(stderr, "Provider memory reconciliation failed: %s\n",
              is_memory_result_name(result));
    } else if (reconcile.assigned_reclaim_needed != 0) {
      evict_mem(reconcile.assigned_reclaim_needed);
    }
    sleep(1);
  }
  return NULL;
}

static void recv_done(struct connection *conn)
{
  uint32_t released_chunks[MAX_MR_SIZE_GB];
  size_t released_count = 0;
  int index;

  for (index = 0; index < MAX_MR_SIZE_GB; index++) {
    if (!conn->recv_message.rkey[index])
      continue;
    if (conn->sess_chunk_map[index] != index) {
      rdma_disconnect(conn->id);
      sem_post(&conn->stop_sem);
      return;
    }
    released_chunks[released_count++] = (uint32_t)index;
  }
  if (released_count != (size_t)conn->recv_message.size_gb ||
      is_memory_manager_release(
          session.memory_manager, conn, released_chunks, released_count,
          IS_MEMORY_RELEASE_PRESSURE) != IS_MEMORY_OK) {
    rdma_disconnect(conn->id);
    sem_post(&conn->stop_sem);
    return;
  }

  pthread_mutex_lock(&session_lock);
  for (index = 0; (size_t)index < released_count; index++)
    conn->sess_chunk_map[released_chunks[index]] = -1;
  conn->mapped_chunk_size -= (int)released_count;
  if (conn->mapped_chunk_size == 0)
    session.conns_state[conn->conn_index] = CONN_CONNECTED;
  pthread_mutex_unlock(&session_lock);
  sem_post(&conn->stop_sem);
}

static void init_outbound_message(struct connection *conn,
                                  struct is_protocol_message *message,
                                  enum is_protocol_message_type type,
                                  int response)
{
  memset(message, 0, sizeof(*message));
  message->header.major = IS_PROTOCOL_MAJOR;
  message->header.minor = conn->protocol_session.negotiated_minor;
  message->header.type = type;
  if (response)
    message->header.flags = IS_PROTOCOL_FLAG_RESPONSE;
  message->header.request_id = response
      ? conn->active_request_id
      : conn->next_provider_request_id++;
  if (!response && type == IS_PROTOCOL_MSG_EVICT)
    conn->pending_evict_request_id = message->header.request_id;
  if (!response && type == IS_PROTOCOL_MSG_RELEASE)
    conn->pending_release_request_id = message->header.request_id;
  message->header.session_id = conn->protocol_session.session_id;
  message->header.capabilities =
      conn->protocol_session.negotiated_capabilities;
  message->header.required_capabilities =
      conn->protocol_session.required_capabilities;
}

static int encode_control_message(struct connection *conn,
                                  uint8_t *frame, size_t *frame_size)
{
  struct is_protocol_message message;
  enum is_protocol_result result;
  size_t index;
  size_t count = 0;

  switch (conn->send_message.type) {
  case CONTROL_FREE_SIZE:
    init_outbound_message(conn, &message, IS_PROTOCOL_MSG_STATUS_RESPONSE, 1);
    message.payload.status.available_opportunistic_chunks =
        (uint32_t)conn->send_message.size_gb;
    message.payload.status.available_committed_chunks =
        conn->send_message.committed_size_gb;
    message.payload.status.provider_failure_deadline_ms =
        conn->protocol_session.failure_deadline_ms;
    message.payload.status.flags = conn->send_message.status_flags;
    break;
  case CONTROL_INFO:
    init_outbound_message(conn, &message, IS_PROTOCOL_MSG_CHUNK_GRANT, 1);
    for (index = 0; index < MAX_MR_SIZE_GB; index++) {
      struct is_protocol_chunk *chunk;

      if (!conn->send_message.rkey[index])
        continue;
      if (count == IS_PROTOCOL_MAX_CHUNKS_PER_FRAME)
        return -1;
      chunk = &message.payload.chunk_grant.chunks[count];
      chunk->logical_chunk_id = conn->requested_logical_start + (uint32_t)count;
      chunk->provider_chunk_id = (uint32_t)index;
      chunk->remote_address = conn->send_message.buf[index];
      chunk->remote_key = conn->send_message.rkey[index];
      chunk->pool = conn->requested_pool;
      count++;
    }
    message.payload.chunk_grant.chunk_count = (uint16_t)count;
    break;
  case CONTROL_EVICT:
    if (conn->pending_evict_request_id != 0)
      return -1;
    memset(conn->pending_evict_chunks, 0,
           sizeof(conn->pending_evict_chunks));
    conn->pending_evict_chunk_count = 0;
    init_outbound_message(conn, &message, IS_PROTOCOL_MSG_EVICT, 0);
    for (index = 0; index < MAX_MR_SIZE_GB; index++) {
      if (conn->sess_chunk_map[index] < 0)
        continue;
      if (count == IS_PROTOCOL_MAX_CHUNKS_PER_FRAME)
        return -1;
      message.payload.chunk_ids.chunk_ids[count++] = (uint32_t)index;
      conn->pending_evict_chunks[index] = 1;
    }
    message.payload.chunk_ids.chunk_count = (uint16_t)count;
    conn->pending_evict_chunk_count = (uint16_t)count;
    break;
  case CONTROL_RELEASE:
    if (conn->pending_release_request_id != 0)
      return -1;
    memset(conn->pending_release_chunks, 0,
           sizeof(conn->pending_release_chunks));
    conn->pending_release_chunk_count = 0;
    init_outbound_message(conn, &message, IS_PROTOCOL_MSG_RELEASE, 0);
    for (index = 0; index < MAX_MR_SIZE_GB; index++) {
      if (!conn->send_message.rkey[index])
        continue;
      if (count == IS_PROTOCOL_MAX_CHUNKS_PER_FRAME)
        return -1;
      message.payload.chunk_ids.chunk_ids[count++] = (uint32_t)index;
      conn->pending_release_chunks[index] = 1;
    }
    message.payload.chunk_ids.chunk_count = (uint16_t)count;
    conn->pending_release_chunk_count = (uint16_t)count;
    break;
  case CONTROL_DONE:
    init_outbound_message(conn, &message, IS_PROTOCOL_MSG_GOODBYE, 0);
    break;
  default:
    return -1;
  }

  result = is_protocol_encode(&message, frame,
                              IS_PROTOCOL_MAX_FRAME_SIZE, frame_size);
  return result == IS_PROTOCOL_OK ? 0 : -1;
}

static void clear_sends(struct connection *conn)
{
  pthread_mutex_lock(&conn->send_lock);
  conn->send_inflight = 0;
  conn->pending_send = 0;
  conn->close_after_send = 0;
  conn->repost_after_send = 0;
  conn->pending_close_after_send = 0;
  conn->pending_repost_after_send = 0;
  pthread_mutex_unlock(&conn->send_lock);
}

static int finish_send(struct connection *conn, int *close_connection,
                       int *repost_receive)
{
  int start_next = 0;

  pthread_mutex_lock(&conn->send_lock);
  *close_connection = conn->close_after_send;
  *repost_receive = conn->repost_after_send;
  conn->close_after_send = 0;
  conn->repost_after_send = 0;
  if (*close_connection) {
    conn->send_inflight = 0;
    conn->pending_send = 0;
    conn->pending_close_after_send = 0;
    conn->pending_repost_after_send = 0;
  } else if (conn->pending_send) {
    memcpy(conn->send_frame, conn->pending_send_frame,
           conn->pending_send_frame_size);
    conn->send_frame_size = conn->pending_send_frame_size;
    conn->close_after_send = conn->pending_close_after_send;
    conn->repost_after_send = conn->pending_repost_after_send;
    conn->pending_send = 0;
    conn->pending_close_after_send = 0;
    conn->pending_repost_after_send = 0;
    start_next = 1;
  } else {
    conn->send_inflight = 0;
  }
  pthread_mutex_unlock(&conn->send_lock);
  return start_next;
}

static int post_encoded_message(struct connection *conn)
{
  struct ibv_send_wr wr;
  struct ibv_send_wr *bad_wr = NULL;
  struct ibv_sge sge;
  int result;

  memset(&wr, 0, sizeof(wr));
  memset(&sge, 0, sizeof(sge));
  wr.wr_id = (uintptr_t)conn;
  wr.opcode = IBV_WR_SEND;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.send_flags = IBV_SEND_SIGNALED;
  sge.addr = (uintptr_t)conn->send_frame;
  sge.length = (uint32_t)conn->send_frame_size;
  sge.lkey = conn->send_mr->lkey;
  if (connection_get_reference(conn) != 0) {
    clear_sends(conn);
    return -1;
  }
  pthread_mutex_lock(&conn->lifetime_lock);
  while (!conn->connected && !conn->closing)
    pthread_cond_wait(&conn->lifetime_idle, &conn->lifetime_lock);
  if (conn->closing) {
    pthread_mutex_unlock(&conn->lifetime_lock);
    connection_put_reference(conn);
    clear_sends(conn);
    return -1;
  }
  pthread_mutex_unlock(&conn->lifetime_lock);
  result = ibv_post_send(conn->qp, &wr, &bad_wr);
  if (result != 0) {
    connection_put_reference(conn);
    clear_sends(conn);
  }
  return result;
}

static int submit_encoded_message(struct connection *conn,
                                  const uint8_t *frame, size_t frame_size,
                                  int close_connection, int repost_receive)
{
  int post_now = 0;
  int result = 0;

  if (!frame || frame_size == 0 ||
      frame_size > IS_PROTOCOL_MAX_FRAME_SIZE)
    return -1;
  pthread_mutex_lock(&conn->send_lock);
  if (!conn->send_inflight) {
    memcpy(conn->send_frame, frame, frame_size);
    conn->send_frame_size = frame_size;
    conn->close_after_send = close_connection;
    conn->repost_after_send = repost_receive;
    conn->send_inflight = 1;
    post_now = 1;
  } else if (!conn->pending_send) {
    memcpy(conn->pending_send_frame, frame, frame_size);
    conn->pending_send_frame_size = frame_size;
    conn->pending_close_after_send = close_connection;
    conn->pending_repost_after_send = repost_receive;
    conn->pending_send = 1;
  } else {
    result = -1;
  }
  pthread_mutex_unlock(&conn->send_lock);
  if (post_now && post_encoded_message(conn) != 0)
    result = -1;
  return result;
}

static void send_protocol_error(struct connection *conn,
                                uint64_t request_id,
                                uint16_t offending_type,
                                enum is_protocol_error_code code)
{
  uint8_t frame[IS_PROTOCOL_MAX_FRAME_SIZE];
  size_t frame_size = 0;

  if (is_provider_session_fail(&conn->protocol_session, request_id,
                               offending_type, code, frame,
                               sizeof(frame), &frame_size) != 0 ||
      submit_encoded_message(conn, frame, frame_size, 1, 0) != 0)
    rdma_disconnect(conn->id);
}

int control_chunk_set_matches(
    const uint8_t expected_chunks[MAX_MR_SIZE_GB],
    uint16_t expected_count, const uint32_t response_chunks[],
    uint16_t response_count)
{
  uint8_t seen[MAX_MR_SIZE_GB] = {0};
  uint16_t index;

  if (!expected_chunks || !response_chunks ||
      response_count != expected_count)
    return 0;
  for (index = 0; index < response_count; index++) {
    uint32_t chunk = response_chunks[index];

    if (chunk >= MAX_MR_SIZE_GB || !expected_chunks[chunk] || seen[chunk])
      return 0;
    seen[chunk] = 1;
  }
  return 1;
}

static int translate_request(struct connection *conn,
                             const struct is_protocol_message *request)
{
  size_t index;
  uint32_t response_chunks[MAX_MR_SIZE_GB] = {0};

  memset(&conn->recv_message, 0, sizeof(conn->recv_message));
  conn->active_request_id = request->header.request_id;
  switch (request->header.type) {
  case IS_PROTOCOL_MSG_STATUS_REQUEST:
    conn->recv_message.type = CONTROL_QUERY;
    return 0;
  case IS_PROTOCOL_MSG_CHUNK_REQUEST:
    conn->recv_message.type = CONTROL_BIND;
    conn->recv_message.size_gb = (int)request->payload.chunk_request.chunk_count;
    conn->requested_logical_start = request->payload.chunk_request.logical_start;
    conn->requested_pool = request->payload.chunk_request.pool;
    return 0;
  case IS_PROTOCOL_MSG_ACTIVITY:
    if ((request->header.flags & IS_PROTOCOL_FLAG_RESPONSE) == 0 ||
        conn->pending_evict_request_id == 0 ||
        request->header.request_id != conn->pending_evict_request_id)
      return -1;
    for (index = 0; index < request->payload.activity.chunk_count; index++)
      response_chunks[index] =
          request->payload.activity.chunks[index].provider_chunk_id;
    if (!control_chunk_set_matches(
            conn->pending_evict_chunks, conn->pending_evict_chunk_count,
            response_chunks, request->payload.activity.chunk_count))
      return -1;
    conn->recv_message.type = CONTROL_ACTIVITY;
    conn->recv_message.size_gb = (int)request->payload.activity.chunk_count;
    for (index = 0; index < request->payload.activity.chunk_count; index++) {
      uint32_t chunk = request->payload.activity.chunks[index].provider_chunk_id;

      if (chunk >= MAX_MR_SIZE_GB ||
          !conn->pending_evict_chunks[chunk] ||
          conn->sess_chunk_map[chunk] < 0)
        return -1;
      conn->recv_message.rkey[chunk] = 1;
      conn->recv_message.buf[chunk] =
          request->payload.activity.chunks[index].activity;
    }
    conn->pending_evict_request_id = 0;
    conn->pending_evict_chunk_count = 0;
    memset(conn->pending_evict_chunks, 0,
           sizeof(conn->pending_evict_chunks));
    return 0;
  case IS_PROTOCOL_MSG_RELEASE:
    if ((request->header.flags & IS_PROTOCOL_FLAG_RESPONSE) == 0 ||
        conn->pending_release_request_id == 0 ||
        request->header.request_id != conn->pending_release_request_id)
      return -1;
    if (!control_chunk_set_matches(
            conn->pending_release_chunks, conn->pending_release_chunk_count,
            request->payload.chunk_ids.chunk_ids,
            request->payload.chunk_ids.chunk_count))
      return -1;
    conn->recv_message.type = CONTROL_RELEASE;
    conn->recv_message.size_gb = (int)request->payload.chunk_ids.chunk_count;
    for (index = 0; index < request->payload.chunk_ids.chunk_count; index++) {
      uint32_t chunk = request->payload.chunk_ids.chunk_ids[index];

      if (chunk >= MAX_MR_SIZE_GB ||
          !conn->pending_release_chunks[chunk] ||
          conn->sess_chunk_map[chunk] < 0)
        return -1;
      conn->recv_message.rkey[chunk] = 1;
    }
    conn->pending_release_request_id = 0;
    conn->pending_release_chunk_count = 0;
    memset(conn->pending_release_chunks, 0,
           sizeof(conn->pending_release_chunks));
    return 0;
  case IS_PROTOCOL_MSG_ERROR:
    if (request->payload.error.offending_type == IS_PROTOCOL_MSG_EVICT &&
        conn->pending_evict_request_id != 0 &&
        request->header.request_id == conn->pending_evict_request_id) {
      conn->pending_evict_request_id = 0;
      conn->pending_evict_chunk_count = 0;
      memset(conn->pending_evict_chunks, 0,
             sizeof(conn->pending_evict_chunks));
    } else if (request->payload.error.offending_type ==
                   IS_PROTOCOL_MSG_RELEASE &&
               conn->pending_release_request_id != 0 &&
               request->header.request_id ==
                   conn->pending_release_request_id) {
      conn->pending_release_request_id = 0;
      conn->pending_release_chunk_count = 0;
      memset(conn->pending_release_chunks, 0,
             sizeof(conn->pending_release_chunks));
    } else {
      return -1;
    }
    conn->recv_message.type = CONTROL_PEER_ERROR;
    return 0;
  case IS_PROTOCOL_MSG_GOODBYE:
    conn->recv_message.type = CONTROL_DONE;
    return 0;
  default:
    return -1;
  }
}

static void handle_authenticated_request(struct connection *conn)
{
  switch (conn->recv_message.type) {
  case CONTROL_QUERY:
    atomic_set(&conn->cq_qp_state, CQ_QP_BUSY);
    send_free_mem_size(conn);
    break;
  case CONTROL_BIND:
    atomic_set(&conn->cq_qp_state, CQ_QP_BUSY);
    conn->server_state = S_BIND;
    send_mr(conn, conn->recv_message.size_gb);
    break;
  case CONTROL_ACTIVITY:
    sem_post(&conn->evict_sem);
    break;
  case CONTROL_RELEASE:
    atomic_set(&conn->cq_qp_state, CQ_QP_BUSY);
    recv_done(conn);
    post_receives(conn);
    break;
  case CONTROL_PEER_ERROR:
  case CONTROL_DONE:
    rdma_disconnect(conn->id);
    break;
  default:
    rdma_disconnect(conn->id);
    break;
  }
}

static void disconnect_revoked_consumer(void *context)
{
  struct connection *conn = context;

  rdma_disconnect(conn->id);
}

void on_completion(struct ibv_wc *wc)
{
  struct connection *conn = (struct connection *)(uintptr_t)wc->wr_id;
  int closing;

  pthread_mutex_lock(&conn->lifetime_lock);
  closing = conn->closing;
  pthread_mutex_unlock(&conn->lifetime_lock);
  if (closing)
    goto done;
  if (wc->status != IBV_WC_SUCCESS) {
    rdma_disconnect(conn->id);
    goto done;
  }

  if (wc->opcode == IBV_WC_RECV) {
    struct is_provider_session_outcome outcome;
    uint8_t session_response[IS_PROTOCOL_MAX_FRAME_SIZE];

    if (is_provider_session_handle(
            &conn->protocol_session, conn->recv_frame, wc->byte_len,
            session_response, sizeof(session_response), &outcome) != 0) {
      rdma_disconnect(conn->id);
      goto done;
    }
    if (outcome.response_ready) {
      if (conn->protocol_session.state == IS_PROVIDER_SESSION_READY &&
          !conn->auth_subscribed) {
        enum is_auth_result result;
        enum is_memory_rejection_reason rejection;
        uint32_t max_connections;
        uint32_t max_opportunistic_chunks;
        uint32_t max_committed_chunks;

        result = is_auth_registry_subscribe(
            provider_auth_registry, conn->protocol_session.consumer_id,
            conn->protocol_session.authorization_version,
            disconnect_revoked_consumer, conn);
        if (result != IS_AUTH_OK ||
            is_auth_registry_get_limits(
                provider_auth_registry,
                conn->protocol_session.consumer_id,
                &max_connections,
                &max_opportunistic_chunks,
                &max_committed_chunks) != IS_AUTH_OK ||
            is_memory_manager_connect(
                session.memory_manager,
                conn->protocol_session.consumer_id, conn,
                max_connections, max_opportunistic_chunks,
                max_committed_chunks, &rejection) != IS_MEMORY_OK) {
          if (result == IS_AUTH_OK)
            is_auth_registry_unsubscribe(provider_auth_registry, conn);
          rdma_disconnect(conn->id);
          goto done;
        }
        conn->auth_subscribed = 1;
        conn->memory_connected = 1;
        if (start_connection_liveness(conn) != 0) {
          rdma_disconnect(conn->id);
          goto done;
        }
      }
      if (submit_encoded_message(
              conn, session_response, outcome.response_size,
              outcome.close_after_response,
              !outcome.close_after_response) != 0) {
        rdma_disconnect(conn->id);
        goto done;
      }
      goto done;
    }
    if (!outcome.request_ready || refresh_connection_liveness(conn) != 0) {
      rdma_disconnect(conn->id);
      goto done;
    }
    {
      enum is_auth_result authorization_result = IS_AUTH_OK;
      int authorization_locked = 0;
      int translate_result;

      if (outcome.request.header.type != IS_PROTOCOL_MSG_ERROR) {
        authorization_result = is_auth_registry_begin_authorized_operation(
            provider_auth_registry, conn->protocol_session.consumer_id,
            conn->protocol_session.authorization_version);
        if (authorization_result != IS_AUTH_OK) {
          send_protocol_error(
              conn, outcome.request.header.request_id,
              outcome.request.header.type,
              authorization_result == IS_AUTH_REVOKED
                  ? IS_PROTOCOL_ERROR_REVOKED
                  : IS_PROTOCOL_ERROR_AUTHENTICATION);
          goto done;
        }
        authorization_locked = 1;
      }
      pthread_mutex_lock(&conn->control_lock);
      translate_result = translate_request(conn, &outcome.request);
      pthread_mutex_unlock(&conn->control_lock);
      if (translate_result != 0) {
        if (authorization_locked)
          is_auth_registry_end_authorized_operation(
              provider_auth_registry);
        if (outcome.request.header.type == IS_PROTOCOL_MSG_ERROR)
          rdma_disconnect(conn->id);
        else
          send_protocol_error(conn, outcome.request.header.request_id,
                              outcome.request.header.type,
                              IS_PROTOCOL_ERROR_MALFORMED);
        goto done;
      }
      handle_authenticated_request(conn);
      if (authorization_locked)
        is_auth_registry_end_authorized_operation(provider_auth_registry);
    }
  } else if (wc->opcode == IBV_WC_SEND) {
    int close_connection;
    int repost_receive;
    int start_next_send;

    start_next_send = finish_send(conn, &close_connection, &repost_receive);
    atomic_set(&conn->cq_qp_state, CQ_QP_IDLE);
    if (close_connection) {
      rdma_disconnect(conn->id);
    } else {
      if (repost_receive)
        post_receives(conn);
      if (start_next_send && post_encoded_message(conn) != 0)
        rdma_disconnect(conn->id);
    }
  } else {
    rdma_disconnect(conn->id);
  }
done:
  connection_put_reference(conn);
}

void on_connect(void *context)
{
  struct connection *conn = (struct connection *)context;

  pthread_mutex_lock(&conn->lifetime_lock);
  conn->connected = 1;
  pthread_cond_broadcast(&conn->lifetime_idle);
  pthread_mutex_unlock(&conn->lifetime_lock);
  /*
   * Soft-RoCE may drop receive WRs posted before the QP reaches RTS.
   * Re-arm after ESTABLISHED so the Consumer HELLO is not met with RNR.
   */
  post_receives(conn);
}

void *poll_cq(void *ctx)
{
  struct ibv_cq *cq;
  struct ibv_wc wc;
  int completion_count;

  while (1) {
    TEST_NZ(ibv_get_cq_event(s_ctx->comp_channel, &cq, &ctx));
    ibv_ack_cq_events(cq, 1);
    TEST_NZ(ibv_req_notify_cq(cq, 0));
    while ((completion_count = ibv_poll_cq(cq, 1, &wc)) > 0)
      on_completion(&wc);
    if (completion_count < 0)
      die("could not poll the RDMA completion queue");
  }
  return NULL;
}

void post_receives(struct connection *conn)
{
  struct ibv_recv_wr wr;
  struct ibv_recv_wr *bad_wr = NULL;
  struct ibv_sge sge;

  memset(&wr, 0, sizeof(wr));
  memset(&sge, 0, sizeof(sge));
  wr.wr_id = (uintptr_t)conn;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  sge.addr = (uintptr_t)conn->recv_frame;
  sge.length = IS_PROTOCOL_MAX_FRAME_SIZE;
  sge.lkey = conn->recv_mr->lkey;
  if (connection_get_reference(conn) != 0)
    return;
  if (ibv_post_recv(conn->qp, &wr, &bad_wr) != 0) {
    connection_put_reference(conn);
    rdma_disconnect(conn->id);
  }
}

void register_memory(struct connection *conn)
{
  conn->send_frame = calloc(1, IS_PROTOCOL_MAX_FRAME_SIZE);
  conn->pending_send_frame = calloc(1, IS_PROTOCOL_MAX_FRAME_SIZE);
  conn->recv_frame = calloc(1, IS_PROTOCOL_MAX_FRAME_SIZE);
  TEST_Z(conn->send_frame);
  TEST_Z(conn->pending_send_frame);
  TEST_Z(conn->recv_frame);
  TEST_Z(conn->send_mr = ibv_reg_mr(
      s_ctx->pd, conn->send_frame, IS_PROTOCOL_MAX_FRAME_SIZE,
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ));
  TEST_Z(conn->recv_mr = ibv_reg_mr(
      s_ctx->pd, conn->recv_frame, IS_PROTOCOL_MAX_FRAME_SIZE,
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ));
}

void send_message(struct connection *conn, int repost_receive)
{
  uint8_t frame[IS_PROTOCOL_MAX_FRAME_SIZE];
  size_t frame_size = 0;

  if (encode_control_message(conn, frame, &frame_size) != 0) {
    send_protocol_error(conn, conn->active_request_id,
                        IS_PROTOCOL_MSG_ERROR,
                        IS_PROTOCOL_ERROR_INTERNAL);
    return;
  }
  if (submit_encoded_message(conn, frame, frame_size, 0,
                             repost_receive) != 0)
    rdma_disconnect(conn->id);
}

static void *register_remote_memory(void *context, void *address,
                                    size_t length)
{
  struct ibv_mr *memory_region;

  memory_region = ibv_reg_mr(
      context, address, length,
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
          IBV_ACCESS_REMOTE_READ);
  if (!memory_region)
    return NULL;
  if (!memory_region->addr || memory_region->rkey == 0) {
    (void)ibv_dereg_mr(memory_region);
    return NULL;
  }
  return memory_region;
}

static int deregister_remote_memory(void *context, void *registration)
{
  (void)context;
  return ibv_dereg_mr(registration);
}

void send_mr(void *context, int size)
{
  struct connection *conn = context;
  struct is_memory_registration_adapter registration = {
    .context = s_ctx->pd,
    .register_chunk = register_remote_memory,
    .deregister_chunk = deregister_remote_memory,
  };
  struct is_memory_grant grants[MAX_MR_SIZE_GB];
  enum is_memory_rejection_reason rejection;
  enum is_memory_result result;
  int index;

  pthread_mutex_lock(&conn->control_lock);
  memset(&conn->send_message, 0, sizeof(conn->send_message));
  result = is_memory_manager_acquire(
      session.memory_manager, conn,
      (enum is_memory_pool)conn->requested_pool, (uint32_t)size,
      &rejection, &registration, grants, MAX_MR_SIZE_GB);
  if (result != IS_MEMORY_OK) {
    pthread_mutex_unlock(&conn->control_lock);
    fprintf(stderr, "Remote Memory admission rejected for %s: %s\n",
            conn->protocol_session.consumer_id,
            is_memory_rejection_name(rejection));
    if (result == IS_MEMORY_CLEANUP_FAILED)
      send_protocol_error(conn, conn->active_request_id,
                          IS_PROTOCOL_MSG_CHUNK_REQUEST,
                          IS_PROTOCOL_ERROR_INTERNAL);
    else
      send_protocol_error(conn, conn->active_request_id,
                          IS_PROTOCOL_MSG_CHUNK_REQUEST,
                          IS_PROTOCOL_ERROR_RESOURCE);
    return;
  }

  for (index = 0; index < size; index++) {
    uint32_t chunk_id = grants[index].provider_chunk_id;
    struct ibv_mr *memory_region = grants[index].registration;

    if (chunk_id >= MAX_MR_SIZE_GB || conn->sess_chunk_map[chunk_id] != -1)
      die("memory manager returned an invalid Remote Chunk assignment");
    conn->sess_chunk_map[chunk_id] = (int)chunk_id;
    conn->send_message.buf[chunk_id] =
        (uint64_t)(uintptr_t)memory_region->addr;
    conn->send_message.rkey[chunk_id] = memory_region->rkey;
  }
  conn->mapped_chunk_size += size;
  pthread_mutex_lock(&session_lock);
  session.conns_state[conn->conn_index] = CONN_MAPPED;
  pthread_mutex_unlock(&session_lock);
  conn->send_message.size_gb = size;
  conn->send_message.type = CONTROL_INFO;
  send_message(conn, 1);
  pthread_mutex_unlock(&conn->control_lock);
}

void send_free_mem_size(void *context)
{
  struct connection *conn = context;
  struct is_memory_manager_status status;

  pthread_mutex_lock(&conn->control_lock);
  if (is_memory_manager_get_status(
          session.memory_manager, conn, &status) != IS_MEMORY_OK) {
    pthread_mutex_unlock(&conn->control_lock);
    send_protocol_error(conn, conn->active_request_id,
                        IS_PROTOCOL_MSG_STATUS_REQUEST,
                        IS_PROTOCOL_ERROR_INTERNAL);
    return;
  }
  memset(&conn->send_message, 0, sizeof(conn->send_message));
  conn->send_message.type = CONTROL_FREE_SIZE;
  conn->send_message.size_gb =
      (int)status.available_opportunistic_chunks;
  conn->send_message.committed_size_gb =
      status.available_committed_chunks;
  conn->send_message.status_flags =
      status.healthy ? IS_PROTOCOL_STATUS_HEALTHY : 0;
  send_message(conn, 1);
  pthread_mutex_unlock(&conn->control_lock);
}

int send_stop(void *context, int n)
{
  struct connection *conn = (struct connection *)context;
  int result;

  if (n == 0)
    return 0;
  if (connection_get_reference(conn) != 0)
    return -1;
  pthread_mutex_lock(&conn->control_lock);
  memset(&conn->send_message, 0, sizeof(conn->send_message));
  conn->send_message.type = CONTROL_RELEASE;
  conn->send_message.size_gb = n;
  memcpy(conn->send_message.rkey, conn->release_chunks,
         sizeof(conn->release_chunks));
  send_message(conn, 0);
  pthread_mutex_unlock(&conn->control_lock);
  result = wait_for_control_response(conn, &conn->stop_sem);
  pthread_mutex_lock(&conn->control_lock);
  memset(conn->release_chunks, 0, sizeof(conn->release_chunks));
  conn->release_chunk_count = 0;
  pthread_mutex_unlock(&conn->control_lock);
  connection_put_reference(conn);
  return result;
}

void send_evict(void *context, int n)
{
  struct connection *conn = (struct connection *)context;

  (void)n;
  pthread_mutex_lock(&conn->control_lock);
  memset(&conn->send_message, 0, sizeof(conn->send_message));
  conn->send_message.type = CONTROL_EVICT;
  send_message(conn, 0);
  pthread_mutex_unlock(&conn->control_lock);
}
