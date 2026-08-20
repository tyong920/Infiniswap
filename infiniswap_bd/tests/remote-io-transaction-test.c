// SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause
#include "../is_remote_io_transaction.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum recorded_effect {
	EFFECT_ALLOCATE = 1,
	EFFECT_PREPARE,
	EFFECT_SUBMIT_BACKING,
	EFFECT_SUBMIT_RDMA,
	EFFECT_COMPLETE_SUCCESS,
	EFFECT_COMPLETE_ERROR,
	EFFECT_BACKING_DEGRADED,
	EFFECT_MARK_LOCAL_ONLY,
	EFFECT_RELEASE,
	EFFECT_SETTLE,
	EFFECT_INVARIANT,
};

struct effect_recorder {
	pthread_mutex_t lock;
	struct is_remote_io_transaction *transaction;
	enum recorded_effect effects[128];
	unsigned int effect_count;
	unsigned int allocations;
	unsigned int preparations;
	unsigned int backing_preparations;
	unsigned int backing_submissions;
	unsigned int rdma_submissions;
	unsigned int completions;
	unsigned int successes;
	unsigned int errors;
	unsigned int degraded;
	unsigned int local_only;
	unsigned int releases;
	unsigned int settlements;
	unsigned int invariants;
	int completion_status;
	int allocation_status;
	int prepare_status;
	int backing_submit_status;
	int rdma_submit_status;
	int backing_completion_status;
	int rdma_completion_status;
	bool complete_backing_during_submit;
	bool complete_rdma_during_submit;
	bool release_rdma_during_submit;
	bool destroy_engine_during_release;
	bool destroy_engine_during_settle;
	int release_destroy_status;
	int settle_destroy_status;
	unsigned long long completed_sector;
	unsigned long long completed_generation;
};

struct test_engine {
	struct is_remote_io_transaction_engine engine;
	struct effect_recorder *recorder;
};

struct test_barrier {
	pthread_mutex_t lock;
	pthread_cond_t changed;
	unsigned int target;
	unsigned int arrived;
	unsigned int generation;
};

enum threaded_event_kind {
	THREADED_EVENT_BACKING = 1,
	THREADED_EVENT_RDMA,
	THREADED_EVENT_RELEASE,
};

struct threaded_event {
	struct is_remote_io_transaction *transaction;
	struct test_barrier *start;
	enum threaded_event_kind kind;
	unsigned long long generation;
	int status;
};

static struct effect_recorder *recorder_from_engine(
	struct is_remote_io_transaction_engine *engine)
{
	return ((struct test_engine *)engine)->recorder;
}

static struct effect_recorder *recorder_from_spec(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec)
{
	return spec ? spec->payload : recorder_from_engine(engine);
}

static int recorder_init(struct effect_recorder *recorder)
{
	memset(recorder, 0, sizeof(*recorder));
	recorder->release_destroy_status = -1;
	recorder->settle_destroy_status = -1;
	return pthread_mutex_init(&recorder->lock, NULL);
}

static void recorder_destroy(struct effect_recorder *recorder)
{
	(void)pthread_mutex_destroy(&recorder->lock);
}

static int test_engine_init(struct test_engine *engine,
			    struct effect_recorder *recorder,
			    enum is_remote_io_transaction_mode mode)
{
	memset(engine, 0, sizeof(*engine));
	engine->recorder = recorder;
	return is_remote_io_transaction_engine_init(&engine->engine, mode);
}

static struct is_remote_io_transaction_spec transaction_spec(
	struct effect_recorder *recorder,
	enum is_remote_io_transaction_direction direction,
	unsigned long long generation)
{
	const struct is_remote_io_transaction_spec spec = {
		.direction = direction,
		.sector = 8,
		.bytes = 4096,
		.generation = generation,
		.payload = recorder,
	};

	return spec;
}

int is_remote_io_transaction_adapter_allocate(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec,
	size_t transaction_size, size_t transaction_alignment,
	struct is_remote_io_transaction_allocation *allocation)
{
	struct effect_recorder *recorder = recorder_from_spec(engine, spec);
	int status;

	(void)transaction_alignment;
	pthread_mutex_lock(&recorder->lock);
	recorder->effects[recorder->effect_count++] = EFFECT_ALLOCATE;
	recorder->allocations++;
	status = recorder->allocation_status;
	pthread_mutex_unlock(&recorder->lock);
	if (status)
		return status;

	allocation->transaction_storage = calloc(1, transaction_size);
	if (!allocation->transaction_storage)
		return -ENOMEM;
	allocation->adapter_context = recorder;
	pthread_mutex_lock(&recorder->lock);
	recorder->transaction = allocation->transaction_storage;
	pthread_mutex_unlock(&recorder->lock);
	return 0;
}

int is_remote_io_transaction_adapter_prepare(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec, bool prepare_backing,
	struct is_remote_io_transaction *transaction, void *adapter_context)
{
	struct effect_recorder *recorder = adapter_context;
	int status;

