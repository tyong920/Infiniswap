/*
 * Infiniswap, remote memory paging over RDMA
 * Copyright 2017 University of Michigan, Ann Arbor
 * GPLv2 License
 */
#include "rdma-common.h"

#include <errno.h>
#include <openssl/rand.h>
#include <time.h>

extern long page_size;
extern int running;

static void build_context(struct ibv_context *verbs);
static void build_qp_attr(struct ibv_qp_init_attr *qp_attr);
static void *monitor_connection_deadlines(void *context);
static void on_completion(struct ibv_wc *);
static void * poll_cq(void *);
static void post_receives(struct connection *conn);
static void register_memory(struct connection *conn);
static int deregister_remote_memory(struct ibv_mr *memory_region);
static void send_message(struct connection *conn, int repost_receive);

struct rdma_session session;

char free_mem_cmd[39] = "vmstat -s | awk 'FNR == 5 {printf $1}'";
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
  if (conn->closing && conn->references == 0)
    pthread_cond_signal(&conn->lifetime_idle);
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

static void *monitor_connection_deadlines(void *context)
{
  struct connection *conn = context;
  struct timespec deadline;
  time_t now;
  int result = 0;
  int disconnect = 0;

  if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
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
  if (!conn->closing && conn->handshake_complete &&
      conn->protocol_session.authenticated_until_unix != 0) {
    now = time(NULL);
    if (now < 0 || (uint64_t)now >=
                       conn->protocol_session.authenticated_until_unix) {
      disconnect = 1;
    } else if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
      disconnect = 1;
    } else {
      deadline.tv_sec += (time_t)(
          conn->protocol_session.authenticated_until_unix - (uint64_t)now);
      result = 0;
      while (!conn->closing && result == 0)
        result = pthread_cond_timedwait(&conn->lifetime_idle,
                                        &conn->lifetime_lock, &deadline);
      if (!conn->closing && result == ETIMEDOUT)
        disconnect = 1;
    }
  }
  pthread_mutex_unlock(&conn->lifetime_lock);
out:
  if (disconnect)
    rdma_disconnect(conn->id);
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

void die(const char *reason)
{
  fprintf(stderr, "%s\n", reason);
  exit(EXIT_FAILURE);
}

