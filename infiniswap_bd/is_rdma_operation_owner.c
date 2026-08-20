/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#include "is_rdma_operation_owner.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>

typedef unsigned long is_owner_lock_flags_t;

static int is_owner_lock_init(is_rdma_operation_owner_lock_t *lock)
{
	spin_lock_init(lock);
	return 0;
}

static void is_owner_lock(is_rdma_operation_owner_lock_t *lock,
			  is_owner_lock_flags_t *flags)
{
	spin_lock_irqsave(lock, *flags);
}

static void is_owner_unlock(is_rdma_operation_owner_lock_t *lock,
			    is_owner_lock_flags_t *flags)
{
	spin_unlock_irqrestore(lock, *flags);
}

static void is_owner_lock_destroy(is_rdma_operation_owner_lock_t *lock)
{
	(void)lock;
}
#else
#include <errno.h>
#include <string.h>

typedef int is_owner_lock_flags_t;

static int is_owner_lock_init(is_rdma_operation_owner_lock_t *lock)
{
	return pthread_mutex_init(lock, NULL);
}

static void is_owner_lock(is_rdma_operation_owner_lock_t *lock,
			  is_owner_lock_flags_t *flags)
{
	(void)flags;
	(void)pthread_mutex_lock(lock);
}

static void is_owner_unlock(is_rdma_operation_owner_lock_t *lock,
			    is_owner_lock_flags_t *flags)
{
	(void)flags;
	(void)pthread_mutex_unlock(lock);
}

static void is_owner_lock_destroy(is_rdma_operation_owner_lock_t *lock)
{
	(void)pthread_mutex_destroy(lock);
}
#endif

enum is_owner_post_state {
	IS_OWNER_POST_PENDING = 0,
	IS_OWNER_POST_SUCCEEDED,
	IS_OWNER_POST_FAILED,
};

enum is_owner_deadline_state {
	IS_OWNER_DEADLINE_UNARMED = 0,
	IS_OWNER_DEADLINE_ARMING,
	IS_OWNER_DEADLINE_ARMED,
	IS_OWNER_DEADLINE_CANCELLING,
	IS_OWNER_DEADLINE_CANCELLED,
	IS_OWNER_DEADLINE_FIRED,
};

static void is_owner_begin_event(struct is_rdma_operation_owner *owner,
				 is_owner_lock_flags_t *flags)
{
	is_owner_lock(&owner->lock, flags);
	owner->active_events++;
}

static bool is_owner_note_invariant(struct is_rdma_operation_owner *owner)
{
	if (owner->invariant_reported)
		return false;
	owner->invariant_reported = 1;
	return true;
}

static void is_owner_finish_event(struct is_rdma_operation_owner *owner,
				  bool release_submit,
				  bool release_completion,
				  bool release_deadline)
{
	is_owner_lock_flags_t flags;
	bool destroy = false;

	is_owner_lock(&owner->lock, &flags);
	if (release_submit)
		owner->submit_claim = 0;
	if (release_completion)
		owner->completion_claim = 0;
	if (release_deadline)
		owner->deadline_claim = 0;
	if (owner->active_events)
		owner->active_events--;
	if (!owner->active_events && !owner->submit_claim &&
	    !owner->completion_claim && !owner->deadline_claim &&
	    !owner->destroying) {
		owner->destroying = 1;
		destroy = true;
	}
	is_owner_unlock(&owner->lock, &flags);
	if (destroy) {
		is_owner_lock_destroy(&owner->lock);
		is_rdma_operation_adapter_destroy(owner);
	}
}

int is_rdma_operation_owner_init(struct is_rdma_operation_owner *owner)
{
	int result;

	if (!owner)
		return -1;
	memset(owner, 0, sizeof(*owner));
	result = is_owner_lock_init(&owner->lock);
	if (result != 0)
		return result;
	owner->submit_claim = 1;
	owner->completion_claim = 1;
	owner->deadline_claim = 1;
	return 0;
}

void is_rdma_operation_owner_post_succeeded(
	struct is_rdma_operation_owner *owner)
{
	is_owner_lock_flags_t flags;
	bool arm_deadline = false;
	bool cancel_deadline = false;
	bool release_deadline = false;
	bool post_succeeded = false;
	bool report = false;

	is_owner_begin_event(owner, &flags);
	if (owner->post_state != IS_OWNER_POST_PENDING) {
		report = is_owner_note_invariant(owner);
	} else {
		owner->post_state = IS_OWNER_POST_SUCCEEDED;
		post_succeeded = true;
		if (owner->completion_seen) {
			owner->deadline_state = IS_OWNER_DEADLINE_CANCELLED;
			release_deadline = true;
		} else {
			owner->deadline_state = IS_OWNER_DEADLINE_ARMING;
			arm_deadline = true;
		}
	}
	is_owner_unlock(&owner->lock, &flags);

