// SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause
#include "../is_rdma_operation_owner.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum recorded_effect {
	EFFECT_ARM_DEADLINE = 1,
	EFFECT_CANCEL_DEADLINE,
	EFFECT_RETIRE_POSTED,
	EFFECT_ABORT_UNPOSTED,
	EFFECT_PUBLISH_TERMINAL,
	EFFECT_RELEASE_IO,
	EFFECT_DESTROY,
	EFFECT_INVARIANT,
};

struct test_barrier {
	pthread_mutex_t lock;
	pthread_cond_t changed;
	unsigned int target;
	unsigned int arrived;
	unsigned int generation;
};

struct effect_recorder {
	pthread_mutex_t lock;
	enum recorded_effect effects[32];
	unsigned int effect_count;
	bool cancel_result;
	bool complete_while_arming;
	bool deadline_while_arming;
	bool deadline_while_cancelling;
	bool complete_while_publishing;
	struct test_barrier *cancel_barrier;
	enum is_rdma_operation_terminal terminal;
	int terminal_status;
	bool late_completion;
};

struct test_operation {
	struct is_rdma_operation_owner owner;
	struct effect_recorder *recorder;
};

struct event_thread {
	struct is_rdma_operation_owner *owner;
	struct test_barrier *start;
	bool completion;
};

static int test_barrier_init(struct test_barrier *barrier, unsigned int target)
{
	int result;

	memset(barrier, 0, sizeof(*barrier));
	barrier->target = target;
	result = pthread_mutex_init(&barrier->lock, NULL);
	if (result != 0)
		return result;
	result = pthread_cond_init(&barrier->changed, NULL);
	if (result != 0)
		(void)pthread_mutex_destroy(&barrier->lock);
	return result;
}

static void test_barrier_wait(struct test_barrier *barrier)
{
	unsigned int generation;

	pthread_mutex_lock(&barrier->lock);
	generation = barrier->generation;
	barrier->arrived++;
	if (barrier->arrived == barrier->target) {
		barrier->arrived = 0;
		barrier->generation++;
		pthread_cond_broadcast(&barrier->changed);
	} else {
		while (generation == barrier->generation)
			pthread_cond_wait(&barrier->changed, &barrier->lock);
	}
	pthread_mutex_unlock(&barrier->lock);
}

static void test_barrier_destroy(struct test_barrier *barrier)
{
	(void)pthread_cond_destroy(&barrier->changed);
	(void)pthread_mutex_destroy(&barrier->lock);
}

static void *run_owner_event(void *context)
{
	struct event_thread *event = context;

	if (event->completion) {
		is_rdma_operation_owner_rdma_completed(event->owner, 0);
	} else {
		test_barrier_wait(event->start);
		is_rdma_operation_owner_provider_deadline(event->owner);
	}
	return NULL;
}

static struct test_operation *test_operation_from_owner(
	struct is_rdma_operation_owner *owner)
{
	return (struct test_operation *)((char *)owner -
		offsetof(struct test_operation, owner));
}

static void record_effect(struct is_rdma_operation_owner *owner,
			  enum recorded_effect effect)
{
	struct effect_recorder *recorder =
		test_operation_from_owner(owner)->recorder;

	pthread_mutex_lock(&recorder->lock);
	recorder->effects[recorder->effect_count++] = effect;
	pthread_mutex_unlock(&recorder->lock);
}

void is_rdma_operation_adapter_arm_deadline(
	struct is_rdma_operation_owner *owner)
{
	struct effect_recorder *recorder =
		test_operation_from_owner(owner)->recorder;

	record_effect(owner, EFFECT_ARM_DEADLINE);
	if (recorder->complete_while_arming)
		is_rdma_operation_owner_rdma_completed(owner, 0);
	if (recorder->deadline_while_arming)
		is_rdma_operation_owner_provider_deadline(owner);
}

bool is_rdma_operation_adapter_cancel_deadline(
	struct is_rdma_operation_owner *owner)
{
	struct effect_recorder *recorder =
		test_operation_from_owner(owner)->recorder;

	record_effect(owner, EFFECT_CANCEL_DEADLINE);
	if (recorder->cancel_barrier)
		test_barrier_wait(recorder->cancel_barrier);
	if (recorder->deadline_while_cancelling)
		is_rdma_operation_owner_provider_deadline(owner);
	return recorder->cancel_result;
}