	(void)engine;
	(void)spec;
	(void)transaction;
	pthread_mutex_lock(&recorder->lock);
	recorder->effects[recorder->effect_count++] = EFFECT_PREPARE;
	recorder->preparations++;
	if (prepare_backing)
		recorder->backing_preparations++;
	status = recorder->prepare_status;
	pthread_mutex_unlock(&recorder->lock);
	return status;
}

int is_remote_io_transaction_adapter_submit_backing(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec,
	struct is_remote_io_transaction *transaction, void *adapter_context)
{
	struct effect_recorder *recorder = adapter_context;
	bool complete;
	int completion_status;
	int submit_status;

	(void)engine;
	(void)spec;
	pthread_mutex_lock(&recorder->lock);
	recorder->effects[recorder->effect_count++] = EFFECT_SUBMIT_BACKING;
	recorder->backing_submissions++;
	complete = recorder->complete_backing_during_submit;
	completion_status = recorder->backing_completion_status;
	submit_status = recorder->backing_submit_status;
	pthread_mutex_unlock(&recorder->lock);
	if (complete)
		is_remote_io_transaction_backing_completed(transaction,
			completion_status);
	return submit_status;
}

int is_remote_io_transaction_adapter_submit_rdma(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec,
	struct is_remote_io_transaction *transaction, void *adapter_context)
{
	struct effect_recorder *recorder = adapter_context;
	bool complete;
	bool release;
	int completion_status;
	int submit_status;

	(void)engine;
	pthread_mutex_lock(&recorder->lock);
	recorder->effects[recorder->effect_count++] = EFFECT_SUBMIT_RDMA;
	recorder->rdma_submissions++;
	complete = recorder->complete_rdma_during_submit;
	release = recorder->release_rdma_during_submit;
	completion_status = recorder->rdma_completion_status;
	submit_status = recorder->rdma_submit_status;
	pthread_mutex_unlock(&recorder->lock);
	if (complete)
		is_remote_io_transaction_rdma_completed(transaction,
			spec->generation, completion_status, false);
	if (release)
		is_remote_io_transaction_rdma_released(transaction,
			spec->generation);
	return submit_status;
}

void is_remote_io_transaction_adapter_complete(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec, void *adapter_context,
	int status)
{
	struct effect_recorder *recorder = adapter_context ? adapter_context :
		recorder_from_spec(engine, spec);

	pthread_mutex_lock(&recorder->lock);
	recorder->completion_status = status;
	recorder->completed_sector = spec->sector;
	recorder->completed_generation = spec->generation;
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
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec, void *adapter_context)
{
	struct effect_recorder *recorder = adapter_context;

	(void)engine;
	(void)spec;
	pthread_mutex_lock(&recorder->lock);
	recorder->degraded++;
	recorder->effects[recorder->effect_count++] = EFFECT_BACKING_DEGRADED;
	pthread_mutex_unlock(&recorder->lock);
}

void is_remote_io_transaction_adapter_mark_local_only(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec, void *adapter_context)
{
	struct effect_recorder *recorder = adapter_context;

	(void)engine;
	(void)spec;
	pthread_mutex_lock(&recorder->lock);
	recorder->local_only++;
	recorder->effects[recorder->effect_count++] = EFFECT_MARK_LOCAL_ONLY;
	pthread_mutex_unlock(&recorder->lock);
}

void is_remote_io_transaction_adapter_release(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec, void *adapter_context)
{
	struct effect_recorder *recorder = adapter_context;
	struct is_remote_io_transaction *transaction;
	bool destroy_engine;

	(void)spec;
	pthread_mutex_lock(&recorder->lock);
	recorder->effects[recorder->effect_count++] = EFFECT_RELEASE;
	recorder->releases++;
	transaction = recorder->transaction;
	recorder->transaction = NULL;
	destroy_engine = recorder->destroy_engine_during_release;
	pthread_mutex_unlock(&recorder->lock);
	if (destroy_engine) {
		int status = is_remote_io_transaction_engine_destroy(engine);

		pthread_mutex_lock(&recorder->lock);
		recorder->release_destroy_status = status;
		pthread_mutex_unlock(&recorder->lock);
	}
	free(transaction);
}

void is_remote_io_transaction_adapter_settle(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec)
{
	struct effect_recorder *recorder = recorder_from_spec(engine, spec);
	bool destroy_engine;

	pthread_mutex_lock(&recorder->lock);
	recorder->effects[recorder->effect_count++] = EFFECT_SETTLE;
	recorder->settlements++;
	destroy_engine = recorder->destroy_engine_during_settle;
	pthread_mutex_unlock(&recorder->lock);
	if (destroy_engine) {
		int status = is_remote_io_transaction_engine_destroy(engine);

		pthread_mutex_lock(&recorder->lock);
		recorder->settle_destroy_status = status;
		pthread_mutex_unlock(&recorder->lock);
	}
}

void is_remote_io_transaction_adapter_invariant(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec, void *adapter_context,
	enum is_remote_io_transaction_event event)
{
	struct effect_recorder *recorder = adapter_context ? adapter_context :
		recorder_from_spec(engine, spec);

	(void)event;
	pthread_mutex_lock(&recorder->lock);
	recorder->invariants++;
	recorder->effects[recorder->effect_count++] = EFFECT_INVARIANT;
	pthread_mutex_unlock(&recorder->lock);
}

