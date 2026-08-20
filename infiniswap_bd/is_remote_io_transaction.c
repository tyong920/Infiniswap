/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#include "is_remote_io_transaction.h"

#include "is_io_policy.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/spinlock.h>
#include <linux/string.h>

typedef spinlock_t is_transaction_lock_t;
typedef unsigned long is_transaction_lock_flags_t;

static int is_transaction_lock_init(is_transaction_lock_t *lock)
{
	spin_lock_init(lock);
	return 0;
}

static void is_transaction_lock(is_transaction_lock_t *lock,
				is_transaction_lock_flags_t *flags)
{
	spin_lock_irqsave(lock, *flags);
}

static void is_transaction_unlock(is_transaction_lock_t *lock,
				  is_transaction_lock_flags_t *flags)
{
	spin_unlock_irqrestore(lock, *flags);
}

static void is_transaction_lock_destroy(is_transaction_lock_t *lock)
{
	(void)lock;
}

#define IS_ENGINE_DESTROYED (1U << 30)

static void is_engine_counter_init(is_remote_io_transaction_counter_t *counter)
{
	atomic_set(counter, 0);
}

static bool is_engine_try_register(
	is_remote_io_transaction_counter_t *counter)
{
	int count = atomic_read(counter);

	while (!((unsigned int)count & IS_ENGINE_DESTROYED)) {
		int observed = atomic_cmpxchg(counter, count, count + 1);

		if (observed == count)
			return true;
		count = observed;
	}
	return false;
}

static int is_engine_try_destroy(is_remote_io_transaction_counter_t *counter)
{
	int count = atomic_cmpxchg(counter, 0, (int)IS_ENGINE_DESTROYED);

	if (count == 0)
		return 0;
	return ((unsigned int)count & IS_ENGINE_DESTROYED) ? -EINVAL : -EBUSY;
}

static void is_engine_counter_decrement(
	is_remote_io_transaction_counter_t *counter)
{
	atomic_dec(counter);
}
#else
#include <errno.h>
#include <pthread.h>
#include <string.h>

typedef pthread_mutex_t is_transaction_lock_t;
typedef int is_transaction_lock_flags_t;

static int is_transaction_lock_init(is_transaction_lock_t *lock)
{
	return pthread_mutex_init(lock, NULL);
}

static void is_transaction_lock(is_transaction_lock_t *lock,
				is_transaction_lock_flags_t *flags)
{
	(void)flags;
	(void)pthread_mutex_lock(lock);
}

static void is_transaction_unlock(is_transaction_lock_t *lock,
				  is_transaction_lock_flags_t *flags)
{
	(void)flags;
	(void)pthread_mutex_unlock(lock);
}

static void is_transaction_lock_destroy(is_transaction_lock_t *lock)
{
	(void)pthread_mutex_destroy(lock);
}

#define IS_ENGINE_DESTROYED (1U << 30)

static void is_engine_counter_init(is_remote_io_transaction_counter_t *counter)
{
	atomic_init(counter, 0);
}

static bool is_engine_try_register(
	is_remote_io_transaction_counter_t *counter)
{
	unsigned int count = atomic_load_explicit(counter, memory_order_acquire);

	while (!(count & IS_ENGINE_DESTROYED)) {
		if (atomic_compare_exchange_weak_explicit(counter, &count,
				count + 1, memory_order_acquire,
				memory_order_relaxed))
			return true;
	}
	return false;
}

static int is_engine_try_destroy(is_remote_io_transaction_counter_t *counter)
{
	unsigned int count = 0;

	if (atomic_compare_exchange_strong_explicit(counter, &count,
			IS_ENGINE_DESTROYED, memory_order_acq_rel,
			memory_order_acquire))
		return 0;
	return count & IS_ENGINE_DESTROYED ? -EINVAL : -EBUSY;
}

static void is_engine_counter_decrement(
	is_remote_io_transaction_counter_t *counter)
{
	(void)atomic_fetch_sub_explicit(counter, 1, memory_order_release);
}
#endif

struct is_remote_io_transaction {
	is_transaction_lock_t lock;
	struct is_remote_io_transaction_engine *engine;
	struct is_remote_io_transaction_spec spec;
	struct is_io_policy policy;
	void *adapter_context;
	unsigned int active_events;
	unsigned int backing_retries;
	unsigned char backing_claim;
	unsigned char rdma_result_claim;
	unsigned char rdma_transport_claim;
	unsigned char invariant_reported;
	unsigned char settling;
};

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

static int is_transaction_completion_status(
	const struct is_remote_io_transaction *transaction)
{
	if (transaction->policy.local_done)
		return transaction->policy.local_status;
	return transaction->policy.remote_status;
}

