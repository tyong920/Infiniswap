/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#include "infiniswap_observability_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define IS_OBSERVABILITY_RESPONSE_MAX 262144U
#define IS_OBSERVABILITY_REQUEST_MAX 1024U

static int send_all(int socket_fd, const char *data, size_t size)
{
  size_t sent = 0;

  while (sent < size) {
    ssize_t result = send(socket_fd, data + sent, size - sent, MSG_NOSIGNAL);

    if (result < 0 && errno == EINTR)
      continue;
    if (result <= 0)
      return -1;
    sent += (size_t)result;
  }
  return 0;
}

static void send_response(int client, int status, const char *content_type,
                          const char *body)
{
  char header[512];
  size_t body_size = strlen(body);
  int header_size = snprintf(
      header, sizeof(header),
      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
      "Connection: close\r\nCache-Control: no-store\r\n\r\n",
      status, status == 200 ? "OK" : status == 404 ? "Not Found" :
                                                    "Service Unavailable",
      content_type, body_size);

  if (header_size <= 0 || (size_t)header_size >= sizeof(header))
    return;
  if (send_all(client, header, (size_t)header_size) == 0)
    (void)send_all(client, body, body_size);
}

static int read_request(int client, char request[IS_OBSERVABILITY_REQUEST_MAX])
{
  size_t used = 0;

  while (used + 1U < IS_OBSERVABILITY_REQUEST_MAX) {
    ssize_t result = recv(client, request + used,
                          IS_OBSERVABILITY_REQUEST_MAX - used - 1U, 0);

    if (result < 0 && errno == EINTR)
      continue;
    if (result <= 0)
      return -1;
    used += (size_t)result;
    request[used] = '\0';
    if (strstr(request, "\r\n\r\n"))
      return 0;
  }
  return -1;
}

static void serve_client(struct is_observability_server *server, int client)
{
  struct is_provider_observability_snapshot snapshot;
  char request[IS_OBSERVABILITY_REQUEST_MAX];
  char *body;
  enum is_observability_result render_result;

  if (read_request(client, request) != 0)
    return;
  if (strncmp(request, "GET /", 5) != 0) {
    send_response(client, 404, "text/plain; charset=utf-8", "not found\n");
    return;
  }
  if (server->snapshot(server->snapshot_context, &snapshot) != 0) {
    send_response(client, 503, "text/plain; charset=utf-8",
                  "snapshot unavailable\n");
    return;
  }
  if (strncmp(request, "GET /healthz HTTP/1.", 20) == 0) {
    send_response(client, snapshot.healthy ? 200 : 503,
                  "text/plain; charset=utf-8",
                  snapshot.healthy ? "healthy\n" : "degraded\n");
    return;
  }
  body = malloc(IS_OBSERVABILITY_RESPONSE_MAX);
  if (!body) {
    send_response(client, 503, "text/plain; charset=utf-8",
                  "render unavailable\n");
    return;
  }
  if (strncmp(request, "GET /status HTTP/1.", 19) == 0) {
    render_result = is_provider_observability_render_status(
        &snapshot, body, IS_OBSERVABILITY_RESPONSE_MAX);
    if (render_result == IS_OBSERVABILITY_OK)
      send_response(client, 200, "application/json; charset=utf-8", body);
    else
      send_response(client, 503, "text/plain; charset=utf-8",
                    "render unavailable\n");
  } else if (strncmp(request, "GET /metrics HTTP/1.", 20) == 0) {
    render_result = is_provider_observability_render_metrics(
        &snapshot, body, IS_OBSERVABILITY_RESPONSE_MAX);
    if (render_result == IS_OBSERVABILITY_OK)
      send_response(client, 200,
                    "application/openmetrics-text; version=1.0.0; charset=utf-8",
                    body);
    else
      send_response(client, 503, "text/plain; charset=utf-8",
                    "render unavailable\n");
  } else {
    send_response(client, 404, "text/plain; charset=utf-8", "not found\n");
  }
  free(body);
}

static void *run_server(void *context)
{
  struct is_observability_server *server = context;

  while (atomic_load(&server->running)) {
    int client = accept(server->listen_fd, NULL, NULL);

    if (client < 0) {
      if (errno == EINTR)
        continue;
      if (!atomic_load(&server->running))
        break;
      continue;
    }
    serve_client(server, client);
    close(client);
  }
  return NULL;
}

int is_observability_server_start(
    struct is_observability_server *server, const char *address, uint16_t port,
    is_provider_snapshot_callback snapshot, void *snapshot_context)
{
  struct sockaddr_in socket_address;
  socklen_t socket_address_size = sizeof(socket_address);
  int reuse = 1;

  if (!server || !address || strcmp(address, "127.0.0.1") != 0 || !snapshot)
    return -1;
  memset(server, 0, sizeof(*server));
  server->listen_fd = -1;
  server->snapshot = snapshot;
  server->snapshot_context = snapshot_context;
  server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server->listen_fd < 0)
    return -1;
  if (setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse,
                 sizeof(reuse)) != 0)
    goto fail;
  memset(&socket_address, 0, sizeof(socket_address));
  socket_address.sin_family = AF_INET;
  socket_address.sin_port = htons(port);
  if (inet_pton(AF_INET, address, &socket_address.sin_addr) != 1 ||
      bind(server->listen_fd, (struct sockaddr *)&socket_address,
           sizeof(socket_address)) != 0 ||
      listen(server->listen_fd, 16) != 0 ||
      getsockname(server->listen_fd, (struct sockaddr *)&socket_address,
                  &socket_address_size) != 0)
    goto fail;
  server->port = ntohs(socket_address.sin_port);
  atomic_init(&server->running, 1);
  if (pthread_create(&server->thread, NULL, run_server, server) != 0)
    goto fail;
  return 0;

fail:
  close(server->listen_fd);
  server->listen_fd = -1;
  return -1;
}

void is_observability_server_stop(struct is_observability_server *server)
{
  if (!server || server->listen_fd < 0)
    return;
  atomic_store(&server->running, 0);
  shutdown(server->listen_fd, SHUT_RDWR);
  close(server->listen_fd);
  server->listen_fd = -1;
  (void)pthread_join(server->thread, NULL);
}