static int expect_effects(const struct effect_recorder *recorder,
			  const enum recorded_effect *expected,
			  unsigned int expected_count, const char *name)
{
	unsigned int index;

	if (recorder->effect_count == expected_count) {
		for (index = 0; index < expected_count; index++) {
			if (recorder->effects[index] != expected[index])
				break;
		}
		if (index == expected_count)
			return 0;
	}
	fprintf(stderr, "%s: effects", name);
	for (index = 0; index < recorder->effect_count; index++)
		fprintf(stderr, " %d", recorder->effects[index]);
	fprintf(stderr, "\n");
	return 1;
}

static int test_barrier_init(struct test_barrier *barrier, unsigned int target)
{
	int status;

	memset(barrier, 0, sizeof(*barrier));
	barrier->target = target;
	status = pthread_mutex_init(&barrier->lock, NULL);
	if (status)
		return status;
	status = pthread_cond_init(&barrier->changed, NULL);
	if (status)
		(void)pthread_mutex_destroy(&barrier->lock);
	return status;
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

static void *run_transaction_event(void *context)
{
	struct threaded_event *event = context;

	test_barrier_wait(event->start);
	switch (event->kind) {
	case THREADED_EVENT_BACKING:
		is_remote_io_transaction_backing_completed(event->transaction,
			event->status);
		break;
	case THREADED_EVENT_RDMA:
		is_remote_io_transaction_rdma_completed(event->transaction,
			event->generation, event->status, false);
		break;
	case THREADED_EVENT_RELEASE:
		is_remote_io_transaction_rdma_released(event->transaction,
			event->generation);
		break;
	}
	return NULL;
}

static int test_backed_write_outcome_and_order_matrix(void)
{
	const enum is_remote_io_transaction_mode modes[] = {
		IS_REMOTE_IO_TRANSACTION_BACKED_STRICT,
		IS_REMOTE_IO_TRANSACTION_BACKED_REMOTE_FIRST,
	};
	unsigned int mode_index;
	int failed = 0;

	for (mode_index = 0; mode_index < sizeof(modes) / sizeof(modes[0]);
	     mode_index++) {
		int backing_status;

		for (backing_status = 0; backing_status >= -EIO;
		     backing_status -= EIO) {
			int rdma_status;

			for (rdma_status = 0; rdma_status >= -EIO;
			     rdma_status -= EIO) {
				unsigned int backing_first;

				for (backing_first = 0; backing_first <= 1;
				     backing_first++) {
					struct effect_recorder recorder;
					struct test_engine engine;
					struct is_remote_io_transaction_spec spec;
					struct is_remote_io_transaction *transaction;
					int expect_success;

					if (recorder_init(&recorder) ||
					    test_engine_init(&engine, &recorder,
						modes[mode_index]))
						return 1;
					spec = transaction_spec(&recorder,
						IS_REMOTE_IO_TRANSACTION_WRITE, 41);
					is_remote_io_transaction_start(&engine.engine, &spec);
					transaction = recorder.transaction;
					if (backing_first) {
						is_remote_io_transaction_backing_completed(
							transaction, backing_status);
						if (modes[mode_index] ==
							    IS_REMOTE_IO_TRANSACTION_BACKED_REMOTE_FIRST &&
						    backing_status)
							is_remote_io_transaction_backing_completed(
								transaction, backing_status);
						is_remote_io_transaction_rdma_completed(
							transaction, 41, rdma_status, false);
					} else {
						is_remote_io_transaction_rdma_completed(
							transaction, 41, rdma_status, false);
						is_remote_io_transaction_backing_completed(
							transaction, backing_status);
						if (modes[mode_index] ==
							    IS_REMOTE_IO_TRANSACTION_BACKED_REMOTE_FIRST &&
						    backing_status)
							is_remote_io_transaction_backing_completed(
								transaction, backing_status);
					}
					if (recorder.settlements)
						failed = 1;
					is_remote_io_transaction_rdma_released(transaction,
						41);
					expect_success = modes[mode_index] ==
						IS_REMOTE_IO_TRANSACTION_BACKED_STRICT ?
						backing_status == 0 :
						(backing_status == 0 || rdma_status == 0);
					if (recorder.completions != 1 ||
					    recorder.successes !=
						    (unsigned int)expect_success ||
					    recorder.errors !=
						    (unsigned int)!expect_success ||
					    recorder.degraded !=
						    (unsigned int)(backing_status != 0) ||
					    recorder.local_only !=
						    (unsigned int)(backing_status == 0 &&
							   rdma_status != 0) ||
					    recorder.backing_submissions !=
						    (unsigned int)(1 +
							(modes[mode_index] ==
							 IS_REMOTE_IO_TRANSACTION_BACKED_REMOTE_FIRST &&
							 backing_status != 0)) ||
					    recorder.rdma_submissions != 1 ||
					    recorder.releases != 1 ||
					    recorder.settlements != 1 ||
					    recorder.invariants != 0) {
						fprintf(stderr,
							"write matrix: mode=%d backing=%d rdma=%d backing_first=%u\n",
							modes[mode_index], backing_status,
							rdma_status, backing_first);
						failed = 1;
					}
					failed |= is_remote_io_transaction_engine_destroy(
						&engine.engine) != 0;
					recorder_destroy(&recorder);
				}
			}
		}
	}
	return failed;
}

static int test_backed_write_submits_recovery_before_rdma(void)
{
	const enum recorded_effect started[] = {
		EFFECT_ALLOCATE,
		EFFECT_PREPARE,
		EFFECT_SUBMIT_BACKING,
		EFFECT_SUBMIT_RDMA,
	};
	struct effect_recorder recorder;
	struct test_engine engine;
	struct is_remote_io_transaction_spec spec;
	struct is_remote_io_transaction *transaction;
	int failed;

	if (recorder_init(&recorder) ||
	    test_engine_init(&engine, &recorder,
		IS_REMOTE_IO_TRANSACTION_BACKED_STRICT))
		return 1;
	spec = transaction_spec(&recorder, IS_REMOTE_IO_TRANSACTION_WRITE, 51);
	is_remote_io_transaction_start(&engine.engine, &spec);
	transaction = recorder.transaction;
	failed = expect_effects(&recorder, started,
		sizeof(started) / sizeof(started[0]), "backed write start");
	failed |= recorder.backing_preparations != 1;
	is_remote_io_transaction_backing_completed(transaction, 0);
	is_remote_io_transaction_rdma_completed(transaction, 51, 0, false);
	is_remote_io_transaction_rdma_released(transaction, 51);
	failed |= is_remote_io_transaction_engine_destroy(&engine.engine) != 0;
	recorder_destroy(&recorder);
	return failed;
}

static int test_remote_first_acknowledges_before_late_settlement(void)
{
	struct effect_recorder recorder;
	struct test_engine engine;
	struct is_remote_io_transaction_spec spec;
	struct is_remote_io_transaction *transaction;
	int failed = 0;

	if (recorder_init(&recorder) ||
	    test_engine_init(&engine, &recorder,
		IS_REMOTE_IO_TRANSACTION_BACKED_REMOTE_FIRST))
		return 1;
	spec = transaction_spec(&recorder, IS_REMOTE_IO_TRANSACTION_WRITE, 61);
	is_remote_io_transaction_start(&engine.engine, &spec);
	transaction = recorder.transaction;
	is_remote_io_transaction_rdma_completed(transaction, 61, 0, false);
	if (recorder.completions != 1 || recorder.successes != 1 ||
	    recorder.settlements != 0)
		failed = 1;
	is_remote_io_transaction_backing_completed(transaction, -EIO);
	if (recorder.completions != 1 || recorder.degraded != 0 ||
	    recorder.backing_submissions != 2 || recorder.settlements != 0)
		failed = 1;
	is_remote_io_transaction_backing_completed(transaction, -EIO);
	if (recorder.completions != 1 || recorder.degraded != 1 ||
	    recorder.settlements != 0)
		failed = 1;
	is_remote_io_transaction_rdma_released(transaction, 61);
	failed |= recorder.settlements != 1;
	failed |= is_remote_io_transaction_engine_destroy(&engine.engine) != 0;
	recorder_destroy(&recorder);
	return failed;
}

static int test_remote_first_retries_backing_before_degrading(void)
{
	struct effect_recorder recorder;
	struct test_engine engine;
	struct is_remote_io_transaction_spec spec;
	struct is_remote_io_transaction *transaction;
	int failed;

	if (recorder_init(&recorder) ||
	    test_engine_init(&engine, &recorder,
		IS_REMOTE_IO_TRANSACTION_BACKED_REMOTE_FIRST))
		return 1;
	spec = transaction_spec(&recorder, IS_REMOTE_IO_TRANSACTION_WRITE, 66);
	is_remote_io_transaction_start(&engine.engine, &spec);
	transaction = recorder.transaction;
	is_remote_io_transaction_backing_completed(transaction, -EIO);
	failed = recorder.backing_submissions != 2 || recorder.degraded != 0 ||
		recorder.completions != 0;
	is_remote_io_transaction_backing_completed(transaction, 0);
	is_remote_io_transaction_rdma_completed(transaction, 66, 0, false);
	is_remote_io_transaction_rdma_released(transaction, 66);
	failed |= recorder.completions != 1 || recorder.successes != 1 ||
		recorder.degraded != 0 || recorder.invariants != 0 ||
		recorder.settlements != 1;
	failed |= is_remote_io_transaction_engine_destroy(&engine.engine) != 0;
	recorder_destroy(&recorder);
	return failed;
}

static int test_remote_read_outcome_matrix(void)
{
	const int rdma_statuses[] = {0, -EIO, -ETIMEDOUT};
	const int backing_statuses[] = {0, -EIO};
	unsigned int rdma_index;
	int failed = 0;

	for (rdma_index = 0;
	     rdma_index < sizeof(rdma_statuses) / sizeof(rdma_statuses[0]);
	     rdma_index++) {
		unsigned int backing_index;
		unsigned int backing_count = rdma_statuses[rdma_index] == 0 ? 1 :
			sizeof(backing_statuses) / sizeof(backing_statuses[0]);

		for (backing_index = 0; backing_index < backing_count;
		     backing_index++) {
			struct effect_recorder recorder;
			struct test_engine engine;
			struct is_remote_io_transaction_spec spec;
			struct is_remote_io_transaction *transaction;
			int expect_success = rdma_statuses[rdma_index] == 0 ||
				backing_statuses[backing_index] == 0;

			if (recorder_init(&recorder) ||
			    test_engine_init(&engine, &recorder,
				IS_REMOTE_IO_TRANSACTION_BACKED_STRICT))
				return 1;
			spec = transaction_spec(&recorder,
				IS_REMOTE_IO_TRANSACTION_READ, 71);
			is_remote_io_transaction_start(&engine.engine, &spec);
			transaction = recorder.transaction;
			is_remote_io_transaction_rdma_completed(transaction, 71,
				rdma_statuses[rdma_index],
				rdma_statuses[rdma_index] == -ETIMEDOUT);
			if (rdma_statuses[rdma_index] != 0)
				is_remote_io_transaction_backing_completed(transaction,
					backing_statuses[backing_index]);
			is_remote_io_transaction_rdma_released(transaction, 71);
			if (recorder.completions != 1 ||
			    recorder.successes != (unsigned int)expect_success ||
			    recorder.errors != (unsigned int)!expect_success ||
			    recorder.backing_submissions !=
				    (unsigned int)(rdma_statuses[rdma_index] != 0) ||
			    recorder.degraded !=
				    (unsigned int)(rdma_statuses[rdma_index] != 0 &&
					backing_statuses[backing_index] != 0) ||
			    recorder.settlements != 1 || recorder.invariants != 0) {
				fprintf(stderr, "read matrix: rdma=%d backing=%d\n",
					rdma_statuses[rdma_index],
					backing_statuses[backing_index]);
				failed = 1;
			}
			failed |= is_remote_io_transaction_engine_destroy(
				&engine.engine) != 0;
			recorder_destroy(&recorder);
		}
	}
	return failed;
}

static int test_remote_read_fallback_allows_adapter_reentry(void)
{
	struct effect_recorder recorder;
	struct test_engine engine;
	struct is_remote_io_transaction_spec spec;
	struct is_remote_io_transaction *transaction;
	int failed;

	if (recorder_init(&recorder) ||
	    test_engine_init(&engine, &recorder,
		IS_REMOTE_IO_TRANSACTION_BACKED_STRICT))
		return 1;
	recorder.complete_backing_during_submit = true;
	spec = transaction_spec(&recorder, IS_REMOTE_IO_TRANSACTION_READ, 81);
	is_remote_io_transaction_start(&engine.engine, &spec);
	transaction = recorder.transaction;
	is_remote_io_transaction_rdma_completed(transaction, 81, -EIO, false);
	failed = recorder.backing_submissions != 1 || recorder.completions != 1 ||
		recorder.successes != 1 || recorder.settlements != 0 ||
		recorder.invariants != 0;
	is_remote_io_transaction_rdma_released(transaction, 81);
	failed |= recorder.settlements != 1;
	failed |= is_remote_io_transaction_engine_destroy(&engine.engine) != 0;
	recorder_destroy(&recorder);
	return failed;
}

static int test_remote_only_outcome_matrix(void)
{
	const enum is_remote_io_transaction_direction directions[] = {
		IS_REMOTE_IO_TRANSACTION_READ,
		IS_REMOTE_IO_TRANSACTION_WRITE,
	};
	unsigned int direction_index;
	int failed = 0;

	for (direction_index = 0;
	     direction_index < sizeof(directions) / sizeof(directions[0]);
	     direction_index++) {
		int status;

		for (status = 0; status >= -EIO; status -= EIO) {
			struct effect_recorder recorder;
			struct test_engine engine;
			struct is_remote_io_transaction_spec spec;
			struct is_remote_io_transaction *transaction;

			if (recorder_init(&recorder) ||
			    test_engine_init(&engine, &recorder,
				IS_REMOTE_IO_TRANSACTION_REMOTE_ONLY))
				return 1;
			spec = transaction_spec(&recorder,
				directions[direction_index], 91);
			is_remote_io_transaction_start(&engine.engine, &spec);
			transaction = recorder.transaction;
			is_remote_io_transaction_rdma_completed(transaction, 91,
				status, status != 0);
			is_remote_io_transaction_rdma_released(transaction, 91);
			if (recorder.completions != 1 ||
			    recorder.successes != (unsigned int)(status == 0) ||
			    recorder.errors != (unsigned int)(status != 0) ||
			    recorder.backing_submissions != 0 ||
			    recorder.backing_preparations != 0 ||
			    recorder.local_only != 0 || recorder.degraded != 0 ||
			    recorder.settlements != 1 || recorder.invariants != 0)
				failed = 1;
			failed |= is_remote_io_transaction_engine_destroy(
				&engine.engine) != 0;
			recorder_destroy(&recorder);
		}
	}
	return failed;
}

static int test_synchronous_prepare_failures_follow_mode_policy(void)
{
	const enum is_remote_io_transaction_mode backed_modes[] = {
		IS_REMOTE_IO_TRANSACTION_BACKED_STRICT,
		IS_REMOTE_IO_TRANSACTION_BACKED_REMOTE_FIRST,
	};
	unsigned int mode_index;
	int failed = 0;

	for (mode_index = 0;
	     mode_index < sizeof(backed_modes) / sizeof(backed_modes[0]);
	     mode_index++) {
		struct effect_recorder recorder;
		struct test_engine engine;
		struct is_remote_io_transaction_spec spec;
		struct is_remote_io_transaction *transaction;

		if (recorder_init(&recorder) ||
		    test_engine_init(&engine, &recorder, backed_modes[mode_index]))
			return 1;
		recorder.prepare_status = -ENOMEM;
		spec = transaction_spec(&recorder, IS_REMOTE_IO_TRANSACTION_WRITE,
			101 + mode_index);
		is_remote_io_transaction_start(&engine.engine, &spec);
		transaction = recorder.transaction;
		if (recorder.backing_submissions != 1 || recorder.rdma_submissions ||
		    recorder.completions || recorder.settlements)
			failed = 1;
		is_remote_io_transaction_backing_completed(transaction, 0);
		if (recorder.completions != 1 || recorder.successes != 1 ||
		    recorder.local_only != 1 || recorder.settlements != 1)
			failed = 1;
		failed |= is_remote_io_transaction_engine_destroy(&engine.engine) != 0;
		recorder_destroy(&recorder);
	}

	{
		struct effect_recorder recorder;
		struct test_engine engine;
		struct is_remote_io_transaction_spec spec;
		struct is_remote_io_transaction *transaction;

		if (recorder_init(&recorder) ||
		    test_engine_init(&engine, &recorder,
			IS_REMOTE_IO_TRANSACTION_BACKED_STRICT))
			return 1;
		recorder.prepare_status = -ENOMEM;
		spec = transaction_spec(&recorder, IS_REMOTE_IO_TRANSACTION_READ, 111);
		is_remote_io_transaction_start(&engine.engine, &spec);
		transaction = recorder.transaction;
		if (recorder.backing_submissions != 1 || recorder.rdma_submissions)
			failed = 1;
		is_remote_io_transaction_backing_completed(transaction, -EIO);
		if (recorder.completions != 1 || recorder.errors != 1 ||
		    recorder.degraded != 1 || recorder.settlements != 1)
			failed = 1;
		failed |= is_remote_io_transaction_engine_destroy(&engine.engine) != 0;
		recorder_destroy(&recorder);
	}

	{
		struct effect_recorder recorder;
		struct test_engine engine;
		struct is_remote_io_transaction_spec spec;

		if (recorder_init(&recorder) ||
		    test_engine_init(&engine, &recorder,
			IS_REMOTE_IO_TRANSACTION_REMOTE_ONLY))
			return 1;
		recorder.prepare_status = -ENOMEM;
		spec = transaction_spec(&recorder, IS_REMOTE_IO_TRANSACTION_READ, 121);
		is_remote_io_transaction_start(&engine.engine, &spec);
		if (recorder.rdma_submissions || recorder.backing_submissions ||
		    recorder.completions != 1 || recorder.completion_status != -ENOMEM ||
		    recorder.releases != 1 || recorder.settlements != 1)
			failed = 1;
		failed |= is_remote_io_transaction_engine_destroy(&engine.engine) != 0;
		recorder_destroy(&recorder);
	}
	return failed;
}

static int test_start_consumes_payload_when_allocation_fails(void)
{
	struct effect_recorder recorder;
	struct test_engine engine;
	struct is_remote_io_transaction_spec spec;
	int failed;

	if (recorder_init(&recorder) ||
	    test_engine_init(&engine, &recorder,
		IS_REMOTE_IO_TRANSACTION_BACKED_STRICT))
		return 1;
	recorder.allocation_status = -ENOMEM;
	spec = transaction_spec(&recorder, IS_REMOTE_IO_TRANSACTION_WRITE, 131);
	is_remote_io_transaction_start(&engine.engine, &spec);
	failed = recorder.allocations != 1 || recorder.preparations ||
		recorder.backing_submissions || recorder.rdma_submissions ||
		recorder.completions != 1 || recorder.completion_status != -ENOMEM ||
		recorder.releases || recorder.settlements != 1;
	failed |= is_remote_io_transaction_engine_destroy(&engine.engine) != 0;
	recorder_destroy(&recorder);
	return failed;
}

static int test_synchronous_submission_failures_settle_once(void)
{
	struct effect_recorder recorder;
	struct test_engine engine;
	struct is_remote_io_transaction_spec spec;
	struct is_remote_io_transaction *transaction;
	int failed;

	if (recorder_init(&recorder) ||
	    test_engine_init(&engine, &recorder,
		IS_REMOTE_IO_TRANSACTION_BACKED_STRICT))
		return 1;
	recorder.backing_submit_status = -ENOMEM;
	recorder.rdma_submit_status = -ENOTCONN;
	spec = transaction_spec(&recorder, IS_REMOTE_IO_TRANSACTION_WRITE, 141);
	is_remote_io_transaction_start(&engine.engine, &spec);
	failed = recorder.backing_submissions != 1 ||
		recorder.rdma_submissions != 1 || recorder.completions != 1 ||
		recorder.errors != 1 || recorder.degraded != 1 ||
		recorder.releases != 1 || recorder.settlements != 1 ||
		recorder.invariants != 0 || recorder.transaction != NULL;
	failed |= is_remote_io_transaction_engine_destroy(&engine.engine) != 0;
	recorder_destroy(&recorder);

	if (recorder_init(&recorder) ||
	    test_engine_init(&engine, &recorder,
		IS_REMOTE_IO_TRANSACTION_BACKED_STRICT))
		return 1;
	recorder.rdma_submit_status = -ENOTCONN;
	spec = transaction_spec(&recorder, IS_REMOTE_IO_TRANSACTION_READ, 142);
	is_remote_io_transaction_start(&engine.engine, &spec);
	transaction = recorder.transaction;
	if (recorder.backing_submissions != 1 || recorder.completions)
		failed = 1;
	is_remote_io_transaction_backing_completed(transaction, 0);
	if (recorder.completions != 1 || recorder.successes != 1 ||
	    recorder.settlements != 1)
		failed = 1;
	failed |= is_remote_io_transaction_engine_destroy(&engine.engine) != 0;
	recorder_destroy(&recorder);
	return failed;
}

static int test_stale_generation_cannot_complete_or_release(void)
{
	struct effect_recorder recorder;
	struct test_engine engine;
	struct is_remote_io_transaction_spec spec;
	struct is_remote_io_transaction *transaction;
	int failed = 0;

	if (recorder_init(&recorder) ||
	    test_engine_init(&engine, &recorder,
		IS_REMOTE_IO_TRANSACTION_REMOTE_ONLY))
		return 1;
	spec = transaction_spec(&recorder, IS_REMOTE_IO_TRANSACTION_WRITE, 151);
	is_remote_io_transaction_start(&engine.engine, &spec);
	transaction = recorder.transaction;
	is_remote_io_transaction_rdma_completed(transaction, 150, 0, false);
	is_remote_io_transaction_rdma_released(transaction, 150);
	if (recorder.completions || recorder.releases || recorder.settlements)
		failed = 1;
	is_remote_io_transaction_rdma_completed(transaction, 151, 0, false);
	if (recorder.completions != 1 || recorder.settlements)
		failed = 1;
	is_remote_io_transaction_rdma_released(transaction, 151);
	failed |= recorder.settlements != 1 || recorder.invariants != 0;
	failed |= is_remote_io_transaction_engine_destroy(&engine.engine) != 0;
	recorder_destroy(&recorder);
	return failed;
}

static int test_duplicate_result_completes_and_reports_once(void)
{
	struct effect_recorder recorder;
	struct test_engine engine;
	struct is_remote_io_transaction_spec spec;
	struct is_remote_io_transaction *transaction;
	int failed;

	if (recorder_init(&recorder) ||
	    test_engine_init(&engine, &recorder,
		IS_REMOTE_IO_TRANSACTION_REMOTE_ONLY))
		return 1;
	spec = transaction_spec(&recorder, IS_REMOTE_IO_TRANSACTION_WRITE, 161);
	is_remote_io_transaction_start(&engine.engine, &spec);
	transaction = recorder.transaction;
	is_remote_io_transaction_rdma_completed(transaction, 161, 0, false);
	is_remote_io_transaction_rdma_completed(transaction, 161, -EIO, false);
	is_remote_io_transaction_rdma_released(transaction, 161);
	failed = recorder.completions != 1 || recorder.successes != 1 ||
		recorder.invariants != 1 || recorder.settlements != 1;
	failed |= is_remote_io_transaction_engine_destroy(&engine.engine) != 0;
	recorder_destroy(&recorder);
	return failed;
}

static int test_immutable_specification_survives_caller_mutation(void)
{
	struct effect_recorder recorder;
	struct test_engine engine;
	struct is_remote_io_transaction_spec spec;
	struct is_remote_io_transaction *transaction;
	int failed;

	if (recorder_init(&recorder) ||
	    test_engine_init(&engine, &recorder,
		IS_REMOTE_IO_TRANSACTION_REMOTE_ONLY))
		return 1;
	spec = transaction_spec(&recorder, IS_REMOTE_IO_TRANSACTION_READ, 171);
	spec.sector = 24;
	is_remote_io_transaction_start(&engine.engine, &spec);
	transaction = recorder.transaction;
	spec.sector = 999;
	spec.generation = 999;
	is_remote_io_transaction_rdma_completed(transaction, 171, 0, false);
	is_remote_io_transaction_rdma_released(transaction, 171);
	failed = recorder.completed_sector != 24 ||
		recorder.completed_generation != 171 || recorder.settlements != 1;
	failed |= is_remote_io_transaction_engine_destroy(&engine.engine) != 0;
	recorder_destroy(&recorder);
	return failed;
}

static int test_engine_destroy_rejects_active_transaction(void)
{
	struct effect_recorder recorder;
	struct test_engine engine;
	struct is_remote_io_transaction_spec spec;
	struct is_remote_io_transaction *transaction;
	int failed;

	if (recorder_init(&recorder) ||
	    test_engine_init(&engine, &recorder,
		IS_REMOTE_IO_TRANSACTION_REMOTE_ONLY))
		return 1;
	spec = transaction_spec(&recorder, IS_REMOTE_IO_TRANSACTION_READ, 181);
	is_remote_io_transaction_start(&engine.engine, &spec);
	transaction = recorder.transaction;
	failed = is_remote_io_transaction_engine_destroy(&engine.engine) != -EBUSY ||
		recorder.invariants != 1;
	is_remote_io_transaction_rdma_completed(transaction, 181, 0, false);
	is_remote_io_transaction_rdma_released(transaction, 181);
	failed |= is_remote_io_transaction_engine_destroy(&engine.engine) != 0;
	recorder_destroy(&recorder);
	return failed;
}

static int test_adapter_reentry_during_start_and_settlement(void)
{
	struct effect_recorder recorder;
	struct test_engine engine;
	struct is_remote_io_transaction_spec spec;
	int failed;

	if (recorder_init(&recorder) ||
	    test_engine_init(&engine, &recorder,
		IS_REMOTE_IO_TRANSACTION_REMOTE_ONLY))
		return 1;
	recorder.complete_rdma_during_submit = true;
	recorder.release_rdma_during_submit = true;
	recorder.destroy_engine_during_release = true;
	recorder.destroy_engine_during_settle = true;
	spec = transaction_spec(&recorder, IS_REMOTE_IO_TRANSACTION_WRITE, 191);
	is_remote_io_transaction_start(&engine.engine, &spec);
	failed = recorder.completions != 1 || recorder.releases != 1 ||
		recorder.settlements != 1 || recorder.release_destroy_status != -EBUSY ||
		recorder.settle_destroy_status != 0 || recorder.invariants != 1 ||
		engine.engine.initialized;
	recorder_destroy(&recorder);
	return failed;
}

static int test_all_results_can_arrive_concurrently(void)
{
	struct effect_recorder recorder;
	struct test_engine engine;
	struct is_remote_io_transaction_spec spec;
	struct is_remote_io_transaction *transaction;
	struct test_barrier barrier;
	struct threaded_event events[3];
	pthread_t threads[3];
	unsigned int index;
	int failed = 0;

	if (recorder_init(&recorder) ||
	    test_engine_init(&engine, &recorder,
		IS_REMOTE_IO_TRANSACTION_BACKED_STRICT) ||
	    test_barrier_init(&barrier, 3))
		return 1;
	spec = transaction_spec(&recorder, IS_REMOTE_IO_TRANSACTION_WRITE, 201);
	is_remote_io_transaction_start(&engine.engine, &spec);
	transaction = recorder.transaction;
	events[0] = (struct threaded_event){
		.transaction = transaction,
		.start = &barrier,
		.kind = THREADED_EVENT_BACKING,
		.generation = 201,
		.status = 0,
	};
	events[1] = (struct threaded_event){
		.transaction = transaction,
		.start = &barrier,
		.kind = THREADED_EVENT_RDMA,
		.generation = 201,
		.status = 0,
	};
	events[2] = (struct threaded_event){
		.transaction = transaction,
		.start = &barrier,
		.kind = THREADED_EVENT_RELEASE,
		.generation = 201,
		.status = 0,
	};
	for (index = 0; index < 3; index++) {
		if (pthread_create(&threads[index], NULL, run_transaction_event,
			&events[index]))
			return 1;
	}
	for (index = 0; index < 3; index++)
		(void)pthread_join(threads[index], NULL);
	if (recorder.completions != 1 || recorder.successes != 1 ||
	    recorder.releases != 1 || recorder.settlements != 1 ||
	    recorder.invariants != 0)
		failed = 1;
	failed |= is_remote_io_transaction_engine_destroy(&engine.engine) != 0;
	test_barrier_destroy(&barrier);
	recorder_destroy(&recorder);
	return failed;
}

int main(void)
{
	int failed = test_backed_write_outcome_and_order_matrix() |
		test_backed_write_submits_recovery_before_rdma() |
		test_remote_first_acknowledges_before_late_settlement() |
		test_remote_first_retries_backing_before_degrading() |
		test_remote_read_outcome_matrix() |
		test_remote_read_fallback_allows_adapter_reentry() |
		test_remote_only_outcome_matrix() |
		test_synchronous_prepare_failures_follow_mode_policy() |
		test_start_consumes_payload_when_allocation_fails() |
		test_synchronous_submission_failures_settle_once() |
		test_stale_generation_cannot_complete_or_release() |
		test_duplicate_result_completes_and_reports_once() |
		test_immutable_specification_survives_caller_mutation() |
		test_engine_destroy_rejects_active_transaction() |
		test_adapter_reentry_during_start_and_settlement() |
		test_all_results_can_arrive_concurrently();

	if (failed)
		fprintf(stderr, "Remote I/O Transaction engine tests failed\n");
	return failed;
}
