// SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause
#include "../is_remote_io_transaction.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum recorded_effect {
	EFFECT_COMPLETE_SUCCESS = 1,
	EFFECT_COMPLETE_ERROR,
	EFFECT_BACKING_DEGRADED,
	EFFECT_MARK_LOCAL_ONLY,
	EFFECT_SUBMIT_LOCAL,
	EFFECT_SETTLE,
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
	enum recorded_effect effects[64];
	unsigned int effect_count;
	unsigned int completions;
	unsigned int successes;
	unsigned int errors;
	unsigned int degraded;
	unsigned int local_only;
	unsigned int local_submissions;
	unsigned int settlements;
	unsigned int invariants;
	int completion_status;
	bool complete_local_during_submit;
	int submitted_local_status;
};

struct test_transaction {
	struct is_remote_io_transaction transaction;
	struct effect_recorder *recorder;
};

enum threaded_event_kind {
	THREADED_EVENT_DISPATCHER = 1,
	THREADED_EVENT_LOCAL,
	THREADED_EVENT_REMOTE,
	THREADED_EVENT_TRANSPORT,
};

struct threaded_event {
	struct is_remote_io_transaction *transaction;
	struct test_barrier *start;
	enum threaded_event_kind kind;
	unsigned long long generation;
	int status;
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

static struct test_transaction *test_transaction_from_embedded_transaction(
	struct is_remote_io_transaction *transaction)
{
	return (struct test_transaction *)((char *)transaction -
		offsetof(struct test_transaction, transaction));
}

void is_remote_io_transaction_adapter_complete(
	struct is_remote_io_transaction *transaction, int status)
{
	struct effect_recorder *recorder =
		test_transaction_from_embedded_transaction(transaction)->recorder;

	pthread_mutex_lock(&recorder->lock);
	recorder->completion_status = status;
	recorder->completions++;
	if (status == 0) {
		recorder->successes++;
		recorder->effects[recorder->effect_count++] = EFFECT_COMPLETE_SUCCESS;
	} else {
		recorder->errors++;
		recorder->effects[recorder->effect_count++] = EFFECT_COMPLETE_ERROR;
	}
	pthread_mutex_unlock(&recorder->lock);
}

void is_remote_io_transaction_adapter_backing_degraded(
	struct is_remote_io_transaction *transaction)
{
	struct effect_recorder *recorder =
		test_transaction_from_embedded_transaction(transaction)->recorder;

	pthread_mutex_lock(&recorder->lock);
	recorder->degraded++;
	recorder->effects[recorder->effect_count++] = EFFECT_BACKING_DEGRADED;
	pthread_mutex_unlock(&recorder->lock);
}

void is_remote_io_transaction_adapter_mark_local_only(
	struct is_remote_io_transaction *transaction)
{
	struct effect_recorder *recorder =
		test_transaction_from_embedded_transaction(transaction)->recorder;

	pthread_mutex_lock(&recorder->lock);
	recorder->local_only++;
	recorder->effects[recorder->effect_count++] = EFFECT_MARK_LOCAL_ONLY;
	pthread_mutex_unlock(&recorder->lock);
}

void is_remote_io_transaction_adapter_submit_local(
	struct is_remote_io_transaction *transaction)
{
	struct effect_recorder *recorder =
		test_transaction_from_embedded_transaction(transaction)->recorder;
	bool complete_local;
	int status;

	pthread_mutex_lock(&recorder->lock);
	recorder->local_submissions++;
	recorder->effects[recorder->effect_count++] = EFFECT_SUBMIT_LOCAL;
	complete_local = recorder->complete_local_during_submit;
	status = recorder->submitted_local_status;
	pthread_mutex_unlock(&recorder->lock);
	if (complete_local)
		is_remote_io_transaction_local_completed(transaction, status);
}

void is_remote_io_transaction_adapter_settle(
	struct is_remote_io_transaction *transaction)
{
	struct effect_recorder *recorder =
		test_transaction_from_embedded_transaction(transaction)->recorder;

	pthread_mutex_lock(&recorder->lock);
	recorder->settlements++;
	recorder->effects[recorder->effect_count++] = EFFECT_SETTLE;
	pthread_mutex_unlock(&recorder->lock);
}

void is_remote_io_transaction_adapter_invariant(
	struct is_remote_io_transaction *transaction,
	enum is_remote_io_transaction_event event)
{
	struct effect_recorder *recorder =
		test_transaction_from_embedded_transaction(transaction)->recorder;