	if (report)
		is_rdma_operation_adapter_invariant(
			owner, IS_RDMA_OPERATION_EVENT_POST_SUCCEEDED);
	if (arm_deadline) {
		is_rdma_operation_adapter_arm_deadline(owner);
		is_owner_lock(&owner->lock, &flags);
		if (owner->deadline_seen) {
			owner->deadline_state = IS_OWNER_DEADLINE_FIRED;
		} else if (owner->completion_seen) {
			owner->deadline_state = IS_OWNER_DEADLINE_CANCELLING;
			cancel_deadline = true;
		} else {
			owner->deadline_state = IS_OWNER_DEADLINE_ARMED;
		}
		is_owner_unlock(&owner->lock, &flags);
	}
	if (cancel_deadline &&
	    is_rdma_operation_adapter_cancel_deadline(owner)) {
		is_owner_lock(&owner->lock, &flags);
		if (!owner->deadline_seen) {
			owner->deadline_state = IS_OWNER_DEADLINE_CANCELLED;
			release_deadline = true;
		}
		is_owner_unlock(&owner->lock, &flags);
	}
	is_owner_finish_event(owner, post_succeeded, false, release_deadline);
}

void is_rdma_operation_owner_post_failed(
	struct is_rdma_operation_owner *owner)
{
	is_owner_lock_flags_t flags;
	bool abort_unposted = false;
	bool post_failed = false;
	bool report = false;

	is_owner_begin_event(owner, &flags);
	if (owner->post_state != IS_OWNER_POST_PENDING) {
		report = is_owner_note_invariant(owner);
	} else {
		owner->post_state = IS_OWNER_POST_FAILED;
		owner->deadline_state = IS_OWNER_DEADLINE_CANCELLED;
		post_failed = true;
		if (owner->completion_seen)
			report = is_owner_note_invariant(owner);
		else
			abort_unposted = true;
	}
	is_owner_unlock(&owner->lock, &flags);

	if (report)
		is_rdma_operation_adapter_invariant(
			owner, IS_RDMA_OPERATION_EVENT_POST_FAILED);
	if (abort_unposted)
		is_rdma_operation_adapter_abort_unposted(owner);
	is_owner_finish_event(owner, post_failed, post_failed, post_failed);
}

void is_rdma_operation_owner_provider_deadline(
	struct is_rdma_operation_owner *owner)
{
	is_owner_lock_flags_t flags;
	bool publish = false;
	bool report = false;
	bool deadline = false;

	is_owner_begin_event(owner, &flags);
	if (owner->deadline_seen ||
	    owner->post_state != IS_OWNER_POST_SUCCEEDED ||
	    owner->deadline_state == IS_OWNER_DEADLINE_CANCELLED) {
		report = is_owner_note_invariant(owner);
	} else if (owner->completion_seen) {
		owner->deadline_seen = 1;
		owner->deadline_state = IS_OWNER_DEADLINE_FIRED;
		deadline = true;
	} else {
		owner->deadline_seen = 1;
		owner->deadline_state = IS_OWNER_DEADLINE_FIRED;
		deadline = true;
		if (owner->terminal == IS_RDMA_OPERATION_TERMINAL_NONE) {
			owner->terminal = IS_RDMA_OPERATION_TERMINAL_DEADLINE;
			publish = true;
		}
	}
	is_owner_unlock(&owner->lock, &flags);

	if (report)
		is_rdma_operation_adapter_invariant(
			owner, IS_RDMA_OPERATION_EVENT_PROVIDER_DEADLINE);
	if (publish)
		is_rdma_operation_adapter_publish_terminal(owner,
			IS_RDMA_OPERATION_TERMINAL_DEADLINE, -ETIMEDOUT);
	is_owner_finish_event(owner, false, false, deadline);
}

void is_rdma_operation_owner_rdma_completed(
	struct is_rdma_operation_owner *owner, int status)
{
	is_owner_lock_flags_t flags;
	bool cancel_deadline = false;
	bool deadline_cancelled = false;
	bool complete = false;
	bool publish = false;
	bool late_completion = false;
	bool report = false;

	is_owner_begin_event(owner, &flags);
	if (owner->completion_seen ||
	    owner->post_state == IS_OWNER_POST_FAILED) {
		report = is_owner_note_invariant(owner);
	} else {
		owner->completion_seen = 1;
		if (owner->terminal == IS_RDMA_OPERATION_TERMINAL_NONE) {
			owner->terminal = IS_RDMA_OPERATION_TERMINAL_COMPLETION;
			publish = true;
		} else {
			late_completion = owner->terminal ==
				IS_RDMA_OPERATION_TERMINAL_DEADLINE;
		}
		cancel_deadline =
			owner->deadline_state == IS_OWNER_DEADLINE_ARMED;
		complete = true;
	}
	is_owner_unlock(&owner->lock, &flags);

	if (report)
		is_rdma_operation_adapter_invariant(
			owner, IS_RDMA_OPERATION_EVENT_RDMA_COMPLETED);
	if (cancel_deadline) {
		deadline_cancelled =
			is_rdma_operation_adapter_cancel_deadline(owner);
		if (deadline_cancelled) {
			is_owner_lock(&owner->lock, &flags);
			owner->deadline_state = IS_OWNER_DEADLINE_CANCELLED;
			is_owner_unlock(&owner->lock, &flags);
		}
	}
	if (complete) {
		is_rdma_operation_adapter_retire_posted(owner, status,
			late_completion);
		if (publish)
			is_rdma_operation_adapter_publish_terminal(owner,
				IS_RDMA_OPERATION_TERMINAL_COMPLETION, status);
		is_rdma_operation_adapter_release_transferred_io(owner);
	}
	is_owner_finish_event(owner, false, complete, deadline_cancelled);
}