void is_rdma_operation_adapter_retire_posted(
	struct is_rdma_operation_owner *owner, int status,
	bool late_completion)
{
	struct effect_recorder *recorder =
		test_operation_from_owner(owner)->recorder;

	(void)status;
	pthread_mutex_lock(&recorder->lock);
	recorder->late_completion = late_completion;
	recorder->effects[recorder->effect_count++] = EFFECT_RETIRE_POSTED;
	pthread_mutex_unlock(&recorder->lock);
}

void is_rdma_operation_adapter_abort_unposted(
	struct is_rdma_operation_owner *owner)
{
	record_effect(owner, EFFECT_ABORT_UNPOSTED);
}

void is_rdma_operation_adapter_publish_terminal(
	struct is_rdma_operation_owner *owner,
	enum is_rdma_operation_terminal terminal, int status)
{
	struct effect_recorder *recorder =
		test_operation_from_owner(owner)->recorder;

	pthread_mutex_lock(&recorder->lock);
	recorder->terminal = terminal;
	recorder->terminal_status = status;
	recorder->effects[recorder->effect_count++] = EFFECT_PUBLISH_TERMINAL;
	pthread_mutex_unlock(&recorder->lock);
	if (recorder->complete_while_publishing)
		is_rdma_operation_owner_rdma_completed(owner, 0);
}

void is_rdma_operation_adapter_release_transferred_io(
	struct is_rdma_operation_owner *owner)
{
	record_effect(owner, EFFECT_RELEASE_IO);
}

void is_rdma_operation_adapter_destroy(
	struct is_rdma_operation_owner *owner)
{
	record_effect(owner, EFFECT_DESTROY);
}

void is_rdma_operation_adapter_invariant(
	struct is_rdma_operation_owner *owner,
	enum is_rdma_operation_owner_event event)
{
	(void)event;
	record_effect(owner, EFFECT_INVARIANT);
}

static int expect_effects(const struct effect_recorder *recorder,
			  const enum recorded_effect *expected,
			  unsigned int expected_count, const char *name)
{
	if (recorder->effect_count != expected_count ||
	    memcmp(recorder->effects, expected,
		   expected_count * sizeof(expected[0])) != 0) {
		unsigned int index;

		fprintf(stderr, "%s: effects", name);
		for (index = 0; index < recorder->effect_count; index++)
			fprintf(stderr, " %d", recorder->effects[index]);
		fprintf(stderr, "\n");
		return 1;
	}
	return 0;
}

static int test_completion_first_retires_before_terminal(void)
{
	const enum recorded_effect expected[] = {
		EFFECT_ARM_DEADLINE,
		EFFECT_CANCEL_DEADLINE,
		EFFECT_RETIRE_POSTED,
		EFFECT_PUBLISH_TERMINAL,
		EFFECT_RELEASE_IO,
		EFFECT_DESTROY,
	};
	struct effect_recorder recorder = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
		.cancel_result = true,
	};
	struct test_operation operation = {
		.recorder = &recorder,
	};

	if (is_rdma_operation_owner_init(&operation.owner) != 0) {
		fprintf(stderr, "completion first: owner init failed\n");
		return 1;
	}
	is_rdma_operation_owner_post_succeeded(&operation.owner);
	is_rdma_operation_owner_rdma_completed(&operation.owner, 0);

	return expect_effects(&recorder, expected,
		sizeof(expected) / sizeof(expected[0]), "completion first") |
		(recorder.terminal != IS_RDMA_OPERATION_TERMINAL_COMPLETION) |
		(recorder.terminal_status != 0) |
		recorder.late_completion;
}

static int test_deadline_first_waits_for_transport_retirement(void)
{
	const enum recorded_effect expected[] = {
		EFFECT_ARM_DEADLINE,
		EFFECT_PUBLISH_TERMINAL,
		EFFECT_RETIRE_POSTED,
		EFFECT_RELEASE_IO,
		EFFECT_DESTROY,
	};
	struct effect_recorder recorder = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
	};
	struct test_operation operation = {
		.recorder = &recorder,
	};

	if (is_rdma_operation_owner_init(&operation.owner) != 0) {
		fprintf(stderr, "deadline first: owner init failed\n");
		return 1;
	}
	is_rdma_operation_owner_post_succeeded(&operation.owner);
	is_rdma_operation_owner_provider_deadline(&operation.owner);
	is_rdma_operation_owner_rdma_completed(&operation.owner, 0);

	return expect_effects(&recorder, expected,
		sizeof(expected) / sizeof(expected[0]), "deadline first") |
		(recorder.terminal != IS_RDMA_OPERATION_TERMINAL_DEADLINE) |
		(recorder.terminal_status != -ETIMEDOUT) |
		!recorder.late_completion;
}