	(void)event;
	pthread_mutex_lock(&recorder->lock);
	recorder->invariants++;
	recorder->effects[recorder->effect_count++] = EFFECT_INVARIANT;
	pthread_mutex_unlock(&recorder->lock);
}

static void *run_transaction_event(void *context)
{
	struct threaded_event *event = context;

	test_barrier_wait(event->start);
	switch (event->kind) {
	case THREADED_EVENT_DISPATCHER:
		is_remote_io_transaction_dispatcher_released(event->transaction);
		break;
	case THREADED_EVENT_LOCAL:
		is_remote_io_transaction_local_completed(event->transaction,
			event->status);
		break;
	case THREADED_EVENT_REMOTE:
		is_remote_io_transaction_remote_completed(event->transaction,
			event->generation, event->status, false);
		break;
	case THREADED_EVENT_TRANSPORT:
		is_remote_io_transaction_transport_released(event->transaction,
			event->generation);
		break;
	}
	return NULL;
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

static int test_write_outcome_and_completion_order_matrix(void)
{
	const enum is_io_policy_kind policies[] = {
		IS_IO_POLICY_STRICT_WRITE,
		IS_IO_POLICY_REMOTE_FIRST_WRITE,
	};
	unsigned int policy_index;
	int failed = 0;

	for (policy_index = 0;
	     policy_index < sizeof(policies) / sizeof(policies[0]);
	     policy_index++) {
		int local_status;

		for (local_status = 0; local_status >= -EIO;
		     local_status -= EIO) {
			int remote_status;

			for (remote_status = 0; remote_status >= -EIO;
			     remote_status -= EIO) {
				unsigned int local_first;

				for (local_first = 0; local_first <= 1;
				     local_first++) {
					struct effect_recorder recorder = {
						.lock = PTHREAD_MUTEX_INITIALIZER,
					};
					struct test_transaction transaction = {
						.recorder = &recorder,
					};
					int expect_success;
					int expect_local_only;

					if (is_remote_io_transaction_init(
						    &transaction.transaction,
						    policies[policy_index], 41) != 0) {
						fprintf(stderr, "matrix: init failed\n");
						return 1;
					}
					is_remote_io_transaction_dispatcher_released(
						&transaction.transaction);
					if (local_first) {
						is_remote_io_transaction_local_completed(
							&transaction.transaction,
							local_status);
						is_remote_io_transaction_remote_completed(
							&transaction.transaction, 41,
							remote_status, false);
					} else {
						is_remote_io_transaction_remote_completed(
							&transaction.transaction, 41,
							remote_status, false);
						is_remote_io_transaction_local_completed(
							&transaction.transaction,
							local_status);
					}
					if (recorder.settlements != 0) {
						fprintf(stderr,
							"matrix settled before transport release\n");
						failed = 1;
					}
					is_remote_io_transaction_transport_released(
						&transaction.transaction, 41);

					expect_success = policies[policy_index] ==
						IS_IO_POLICY_STRICT_WRITE ?
						local_status == 0 :
						(local_status == 0 || remote_status == 0);
					expect_local_only = local_status == 0 &&
						remote_status != 0;
					if (recorder.completions != 1 ||
					    recorder.successes !=
						    (unsigned int)expect_success ||
					    recorder.errors !=
						    (unsigned int)!expect_success ||
					    recorder.degraded !=
						    (unsigned int)(local_status != 0) ||
					    recorder.local_only !=
						    (unsigned int)expect_local_only ||
					    recorder.local_submissions != 0 ||
					    recorder.settlements != 1 ||
					    recorder.invariants != 0) {
						fprintf(stderr,
							"matrix failed: policy=%d local=%d remote=%d local_first=%u\n",
							policies[policy_index],
							local_status, remote_status,
							local_first);
						failed = 1;
					}
				}
			}
		}
	}
	return failed;
}

static int test_remote_first_acknowledges_before_late_settlement(void)
{
	const enum recorded_effect before_local[] = {
		EFFECT_COMPLETE_SUCCESS,
	};
	const enum recorded_effect after_local[] = {
		EFFECT_COMPLETE_SUCCESS,
		EFFECT_BACKING_DEGRADED,
	};
	const enum recorded_effect after_transport[] = {
		EFFECT_COMPLETE_SUCCESS,
		EFFECT_BACKING_DEGRADED,
		EFFECT_SETTLE,
	};
	struct effect_recorder recorder = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
	};
	struct test_transaction transaction = {
		.recorder = &recorder,
	};
	int failed = 0;

	if (is_remote_io_transaction_init(&transaction.transaction,
		    IS_IO_POLICY_REMOTE_FIRST_WRITE, 52) != 0)
		return 1;
	is_remote_io_transaction_dispatcher_released(&transaction.transaction);
	is_remote_io_transaction_remote_completed(&transaction.transaction, 52, 0,
		false);
	failed |= expect_effects(&recorder, before_local,
		sizeof(before_local) / sizeof(before_local[0]),
		"remote-first before local");
	is_remote_io_transaction_local_completed(&transaction.transaction, -EIO);
	failed |= expect_effects(&recorder, after_local,
		sizeof(after_local) / sizeof(after_local[0]),
		"remote-first after local");
	is_remote_io_transaction_transport_released(&transaction.transaction, 52);
	failed |= expect_effects(&recorder, after_transport,
		sizeof(after_transport) / sizeof(after_transport[0]),
		"remote-first after transport");
	return failed;
}

static int test_remote_read_fallback_claim_precedes_adapter_reentry(void)
{
	const enum recorded_effect before_transport[] = {
		EFFECT_SUBMIT_LOCAL,
		EFFECT_COMPLETE_SUCCESS,
	};
	const enum recorded_effect after_transport[] = {
		EFFECT_SUBMIT_LOCAL,
		EFFECT_COMPLETE_SUCCESS,
		EFFECT_SETTLE,
	};
	struct effect_recorder recorder = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
		.complete_local_during_submit = true,
		.submitted_local_status = 0,
	};
	struct test_transaction transaction = {
		.recorder = &recorder,
	};
	int failed = 0;

