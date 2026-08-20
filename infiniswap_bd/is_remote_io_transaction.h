/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#ifndef INFINISWAP_REMOTE_IO_TRANSACTION_H
#define INFINISWAP_REMOTE_IO_TRANSACTION_H

#include "is_io_policy.h"

#ifdef __KERNEL__
#include <linux/spinlock.h>
#include <linux/types.h>

typedef spinlock_t is_remote_io_transaction_lock_t;
#else
#include <pthread.h>
#include <stdbool.h>

typedef pthread_mutex_t is_remote_io_transaction_lock_t;
#endif

enum is_remote_io_transaction_event {
	IS_REMOTE_IO_TRANSACTION_EVENT_DISPATCHER_RELEASED = 1,
	IS_REMOTE_IO_TRANSACTION_EVENT_LOCAL_COMPLETED,
	IS_REMOTE_IO_TRANSACTION_EVENT_REMOTE_COMPLETED,
	IS_REMOTE_IO_TRANSACTION_EVENT_TRANSPORT_RELEASED,
};

/*
 * Each claim names the only producer allowed to deliver its lifecycle event.
 * An event consumes that claim before Adapter effects become observable; the
 * producer must not access the transaction after the event returns. Active
 * events keep Adapter re-entry alive while effects run unlocked.
 */
struct is_remote_io_transaction {
	is_remote_io_transaction_lock_t lock;
	struct is_io_policy policy;
	unsigned int active_events;
	unsigned char dispatcher_claim;
	unsigned char local_branch_claim;
	unsigned char remote_result_claim;
	unsigned char remote_transport_claim;
	unsigned char invariant_reported;
	unsigned char settling;
};

int is_remote_io_transaction_init(struct is_remote_io_transaction *transaction,
			       enum is_io_policy_kind kind,
			       unsigned long long generation);
void is_remote_io_transaction_dispatcher_released(
	struct is_remote_io_transaction *transaction);
void is_remote_io_transaction_local_completed(
	struct is_remote_io_transaction *transaction, int status);
void is_remote_io_transaction_remote_completed(
	struct is_remote_io_transaction *transaction, unsigned long long generation,
	int status, bool cancelled);
void is_remote_io_transaction_transport_released(
	struct is_remote_io_transaction *transaction, unsigned long long generation);

/* Link-time Adapters implemented by the kernel environment and host tests. */
void is_remote_io_transaction_adapter_complete(
	struct is_remote_io_transaction *transaction, int status);
void is_remote_io_transaction_adapter_backing_degraded(
	struct is_remote_io_transaction *transaction);
void is_remote_io_transaction_adapter_mark_local_only(
	struct is_remote_io_transaction *transaction);
void is_remote_io_transaction_adapter_submit_local(
	struct is_remote_io_transaction *transaction);
void is_remote_io_transaction_adapter_settle(
	struct is_remote_io_transaction *transaction);
void is_remote_io_transaction_adapter_invariant(
	struct is_remote_io_transaction *transaction,
	enum is_remote_io_transaction_event event);

#endif /* INFINISWAP_REMOTE_IO_TRANSACTION_H */