static int test_completion_before_submit_returns_does_not_arm_deadline(void)
{
	const enum recorded_effect expected[] = {
		EFFECT_RETIRE_POSTED,
		EFFECT_PUBLISH_TERMINAL,
		EFFECT_RELEASE_IO,
		EFFECT_DESTROY,
	};
	struct effect_recorder recorder = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
	};
	struct test_operation operation = {
		.recorder = &recorder,
	};

	if (is_rdma_operation_owner_init(&operation.owner) != 0) {
		fprintf(stderr, "completion before submit: owner init failed\n");
		return 1;
	}
	is_rdma_operation_owner_rdma_completed(&operation.owner, 0);
	is_rdma_operation_owner_post_succeeded(&operation.owner);

	return expect_effects(&recorder, expected,
		sizeof(expected) / sizeof(expected[0]),
		"completion before submit") |
		(recorder.terminal != IS_RDMA_OPERATION_TERMINAL_COMPLETION);
}

static int test_post_failure_aborts_without_transferring_io(void)
{
	const enum recorded_effect expected[] = {
		EFFECT_ABORT_UNPOSTED,
		EFFECT_DESTROY,
	};
	struct effect_recorder recorder = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
	};
	struct test_operation operation = {
		.recorder = &recorder,
	};

	if (is_rdma_operation_owner_init(&operation.owner) != 0) {
		fprintf(stderr, "post failure: owner init failed\n");
		return 1;
	}
	is_rdma_operation_owner_post_failed(&operation.owner);

	return expect_effects(&recorder, expected,
		sizeof(expected) / sizeof(expected[0]), "post failure");
}

static int test_completion_during_deadline_arm_is_cancelled(void)
{
	const enum recorded_effect expected[] = {
		EFFECT_ARM_DEADLINE,
		EFFECT_RETIRE_POSTED,
		EFFECT_PUBLISH_TERMINAL,
		EFFECT_RELEASE_IO,
		EFFECT_CANCEL_DEADLINE,
		EFFECT_DESTROY,
	};
	struct effect_recorder recorder = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
		.cancel_result = true,
		.complete_while_arming = true,
	};
	struct test_operation operation = {
		.recorder = &recorder,
	};

	if (is_rdma_operation_owner_init(&operation.owner) != 0) {
		fprintf(stderr, "completion during arm: owner init failed\n");
		return 1;
	}
	is_rdma_operation_owner_post_succeeded(&operation.owner);

	return expect_effects(&recorder, expected,
		sizeof(expected) / sizeof(expected[0]),
		"completion during arm");
}

static int test_running_deadline_loses_to_completion_claim(void)
{
	const enum recorded_effect expected[] = {
		EFFECT_ARM_DEADLINE,
		EFFECT_CANCEL_DEADLINE,
		EFFECT_RETIRE_POSTED,
		EFFECT_PUBLISH_TERMINAL,
		EFFECT_RELEASE_IO,
		EFFECT_DESTROY,
	};
	struct effect_recorder recorder = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
		.cancel_result = false,
		.deadline_while_cancelling = true,
	};
	struct test_operation operation = {
		.recorder = &recorder,
	};

	if (is_rdma_operation_owner_init(&operation.owner) != 0) {
		fprintf(stderr, "running deadline: owner init failed\n");
		return 1;
	}
	is_rdma_operation_owner_post_succeeded(&operation.owner);
	is_rdma_operation_owner_rdma_completed(&operation.owner, 0);

	return expect_effects(&recorder, expected,
		sizeof(expected) / sizeof(expected[0]), "running deadline") |
		(recorder.terminal != IS_RDMA_OPERATION_TERMINAL_COMPLETION);
}

