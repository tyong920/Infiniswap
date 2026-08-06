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
static void usage(const char *argv0);

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
  while (sigwait(&signals, &signal_number) == 0) {
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

long page_size;
int running;
int main(int argc, char **argv)
{
  struct sockaddr_in6 addr;
  struct rdma_cm_event *event = NULL;
  struct rdma_cm_id *listener = NULL;
  struct rdma_event_channel *ec = NULL;
  struct is_auth_registry auth_registry;
  struct allowlist_reloader reloader;
  sigset_t blocked_signals;
  pthread_t reload_thread;
  uint16_t port = 0;
  pthread_t free_mem_thread;

  if (argc != 4)
    usage(argv[0]);
  page_size = sysconf(_SC_PAGE_SIZE);
  sigemptyset(&blocked_signals);
  sigaddset(&blocked_signals, SIGHUP);
  TEST_NZ(pthread_sigmask(SIG_BLOCK, &blocked_signals, NULL));
  is_auth_registry_init(&auth_registry);
  if (is_auth_registry_load_file(&auth_registry, argv[3]) != IS_AUTH_OK) {
    fprintf(stderr, "could not load the Memory Consumer allowlist\n");
    is_auth_registry_destroy(&auth_registry);
    return 1;
  }
  set_provider_auth_registry(&auth_registry);
  reloader.registry = &auth_registry;
  reloader.path = argv[3];
  TEST_NZ(pthread_create(&reload_thread, NULL, reload_allowlist, &reloader));

  memset(&addr, 0, sizeof(addr));
  addr.sin6_family = AF_INET6;
  inet_pton(AF_INET6, argv[1], &addr.sin6_addr);
  addr.sin6_port = htons(atoi(argv[2]));

  TEST_Z(ec = rdma_create_event_channel());
  TEST_NZ(rdma_create_id(ec, &listener, NULL, RDMA_PS_TCP));
  TEST_NZ(rdma_bind_addr(listener, (struct sockaddr *)&addr));
  TEST_NZ(rdma_listen(listener, 10)); /* backlog=10 is arbitrary */

  port = ntohs(rdma_get_src_port(listener));

  printf("listening on port %d.\n", port);

  //free
  running = 1;
  rdma_session_init(&session);
  TEST_NZ(pthread_create(&free_mem_thread, NULL, free_mem, NULL));

  while (rdma_get_cm_event(ec, &event) == 0) {
    struct rdma_cm_event event_copy;

    memcpy(&event_copy, event, sizeof(*event));
    rdma_ack_cm_event(event);

    if (on_event(&event_copy))
      break;
  }

  running = 0;
  fprintf(stderr, "RDMA event loop terminated unexpectedly\n");
  exit(EXIT_FAILURE);
}

int on_connect_request(struct rdma_cm_id *id)
{
  struct rdma_conn_param cm_params;

  printf("received connection request.\n");
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
  fprintf(stderr, "usage: %s ip port allowlist\n", argv0);
  exit(1);
}
