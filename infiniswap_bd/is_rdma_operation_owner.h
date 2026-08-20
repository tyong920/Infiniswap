/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#ifndef INFINISWAP_RDMA_OPERATION_OWNER_H
#define INFINISWAP_RDMA_OPERATION_OWNER_H

#ifdef __KERNEL__
#include <linux/spinlock.h>
#include <linux/types.h>

typedef spinlock_t is_rdma_operation_owner_lock_t;
#else
#include <pthread.h>
#include <stdbool.h>

typedef pthread_mutex_t is_rdma_operation_owner_lock_t;
#endif

enum is_rdma_operation_owner_event {
	IS_RDMA_OPERATION_EVENT_POST_SUCCEEDED = 1,
	IS_RDMA_OPERATION_EVENT_POST_FAILED,
	IS_RDMA_OPERATION_EVENT_PROVIDER_DEADLINE,
	IS_RDMA_OPERATION_EVENT_RDMA_COMPLETED,
};

enum is_rdma_operation_terminal {
	IS_RDMA_OPERATION_TERMINAL_NONE = 0,
	IS_RDMA_OPERATION_TERMINAL_COMPLETION,
	IS_RDMA_OPERATION_TERMINAL_DEADLINE,
};

/*
 * The owner is embedded in the transport operation. Named claims describe
 * which caller may still deliver an event; active_events protects Adapter
 * re-entry while effects execute outside the lock.
 */
struct is_rdma_operation_owner {
	is_rdma_operation_owner_lock_t lock;
	unsigned int active_events;
	unsigned char submit_claim;
	unsigned char completion_claim;
	unsigned char deadline_claim;
	unsigned char post_state;
	unsigned char deadline_state;
	unsigned char completion_seen;
	unsigned char deadline_seen;
	unsigned char invariant_reported;
	unsigned char destroying;
	enum is_rdma_operation_terminal terminal;
};

int is_rdma_operation_owner_init(struct is_rdma_operation_owner *owner);
void is_rdma_operation_owner_post_succeeded(
	struct is_rdma_operation_owner *owner);
void is_rdma_operation_owner_post_failed(
	struct is_rdma_operation_owner *owner);
void is_rdma_operation_owner_provider_deadline(
	struct is_rdma_operation_owner *owner);
void is_rdma_operation_owner_rdma_completed(
	struct is_rdma_operation_owner *owner, int status);

/* Link-time Adapters implemented by the kernel transport and host tests. */
void is_rdma_operation_adapter_arm_deadline(
	struct is_rdma_operation_owner *owner);
bool is_rdma_operation_adapter_cancel_deadline(
	struct is_rdma_operation_owner *owner);
void is_rdma_operation_adapter_retire_posted(
	struct is_rdma_operation_owner *owner, int status,
	bool late_completion);
void is_rdma_operation_adapter_abort_unposted(
	struct is_rdma_operation_owner *owner);
void is_rdma_operation_adapter_publish_terminal(
	struct is_rdma_operation_owner *owner,
	enum is_rdma_operation_terminal terminal, int status);
void is_rdma_operation_adapter_release_transferred_io(
	struct is_rdma_operation_owner *owner);
void is_rdma_operation_adapter_destroy(
	struct is_rdma_operation_owner *owner);
void is_rdma_operation_adapter_invariant(
	struct is_rdma_operation_owner *owner,
	enum is_rdma_operation_owner_event event);

#endif /* INFINISWAP_RDMA_OPERATION_OWNER_H */