static void is_transaction_finish_event(
	struct is_remote_io_transaction *transaction,
	enum is_remote_io_transaction_event event)
{
	struct is_remote_io_transaction_engine *engine;
	struct is_remote_io_transaction_spec spec;
	is_transaction_lock_flags_t flags;
	void *adapter_context;
	bool report = false;
	bool settle = false;

	is_transaction_lock(&transaction->lock, &flags);
	if (transaction->active_events)
		transaction->active_events--;
	if (!transaction->active_events && !transaction->backing_claim &&
	    !transaction->rdma_result_claim &&
	    !transaction->rdma_transport_claim && !transaction->settling) {
		if (!is_io_policy_releasable(&transaction->policy)) {
			report = is_transaction_note_invariant(transaction);
		} else {
			transaction->settling = 1;
			settle = true;
		}
	}
	if (report || settle) {
		engine = transaction->engine;
		spec = transaction->spec;
		adapter_context = transaction->adapter_context;
	}
	is_transaction_unlock(&transaction->lock, &flags);

	if (report)
		is_remote_io_transaction_adapter_invariant(engine, &spec,
			adapter_context, event);
	if (!settle)
		return;

	is_transaction_lock_destroy(&transaction->lock);
	is_remote_io_transaction_adapter_release(engine, &spec, adapter_context);
	is_engine_counter_decrement(&engine->active_transactions);
	is_remote_io_transaction_adapter_settle(engine, &spec);
}

static void is_transaction_submit_backing(
	struct is_remote_io_transaction *transaction)
{
	int status = is_remote_io_transaction_adapter_submit_backing(
		transaction->engine, &transaction->spec, transaction,
		transaction->adapter_context);

	if (status)
		is_remote_io_transaction_backing_completed(transaction, status);
}

static void is_transaction_apply_action(
	struct is_remote_io_transaction *transaction, enum is_io_action action,
	int completion_status)
{
	if (action & IS_IO_BACKING_DEGRADED)
		is_remote_io_transaction_adapter_backing_degraded(
			transaction->engine, &transaction->spec,
			transaction->adapter_context);
	if (action & IS_IO_MARK_LOCAL_ONLY)
		is_remote_io_transaction_adapter_mark_local_only(
			transaction->engine, &transaction->spec,
			transaction->adapter_context);
	if (action & IS_IO_SUBMIT_LOCAL)
		is_transaction_submit_backing(transaction);
	if (action & IS_IO_COMPLETE_SUCCESS)
		is_remote_io_transaction_adapter_complete(transaction->engine,
			&transaction->spec, transaction->adapter_context, 0);
	else if (action & IS_IO_COMPLETE_ERROR)
		is_remote_io_transaction_adapter_complete(transaction->engine,
			&transaction->spec, transaction->adapter_context,
			completion_status);
}

static int is_transaction_init_policy(
	struct is_remote_io_transaction *transaction)
{
	enum is_io_policy_kind kind;

	switch (transaction->engine->mode) {
	case IS_REMOTE_IO_TRANSACTION_BACKED_STRICT:
		kind = transaction->spec.direction == IS_REMOTE_IO_TRANSACTION_WRITE ?
			IS_IO_POLICY_STRICT_WRITE : IS_IO_POLICY_REMOTE_READ;
		break;
	case IS_REMOTE_IO_TRANSACTION_BACKED_REMOTE_FIRST:
		kind = transaction->spec.direction == IS_REMOTE_IO_TRANSACTION_WRITE ?
			IS_IO_POLICY_REMOTE_FIRST_WRITE : IS_IO_POLICY_REMOTE_READ;
		break;
	case IS_REMOTE_IO_TRANSACTION_REMOTE_ONLY:
		kind = transaction->spec.direction == IS_REMOTE_IO_TRANSACTION_WRITE ?
			IS_IO_POLICY_REMOTE_ONLY_WRITE :
			IS_IO_POLICY_REMOTE_ONLY_READ;
		break;
	default:
		return -EINVAL;
	}

	if (kind == IS_IO_POLICY_STRICT_WRITE ||
	    kind == IS_IO_POLICY_REMOTE_FIRST_WRITE) {
		is_io_policy_init_write(&transaction->policy, kind,
			transaction->spec.generation);
		transaction->backing_claim = 1;
	} else if (kind == IS_IO_POLICY_REMOTE_READ) {
		is_io_policy_init_remote_read(&transaction->policy,
			transaction->spec.generation);
	} else if (kind == IS_IO_POLICY_REMOTE_ONLY_WRITE) {
		is_io_policy_init_remote_only_write(&transaction->policy,
			transaction->spec.generation);
	} else {
		is_io_policy_init_remote_only_read(&transaction->policy,
			transaction->spec.generation);
	}
	transaction->rdma_result_claim = 1;
	transaction->rdma_transport_claim = 1;
	return 0;
}

