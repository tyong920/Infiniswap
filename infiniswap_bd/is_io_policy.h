/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#ifndef INFINISWAP_IO_POLICY_H
#define INFINISWAP_IO_POLICY_H

/*
 * Completion decisions stay independent of interrupt context so the block
 * request, Backing Store copy, and RDMA transport can have separate lifetimes.
 */
enum is_io_action {
	IS_IO_NO_ACTION = 0,
	IS_IO_SUBMIT_LOCAL = 1U << 0,
	IS_IO_COMPLETE_SUCCESS = 1U << 1,
	IS_IO_COMPLETE_ERROR = 1U << 2,
	IS_IO_BACKING_DEGRADED = 1U << 3,
	IS_IO_MARK_LOCAL_ONLY = 1U << 4,
};

enum is_io_policy_kind {
	IS_IO_POLICY_STRICT_WRITE = 1,
	IS_IO_POLICY_REMOTE_FIRST_WRITE,
	IS_IO_POLICY_REMOTE_READ,
	IS_IO_POLICY_REMOTE_ONLY_WRITE,
	IS_IO_POLICY_REMOTE_ONLY_READ,
};

struct is_io_policy {
	enum is_io_policy_kind kind;
	unsigned long long generation;
	unsigned char local_pending;
	unsigned char remote_pending;
	unsigned char local_done;
	unsigned char remote_done;
	unsigned char remote_cancelled;
	unsigned char request_complete;
	unsigned char backing_degraded;
	int local_status;
	int remote_status;
};

static inline void is_io_policy_init_write(struct is_io_policy *state,
					   enum is_io_policy_kind kind,
					   unsigned long long generation)
{
	state->kind = kind;
	state->generation = generation;
	state->local_pending = 1;
	state->remote_pending = 1;
	state->local_done = 0;
	state->remote_done = 0;
	state->remote_cancelled = 0;
	state->request_complete = 0;
	state->backing_degraded = 0;
	state->local_status = 0;
	state->remote_status = 0;
}

static inline void is_io_policy_init_remote(
	struct is_io_policy *state, enum is_io_policy_kind kind,
	unsigned long long generation)
{
	state->kind = kind;
	state->generation = generation;
	state->local_pending = 0;
	state->remote_pending = 1;
	state->local_done = 0;
	state->remote_done = 0;
	state->remote_cancelled = 0;
	state->request_complete = 0;
	state->backing_degraded = 0;
	state->local_status = 0;
	state->remote_status = 0;
}

static inline void is_io_policy_init_remote_read(struct is_io_policy *state,
						 unsigned long long generation)
{
	is_io_policy_init_remote(state, IS_IO_POLICY_REMOTE_READ, generation);
}

static inline void is_io_policy_init_remote_only_write(
	struct is_io_policy *state, unsigned long long generation)
{
	is_io_policy_init_remote(state, IS_IO_POLICY_REMOTE_ONLY_WRITE,
		generation);
}

static inline void is_io_policy_init_remote_only_read(
	struct is_io_policy *state, unsigned long long generation)
{
	is_io_policy_init_remote(state, IS_IO_POLICY_REMOTE_ONLY_READ,
		generation);
}

static inline enum is_io_action is_io_policy_complete_request(
	struct is_io_policy *state, int status)
{
	if (state->request_complete)
		return IS_IO_NO_ACTION;
	state->request_complete = 1;
	return status == 0 ? IS_IO_COMPLETE_SUCCESS : IS_IO_COMPLETE_ERROR;
}

static inline enum is_io_action is_io_policy_finish_write(
	struct is_io_policy *state)
{
	enum is_io_action action = IS_IO_NO_ACTION;

	if (state->kind == IS_IO_POLICY_STRICT_WRITE) {
		if (state->local_status == 0 && state->remote_status != 0)
			action |= IS_IO_MARK_LOCAL_ONLY;
		action |= is_io_policy_complete_request(state,
			state->local_status);
		return action;
	}

	if (state->remote_status == 0)
		return is_io_policy_complete_request(state, 0);
	if (state->local_status == 0)
		action |= IS_IO_MARK_LOCAL_ONLY;
	action |= is_io_policy_complete_request(state, state->local_status);
	return action;
}

static inline enum is_io_action is_io_policy_local_complete(
	struct is_io_policy *state, int status)
{
	enum is_io_action action = IS_IO_NO_ACTION;

	if (!state->local_pending || state->local_done)
		return IS_IO_NO_ACTION;
	state->local_pending = 0;
	state->local_done = 1;
	state->local_status = status;
	if (status != 0) {
		state->backing_degraded = 1;
		action |= IS_IO_BACKING_DEGRADED;
	}

	if (state->kind == IS_IO_POLICY_REMOTE_READ)
		return action | is_io_policy_complete_request(state, status);
	if (!state->remote_done)
		return action;
	return action | is_io_policy_finish_write(state);
}

static inline enum is_io_action is_io_policy_remote_terminal(
	struct is_io_policy *state, int status)
{
	state->remote_pending = 0;
	state->remote_done = 1;
	state->remote_status = status;

	if (state->kind == IS_IO_POLICY_REMOTE_ONLY_WRITE ||
	    state->kind == IS_IO_POLICY_REMOTE_ONLY_READ)
		return is_io_policy_complete_request(state, status);
	if (state->kind == IS_IO_POLICY_REMOTE_READ) {
		if (status == 0)
			return is_io_policy_complete_request(state, 0);
		state->local_pending = 1;
		return IS_IO_SUBMIT_LOCAL;
	}
	if (state->kind == IS_IO_POLICY_REMOTE_FIRST_WRITE && status == 0)
		return is_io_policy_complete_request(state, 0);
	if (!state->local_done)
		return IS_IO_NO_ACTION;
	return is_io_policy_finish_write(state);
}

static inline enum is_io_action is_io_policy_remote_complete(
	struct is_io_policy *state, unsigned long long generation, int status)
{
	if (generation != state->generation || state->remote_cancelled ||
	    !state->remote_pending || state->remote_done)
		return IS_IO_NO_ACTION;
	return is_io_policy_remote_terminal(state, status);
}

static inline enum is_io_action is_io_policy_cancel_remote(
	struct is_io_policy *state, unsigned long long generation, int status)
{
	if (generation != state->generation || state->remote_cancelled ||
	    !state->remote_pending || state->remote_done)
		return IS_IO_NO_ACTION;
	state->remote_cancelled = 1;
	return is_io_policy_remote_terminal(state, status);
}

static inline int is_io_policy_releasable(const struct is_io_policy *state)
{
	return state->request_complete && !state->local_pending &&
		!state->remote_pending;
}

#endif /* INFINISWAP_IO_POLICY_H */