long get_free_mem(void)
{
  char result[60];
  FILE *fd = fopen("/proc/meminfo", "r");
  int i;
  long res = 0;

  if (!fd)
    return 0;
  if (!fgets(result, sizeof(result), fd) ||
      !fgets(result, sizeof(result), fd)) {
    fclose(fd);
    return 0;
  }
  for (i = 0; result[i] != '\0'; i++) {
    if (result[i] >= '0' && result[i] <= '9') {
      res *= 10;
      res += result[i] - '0';
    }
  }
  fclose(fd);
  return res;
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
  conn->free_mem_gb = 0;

  sem_init(&conn->stop_sem, 0, 0);
  sem_init(&conn->evict_sem, 0, 0);
  conn->sess = &session;
  for (i = 0; i < MAX_FREE_MEM_GB; i++){
    conn->sess_chunk_map[i] = -1;
  }
  conn->mapped_chunk_size = 0;
  conn->next_provider_request_id = 1;
  is_provider_session_init(
      &conn->protocol_session, provider_auth_registry,
      IS_PROTOCOL_MINOR_CURRENT,
      IS_PROTOCOL_CAP_BACKED | IS_PROTOCOL_CAP_OPPORTUNISTIC_POOL |
          IS_PROTOCOL_CAP_FAILURE_DEADLINE | IS_PROTOCOL_CAP_STATUS |
          IS_PROTOCOL_CAP_AUTH_HMAC_SHA256,
      IS_PROTOCOL_CAP_AUTH_HMAC_SHA256, provider_nonce, session_id);
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
  TEST_Z(s_ctx->cq = ibv_create_cq(s_ctx->ctx, 10, NULL, s_ctx->comp_channel, 0)); /* cqe=10 is arbitrary */
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

void destroy_connection(void *context)
{
  struct connection *conn = (struct connection *)context;
  struct ibv_qp_attr qp_attr;
  struct ibv_qp_init_attr qp_init_attr;

  pthread_mutex_lock(&conn->lifetime_lock);
  conn->closing = 1;
  pthread_cond_broadcast(&conn->lifetime_idle);
  pthread_mutex_unlock(&conn->lifetime_lock);
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
  pthread_mutex_lock(&conn->lifetime_lock);
  while (conn->references != 0)
    pthread_cond_wait(&conn->lifetime_idle, &conn->lifetime_lock);
  pthread_mutex_unlock(&conn->lifetime_lock);
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

  pthread_mutex_lock(&session_lock);
  if (release_connection_remote_chunks(
          conn, &session, deregister_remote_memory) != 0) {
    pthread_mutex_unlock(&session_lock);
    die("could not release registered Remote Memory");
  }
  session.conns[conn->conn_index] = NULL;
  session.conns_state[conn->conn_index] = CONN_IDLE;
  session.conn_num -= 1;
  if (session.conn_num == 0){
    running = 0;
  }
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

void * get_serving_mem_region(void *context)
{
  return ((struct connection *)context)->rdma_remote_region;
}

void rdma_session_init(struct rdma_session *sess){
  int free_mem_g;
  int i;

  free_mem_g = (int)(get_free_mem() / ONE_MB);
  printf("%s, get free_mem %d\n", __func__, free_mem_g);
  for (i=0; i<MAX_FREE_MEM_GB; i++) {
    sess->rdma_remote.conn_map[i] = -1;
    sess->rdma_remote.conn_chunk_map[i] = -1;
    sess->rdma_remote.malloc_map[i] = CHUNK_EMPTY;
  }

  if (free_mem_g > FREE_MEM_EXPAND_THRESHOLD){
    free_mem_g -= (FREE_MEM_EVICT_THRESHOLD + FREE_MEM_EXPAND_THRESHOLD) / 2;
  } else if (free_mem_g > FREE_MEM_EVICT_THRESHOLD){
    free_mem_g  -= FREE_MEM_EVICT_THRESHOLD;
  }else{
    free_mem_g = 0;
  }
  if (free_mem_g > MAX_FREE_MEM_GB) {
    free_mem_g = MAX_FREE_MEM_GB;
  }

  for (i=0; i < free_mem_g; i++){
    if (posix_memalign((void **)&(sess->rdma_remote.region_list[i]),
                       page_size, ONE_GB) != 0)
      die("could not allocate a Remote Memory chunk");
    memset(sess->rdma_remote.region_list[i], 0x00, ONE_GB);
    sess->rdma_remote.malloc_map[i] = CHUNK_MALLOCED;
  }
  sess->rdma_remote.size_gb = free_mem_g;
  sess->rdma_remote.mapped_size = 0;

  for (i=0; i<MAX_CLIENT; i++){
    sess->conns[i] = NULL;
    sess->conns_state[i] = CONN_IDLE;
  }
  sess->conn_num = 0;

  printf("%s, allocated mem %d\n", __func__, sess->rdma_remote.size_gb);

}

void evict_mem(int stop_g)
{
  int i, j, k, n, m;
  int freed_g = 0;
  int evict_g = stop_g;
  struct connection *conn;
  int avail_chunk;
  int random_chunk_select[MAX_FREE_MEM_GB];
  int send_list[MAX_CLIENT];
  int reference_held[MAX_CLIENT];
  struct connection *held_connections[MAX_CLIENT];
  unsigned int random_num;
  int conn_index;
  int session_locked = 0;
  struct chunk_activity tmp_activity;
  int chunk_index;


  srand((unsigned)time(NULL));

  pthread_mutex_lock(&session_lock);
  session_locked = 1;
  printf("need to evict %d GB\n", evict_g);
  //free unmapped chunk
  for (i = 0; i < MAX_FREE_MEM_GB ;i++) {
    if (session.rdma_remote.malloc_map[i] == CHUNK_MALLOCED && session.rdma_remote.conn_map[i] == -1){
      free(session.rdma_remote.region_list[i]);
      session.rdma_remote.malloc_map[i] = CHUNK_EMPTY;
      freed_g += 1;
      if (freed_g == evict_g){
        session.rdma_remote.size_gb -= evict_g;
        printf("free unmapped chunk %d\n", freed_g);
        pthread_mutex_unlock(&session_lock);
        return;
      }
    }
  }
  //not enough
  session.rdma_remote.size_gb -= freed_g;
  evict_g -= freed_g;

  //get availe_conn
  avail_chunk = MAX_FREE_MEM_GB;
  for (i=0; i<MAX_CLIENT; i++){
    send_list[i] = -1;
    reference_held[i] = 0;
    held_connections[i] = NULL;
  }
  for (i = 0; i < MAX_FREE_MEM_GB; i++) {
    if (session.rdma_remote.conn_map[i] == -1) { // unmapped chunk
      avail_chunk -= 1;
      random_chunk_select[i] = -1; //can't select
    }else {
      random_chunk_select[i] = 0; //can select
    }
  }

  if (avail_chunk != session.rdma_remote.mapped_size){
    printf("%s, avail_chunk %d, mapped_size %d", __func__, avail_chunk, session.rdma_remote.mapped_size);
  }

  j = 0;  
  // evict_g += EXTRA_CHUNK_NUM;
  if (session.rdma_remote.mapped_size < (evict_g + EXTRA_CHUNK_NUM)){ //not enough
    if (session.rdma_remote.mapped_size < evict_g){
      evict_g = session.rdma_remote.mapped_size;
    }
    //send evict to all mapped cb
    for (i=0; i<MAX_CLIENT; i++){
      if (session.conns_state[i] == CONN_MAPPED){
        send_list[i] = 1; 
        j += session.conns[i]->mapped_chunk_size;
      }
    }  

    if (session.rdma_remote.mapped_size != j){
      printf("%s, error j %d, total mapped_size %d\n", __func__, j, session.rdma_remote.mapped_size);
    }
    j = evict_g;
  }else {
    printf(" mapped_size %d >= evict_g %d + EXTRA_CHUNK_NUM\n", session.rdma_remote.mapped_size, evict_g);
    for (j = 0; j < (evict_g + EXTRA_CHUNK_NUM); j++){
      random_num = rand() % MAX_FREE_MEM_GB;
      while (random_chunk_select[random_num] != 0){ //unmapped or selected
        random_num += 1;
        random_num %= MAX_FREE_MEM_GB;
      }
      random_chunk_select[random_num] = 1;
      send_list[session.rdma_remote.conn_map[random_num]] = 1; //send msg to this client
    } 
    j = evict_g;
    printf("evict_g %d\n", evict_g);
  } 
  printf("%s, selected chunk is %d\n", __func__, j);

  k = 0;
  for (i=0; i< MAX_CLIENT; i++){
    printf("i = %d ", i);
    if (send_list[i] == 1){
      k += session.conns[i]->mapped_chunk_size;
      printf("k is %d\n", k);
    }
  }
  printf("%s, total selected chunk is %d\n", __func__, k);
  session.evict_list = (struct chunk_activity *)malloc(sizeof(struct chunk_activity) * k);

  for (i=0; i< MAX_CLIENT; i++){
    if (send_list[i] == 1){
      conn = session.conns[i];
      if (!conn || connection_get_reference(conn) != 0)
        goto abort_evict;
      held_connections[i] = conn;
      reference_held[i] = 1;
    }
  }
  pthread_mutex_unlock(&session_lock);
  session_locked = 0;
  for (i = 0; i < MAX_CLIENT; i++) {
    if (reference_held[i]) {
      printf("%s, send evict to conn[%d]\n", __func__, i);
      send_evict(held_connections[i], j);
    }
  }
  n = 0;
  for (i=0; i<MAX_CLIENT; i++){
    if (send_list[i] == 1){
      conn = held_connections[i];
      if (wait_for_control_response(conn, &conn->evict_sem) != 0)
        goto abort_evict;
      memset(conn->release_chunks, 0, sizeof(conn->release_chunks));
      conn->release_chunk_count = 0;
      for (m=0; m<MAX_MR_SIZE_GB; m++){
        if (conn->recv_message.rkey[m]){
          session.evict_list[n].activity = conn->recv_message.buf[m];
          session.evict_list[n].chunk_index = m;
          n += 1;
        }
      }
      post_receives(conn);
    }
  }
  if (n != k){
    printf("%s, received bitmap_info %d is not total_chunk %d\n", __func__, n, k);
  } 

  for (n=0; n < j; n++){//need evict chunk
    for (m=n+1; m < k; m++){ //total sorted chunk
      if (session.evict_list[n].activity > session.evict_list[m].activity){
        tmp_activity.activity = session.evict_list[n].activity; 
        tmp_activity.chunk_index = session.evict_list[n].chunk_index; 
        session.evict_list[n].activity = session.evict_list[m].activity;
        session.evict_list[n].chunk_index = session.evict_list[m].chunk_index;
        session.evict_list[m].activity = tmp_activity.activity;
        session.evict_list[m].chunk_index = tmp_activity.chunk_index;
      }
    }
    chunk_index = session.evict_list[n].chunk_index;
    conn_index = session.rdma_remote.conn_map[chunk_index];
    if (send_list[conn_index] == -1){
      printf("%s, send_list[%d] is -1 \n", __func__, conn_index);
    }
    if (send_list[conn_index] == 1){
      send_list[conn_index] = 2;
    }
    held_connections[conn_index]->release_chunks[chunk_index] = 1;
    held_connections[conn_index]->release_chunk_count++;
  } 

  for (i=0; i<MAX_CLIENT; i++){
    int stop_result;

    if (send_list[i] == 2)
      stop_result = send_stop(held_connections[i],
                              held_connections[i]->release_chunk_count);
    else if (send_list[i] == 1)
      stop_result = send_stop(held_connections[i], 0);
    else
      continue;
    if (stop_result != 0)
      goto abort_evict;
    connection_put_reference(held_connections[i]);
    reference_held[i] = 0;
  }
  free(session.evict_list);
  session.evict_list = NULL;
  return;

abort_evict:
  if (session_locked)
    pthread_mutex_unlock(&session_lock);
  for (i = 0; i < MAX_CLIENT; i++) {
    if (reference_held[i])
      connection_put_reference(held_connections[i]);
  }
  free(session.evict_list);
  session.evict_list = NULL;

}

void *free_mem(void *data)
{
  int free_mem_g = 0;
  int last_free_mem_g;
  int filtered_free_mem_g = 0;
  int evict_hit_count = 0;
  int expand_hit_count = 0;
  float last_free_mem_weight = 1 - CURR_FREE_MEM_WEIGHT;
  int stop_size_g;
  int expand_size_g;
  int expanding_chunks[MAX_FREE_MEM_GB];
  int expanding_count;
  int i, j;

  (void)data;
  last_free_mem_g = (int)(get_free_mem() / ONE_MB);
  printf("%s, is called, last %d GB, weight: %f, %f\n", __func__, last_free_mem_g, (float)(CURR_FREE_MEM_WEIGHT), last_free_mem_weight); 

  while (running) {// server is working
    free_mem_g = (int)(get_free_mem() / ONE_MB);
    //need a filter
    filtered_free_mem_g = (int)(CURR_FREE_MEM_WEIGHT * free_mem_g + last_free_mem_g * last_free_mem_weight); 
    last_free_mem_g = filtered_free_mem_g;
    if (filtered_free_mem_g < FREE_MEM_EVICT_THRESHOLD){
      evict_hit_count += 1;
      expand_hit_count = 0;
      if (evict_hit_count >= MEM_EVICT_HIT_THRESHOLD){
        evict_hit_count = 0;
        //evict  down_threshold - free_mem
        stop_size_g = FREE_MEM_EVICT_THRESHOLD - last_free_mem_g;
        printf(", evict %d GB ", stop_size_g);
        pthread_mutex_lock(&session_lock);
        if (session.rdma_remote.size_gb < stop_size_g)
          stop_size_g = session.rdma_remote.size_gb;
        pthread_mutex_unlock(&session_lock);
        if (stop_size_g > 0){ //stop_size_g has to be meaningful.
          evict_mem(stop_size_g);
        }
        last_free_mem_g += stop_size_g;
      }
    }else if (filtered_free_mem_g > FREE_MEM_EXPAND_THRESHOLD) {
      expand_hit_count += 1;
      evict_hit_count = 0;
      if (expand_hit_count >= MEM_EXPAND_HIT_THRESHOLD){
        expand_hit_count = 0;
        expand_size_g =  last_free_mem_g - FREE_MEM_EXPAND_THRESHOLD;
        expanding_count = 0;
        pthread_mutex_lock(&session_lock);
        if ((expand_size_g + session.rdma_remote.size_gb) > MAX_FREE_MEM_GB)
          expand_size_g = MAX_FREE_MEM_GB - session.rdma_remote.size_gb;
        for (i = 0; i < MAX_FREE_MEM_GB &&
                    expanding_count < expand_size_g; i++) {
          if (session.rdma_remote.malloc_map[i] != CHUNK_EMPTY)
            continue;
          session.rdma_remote.malloc_map[i] = CHUNK_ALLOCATING;
          expanding_chunks[expanding_count++] = i;
        }
        pthread_mutex_unlock(&session_lock);

        for (j = 0; j < expanding_count; j++) {
          i = expanding_chunks[j];
          if (posix_memalign((void **)&session.rdma_remote.region_list[i],
                             page_size, ONE_GB) != 0)
            die("could not allocate a Remote Memory chunk");
          memset(session.rdma_remote.region_list[i], 0x00, ONE_GB);
        }

        pthread_mutex_lock(&session_lock);
        for (j = 0; j < expanding_count; j++)
          session.rdma_remote.malloc_map[expanding_chunks[j]] =
              CHUNK_MALLOCED;
        session.rdma_remote.size_gb += expanding_count;
        pthread_mutex_unlock(&session_lock);
        last_free_mem_g -= expanding_count;
      }
    }
    // printf("\n"); 
    sleep(1); 
  }
  return NULL;
}

void recv_done(struct connection *conn)
{
  int evict_g = conn->recv_message.size_gb;
  int i;
  int released = 0;

  pthread_mutex_lock(&session_lock);
  for (i = 0; i < MAX_MR_SIZE_GB; i++) {
    int index;

    if (!conn->recv_message.rkey[i])
      continue;
    index = conn->sess_chunk_map[i];
    if (index < 0 || index >= MAX_FREE_MEM_GB)
      continue;
    conn->sess_chunk_map[i] = -1;
    if (session.rdma_remote.mr_list[index]) {
      deregister_memory(session.rdma_remote.mr_list[index]);
      session.rdma_remote.mr_list[index] = NULL;
    }
    free(session.rdma_remote.region_list[index]);
    session.rdma_remote.region_list[index] = NULL;
    session.rdma_remote.conn_map[index] = -1;
    session.rdma_remote.malloc_map[index] = CHUNK_EMPTY;
    session.rdma_remote.conn_chunk_map[index] = -1;
    released++;
  }
  if (released != evict_g)
    fprintf(stderr, "release count mismatch\n");
  session.rdma_remote.size_gb -= released;
  session.rdma_remote.mapped_size -= released;
  conn->mapped_chunk_size -= released;
  is_provider_session_release_chunks(&conn->protocol_session,
                                     (uint32_t)released);
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
    message.payload.status.available_committed_chunks = 0;
    message.payload.status.provider_failure_deadline_ms =
        conn->protocol_session.failure_deadline_ms;
    message.payload.status.flags = 0;
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

int release_connection_remote_chunks(
    struct connection *conn, struct rdma_session *provider_session,
    remote_memory_deregister_fn deregister_region)
{
  int local_chunk;
  int released = 0;
  int cleanup_failed = 0;

  if (!conn || !provider_session || !deregister_region)
    return -1;
  for (local_chunk = 0; local_chunk < MAX_MR_SIZE_GB; local_chunk++) {
    int provider_chunk = conn->sess_chunk_map[local_chunk];

    if (provider_chunk == -1)
      continue;
    if (provider_chunk < 0 || provider_chunk >= MAX_FREE_MEM_GB) {
      cleanup_failed = 1;
      continue;
    }
    if (provider_session->rdma_remote.mr_list[provider_chunk] &&
        deregister_region(
            provider_session->rdma_remote.mr_list[provider_chunk]) != 0) {
      cleanup_failed = 1;
      continue;
    }
    provider_session->rdma_remote.mr_list[provider_chunk] = NULL;
    provider_session->rdma_remote.conn_map[provider_chunk] = -1;
    provider_session->rdma_remote.conn_chunk_map[provider_chunk] = -1;
    conn->sess_chunk_map[local_chunk] = -1;
    released++;
  }
  if (released > provider_session->rdma_remote.mapped_size ||
      released > conn->mapped_chunk_size)
    return -1;
  provider_session->rdma_remote.mapped_size -= released;
  conn->mapped_chunk_size -= released;
  return cleanup_failed ? -1 : 0;
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
    pthread_mutex_lock(&session_lock);
    session.conns_state[conn->conn_index] = CONN_MAPPED;
    pthread_mutex_unlock(&session_lock);
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
        enum is_auth_result result = is_auth_registry_subscribe(
            provider_auth_registry, conn->protocol_session.consumer_id,
            conn->protocol_session.authorization_version,
            disconnect_revoked_consumer, conn);

        if (result != IS_AUTH_OK) {
          rdma_disconnect(conn->id);
          goto done;
        }
        conn->auth_subscribed = 1;
        pthread_mutex_lock(&conn->lifetime_lock);
        conn->handshake_complete = 1;
        pthread_cond_broadcast(&conn->lifetime_idle);
        pthread_mutex_unlock(&conn->lifetime_lock);
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
    if (!outcome.request_ready) {
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

static struct ibv_mr *register_remote_memory(
    struct ibv_pd *protection_domain, void *address, size_t length,
    int access)
{
  return ibv_reg_mr(protection_domain, address, length, access);
}

static int deregister_remote_memory(struct ibv_mr *memory_region)
{
  return ibv_dereg_mr(memory_region);
}

int register_remote_chunks(
    struct connection *conn, struct rdma_session *provider_session,
    struct ibv_pd *protection_domain, int requested_chunks,
    remote_memory_register_fn register_region,
    remote_memory_deregister_fn deregister_region)
{
  int index;
  int granted = 0;
  int cleanup_failed = 0;
  uint8_t registered_chunks[MAX_FREE_MEM_GB] = {0};

  if (!conn || !provider_session || !register_region ||
      !deregister_region || requested_chunks <= 0)
    return -1;
  memset(&conn->send_message, 0, sizeof(conn->send_message));
  for (index = 0;
       index < MAX_FREE_MEM_GB && granted < requested_chunks; index++) {
    if (provider_session->rdma_remote.malloc_map[index] != CHUNK_MALLOCED ||
        provider_session->rdma_remote.conn_map[index] != -1)
      continue;
    conn->sess_chunk_map[index] = index;
    provider_session->rdma_remote.conn_map[index] = conn->conn_index;
    provider_session->rdma_remote.conn_chunk_map[index] = index;
    provider_session->rdma_remote.mr_list[index] = register_region(
        protection_domain, provider_session->rdma_remote.region_list[index],
        ONE_GB, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                    IBV_ACCESS_REMOTE_READ);
    if (!provider_session->rdma_remote.mr_list[index]) {
      provider_session->rdma_remote.conn_map[index] = -1;
      provider_session->rdma_remote.conn_chunk_map[index] = -1;
      conn->sess_chunk_map[index] = -1;
      break;
    }
    registered_chunks[index] = 1;
    if (!provider_session->rdma_remote.mr_list[index]->addr ||
        provider_session->rdma_remote.mr_list[index]->rkey == 0)
      break;
    conn->send_message.buf[index] = (uint64_t)(uintptr_t)
        provider_session->rdma_remote.mr_list[index]->addr;
    conn->send_message.rkey[index] =
        provider_session->rdma_remote.mr_list[index]->rkey;
    granted++;
  }
  if (granted == requested_chunks)
    return granted;

  for (index = 0; index < MAX_FREE_MEM_GB; index++) {
    if (!registered_chunks[index])
      continue;
    if (deregister_region(provider_session->rdma_remote.mr_list[index]) != 0)
      cleanup_failed = 1;
    provider_session->rdma_remote.mr_list[index] = NULL;
    provider_session->rdma_remote.conn_map[index] = -1;
    provider_session->rdma_remote.conn_chunk_map[index] = -1;
    conn->sess_chunk_map[index] = -1;
    conn->send_message.rkey[index] = 0;
    conn->send_message.buf[index] = 0;
  }
  return cleanup_failed ? -2 : -1;
}

void send_mr(void *context, int size)
{
  struct connection *conn = (struct connection *)context;
  int granted;

  pthread_mutex_lock(&conn->control_lock);
  pthread_mutex_lock(&session_lock);
  granted = register_remote_chunks(
      conn, &session, s_ctx->pd, size,
      register_remote_memory, deregister_remote_memory);
  if (granted < 0) {
    pthread_mutex_unlock(&session_lock);
    pthread_mutex_unlock(&conn->control_lock);
    if (granted == -2)
      die("could not roll back Remote Memory registration");
    send_protocol_error(conn, conn->active_request_id,
                        IS_PROTOCOL_MSG_CHUNK_REQUEST,
                        IS_PROTOCOL_ERROR_RESOURCE);
    return;
  }
  session.rdma_remote.mapped_size += granted;
  conn->mapped_chunk_size += granted;
  pthread_mutex_unlock(&session_lock);
  conn->send_message.size_gb = granted;
  conn->send_message.type = CONTROL_INFO;
  send_message(conn, 1);
  pthread_mutex_unlock(&conn->control_lock);
}

void send_free_mem_size(void *context)
{
  struct connection *conn = (struct connection *)context;

  pthread_mutex_lock(&conn->control_lock);
  pthread_mutex_lock(&session_lock);
  memset(&conn->send_message, 0, sizeof(conn->send_message));
  conn->send_message.type = CONTROL_FREE_SIZE;
  conn->send_message.size_gb =
      session.rdma_remote.size_gb - session.rdma_remote.mapped_size;
  pthread_mutex_unlock(&session_lock);
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
