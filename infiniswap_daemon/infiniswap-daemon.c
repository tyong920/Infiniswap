/*
 * Infiniswap, remote memory paging over RDMA
 * Copyright 2017 University of Michigan, Ann Arbor
 * GPLv2 License
 */

#include "rdma-common.h"
#include "infiniswap_observability_server.h"

#include <errno.h>
#include <signal.h>
#include <time.h>

static int on_connect_request(struct rdma_cm_id *id);
static int on_connection(struct rdma_cm_id *id);
static int on_disconnect(struct rdma_cm_id *id);
static int on_event(struct rdma_cm_event *event);
static void request_shutdown(int signal_number);
static void usage(const char *argv0);

volatile sig_atomic_t running;

struct allowlist_reloader {
  struct is_auth_registry *registry;
  const char *path;
  const char *provider_id;
};

struct provider_snapshot_context {
  const char *provider_id;
};

static int provider_id_valid(const char *provider_id)
{
  size_t index;

  if (!provider_id || !provider_id[0])
    return 0;
  for (index = 0; index <= IS_PROVIDER_OBSERVABILITY_ID_MAX; index++) {
    unsigned char character = (unsigned char)provider_id[index];

    if (character == '\0')
      return 1;
    if (!isalnum(character) && character != '.' && character != '_' &&
        character != '-')
      return 0;
  }
  return 0;
}

static int collect_provider_snapshot(
    void *context, struct is_provider_observability_snapshot *snapshot)
{
  struct provider_snapshot_context *provider = context;

  return provider_observability_snapshot(provider->provider_id, snapshot);
}

static void log_key_reload_audit(const char *provider_id, const char *outcome)
{
  char timestamp[32] = "unknown";
  struct tm utc;
  time_t now = time(NULL);

  if (now >= 0 && gmtime_r(&now, &utc))
    (void)strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
  fprintf(stderr,
          "{\"schema_version\":1,\"timestamp\":\"%s\","
          "\"event\":\"key.configuration-reloaded\","
          "\"subject\":\"%s\",\"details\":{\"outcome\":\"%s\"}}\n",
          timestamp, provider_id, outcome);
}

static void *reload_allowlist(void *context)
{
  struct allowlist_reloader *reloader = context;
  sigset_t signals;
  int signal_number;

  sigemptyset(&signals);
  sigaddset(&signals, SIGHUP);
  while (running && sigwait(&signals, &signal_number) == 0) {
    if (!running)
      break;
    if (signal_number != SIGHUP)
      continue;
    if (is_auth_registry_load_file(reloader->registry, reloader->path) ==
        IS_AUTH_OK)
      log_key_reload_audit(reloader->provider_id, "completed");
    else
      log_key_reload_audit(reloader->provider_id, "failed");
  }
  return NULL;
}

static void request_shutdown(int signal_number)
{
  (void)signal_number;
  running = 0;
}