	if (is_remote_io_transaction_init(&transaction.transaction,
		    IS_IO_POLICY_REMOTE_READ, 73) != 0)
		return 1;
	is_remote_io_transaction_dispatcher_released(&transaction.transaction);
	is_remote_io_transaction_remote_completed(&transaction.transaction, 73,
		-EIO, false);
	failed |= expect_effects(&recorder, before_transport,
		sizeof(before_transport) / sizeof(before_transport[0]),
		"fallback before transport");
	is_remote_io_transaction_transport_released(&transaction.transaction, 73);
	failed |= expect_effects(&recorder, after_transport,
		sizeof(after_transport) / sizeof(after_transport[0]),
		"fallback after transport");
	return failed | (recorder.local_submissions != 1) |
		(recorder.completions != 1) | (recorder.settlements != 1) |
		(recorder.invariants != 0);
}

static int test_remote_read_outcomes(void)
{
	const int remote_statuses[] = {0, -EIO, -ETIMEDOUT};
	const int local_statuses[] = {0, -EIO};
	unsigned int remote_index;
	int failed = 0;

	for (remote_index = 0;
	     remote_index < sizeof(remote_statuses) / sizeof(remote_statuses[0]);
	     remote_index++) {
		unsigned int local_index;
		unsigned int local_count = remote_statuses[remote_index] == 0 ? 1 :
			sizeof(local_statuses) / sizeof(local_statuses[0]);

		for (local_index = 0; local_index < local_count; local_index++) {
			struct effect_recorder recorder = {
				.lock = PTHREAD_MUTEX_INITIALIZER,
			};
			struct test_transaction transaction = {
				.recorder = &recorder,
			};
			int expect_success = remote_statuses[remote_index] == 0 ||
				local_statuses[local_index] == 0;

			if (is_remote_io_transaction_init(&transaction.transaction,
				    IS_IO_POLICY_REMOTE_READ, 81) != 0)
				return 1;
			is_remote_io_transaction_dispatcher_released(
				&transaction.transaction);
			is_remote_io_transaction_remote_completed(
				&transaction.transaction, 81,
				remote_statuses[remote_index],
				remote_statuses[remote_index] == -ETIMEDOUT);
			if (remote_statuses[remote_index] != 0)
				is_remote_io_transaction_local_completed(
					&transaction.transaction,
					local_statuses[local_index]);
			is_remote_io_transaction_transport_released(
				&transaction.transaction, 81);
			if (recorder.completions != 1 ||
			    recorder.successes != (unsigned int)expect_success ||
			    recorder.errors != (unsigned int)!expect_success ||
			    recorder.local_submissions !=
				    (unsigned int)(remote_statuses[remote_index] != 0) ||
			    recorder.degraded !=
				    (unsigned int)(remote_statuses[remote_index] != 0 &&
					local_statuses[local_index] != 0) ||
			    recorder.settlements != 1 || recorder.invariants != 0) {
				fprintf(stderr,
					"remote read failed: remote=%d local=%d\n",
					remote_statuses[remote_index],
					local_statuses[local_index]);
				failed = 1;
			}
		}
	}
	return failed;
}

