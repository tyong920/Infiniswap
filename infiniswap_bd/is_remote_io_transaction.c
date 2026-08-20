/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#include "is_remote_io_transaction.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>

typedef unsigned long is_transaction_lock_flags_t;

static int is_transaction_lock_init(is_remote_io_transaction_lock_t *lock)
{
	spin_lock_init(lock);
	return 0;
}

static void is_transaction_lock(is_remote_io_transaction_lock_t *lock,
				is_transaction_lock_flags_t *flags)
{
	spin_lock_irqsave(lock, *flags);
}

static void is_transaction_unlock(is_remote_io_transaction_lock_t *lock,
				  is_transaction_lock_flags_t *flags)
{
	spin_unlock_irqrestore(lock, *flags);
}

static void is_transaction_lock_destroy(is_remote_io_transaction_lock_t *lock)
{
	(void)lock;
}
#else
#include <errno.h>
#include <string.h>

typedef int is_transaction_lock_flags_t;

static int is_transaction_lock_init(is_remote_io_transaction_lock_t *lock)
{
	return pthread_mutex_init(lock, NULL);
}

static void is_transaction_lock(is_remote_io_transaction_lock_t *lock,
				is_transaction_lock_flags_t *flags)
{
	(void)flags;
	(void)pthread_mutex_lock(lock);
}

static void is_transaction_unlock(is_remote_io_transaction_lock_t *lock,
				  is_transaction_lock_flags_t *flags)
{
	(void)flags;
	(void)pthread_mutex_unlock(lock);
}

static void is_transaction_lock_destroy(is_remote_io_transaction_lock_t *lock)
{
	(void)pthread_mutex_destroy(lock);
}
#endif

static void is_transaction_begin_event(
	struct is_remote_io_transaction *transaction,
	is_transaction_lock_flags_t *flags)
{
	is_transaction_lock(&transaction->lock, flags);
	transaction->active_events++;
}

static bool is_transaction_note_invariant(
	struct is_remote_io_transaction *transaction)
{
	if (transaction->invariant_reported)
		return false;
	transaction->invariant_reported = 1;
	return true;
}

static void is_transaction_finish_event(
	struct is_remote_io_transaction *transaction,
	enum is_remote_io_transaction_event event)
{
	is_transaction_lock_flags_t flags;
	bool report = false;
	bool settle = false;

	is_transaction_lock(&transaction->lock, &flags);
	if (transaction->active_events)
		transaction->active_events--;
	if (!transaction->active_events && !transaction->dispatcher_claim &&
	    !transaction->local_branch_claim &&
	    !transaction->remote_result_claim &&
	    !transaction->remote_transport_claim && !transaction->settling) {
		if (!is_io_policy_releasable(&transaction->policy)) {
			report = is_transaction_note_invariant(transaction);
		} else {
			transaction->settling = 1;
			settle = true;
		}
	}
	is_transaction_unlock(&transaction->lock, &flags);

	if (report)
		is_remote_io_transaction_adapter_invariant(transaction, event);
	if (settle) {
		is_transaction_lock_destroy(&transaction->lock);
		is_remote_io_transaction_adapter_settle(transaction);
	}
}

static int is_transaction_completion_status(
	const struct is_remote_io_transaction *transaction)
{
	if (transaction->policy.local_done)
		return transaction->policy.local_status;
	return transaction->policy.remote_status;
}

static void is_transaction_apply_action(
	struct is_remote_io_transaction *transaction, enum is_io_action action,
	int completion_status)
{
	if (action & IS_IO_BACKING_DEGRADED)
		is_remote_io_transaction_adapter_backing_degraded(transaction);
	if (action & IS_IO_MARK_LOCAL_ONLY)
		is_remote_io_transaction_adapter_mark_local_only(transaction);
	if (action & IS_IO_SUBMIT_LOCAL)
		is_remote_io_transaction_adapter_submit_local(transaction);
	if (action & IS_IO_COMPLETE_SUCCESS)
		is_remote_io_transaction_adapter_complete(transaction, 0);
	else if (action & IS_IO_COMPLETE_ERROR)
		is_remote_io_transaction_adapter_complete(transaction,
			completion_status);
}

int is_remote_io_transaction_init(struct is_remote_io_transaction *transaction,
			       enum is_io_policy_kind kind,
			       unsigned long long generation)
{
	int result;

	if (!transaction || !generation)
		return -EINVAL;
	memset(transaction, 0, sizeof(*transaction));
	result = is_transaction_lock_init(&transaction->lock);
	if (result != 0)
		return result;

	switch (kind) {
	case IS_IO_POLICY_STRICT_WRITE:
	case IS_IO_POLICY_REMOTE_FIRST_WRITE:
		is_io_policy_init_write(&transaction->policy, kind, generation);
		transaction->local_branch_claim = 1;
		break;
	case IS_IO_POLICY_REMOTE_READ:
		is_io_policy_init_remote_read(&transaction->policy, generation);
		break;
	case IS_IO_POLICY_REMOTE_ONLY_WRITE:
		is_io_policy_init_remote_only_write(&transaction->policy,
			generation);
		break;
	case IS_IO_POLICY_REMOTE_ONLY_READ:
		is_io_policy_init_remote_only_read(&transaction->policy,
			generation);
		break;
	default:
		is_transaction_lock_destroy(&transaction->lock);
		return -EINVAL;
	}
	transaction->dispatcher_claim = 1;
	transaction->remote_result_claim = 1;
	transaction->remote_transport_claim = 1;
	return 0;
}

