#include "infiniswap_memory_manager.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct fake_environment {
  uint64_t available_bytes;
  unsigned int allocation_calls;
  unsigned int touch_calls;
  unsigned int free_calls;
  unsigned int registration_calls;
  unsigned int deregistration_calls;
  unsigned int live_allocations;
  unsigned int live_registrations;
  unsigned int fail_allocation_call;
  unsigned int fail_touch_call;
  unsigned int fail_registration_call;
  unsigned int fail_deregistration_call;
};

static int fake_read_available(void *context, uint64_t *available_bytes)
{
  struct fake_environment *environment = context;

  *available_bytes = environment->available_bytes;
  return 0;
}

static int fake_allocate(void *context, size_t alignment, size_t size,
                         void **address)
{
  struct fake_environment *environment = context;

  (void)alignment;
  (void)size;
  environment->allocation_calls++;
  if (environment->fail_allocation_call == environment->allocation_calls)
    return -1;
  *address = malloc(1);
  if (!*address)
    return -1;
  environment->live_allocations++;
  if (environment->available_bytes >= IS_MEMORY_CHUNK_BYTES)
    environment->available_bytes -= IS_MEMORY_CHUNK_BYTES;
  return 0;
}

static int fake_touch(void *context, void *address, size_t size)
{
  struct fake_environment *environment = context;

  (void)address;
  (void)size;
  environment->touch_calls++;
  return environment->fail_touch_call == environment->touch_calls ? -1 : 0;
}

static void fake_free(void *context, void *address, size_t size)
{
  struct fake_environment *environment = context;

  (void)size;
  free(address);
  environment->free_calls++;
  environment->live_allocations--;
  environment->available_bytes += IS_MEMORY_CHUNK_BYTES;
}

static void *fake_register(void *context, void *address, size_t size)
{
  struct fake_environment *environment = context;
  void **registration;

  (void)size;
  environment->registration_calls++;
  if (environment->fail_registration_call ==
      environment->registration_calls)
    return NULL;
  registration = malloc(sizeof(*registration));
  if (!registration)
    return NULL;
  *registration = address;
  environment->live_registrations++;
  return registration;
}

static int fake_deregister(void *context, void *registration)
{
  struct fake_environment *environment = context;

  environment->deregistration_calls++;
  if (environment->fail_deregistration_call ==
      environment->deregistration_calls)
    return -1;
  free(registration);
  environment->live_registrations--;
  return 0;
}

static struct is_memory_manager *create_manager(
    struct fake_environment *environment,
    uint32_t host_reserve_gib,
    uint32_t max_opportunistic_gib,
    uint32_t max_committed_gib)
{
  struct is_memory_manager_config config = {
    .host_reserve_gib = host_reserve_gib,
    .max_opportunistic_gib = max_opportunistic_gib,
    .max_committed_gib = max_committed_gib,
  };
  struct is_memory_accounting_adapter accounting = {
    .context = environment,
    .read_available_bytes = fake_read_available,
  };
  struct is_memory_allocation_adapter allocation = {
    .context = environment,
    .allocate = fake_allocate,
    .touch = fake_touch,
    .release = fake_free,
  };

  return is_memory_manager_create(&config, &accounting, &allocation);
}

static struct is_memory_registration_adapter registration_adapter(
    struct fake_environment *environment)
{
  struct is_memory_registration_adapter registration = {
    .context = environment,
    .register_chunk = fake_register,
    .deregister_chunk = fake_deregister,
  };

  return registration;
}

static int expect_status(struct is_memory_manager *manager,
                         void *owner,
                         uint32_t allocated_opportunistic,
                         uint32_t assigned_opportunistic,
                         uint32_t allocated_committed,
                         uint32_t assigned_committed)
{
  struct is_memory_manager_status status;

  if (is_memory_manager_get_status(manager, owner, &status) !=
          IS_MEMORY_OK ||
      status.allocated_opportunistic_chunks != allocated_opportunistic ||
      status.assigned_opportunistic_chunks != assigned_opportunistic ||
      status.allocated_committed_chunks != allocated_committed ||
      status.assigned_committed_chunks != assigned_committed) {
    fprintf(stderr,
            "unexpected pool status: opportunistic %u/%u, committed %u/%u\n",
            status.assigned_opportunistic_chunks,
            status.allocated_opportunistic_chunks,
            status.assigned_committed_chunks,
            status.allocated_committed_chunks);
    return 1;
  }
  return 0;
}