static int test_remote_only_outcome_matrix(void)
{
	const enum is_io_policy_kind kinds[] = {
		IS_IO_POLICY_REMOTE_ONLY_READ,
		IS_IO_POLICY_REMOTE_ONLY_WRITE,
	};
	unsigned int kind_index;
	int failed = 0;

	for (kind_index = 0;
	     kind_index < sizeof(kinds) / sizeof(kinds[0]); kind_index++) {
		int status;

		for (status = 0; status >= -EIO; status -= EIO) {
			struct effect_recorder recorder = {
				.lock = PTHREAD_MUTEX_INITIALIZER,
			};
			struct test_transaction transaction = {
				.recorder = &recorder,
			};

			if (is_remote_io_transaction_init(&transaction.transaction,
				    kinds[kind_index], 91) != 0)
				return 1;
			is_remote_io_transaction_dispatcher_released(
				&transaction.transaction);
			is_remote_io_transaction_remote_completed(
				&transaction.transaction, 91, status, status != 0);
			is_remote_io_transaction_transport_released(
				&transaction.transaction, 91);
			if (recorder.completions != 1 ||
			    recorder.successes != (unsigned int)(status == 0) ||
			    recorder.errors != (unsigned int)(status != 0) ||
			    recorder.local_submissions != 0 ||
			    recorder.local_only != 0 || recorder.degraded != 0 ||
			    recorder.settlements != 1 || recorder.invariants != 0) {
				fprintf(stderr,
					"Remote-Only failed: kind=%d status=%d\n",
					kinds[kind_index], status);
				failed = 1;
			}
		}
	}
	return failed;
}

static int test_remote_only_failure_never_acquires_local_claim(void)
{
	const enum recorded_effect expected[] = {
		EFFECT_COMPLETE_ERROR,
		EFFECT_SETTLE,
	};
	struct effect_recorder recorder = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
	};
	struct test_transaction transaction = {
		.recorder = &recorder,
	};

	if (is_remote_io_transaction_init(&transaction.transaction,
		    IS_IO_POLICY_REMOTE_ONLY_READ, 91) != 0)
		return 1;
	is_remote_io_transaction_dispatcher_released(&transaction.transaction);
	is_remote_io_transaction_remote_completed(&transaction.transaction, 91,
		-ETIMEDOUT, true);
	is_remote_io_transaction_transport_released(&transaction.transaction, 91);
	return expect_effects(&recorder, expected,
		sizeof(expected) / sizeof(expected[0]), "Remote-Only failure") |
		(recorder.completion_status != -ETIMEDOUT) |
		(recorder.local_submissions != 0) | (recorder.local_only != 0);
}

static int test_stale_generation_cannot_complete_or_release(void)
{
	const enum recorded_effect completed[] = {
		EFFECT_COMPLETE_SUCCESS,
	};
	const enum recorded_effect settled[] = {
		EFFECT_COMPLETE_SUCCESS,
		EFFECT_SETTLE,
	};
	struct effect_recorder recorder = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
	};
	struct test_transaction transaction = {
		.recorder = &recorder,
	};
	int failed = 0;

	if (is_remote_io_transaction_init(&transaction.transaction,
		    IS_IO_POLICY_REMOTE_ONLY_WRITE, 101) != 0)
		return 1;
	is_remote_io_transaction_dispatcher_released(&transaction.transaction);
	is_remote_io_transaction_remote_completed(&transaction.transaction, 100,
		0, false);
	is_remote_io_transaction_transport_released(&transaction.transaction, 100);
	if (recorder.effect_count != 0) {
		fprintf(stderr, "stale generation produced an effect\n");
		failed = 1;
	}
	is_remote_io_transaction_remote_completed(&transaction.transaction, 101,
		0, false);
	failed |= expect_effects(&recorder, completed,
		sizeof(completed) / sizeof(completed[0]),
		"current generation completed");
	is_remote_io_transaction_transport_released(&transaction.transaction, 101);
	failed |= expect_effects(&recorder, settled,
		sizeof(settled) / sizeof(settled[0]),
		"current generation settled");
	return failed;
}