static int test_duplicate_and_illegal_events_report_once(void)
{
	const enum recorded_effect expected[] = {
		EFFECT_ARM_DEADLINE,
		EFFECT_INVARIANT,
		EFFECT_CANCEL_DEADLINE,
		EFFECT_RETIRE_POSTED,
		EFFECT_PUBLISH_TERMINAL,
		EFFECT_RELEASE_IO,
		EFFECT_DESTROY,
	};
	struct effect_recorder recorder = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
		.cancel_result = true,
	};
	struct test_operation operation = {
		.recorder = &recorder,
	};

	if (is_rdma_operation_owner_init(&operation.owner) != 0) {
		fprintf(stderr, "duplicate events: owner init failed\n");
		return 1;
	}
	is_rdma_operation_owner_post_succeeded(&operation.owner);
	is_rdma_operation_owner_post_succeeded(&operation.owner);
	is_rdma_operation_owner_post_failed(&operation.owner);
	is_rdma_operation_owner_rdma_completed(&operation.owner, 0);

	return expect_effects(&recorder, expected,
		sizeof(expected) / sizeof(expected[0]), "duplicate events");
}

static int test_deadline_adapter_reentry_keeps_owner_alive(void)
{
	const enum recorded_effect expected[] = {
		EFFECT_ARM_DEADLINE,
		EFFECT_PUBLISH_TERMINAL,
		EFFECT_RETIRE_POSTED,
		EFFECT_RELEASE_IO,
		EFFECT_DESTROY,
	};
	struct effect_recorder recorder = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
		.complete_while_publishing = true,
	};
	struct test_operation operation = {
		.recorder = &recorder,
	};

	if (is_rdma_operation_owner_init(&operation.owner) != 0) {
		fprintf(stderr, "deadline reentry: owner init failed\n");
		return 1;
	}
	is_rdma_operation_owner_post_succeeded(&operation.owner);
	is_rdma_operation_owner_provider_deadline(&operation.owner);

	return expect_effects(&recorder, expected,
		sizeof(expected) / sizeof(expected[0]), "deadline reentry") |
		(recorder.terminal != IS_RDMA_OPERATION_TERMINAL_DEADLINE) |
		!recorder.late_completion;
}

static int test_concurrent_completion_claim_wins_before_running_deadline(void)
{
	const enum recorded_effect expected[] = {
		EFFECT_ARM_DEADLINE,
		EFFECT_CANCEL_DEADLINE,
		EFFECT_RETIRE_POSTED,
		EFFECT_PUBLISH_TERMINAL,
		EFFECT_RELEASE_IO,
		EFFECT_DESTROY,
	};
	struct test_barrier barrier;
	struct effect_recorder recorder = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
		.cancel_result = false,
		.cancel_barrier = &barrier,
	};
	struct test_operation operation = {
		.recorder = &recorder,
	};
	struct event_thread completion = {
		.owner = &operation.owner,
		.start = &barrier,
		.completion = true,
	};
	struct event_thread deadline = {
		.owner = &operation.owner,
		.start = &barrier,
		.completion = false,
	};
	pthread_t completion_thread;
	pthread_t deadline_thread;
	int failed = 0;

	if (test_barrier_init(&barrier, 2) != 0 ||
	    is_rdma_operation_owner_init(&operation.owner) != 0) {
		fprintf(stderr, "concurrent events: init failed\n");
		return 1;
	}
	is_rdma_operation_owner_post_succeeded(&operation.owner);
	if (pthread_create(&completion_thread, NULL, run_owner_event,
			   &completion) != 0 ||
	    pthread_create(&deadline_thread, NULL, run_owner_event, &deadline) !=
		0) {
		fprintf(stderr, "concurrent events: thread creation failed\n");
		return 1;
	}
	(void)pthread_join(completion_thread, NULL);
	(void)pthread_join(deadline_thread, NULL);
	failed = expect_effects(&recorder, expected,
		sizeof(expected) / sizeof(expected[0]), "concurrent events") |
		(recorder.terminal != IS_RDMA_OPERATION_TERMINAL_COMPLETION);
	test_barrier_destroy(&barrier);
	return failed;
}

static int test_completion_error_is_published_once(void)
{
	const enum recorded_effect expected[] = {
		EFFECT_ARM_DEADLINE,
		EFFECT_CANCEL_DEADLINE,
		EFFECT_RETIRE_POSTED,
		EFFECT_PUBLISH_TERMINAL,
		EFFECT_RELEASE_IO,
		EFFECT_DESTROY,
	};
	struct effect_recorder recorder = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
		.cancel_result = true,
	};
	struct test_operation operation = {
		.recorder = &recorder,
	};

	if (is_rdma_operation_owner_init(&operation.owner) != 0) {
		fprintf(stderr, "completion error: owner init failed\n");
		return 1;
	}
	is_rdma_operation_owner_post_succeeded(&operation.owner);
	is_rdma_operation_owner_rdma_completed(&operation.owner, -EIO);

	return expect_effects(&recorder, expected,
		sizeof(expected) / sizeof(expected[0]), "completion error") |
		(recorder.terminal_status != -EIO);
}