static int test_host_reserve_threshold_crossings(void)
{
  struct fake_environment environment = {
    .available_bytes = UINT64_C(12) * IS_MEMORY_CHUNK_BYTES,
  };
  struct is_memory_manager *manager = create_manager(&environment, 8, 4, 0);
  struct is_memory_reconcile_result reconcile;
  struct is_memory_registration_adapter registration =
      registration_adapter(&environment);
  struct is_memory_grant grants[2];
  int owner;

  if (!manager ||
      is_memory_manager_reconcile(manager, &reconcile) != IS_MEMORY_OK ||
      reconcile.grown_opportunistic_chunks != 4 ||
      expect_status(manager, NULL, 4, 0, 0, 0) ||
      is_memory_manager_connect(manager, "consumer-a", &owner, 1, 4, 0,
                                NULL) != IS_MEMORY_OK ||
      is_memory_manager_acquire(manager, &owner,
                                IS_MEMORY_POOL_OPPORTUNISTIC, 2,
                                NULL, &registration, grants, 2) !=
          IS_MEMORY_OK ||
      expect_status(manager, &owner, 4, 2, 0, 0)) {
    fprintf(stderr, "Host Reserve growth setup failed\n");
    return 1;
  }

  environment.available_bytes = UINT64_C(6) * IS_MEMORY_CHUNK_BYTES;
  if (is_memory_manager_reconcile(manager, &reconcile) != IS_MEMORY_OK ||
      reconcile.reclaimed_unassigned_chunks != 2 ||
      reconcile.assigned_reclaim_needed != 0 ||
      expect_status(manager, &owner, 2, 2, 0, 0)) {
    fprintf(stderr, "threshold crossing did not reclaim only idle capacity\n");
    return 1;
  }

  environment.available_bytes = UINT64_C(7) * IS_MEMORY_CHUNK_BYTES;
  if (is_memory_manager_reconcile(manager, &reconcile) != IS_MEMORY_OK ||
      reconcile.assigned_reclaim_needed != 1 ||
      expect_status(manager, &owner, 2, 2, 0, 0)) {
    fprintf(stderr, "assigned Opportunistic Pool pressure was misreported\n");
    return 1;
  }
  {
    enum is_memory_rejection_reason rejection = IS_MEMORY_REJECTION_NONE;

    if (is_memory_manager_acquire(
            manager, &owner, IS_MEMORY_POOL_OPPORTUNISTIC, 1,
            &rejection, &registration, grants, 1) != IS_MEMORY_HOST_RESERVE ||
        rejection != IS_MEMORY_REJECTION_HOST_RESERVE ||
        expect_status(manager, &owner, 2, 2, 0, 0)) {
      fprintf(stderr, "admission crossed Host Reserve\n");
      return 1;
    }
  }

  if (is_memory_manager_disconnect(manager, &owner) != IS_MEMORY_OK ||
      is_memory_manager_destroy(manager) != IS_MEMORY_OK ||
      environment.live_allocations != 0 ||
      environment.live_registrations != 0)
    return 1;
  return 0;
}

static int test_partial_failures_are_transactional(void)
{
  struct fake_environment environment = {
    .available_bytes = UINT64_C(12) * IS_MEMORY_CHUNK_BYTES,
    .fail_allocation_call = 2,
  };
  struct is_memory_manager *manager = create_manager(&environment, 8, 4, 0);
  struct is_memory_registration_adapter registration =
      registration_adapter(&environment);
  struct is_memory_grant grants[2];
  enum is_memory_rejection_reason rejection = IS_MEMORY_REJECTION_NONE;
  int owner;

  if (!manager ||
      is_memory_manager_connect(manager, "consumer-a", &owner, 1, 4, 0,
                                NULL) != IS_MEMORY_OK)
    return 1;

  {
    enum is_memory_result acquire_result = is_memory_manager_acquire(
        manager, &owner, IS_MEMORY_POOL_OPPORTUNISTIC, 2,
        &rejection, &registration, grants, 2);

    if (acquire_result != IS_MEMORY_ALLOCATION_FAILED ||
        rejection != IS_MEMORY_REJECTION_ALLOCATION_FAILED ||
        expect_status(manager, &owner, 0, 0, 0, 0) ||
        environment.live_allocations != 0 ||
        environment.live_registrations != 0) {
      fprintf(stderr,
              "partial allocation changed manager accounting "
              "(result=%s, rejection=%s, live=%u/%u)\n",
              is_memory_result_name(acquire_result),
              is_memory_rejection_name(rejection),
              environment.live_allocations,
              environment.live_registrations);
      return 1;
    }
  }

  environment.fail_allocation_call = 0;
  environment.fail_registration_call = 2;
  if (is_memory_manager_acquire(manager, &owner,
                                IS_MEMORY_POOL_OPPORTUNISTIC, 2,
                                &rejection, &registration, grants, 2) !=
          IS_MEMORY_REGISTRATION_FAILED ||
      rejection != IS_MEMORY_REJECTION_REGISTRATION_FAILED ||
      expect_status(manager, &owner, 0, 0, 0, 0) ||
      environment.live_allocations != 0 ||
      environment.live_registrations != 0 ||
      environment.deregistration_calls != 1) {
    fprintf(stderr, "partial registration changed memory or counters\n");
    return 1;
  }

  if (is_memory_manager_disconnect(manager, &owner) != IS_MEMORY_OK ||
      is_memory_manager_destroy(manager) != IS_MEMORY_OK)
    return 1;
  return 0;
}