static int test_duplicate_remote_result_completes_once(void)
{
	struct effect_recorder recorder = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
	};
	struct test_transaction transaction = {
		.recorder = &recorder,
	};

	if (is_remote_io_transaction_init(&transaction.transaction,
		    IS_IO_POLICY_REMOTE_ONLY_WRITE, 121) != 0)
		return 1;
	is_remote_io_transaction_dispatcher_released(&transaction.transaction);
	is_remote_io_transaction_remote_completed(&transaction.transaction, 121,
		0, false);
	is_remote_io_transaction_remote_completed(&transaction.transaction, 121,
		-EIO, false);
	is_remote_io_transaction_transport_released(&transaction.transaction, 121);
	return (recorder.completions != 1) | (recorder.successes != 1) |
		(recorder.errors != 0) | (recorder.settlements != 1) |
		(recorder.invariants != 1);
}

static int test_synchronous_branch_failures_settle_once(void)
{
	struct effect_recorder recorder = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
	};
	struct test_transaction transaction = {
		.recorder = &recorder,
	};

	if (is_remote_io_transaction_init(&transaction.transaction,
		    IS_IO_POLICY_STRICT_WRITE, 131) != 0)
		return 1;
	is_remote_io_transaction_local_completed(&transaction.transaction,
		-ENOMEM);
	is_remote_io_transaction_transport_released(&transaction.transaction, 131);
	is_remote_io_transaction_remote_completed(&transaction.transaction, 131,
		-ENOTCONN, false);
	if (recorder.settlements != 0) {
		fprintf(stderr, "sync failures settled before dispatcher release\n");
		return 1;
	}
	is_remote_io_transaction_dispatcher_released(&transaction.transaction);
	return (recorder.completions != 1) | (recorder.errors != 1) |
		(recorder.degraded != 1) | (recorder.settlements != 1) |
		(recorder.invariants != 0);
}

static int test_concurrent_local_and_remote_release_settles_once(void)
{
	struct test_barrier barrier;
	struct effect_recorder recorder = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
	};
	struct test_transaction transaction = {
		.recorder = &recorder,
	};
	struct threaded_event local = {
		.transaction = &transaction.transaction,
		.start = &barrier,
		.kind = THREADED_EVENT_LOCAL,
		.generation = 151,
		.status = 0,
	};
	struct threaded_event remote = {
		.transaction = &transaction.transaction,
		.start = &barrier,
		.kind = THREADED_EVENT_REMOTE,
		.generation = 151,
		.status = 0,
	};
	pthread_t local_thread;
	pthread_t remote_thread;
	int failed;

	if (test_barrier_init(&barrier, 2) != 0 ||
	    is_remote_io_transaction_init(&transaction.transaction,
		IS_IO_POLICY_STRICT_WRITE, 151) != 0)
		return 1;
	is_remote_io_transaction_dispatcher_released(&transaction.transaction);
	is_remote_io_transaction_transport_released(&transaction.transaction, 151);
	if (pthread_create(&local_thread, NULL, run_transaction_event, &local) != 0 ||
	    pthread_create(&remote_thread, NULL, run_transaction_event, &remote) !=
		0) {
		fprintf(stderr, "concurrent result threads failed\n");
		return 1;
	}
	(void)pthread_join(local_thread, NULL);
	(void)pthread_join(remote_thread, NULL);
	failed = (recorder.completions != 1) | (recorder.successes != 1) |
		(recorder.settlements != 1) | (recorder.invariants != 0);
	test_barrier_destroy(&barrier);
	return failed;
}