void is_remote_io_transaction_dispatcher_released(
	struct is_remote_io_transaction *transaction)
{
	is_transaction_lock_flags_t flags;
	bool report = false;

	is_transaction_begin_event(transaction, &flags);
	if (!transaction->dispatcher_claim) {
		report = is_transaction_note_invariant(transaction);
	} else {
		transaction->dispatcher_claim = 0;
	}
	is_transaction_unlock(&transaction->lock, &flags);

	if (report)
		is_remote_io_transaction_adapter_invariant(transaction,
			IS_REMOTE_IO_TRANSACTION_EVENT_DISPATCHER_RELEASED);
	is_transaction_finish_event(transaction,
		IS_REMOTE_IO_TRANSACTION_EVENT_DISPATCHER_RELEASED);
}

void is_remote_io_transaction_local_completed(
	struct is_remote_io_transaction *transaction, int status)
{
	is_transaction_lock_flags_t flags;
	enum is_io_action action = IS_IO_NO_ACTION;
	int completion_status = status;
	bool report = false;

	is_transaction_begin_event(transaction, &flags);
	if (!transaction->local_branch_claim) {
		report = is_transaction_note_invariant(transaction);
	} else {
		action = is_io_policy_local_complete(&transaction->policy, status);
		completion_status = is_transaction_completion_status(transaction);
		transaction->local_branch_claim = 0;
	}
	is_transaction_unlock(&transaction->lock, &flags);

	if (report)
		is_remote_io_transaction_adapter_invariant(transaction,
			IS_REMOTE_IO_TRANSACTION_EVENT_LOCAL_COMPLETED);
	is_transaction_apply_action(transaction, action, completion_status);
	is_transaction_finish_event(transaction,
		IS_REMOTE_IO_TRANSACTION_EVENT_LOCAL_COMPLETED);
}

void is_remote_io_transaction_remote_completed(
	struct is_remote_io_transaction *transaction, unsigned long long generation,
	int status, bool cancelled)
{
	is_transaction_lock_flags_t flags;
	enum is_io_action action = IS_IO_NO_ACTION;
	int completion_status = status;
	bool report = false;

	is_transaction_begin_event(transaction, &flags);
	if (generation != transaction->policy.generation) {
		/* A stale operation owns no claim in this transaction. */
	} else if (!transaction->remote_result_claim) {
		report = is_transaction_note_invariant(transaction);
	} else {
		action = cancelled ?
			is_io_policy_cancel_remote(&transaction->policy, generation,
				status) :
			is_io_policy_remote_complete(&transaction->policy, generation,
				status);
		if (action & IS_IO_SUBMIT_LOCAL) {
			if (transaction->local_branch_claim)
				report = is_transaction_note_invariant(transaction);
			else
				transaction->local_branch_claim = 1;
		}
		completion_status = is_transaction_completion_status(transaction);
		transaction->remote_result_claim = 0;
	}
	is_transaction_unlock(&transaction->lock, &flags);

	if (report)
		is_remote_io_transaction_adapter_invariant(transaction,
			IS_REMOTE_IO_TRANSACTION_EVENT_REMOTE_COMPLETED);
	is_transaction_apply_action(transaction, action, completion_status);
	is_transaction_finish_event(transaction,
		IS_REMOTE_IO_TRANSACTION_EVENT_REMOTE_COMPLETED);
}

void is_remote_io_transaction_transport_released(
	struct is_remote_io_transaction *transaction, unsigned long long generation)
{
	is_transaction_lock_flags_t flags;
	bool report = false;

	is_transaction_begin_event(transaction, &flags);
	if (generation != transaction->policy.generation) {
		/* A stale transport event cannot release the current claim. */
	} else if (!transaction->remote_transport_claim) {
		report = is_transaction_note_invariant(transaction);
	} else {
		transaction->remote_transport_claim = 0;
	}
	is_transaction_unlock(&transaction->lock, &flags);

	if (report)
		is_remote_io_transaction_adapter_invariant(transaction,
			IS_REMOTE_IO_TRANSACTION_EVENT_TRANSPORT_RELEASED);
	is_transaction_finish_event(transaction,
		IS_REMOTE_IO_TRANSACTION_EVENT_TRANSPORT_RELEASED);
}
