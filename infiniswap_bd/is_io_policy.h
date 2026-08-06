/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#ifndef INFINISWAP_IO_POLICY_H
#define INFINISWAP_IO_POLICY_H

/*
 * Completion decisions are kept independent of interrupt context so the block
 * request and the backing copy can have different lifetimes.
 */
enum is_io_action {
	IS_IO_NO_ACTION = 0,
	IS_IO_SUBMIT_LOCAL,
	IS_IO_COMPLETE_SUCCESS,
	IS_IO_COMPLETE_ERROR,
};

enum is_io_policy_kind {
	IS_IO_POLICY_REMOTE_FIRST_WRITE = 1,
	IS_IO_POLICY_REMOTE_READ,
};

struct is_io_policy {
	enum is_io_policy_kind kind;
	unsigned char local_pending;
	unsigned char remote_pending;
	unsigned char local_done;
	unsigned char remote_done;
	unsigned char request_complete;
	int local_status;
	int remote_status;
};

static inline void is_io_policy_init_remote_first_write(
	struct is_io_policy *state)
{
	state->kind = IS_IO_POLICY_REMOTE_FIRST_WRITE;
	state->local_pending = 1;
	state->remote_pending = 1;
	state->local_done = 0;
	state->remote_done = 0;
	state->request_complete = 0;
	state->local_status = 0;
	state->remote_status = 0;
}

static inline void is_io_policy_init_remote_read(struct is_io_policy *state)
{
	state->kind = IS_IO_POLICY_REMOTE_READ;
	state->local_pending = 0;
	state->remote_pending = 1;
	state->local_done = 0;
	state->remote_done = 0;
	state->request_complete = 0;
	state->local_status = 0;
	state->remote_status = 0;
}

static inline enum is_io_action is_io_policy_complete_request(
	struct is_io_policy *state, int status)
{
	if (state->request_complete)
		return IS_IO_NO_ACTION;
	state->request_complete = 1;
	return status == 0 ? IS_IO_COMPLETE_SUCCESS : IS_IO_COMPLETE_ERROR;
}

static inline enum is_io_action is_io_policy_local_complete(
	struct is_io_policy *state, int status)
{
	if (!state->local_pending || state->local_done)
		return IS_IO_NO_ACTION;
	state->local_pending = 0;
	state->local_done = 1;
	state->local_status = status;

	if (state->kind == IS_IO_POLICY_REMOTE_READ)
		return is_io_policy_complete_request(state, status);
	if (!state->remote_done || state->remote_status == 0)
		return IS_IO_NO_ACTION;
	return is_io_policy_complete_request(state, status);
}

static inline enum is_io_action is_io_policy_remote_complete(
	struct is_io_policy *state, int status)
{
	if (!state->remote_pending || state->remote_done)
		return IS_IO_NO_ACTION;
	state->remote_pending = 0;
	state->remote_done = 1;
	state->remote_status = status;

	if (status == 0)
		return is_io_policy_complete_request(state, 0);
	if (state->kind == IS_IO_POLICY_REMOTE_READ) {
		state->local_pending = 1;
		return IS_IO_SUBMIT_LOCAL;
	}
	if (!state->local_done)
		return IS_IO_NO_ACTION;
	return is_io_policy_complete_request(state, state->local_status);
}

static inline int is_io_policy_releasable(const struct is_io_policy *state)
{
	return state->request_complete && !state->local_pending &&
		!state->remote_pending;
}

#endif /* INFINISWAP_IO_POLICY_H */