int main(int argc, char **argv)
{
  struct sockaddr_in6 addr;
  struct rdma_cm_event *event = NULL;
  struct rdma_cm_id *listener = NULL;
  struct rdma_event_channel *ec = NULL;
  struct is_auth_registry auth_registry;
  struct is_memory_manager_config memory_config;
  struct is_memory_accounting_adapter memory_accounting;
  struct is_memory_allocation_adapter memory_allocation;
  struct is_memory_manager *memory_manager;
  struct allowlist_reloader reloader;
  struct provider_snapshot_context snapshot_context;
  struct is_observability_server observability_server;
  struct sigaction shutdown_action;
  sigset_t blocked_signals;
  pthread_t reload_thread;
  char provider_id[IS_PROVIDER_OBSERVABILITY_ID_MAX + 1U];
  const char *configured_provider_id;
  const char *configured_observability_port;
  char *port_end = NULL;
  unsigned long observability_port = 9401;
  uint16_t port = 0;
  pthread_t free_mem_thread;
  int orderly_shutdown;

  if (argc != 5)
    usage(argv[0]);
  if (is_memory_manager_config_load(argv[3], &memory_config) !=
      IS_MEMORY_OK) {
    fprintf(stderr, "could not load Provider memory configuration\n");
    return 1;
  }
  is_memory_linux_accounting_adapter(&memory_accounting);
  is_memory_posix_allocation_adapter(&memory_allocation);
  memory_manager = is_memory_manager_create(
      &memory_config, &memory_accounting, &memory_allocation);
  if (!memory_manager) {
    fprintf(stderr, "could not initialize Provider memory pools\n");
    return 1;
  }
  sigemptyset(&blocked_signals);
  sigaddset(&blocked_signals, SIGHUP);
  TEST_NZ(pthread_sigmask(SIG_BLOCK, &blocked_signals, NULL));
  memset(&shutdown_action, 0, sizeof(shutdown_action));
  shutdown_action.sa_handler = request_shutdown;
  sigemptyset(&shutdown_action.sa_mask);
  TEST_NZ(sigaction(SIGINT, &shutdown_action, NULL));
  TEST_NZ(sigaction(SIGTERM, &shutdown_action, NULL));
  running = 1;
  configured_provider_id = getenv("INFINISWAP_PROVIDER_ID");
  if (configured_provider_id) {
    if (snprintf(provider_id, sizeof(provider_id), "%s",
                 configured_provider_id) >= (int)sizeof(provider_id)) {
      fprintf(stderr, "INFINISWAP_PROVIDER_ID is too long\n");
      (void)is_memory_manager_destroy(memory_manager);
      return 1;
    }
  } else if (gethostname(provider_id, sizeof(provider_id)) != 0 ||
             !memchr(provider_id, '\0', sizeof(provider_id))) {
    fprintf(stderr, "could not determine the Provider identity\n");
    (void)is_memory_manager_destroy(memory_manager);
    return 1;
  }
  if (!provider_id_valid(provider_id)) {
    fprintf(stderr, "INFINISWAP_PROVIDER_ID is invalid\n");
    (void)is_memory_manager_destroy(memory_manager);
    return 1;
  }
  configured_observability_port = getenv("INFINISWAP_OBSERVABILITY_PORT");
  if (configured_observability_port) {
    errno = 0;
    observability_port = strtoul(configured_observability_port, &port_end, 10);
    if (errno != 0 || !port_end || *port_end || observability_port > 65535) {
      fprintf(stderr, "INFINISWAP_OBSERVABILITY_PORT is invalid\n");
      (void)is_memory_manager_destroy(memory_manager);
      return 1;
    }
  }
  is_auth_registry_init(&auth_registry);
  if (is_auth_registry_load_file(&auth_registry, argv[4]) != IS_AUTH_OK) {
    fprintf(stderr, "could not load the Memory Consumer allowlist\n");
    is_auth_registry_destroy(&auth_registry);
    (void)is_memory_manager_destroy(memory_manager);
    return 1;
  }
  set_provider_auth_registry(&auth_registry);
  rdma_session_init(&session, memory_manager);
  snapshot_context.provider_id = provider_id;
  memset(&observability_server, 0, sizeof(observability_server));
  observability_server.listen_fd = -1;
  if (observability_port != 0 &&
      is_observability_server_start(
          &observability_server, "127.0.0.1", (uint16_t)observability_port,
          collect_provider_snapshot, &snapshot_context) != 0) {
    fprintf(stderr, "could not start the Provider observability endpoint\n");
    is_auth_registry_destroy(&auth_registry);
    (void)is_memory_manager_destroy(memory_manager);
    return 1;
  }
  reloader.registry = &auth_registry;
  reloader.path = argv[4];
  reloader.provider_id = provider_id;
  TEST_NZ(pthread_create(&reload_thread, NULL, reload_allowlist, &reloader));

  memset(&addr, 0, sizeof(addr));
  addr.sin6_family = AF_INET6;
  inet_pton(AF_INET6, argv[1], &addr.sin6_addr);
  addr.sin6_port = htons(atoi(argv[2]));

  TEST_Z(ec = rdma_create_event_channel());
  TEST_NZ(rdma_create_id(ec, &listener, NULL, RDMA_PS_TCP));
  TEST_NZ(rdma_bind_addr(listener, (struct sockaddr *)&addr));
  TEST_NZ(rdma_listen(listener, MAX_CLIENT));

  port = ntohs(rdma_get_src_port(listener));

  printf("listening on port %d.\n", port);

  TEST_NZ(pthread_create(&free_mem_thread, NULL, free_mem, &session));

  while (running && rdma_get_cm_event(ec, &event) == 0) {
    struct rdma_cm_event event_copy;

    memcpy(&event_copy, event, sizeof(*event));
    rdma_ack_cm_event(event);

    if (on_event(&event_copy))
      break;
  }

  orderly_shutdown = !running;
  if (!orderly_shutdown)
    fprintf(stderr, "RDMA event loop terminated unexpectedly\n");
  running = 0;
  is_observability_server_stop(&observability_server);
  if (listener) {
    (void)rdma_destroy_id(listener);
    listener = NULL;
  }
  disconnect_provider_connections();
  while (provider_connection_count() != 0 &&
         rdma_get_cm_event(ec, &event) == 0) {
    struct rdma_cm_event event_copy;

    memcpy(&event_copy, event, sizeof(*event));
    rdma_ack_cm_event(event);
    (void)on_event(&event_copy);
    disconnect_provider_connections();
  }
  TEST_NZ(pthread_join(free_mem_thread, NULL));
  TEST_NZ(pthread_kill(reload_thread, SIGHUP));
  TEST_NZ(pthread_join(reload_thread, NULL));
  if (provider_connection_count() != 0) {
    fprintf(stderr, "could not drain Provider connections during shutdown\n");
    return 1;
  }
  rdma_destroy_event_channel(ec);
  if (is_memory_manager_destroy(memory_manager) != IS_MEMORY_OK) {
    fprintf(stderr, "could not release Provider memory pools\n");
    is_auth_registry_destroy(&auth_registry);
    return 1;
  }
  is_auth_registry_destroy(&auth_registry);
  return orderly_shutdown ? 0 : 1;
}

int on_connect_request(struct rdma_cm_id *id)
{
  struct rdma_conn_param cm_params;

  printf("received connection request.\n");
  if (!running) {
    (void)rdma_reject(id, NULL, 0);
    rdma_destroy_id(id);
    return 0;
  }
  if (build_connection(id) != 0) {
    (void)rdma_reject(id, NULL, 0);
    rdma_destroy_id(id);
    return 0;
  }
  build_params(&cm_params);
  TEST_NZ(rdma_accept(id, &cm_params));

  return 0;
}

int on_connection(struct rdma_cm_id *id)
{
  on_connect(id->context);

  printf("connection established; awaiting authenticated HELLO\n");

  return 0;
}

int on_disconnect(struct rdma_cm_id *id)
{
  printf("peer disconnected.\n");

  destroy_connection(id->context);
  return 0;
}

int on_event(struct rdma_cm_event *event)
{
  int r = 0;

  if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST)
    r = on_connect_request(event->id);
  else if (event->event == RDMA_CM_EVENT_ESTABLISHED)
    r = on_connection(event->id);
  else if (event->event == RDMA_CM_EVENT_DISCONNECTED)
    r = on_disconnect(event->id);
  else
    die("on_event: unknown event.");

  return r;
}

void usage(const char *argv0)
{
  fprintf(stderr,
          "usage: %s ip port provider-memory-config allowlist\n", argv0);
  exit(1);
}
