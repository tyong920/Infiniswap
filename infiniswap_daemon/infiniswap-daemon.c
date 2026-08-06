/*
 * Infiniswap, remote memory paging over RDMA
 * Copyright 2017 University of Michigan, Ann Arbor
 * GPLv2 License
 */

#include "rdma-common.h"

#include <signal.h>

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
};

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
      fprintf(stderr, "reloaded the Memory Consumer allowlist\n");
    else
      fprintf(stderr, "could not reload the Memory Consumer allowlist\n");
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
  struct sigaction shutdown_action;
  sigset_t blocked_signals;
  pthread_t reload_thread;
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
  is_auth_registry_init(&auth_registry);
  if (is_auth_registry_load_file(&auth_registry, argv[4]) != IS_AUTH_OK) {
    fprintf(stderr, "could not load the Memory Consumer allowlist\n");
    is_auth_registry_destroy(&auth_registry);
    (void)is_memory_manager_destroy(memory_manager);
    return 1;
  }
  set_provider_auth_registry(&auth_registry);
  reloader.registry = &auth_registry;
  reloader.path = argv[4];
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

  rdma_session_init(&session, memory_manager);
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