static int test_failed_cleanup_is_quarantined(void)
{
  struct fake_environment environment = {
    .available_bytes = UINT64_C(12) * IS_MEMORY_CHUNK_BYTES,
    .fail_registration_call = 2,
    .fail_deregistration_call = 1,
  };
  struct is_memory_manager *manager = create_manager(&environment, 8, 2, 0);
  struct is_memory_registration_adapter registration =
      registration_adapter(&environment);
  struct is_memory_grant grants[2];
  struct is_memory_manager_status status;
  enum is_memory_rejection_reason rejection = IS_MEMORY_REJECTION_NONE;
  int owner;

  if (!manager ||
      is_memory_manager_connect(manager, "consumer-a", &owner, 1, 2, 0,
                                NULL) != IS_MEMORY_OK ||
      is_memory_manager_acquire(manager, &owner,
                                IS_MEMORY_POOL_OPPORTUNISTIC, 2,
                                &rejection, &registration, grants, 2) !=
          IS_MEMORY_CLEANUP_FAILED ||
      rejection != IS_MEMORY_REJECTION_CLEANUP_FAILED ||
      is_memory_manager_get_status(manager, &owner, &status) != IS_MEMORY_OK ||
      status.healthy || status.quarantined_chunks != 1 ||
      status.allocated_opportunistic_chunks != 1 ||
      status.assigned_opportunistic_chunks != 0 ||
      environment.live_allocations != 1 ||
      environment.live_registrations != 1) {
    fprintf(stderr, "failed RDMA cleanup was not tracked safely\n");
    return 1;
  }

  if (is_memory_manager_destroy(manager) != IS_MEMORY_OK ||
      environment.live_allocations != 0 ||
      environment.live_registrations != 0)
    return 1;
  return 0;
}

static int test_committed_assignments_survive_pressure(void)
{
  struct fake_environment environment = {
    .available_bytes = UINT64_C(10) * IS_MEMORY_CHUNK_BYTES,
  };
  struct is_memory_manager *manager = create_manager(&environment, 8, 0, 2);
  struct is_memory_registration_adapter registration =
      registration_adapter(&environment);
  struct is_memory_grant grants[2];
  struct is_memory_reconcile_result reconcile;
  enum is_memory_rejection_reason rejection = IS_MEMORY_REJECTION_NONE;
  uint32_t chunk_id;
  int owner;
  int other_owner;

  if (!manager ||
      is_memory_manager_connect(manager, "consumer-a", &owner, 1, 0, 2,
                                NULL) != IS_MEMORY_OK ||
      is_memory_manager_acquire(manager, &owner, IS_MEMORY_POOL_COMMITTED, 2,
                                &rejection, &registration, grants, 2) !=
          IS_MEMORY_OK)
    return 1;

  environment.available_bytes = IS_MEMORY_CHUNK_BYTES;
  chunk_id = grants[0].provider_chunk_id;
  if (is_memory_manager_reconcile(manager, &reconcile) != IS_MEMORY_OK ||
      reconcile.assigned_reclaim_needed != 0 ||
      is_memory_manager_release(manager, &owner, &chunk_id, 1,
                                IS_MEMORY_RELEASE_PRESSURE) !=
          IS_MEMORY_COMMITTED_PROTECTED ||
      expect_status(manager, &owner, 0, 0, 2, 2)) {
    fprintf(stderr, "Provider-local pressure reclaimed committed capacity\n");
    return 1;
  }

  if (is_memory_manager_acquire(manager, &owner, IS_MEMORY_POOL_COMMITTED, 1,
                                &rejection, &registration, grants, 1) !=
          IS_MEMORY_QUOTA_EXCEEDED ||
      rejection != IS_MEMORY_REJECTION_CONSUMER_QUOTA ||
      is_memory_manager_connect(manager, "consumer-b", &other_owner,
                                1, 0, 1, NULL) != IS_MEMORY_OK ||
      is_memory_manager_acquire(manager, &other_owner,
                                IS_MEMORY_POOL_COMMITTED, 1,
                                &rejection, &registration, grants, 1) !=
          IS_MEMORY_POOL_EXHAUSTED ||
      rejection != IS_MEMORY_REJECTION_POOL_LIMIT) {
    fprintf(stderr, "committed overcommit was not rejected at admission\n");
    return 1;
  }

  if (is_memory_manager_disconnect(manager, &other_owner) != IS_MEMORY_OK ||
      is_memory_manager_disconnect(manager, &owner) != IS_MEMORY_OK ||
      is_memory_manager_destroy(manager) != IS_MEMORY_OK ||
      environment.live_allocations != 0 ||
      environment.live_registrations != 0)
    return 1;
  return 0;
}