static int test_deadline_during_arm_remains_retirable(void)
{
	const enum recorded_effect expected[] = {
		EFFECT_ARM_DEADLINE,
		EFFECT_PUBLISH_TERMINAL,
		EFFECT_RETIRE_POSTED,
		EFFECT_RELEASE_IO,
		EFFECT_DESTROY,
	};
	struct effect_recorder recorder = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
		.deadline_while_arming = true,
	};
	struct test_operation operation = {
		.recorder = &recorder,
	};

	if (is_rdma_operation_owner_init(&operation.owner) != 0) {
		fprintf(stderr, "deadline during arm: owner init failed\n");
		return 1;
	}
	is_rdma_operation_owner_post_succeeded(&operation.owner);
	is_rdma_operation_owner_rdma_completed(&operation.owner, 0);

	return expect_effects(&recorder, expected,
		sizeof(expected) / sizeof(expected[0]), "deadline during arm") |
		(recorder.terminal != IS_RDMA_OPERATION_TERMINAL_DEADLINE);
}

static int test_duplicate_completion_and_deadline_have_no_effect(void)
{
	const enum recorded_effect duplicate_completion[] = {
		EFFECT_RETIRE_POSTED,
		EFFECT_PUBLISH_TERMINAL,
		EFFECT_RELEASE_IO,
		EFFECT_INVARIANT,
		EFFECT_DESTROY,
	};
	const enum recorded_effect duplicate_deadline[] = {
		EFFECT_ARM_DEADLINE,
		EFFECT_PUBLISH_TERMINAL,
		EFFECT_INVARIANT,
		EFFECT_RETIRE_POSTED,
		EFFECT_RELEASE_IO,
		EFFECT_DESTROY,
	};
	struct effect_recorder completion_recorder = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
	};
	struct effect_recorder deadline_recorder = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
	};
	struct test_operation completion_operation = {
		.recorder = &completion_recorder,
	};
	struct test_operation deadline_operation = {
		.recorder = &deadline_recorder,
	};
	int failed = 0;

	if (is_rdma_operation_owner_init(&completion_operation.owner) != 0 ||
	    is_rdma_operation_owner_init(&deadline_operation.owner) != 0) {
		fprintf(stderr, "duplicate terminal events: owner init failed\n");
		return 1;
	}
	is_rdma_operation_owner_rdma_completed(&completion_operation.owner, 0);
	is_rdma_operation_owner_rdma_completed(&completion_operation.owner, 0);
	is_rdma_operation_owner_provider_deadline(&completion_operation.owner);
	is_rdma_operation_owner_post_succeeded(&completion_operation.owner);
	failed |= expect_effects(&completion_recorder, duplicate_completion,
		sizeof(duplicate_completion) / sizeof(duplicate_completion[0]),
		"duplicate completion");

	is_rdma_operation_owner_post_succeeded(&deadline_operation.owner);
	is_rdma_operation_owner_provider_deadline(&deadline_operation.owner);
	is_rdma_operation_owner_provider_deadline(&deadline_operation.owner);
	is_rdma_operation_owner_rdma_completed(&deadline_operation.owner, 0);
	failed |= expect_effects(&deadline_recorder, duplicate_deadline,
		sizeof(duplicate_deadline) / sizeof(duplicate_deadline[0]),
		"duplicate deadline");
	return failed;
}

int main(void)
{
	return test_completion_first_retires_before_terminal() |
		test_deadline_first_waits_for_transport_retirement() |
		test_completion_before_submit_returns_does_not_arm_deadline() |
		test_post_failure_aborts_without_transferring_io() |
		test_completion_during_deadline_arm_is_cancelled() |
		test_running_deadline_loses_to_completion_claim() |
		test_duplicate_and_illegal_events_report_once() |
		test_deadline_adapter_reentry_keeps_owner_alive() |
		test_concurrent_completion_claim_wins_before_running_deadline() |
		test_completion_error_is_published_once() |
		test_deadline_during_arm_remains_retirable() |
		test_duplicate_completion_and_deadline_have_no_effect();
}
