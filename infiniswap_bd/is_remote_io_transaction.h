/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#ifndef INFINISWAP_REMOTE_IO_TRANSACTION_H
#define INFINISWAP_REMOTE_IO_TRANSACTION_H

#ifdef __KERNEL__
#include <linux/atomic.h>
#include <linux/types.h>

typedef atomic_t is_remote_io_transaction_counter_t;
#else
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

typedef atomic_uint is_remote_io_transaction_counter_t;
#endif

struct is_remote_io_transaction;

enum is_remote_io_transaction_mode {
	IS_REMOTE_IO_TRANSACTION_BACKED_STRICT = 1,
	IS_REMOTE_IO_TRANSACTION_BACKED_REMOTE_FIRST,
	IS_REMOTE_IO_TRANSACTION_REMOTE_ONLY,
};

enum is_remote_io_transaction_direction {
	IS_REMOTE_IO_TRANSACTION_READ = 1,
	IS_REMOTE_IO_TRANSACTION_WRITE,
};

/* The engine copies this immutable specification before start returns. */
struct is_remote_io_transaction_spec {
	enum is_remote_io_transaction_direction direction;
	unsigned long long sector;
	unsigned int bytes;
	unsigned long long generation;
	void *payload;
};

struct is_remote_io_transaction_engine {
	is_remote_io_transaction_counter_t active_transactions;
	enum is_remote_io_transaction_mode mode;
	unsigned char initialized;
};

struct is_remote_io_transaction_allocation {
	void *transaction_storage;
	void *adapter_context;
};

enum is_remote_io_transaction_event {
	IS_REMOTE_IO_TRANSACTION_EVENT_BACKING_COMPLETED = 1,
	IS_REMOTE_IO_TRANSACTION_EVENT_RDMA_COMPLETED,
	IS_REMOTE_IO_TRANSACTION_EVENT_RDMA_RELEASED,
	IS_REMOTE_IO_TRANSACTION_EVENT_ENGINE_DESTROY_ACTIVE,
};

int is_remote_io_transaction_engine_init(
	struct is_remote_io_transaction_engine *engine,
	enum is_remote_io_transaction_mode mode);
int is_remote_io_transaction_engine_destroy(
	struct is_remote_io_transaction_engine *engine);

/* Start always consumes payload ownership, including synchronous failures. */
void is_remote_io_transaction_start(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec);

/* Typed lifecycle entries used only by the environment Adapters. */
void is_remote_io_transaction_backing_completed(
	struct is_remote_io_transaction *transaction, int status);
void is_remote_io_transaction_rdma_completed(
	struct is_remote_io_transaction *transaction,
	unsigned long long generation, int status, bool cancelled);
void is_remote_io_transaction_rdma_released(
	struct is_remote_io_transaction *transaction,
	unsigned long long generation);

/* Link-time Adapters implemented by the kernel environment and host tests. */
int is_remote_io_transaction_adapter_allocate(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec,
	size_t transaction_size, size_t transaction_alignment,
	struct is_remote_io_transaction_allocation *allocation);
int is_remote_io_transaction_adapter_prepare(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec, bool prepare_backing,
	struct is_remote_io_transaction *transaction, void *adapter_context);
int is_remote_io_transaction_adapter_submit_backing(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec,
	struct is_remote_io_transaction *transaction, void *adapter_context);
int is_remote_io_transaction_adapter_submit_rdma(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec,
	struct is_remote_io_transaction *transaction, void *adapter_context);
void is_remote_io_transaction_adapter_complete(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec, void *adapter_context,
	int status);
void is_remote_io_transaction_adapter_backing_degraded(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec, void *adapter_context);
void is_remote_io_transaction_adapter_mark_local_only(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec, void *adapter_context);
void is_remote_io_transaction_adapter_release(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec, void *adapter_context);
void is_remote_io_transaction_adapter_settle(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec);
void is_remote_io_transaction_adapter_invariant(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec, void *adapter_context,
	enum is_remote_io_transaction_event event);

#endif /* INFINISWAP_REMOTE_IO_TRANSACTION_H */