static int test_repeated_committed_reservations_are_atomic(void)
{
  struct fake_environment environment = {
    .available_bytes = UINT64_C(9) * IS_MEMORY_CHUNK_BYTES,
  };
  struct is_memory_manager *manager = create_manager(&environment, 8, 0, 2);
  struct is_memory_registration_adapter registration =
      registration_adapter(&environment);
  struct is_memory_grant grants[2];
  enum is_memory_rejection_reason rejection = IS_MEMORY_REJECTION_NONE;
  int owner;
  int iteration;

  if (!manager ||
      is_memory_manager_connect(manager, "consumer-a", &owner, 1, 0, 2,
                                NULL) != IS_MEMORY_OK ||
      is_memory_manager_acquire(manager, &owner, IS_MEMORY_POOL_COMMITTED, 2,
                                &rejection, &registration, grants, 2) !=
          IS_MEMORY_HOST_RESERVE ||
      rejection != IS_MEMORY_REJECTION_HOST_RESERVE ||
      expect_status(manager, &owner, 0, 0, 0, 0) ||
      environment.live_allocations != 0 ||
      environment.live_registrations != 0 ||
      is_memory_manager_disconnect(manager, &owner) != IS_MEMORY_OK) {
    fprintf(stderr, "partial committed capacity escaped failed admission\n");
    return 1;
  }

  environment.available_bytes = UINT64_C(10) * IS_MEMORY_CHUNK_BYTES;
  for (iteration = 0; iteration < 3; iteration++) {
    if (is_memory_manager_connect(manager, "consumer-a", &owner, 1, 0, 2,
                                  NULL) != IS_MEMORY_OK ||
        is_memory_manager_acquire(manager, &owner, IS_MEMORY_POOL_COMMITTED, 2,
                                  NULL, &registration, grants, 2) !=
            IS_MEMORY_OK ||
        expect_status(manager, &owner, 0, 0, 2, 2) ||
        is_memory_manager_disconnect(manager, &owner) != IS_MEMORY_OK ||
        expect_status(manager, NULL, 0, 0, 0, 0) ||
        environment.live_allocations != 0 ||
        environment.live_registrations != 0) {
      fprintf(stderr, "committed reserve/release cycle did not reconcile\n");
      return 1;
    }
  }

  return is_memory_manager_destroy(manager) == IS_MEMORY_OK ? 0 : 1;
}