static bool is_transaction_spec_valid(
	const struct is_remote_io_transaction_spec *spec)
{
	return spec && spec->payload && spec->bytes && spec->generation &&
		(spec->direction == IS_REMOTE_IO_TRANSACTION_READ ||
		 spec->direction == IS_REMOTE_IO_TRANSACTION_WRITE);
}

static void is_transaction_reject_start(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec, int status,
	bool registered)
{
	is_remote_io_transaction_adapter_complete(engine, spec, NULL, status);
	if (registered)
		is_engine_counter_decrement(&engine->active_transactions);
	is_remote_io_transaction_adapter_settle(engine, spec);
}

static void is_transaction_fail_rdma_submission(
	struct is_remote_io_transaction *transaction, int status)
{
	is_remote_io_transaction_rdma_released(transaction,
		transaction->spec.generation);
	is_remote_io_transaction_rdma_completed(transaction,
		transaction->spec.generation, status, false);
}

int is_remote_io_transaction_engine_init(
	struct is_remote_io_transaction_engine *engine,
	enum is_remote_io_transaction_mode mode)
{
	if (!engine || (mode != IS_REMOTE_IO_TRANSACTION_BACKED_STRICT &&
		mode != IS_REMOTE_IO_TRANSACTION_BACKED_REMOTE_FIRST &&
		mode != IS_REMOTE_IO_TRANSACTION_REMOTE_ONLY))
		return -EINVAL;
	memset(engine, 0, sizeof(*engine));
	is_engine_counter_init(&engine->active_transactions);
	engine->mode = mode;
	engine->initialized = 1;
	return 0;
}

int is_remote_io_transaction_engine_destroy(
	struct is_remote_io_transaction_engine *engine)
{
	int status;

	if (!engine)
		return -EINVAL;
	status = is_engine_try_destroy(&engine->active_transactions);
	if (status == -EBUSY) {
		is_remote_io_transaction_adapter_invariant(engine, NULL, NULL,
			IS_REMOTE_IO_TRANSACTION_EVENT_ENGINE_DESTROY_ACTIVE);
		return status;
	}
	if (status)
		return status;
	engine->initialized = 0;
	return 0;
}

void is_remote_io_transaction_start(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec)
{
	struct is_remote_io_transaction_allocation allocation = {0};
	struct is_remote_io_transaction *transaction;
	int status;

	if (!engine || !spec)
		return;
	if (!is_engine_try_register(&engine->active_transactions)) {
		is_transaction_reject_start(engine, spec, -ESHUTDOWN, false);
		return;
	}
	if (!is_transaction_spec_valid(spec)) {
		is_transaction_reject_start(engine, spec, -EINVAL, true);
		return;
	}

	status = is_remote_io_transaction_adapter_allocate(engine, spec,
		sizeof(*transaction), __alignof__(struct is_remote_io_transaction),
		&allocation);
	if (status || !allocation.transaction_storage) {
		is_transaction_reject_start(engine, spec,
			status ? status : -ENOMEM, true);
		return;
	}
	transaction = allocation.transaction_storage;
	memset(transaction, 0, sizeof(*transaction));
	transaction->engine = engine;
	transaction->spec = *spec;
	transaction->adapter_context = allocation.adapter_context;
	status = is_transaction_lock_init(&transaction->lock);
	if (status) {
		is_remote_io_transaction_adapter_release(engine, spec,
			allocation.adapter_context);
		is_transaction_reject_start(engine, spec, status, true);
		return;
	}
	transaction->active_events = 1;
	status = is_transaction_init_policy(transaction);
	if (status) {
		is_transaction_lock_destroy(&transaction->lock);
		is_remote_io_transaction_adapter_release(engine, spec,
			allocation.adapter_context);
		is_transaction_reject_start(engine, spec, status, true);
		return;
	}

	status = is_remote_io_transaction_adapter_prepare(engine,
		&transaction->spec, transaction->backing_claim, transaction,
		transaction->adapter_context);
	if (status) {
		if (transaction->backing_claim)
			is_transaction_submit_backing(transaction);
		is_transaction_fail_rdma_submission(transaction, status);
		is_transaction_finish_event(transaction,
			IS_REMOTE_IO_TRANSACTION_EVENT_RDMA_COMPLETED);
		return;
	}

	/* Backed writes always submit their recovery copy before Remote Memory. */
	if (transaction->backing_claim)
		is_transaction_submit_backing(transaction);
	status = is_remote_io_transaction_adapter_submit_rdma(engine,
		&transaction->spec, transaction, transaction->adapter_context);
	if (status)
		is_transaction_fail_rdma_submission(transaction, status);
	is_transaction_finish_event(transaction,
		IS_REMOTE_IO_TRANSACTION_EVENT_RDMA_COMPLETED);
}

