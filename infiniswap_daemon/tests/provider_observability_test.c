/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#include "infiniswap_observability.h"
#include "infiniswap_observability_server.h"

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int copy_snapshot(
    void *context, struct is_provider_observability_snapshot *snapshot)
{
  *snapshot = *(struct is_provider_observability_snapshot *)context;
  return 0;
}

static void request_endpoint(uint16_t port, const char *path, char *response,
                             size_t response_size)
{
  struct sockaddr_in address;
  char request[128];
  size_t used = 0;
  int client = socket(AF_INET, SOCK_STREAM, 0);

  assert(client >= 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  assert(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
  assert(connect(client, (struct sockaddr *)&address, sizeof(address)) == 0);
  snprintf(request, sizeof(request),
           "GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
           path);
  assert(send(client, request, strlen(request), 0) == (ssize_t)strlen(request));
  while (used + 1U < response_size) {
    ssize_t received = recv(client, response + used, response_size - used - 1U,
                            0);

    if (received <= 0)
      break;
    used += (size_t)received;
  }
  response[used] = '\0';
  close(client);
}

static void assert_contract(const char *path, const char *actual)
{
  char expected[32768];
  FILE *stream = fopen(path, "rb");
  size_t size;

  assert(stream != NULL);
  size = fread(expected, 1, sizeof(expected) - 1U, stream);
  assert(!ferror(stream));
  assert(feof(stream));
  assert(fclose(stream) == 0);
  expected[size] = '\0';
  assert(strcmp(actual, expected) == 0);
}

int main(int argc, char **argv)
{
  struct is_provider_observability_snapshot snapshot;
  struct is_observability_server server;
  char output[32768];

  memset(&snapshot, 0, sizeof(snapshot));
  assert(argc == 3);
  strcpy(snapshot.provider_id, "provider-a");
  snapshot.healthy = 1;
  snapshot.host_reserve_chunks = 8;
  snapshot.max_opportunistic_chunks = 24;
  snapshot.max_committed_chunks = 8;
  snapshot.allocated_opportunistic_chunks = 10;
  snapshot.assigned_opportunistic_chunks = 6;
  snapshot.available_opportunistic_chunks = 12;
  snapshot.available_committed_chunks = 4;
  snapshot.active_connections = 1;
  snapshot.admissions_total = 9;
  snapshot.admission_rejections_total = 2;
  snapshot.authentication_failures_total = 1;
  snapshot.deadline_expiries_total = 3;
  snapshot.last_rejection = IS_MEMORY_REJECTION_HOST_RESERVE;
  snapshot.consumer_count = 1;
  strcpy(snapshot.consumers[0].consumer_id, "consumer-a");
  snapshot.consumers[0].state = IS_PROVIDER_CONSUMER_READY;
  snapshot.consumers[0].mode = IS_PROTOCOL_MODE_BACKED;
  snapshot.consumers[0].pool = IS_PROTOCOL_POOL_OPPORTUNISTIC;
  snapshot.consumers[0].failure_deadline_ms = 2000;
  snapshot.consumers[0].assigned_opportunistic_chunks = 6;
  snapshot.consumers[0].inflight_control_requests = 1;

  assert(is_provider_observability_render_status(
             &snapshot, output, sizeof(output)) == IS_OBSERVABILITY_OK);
  assert(strstr(output, "\"schema_version\":1") != NULL);
  assert(strstr(output, "\"kind\":\"infiniswap.provider-status\"") != NULL);
  assert(strstr(output, "\"provider_id\":\"provider-a\"") != NULL);
  assert(strstr(output, "\"name\":\"opportunistic\"") != NULL);
  assert(strstr(output, "\"consumer_id\":\"consumer-a\"") != NULL);
  assert(strstr(output, "\"failure_deadline_ms\":2000") != NULL);
  assert(strstr(output, "\"inflight_control_requests\":1") != NULL);
  assert(strstr(output, "psk") == NULL);
  assert(strstr(output, "key_id") == NULL);
  assert_contract(argv[1], output);

  assert(is_provider_observability_render_metrics(
             &snapshot, output, sizeof(output)) == IS_OBSERVABILITY_OK);
  assert(strstr(output, "# infiniswap_metrics_schema_version 1\n") != NULL);
  assert(strstr(output,
                "infiniswap_provider_pool_assigned_chunks{provider=\"provider-a\",pool=\"opportunistic\"} 6\n") != NULL);
  assert(strstr(output,
                "infiniswap_provider_consumer_connected{provider=\"provider-a\",consumer=\"consumer-a\"} 1\n") != NULL);
  assert(strstr(output,
                "infiniswap_provider_authentication_failures_total{provider=\"provider-a\"} 1\n") != NULL);
  assert(strstr(output, "# EOF\n") != NULL);
  assert(strstr(output, "psk") == NULL);
  assert_contract(argv[2], output);

  assert(is_observability_server_start(&server, "127.0.0.1", 0,
                                       copy_snapshot, &snapshot) == 0);
  request_endpoint(server.port, "/status", output, sizeof(output));
  assert(strstr(output, "HTTP/1.1 200 OK") != NULL);
  assert(strstr(output, "application/json") != NULL);
  assert(strstr(output, "infiniswap.provider-status") != NULL);
  request_endpoint(server.port, "/metrics", output, sizeof(output));
  assert(strstr(output, "application/openmetrics-text") != NULL);
  assert(strstr(output, "# EOF\n") != NULL);
  request_endpoint(server.port, "/healthz", output, sizeof(output));
  assert(strstr(output, "healthy\n") != NULL);
  is_observability_server_stop(&server);

  puts("provider observability tests passed");
  return 0;
}