static int test_connection_quota_eviction_disconnect_and_shutdown(void)
{
  struct fake_environment environment = {
    .available_bytes = UINT64_C(12) * IS_MEMORY_CHUNK_BYTES,
  };
  struct is_memory_manager *manager = create_manager(&environment, 8, 4, 0);
  struct is_memory_registration_adapter registration =
      registration_adapter(&environment);
  struct is_memory_reconcile_result reconcile;
  struct is_memory_manager_status status;
  struct is_memory_grant grants[3];
  enum is_memory_rejection_reason rejection = IS_MEMORY_REJECTION_NONE;
  uint32_t released_chunk;
  int owner_a;
  int owner_b;
  int owner_c;

  if (!manager ||
      is_memory_manager_reconcile(manager, &reconcile) != IS_MEMORY_OK ||
      is_memory_manager_connect(manager, "consumer-a", &owner_a, 2, 3, 0,
                                NULL) != IS_MEMORY_OK ||
      is_memory_manager_connect(manager, "consumer-a", &owner_b, 2, 3, 0,
                                NULL) != IS_MEMORY_OK ||
      is_memory_manager_connect(manager, "consumer-a", &owner_c, 2, 3, 0,
                                &rejection) !=
          IS_MEMORY_CONNECTION_LIMIT ||
      rejection != IS_MEMORY_REJECTION_CONNECTION_LIMIT ||
      is_memory_manager_acquire(manager, &owner_a,
                                IS_MEMORY_POOL_OPPORTUNISTIC, 3,
                                NULL, &registration, grants, 3) !=
          IS_MEMORY_OK ||
      is_memory_manager_acquire(manager, &owner_b,
                                IS_MEMORY_POOL_OPPORTUNISTIC, 1,
                                &rejection, &registration, grants, 1) !=
          IS_MEMORY_QUOTA_EXCEEDED ||
      rejection != IS_MEMORY_REJECTION_CONSUMER_QUOTA)
    return 1;

  released_chunk = grants[0].provider_chunk_id;
  if (is_memory_manager_release(manager, &owner_a, &released_chunk, 1,
                                IS_MEMORY_RELEASE_PRESSURE) != IS_MEMORY_OK ||
      expect_status(manager, &owner_a, 3, 2, 0, 0) ||
      is_memory_manager_acquire(manager, &owner_b,
                                IS_MEMORY_POOL_OPPORTUNISTIC, 1,
                                NULL, &registration, grants, 1) !=
          IS_MEMORY_OK ||
      is_memory_manager_get_status(manager, &owner_b, &status) !=
          IS_MEMORY_OK ||
      status.consumer_connections != 2 ||
      status.consumer_assigned_opportunistic_chunks != 3 ||
      is_memory_manager_disconnect(manager, &owner_a) != IS_MEMORY_OK ||
      expect_status(manager, &owner_b, 3, 1, 0, 0) ||
      is_memory_manager_disconnect(manager, &owner_b) != IS_MEMORY_OK ||
      expect_status(manager, NULL, 3, 0, 0, 0)) {
    fprintf(stderr, "eviction or disconnect accounting did not reconcile\n");
    return 1;
  }

  if (is_memory_manager_connect(manager, "consumer-a", &owner_b, 1, 3, 0,
                                NULL) != IS_MEMORY_OK ||
      is_memory_manager_acquire(manager, &owner_b,
                                IS_MEMORY_POOL_OPPORTUNISTIC, 1,
                                NULL, &registration, grants, 1) !=
          IS_MEMORY_OK ||
      is_memory_manager_destroy(manager) != IS_MEMORY_OK ||
      environment.live_allocations != 0 ||
      environment.live_registrations != 0) {
    fprintf(stderr, "shutdown left pool or per-Consumer accounting live\n");
    return 1;
  }
  return 0;
}

static int test_runtime_config_loader(void)
{
  static const char valid_config[] =
      "version = 1\n"
      "host_reserve_gib = 8\n"
      "max_opportunistic_gib = 24\n"
      "max_committed_gib = 8\n";
  static const char invalid_config[] =
      "version = 1\n"
      "host_reserve_gib = 8\n"
      "max_opportunistic_gib = 128\n"
      "max_committed_gib = 1\n";
  struct is_memory_manager_config config;
  char valid_path[] = "/tmp/infiniswap-memory-valid-XXXXXX";
  char invalid_path[] = "/tmp/infiniswap-memory-invalid-XXXXXX";
  int valid_fd = mkstemp(valid_path);
  int invalid_fd = mkstemp(invalid_path);
  int failed = 0;

  if (valid_fd < 0 || invalid_fd < 0 ||
      write(valid_fd, valid_config, sizeof(valid_config) - 1U) !=
          (ssize_t)(sizeof(valid_config) - 1U) ||
      write(invalid_fd, invalid_config, sizeof(invalid_config) - 1U) !=
          (ssize_t)(sizeof(invalid_config) - 1U) ||
      close(valid_fd) != 0 || close(invalid_fd) != 0 ||
      is_memory_manager_config_load(valid_path, &config) != IS_MEMORY_OK ||
      config.host_reserve_gib != 8 ||
      config.max_opportunistic_gib != 24 ||
      config.max_committed_gib != 8 ||
      is_memory_manager_config_load(invalid_path, &config) !=
          IS_MEMORY_CONFIG_INVALID) {
    fprintf(stderr, "runtime Provider pool configuration was not validated\n");
    failed = 1;
  }
  unlink(valid_path);
  unlink(invalid_path);
  return failed;
}

int main(void)
{
  int failures = 0;

  failures += test_host_reserve_threshold_crossings();
  failures += test_partial_failures_are_transactional();
  failures += test_failed_cleanup_is_quarantined();
  failures += test_committed_assignments_survive_pressure();
  failures += test_repeated_committed_reservations_are_atomic();
  failures += test_connection_quota_eviction_disconnect_and_shutdown();
  failures += test_runtime_config_loader();
  return failures == 0 ? 0 : 1;
}