void is_remote_io_transaction_backing_completed(
	struct is_remote_io_transaction *transaction, int status)
{
	is_transaction_lock_flags_t flags;
	enum is_io_action action = IS_IO_NO_ACTION;
	int completion_status = status;
	bool retry = false;
	bool report = false;

	is_transaction_begin_event(transaction, &flags);
	if (!transaction->backing_claim) {
		report = is_transaction_note_invariant(transaction);
	} else if (status && transaction->engine->mode ==
			IS_REMOTE_IO_TRANSACTION_BACKED_REMOTE_FIRST &&
		   transaction->spec.direction == IS_REMOTE_IO_TRANSACTION_WRITE &&
		   !transaction->backing_retries) {
		transaction->backing_retries++;
		retry = true;
	} else {
		action = is_io_policy_local_complete(&transaction->policy, status);
		completion_status = is_transaction_completion_status(transaction);
		transaction->backing_claim = 0;
	}
	is_transaction_unlock(&transaction->lock, &flags);

	if (report)
		is_remote_io_transaction_adapter_invariant(transaction->engine,
			&transaction->spec, transaction->adapter_context,
			IS_REMOTE_IO_TRANSACTION_EVENT_BACKING_COMPLETED);
	if (retry)
		is_transaction_submit_backing(transaction);
	else
		is_transaction_apply_action(transaction, action, completion_status);
	is_transaction_finish_event(transaction,
		IS_REMOTE_IO_TRANSACTION_EVENT_BACKING_COMPLETED);
}

void is_remote_io_transaction_rdma_completed(
	struct is_remote_io_transaction *transaction,
	unsigned long long generation, int status, bool cancelled)
{
	is_transaction_lock_flags_t flags;
	enum is_io_action action = IS_IO_NO_ACTION;
	int completion_status = status;
	bool report = false;

	is_transaction_begin_event(transaction, &flags);
	if (generation != transaction->spec.generation) {
		/* A stale operation owns no claim in this transaction. */
	} else if (!transaction->rdma_result_claim) {
		report = is_transaction_note_invariant(transaction);
	} else {
		action = cancelled ?
			is_io_policy_cancel_remote(&transaction->policy, generation,
				status) :
			is_io_policy_remote_complete(&transaction->policy, generation,
				status);
		if (action & IS_IO_SUBMIT_LOCAL) {
			if (transaction->backing_claim)
				report = is_transaction_note_invariant(transaction);
			else
				transaction->backing_claim = 1;
		}
		completion_status = is_transaction_completion_status(transaction);
		transaction->rdma_result_claim = 0;
	}
	is_transaction_unlock(&transaction->lock, &flags);

	if (report)
		is_remote_io_transaction_adapter_invariant(transaction->engine,
			&transaction->spec, transaction->adapter_context,
			IS_REMOTE_IO_TRANSACTION_EVENT_RDMA_COMPLETED);
	is_transaction_apply_action(transaction, action, completion_status);
	is_transaction_finish_event(transaction,
		IS_REMOTE_IO_TRANSACTION_EVENT_RDMA_COMPLETED);
}

void is_remote_io_transaction_rdma_released(
	struct is_remote_io_transaction *transaction,
	unsigned long long generation)
{
	is_transaction_lock_flags_t flags;
	bool report = false;

	is_transaction_begin_event(transaction, &flags);
	if (generation != transaction->spec.generation) {
		/* A stale transport event cannot release the current claim. */
	} else if (!transaction->rdma_transport_claim) {
		report = is_transaction_note_invariant(transaction);
	} else {
		transaction->rdma_transport_claim = 0;
	}
	is_transaction_unlock(&transaction->lock, &flags);

	if (report)
		is_remote_io_transaction_adapter_invariant(transaction->engine,
			&transaction->spec, transaction->adapter_context,
			IS_REMOTE_IO_TRANSACTION_EVENT_RDMA_RELEASED);
	is_transaction_finish_event(transaction,
		IS_REMOTE_IO_TRANSACTION_EVENT_RDMA_RELEASED);
}