static int test_concurrent_late_branches_settle_after_early_ack(void)
{
	struct test_barrier barrier;
	struct effect_recorder recorder = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
	};
	struct test_transaction transaction = {
		.recorder = &recorder,
	};
	struct threaded_event local = {
		.transaction = &transaction.transaction,
		.start = &barrier,
		.kind = THREADED_EVENT_LOCAL,
		.generation = 181,
		.status = 0,
	};
	struct threaded_event transport = {
		.transaction = &transaction.transaction,
		.start = &barrier,
		.kind = THREADED_EVENT_TRANSPORT,
		.generation = 181,
		.status = 0,
	};
	pthread_t local_thread;
	pthread_t transport_thread;
	int failed;

	if (test_barrier_init(&barrier, 2) != 0 ||
	    is_remote_io_transaction_init(&transaction.transaction,
		IS_IO_POLICY_REMOTE_FIRST_WRITE, 181) != 0)
		return 1;
	is_remote_io_transaction_dispatcher_released(&transaction.transaction);
	is_remote_io_transaction_remote_completed(&transaction.transaction, 181, 0,
		false);
	if (recorder.completions != 1 || recorder.settlements != 0)
		return 1;
	if (pthread_create(&local_thread, NULL, run_transaction_event, &local) != 0 ||
	    pthread_create(&transport_thread, NULL, run_transaction_event,
		&transport) != 0) {
		fprintf(stderr, "concurrent late branch threads failed\n");
		return 1;
	}
	(void)pthread_join(local_thread, NULL);
	(void)pthread_join(transport_thread, NULL);
	failed = (recorder.completions != 1) | (recorder.successes != 1) |
		(recorder.settlements != 1) | (recorder.invariants != 0);
	test_barrier_destroy(&barrier);
	return failed;
}

static int test_all_claims_can_release_concurrently(void)
{
	struct test_barrier barrier;
	struct effect_recorder recorder = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
	};
	struct test_transaction transaction = {
		.recorder = &recorder,
	};
	struct threaded_event events[] = {
		{
			.transaction = &transaction.transaction,
			.start = &barrier,
			.kind = THREADED_EVENT_DISPATCHER,
			.generation = 211,
			.status = 0,
		},
		{
			.transaction = &transaction.transaction,
			.start = &barrier,
			.kind = THREADED_EVENT_LOCAL,
			.generation = 211,
			.status = 0,
		},
		{
			.transaction = &transaction.transaction,
			.start = &barrier,
			.kind = THREADED_EVENT_REMOTE,
			.generation = 211,
			.status = 0,
		},
		{
			.transaction = &transaction.transaction,
			.start = &barrier,
			.kind = THREADED_EVENT_TRANSPORT,
			.generation = 211,
			.status = 0,
		},
	};
	pthread_t threads[sizeof(events) / sizeof(events[0])];
	unsigned int index;
	int failed;

	if (test_barrier_init(&barrier,
		    sizeof(events) / sizeof(events[0])) != 0 ||
	    is_remote_io_transaction_init(&transaction.transaction,
		IS_IO_POLICY_STRICT_WRITE, 211) != 0)
		return 1;
	for (index = 0; index < sizeof(events) / sizeof(events[0]); index++) {
		if (pthread_create(&threads[index], NULL, run_transaction_event,
			   &events[index]) != 0) {
			fprintf(stderr, "all-claim thread creation failed\n");
			return 1;
		}
	}
	for (index = 0; index < sizeof(events) / sizeof(events[0]); index++)
		(void)pthread_join(threads[index], NULL);
	failed = (recorder.completions != 1) | (recorder.successes != 1) |
		(recorder.settlements != 1) | (recorder.invariants != 0);
	test_barrier_destroy(&barrier);
	return failed;
}

int main(void)
{
	return test_write_outcome_and_completion_order_matrix() |
		test_remote_first_acknowledges_before_late_settlement() |
		test_remote_read_fallback_claim_precedes_adapter_reentry() |
		test_remote_read_outcomes() |
		test_remote_only_outcome_matrix() |
		test_remote_only_failure_never_acquires_local_claim() |
		test_stale_generation_cannot_complete_or_release() |
		test_duplicate_remote_result_completes_once() |
		test_synchronous_branch_failures_settle_once() |
		test_concurrent_local_and_remote_release_settles_once() |
		test_concurrent_late_branches_settle_after_early_ack() |
		test_all_claims_can_release_concurrently();
}
