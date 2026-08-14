/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#ifndef INFINISWAP_OBSERVABILITY_SERVER_H
#define INFINISWAP_OBSERVABILITY_SERVER_H

#include "infiniswap_observability.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

typedef int (*is_provider_snapshot_callback)(
    void *context, struct is_provider_observability_snapshot *snapshot);

struct is_observability_server {
  pthread_t thread;
  int listen_fd;
  _Atomic int running;
  uint16_t port;
  is_provider_snapshot_callback snapshot;
  void *snapshot_context;
};

int is_observability_server_start(
    struct is_observability_server *server, const char *address, uint16_t port,
    is_provider_snapshot_callback snapshot, void *snapshot_context);

void is_observability_server_stop(struct is_observability_server *server);

#endif /* INFINISWAP_OBSERVABILITY_SERVER_H */
