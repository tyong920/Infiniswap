#include "../is_io_policy.h"

#include <stdio.h>
#include <string.h>

struct observed_actions {
	unsigned int completions;
	unsigned int successes;
	unsigned int errors;
	unsigned int degraded;
	unsigned int local_only;
	unsigned int local_submissions;
};

static void observe_action(struct observed_actions *observed,
			   enum is_io_action action)
{
	if (action & IS_IO_COMPLETE_SUCCESS) {
		observed->completions++;
		observed->successes++;
	}
	if (action & IS_IO_COMPLETE_ERROR) {
		observed->completions++;
		observed->errors++;
	}
	if (action & IS_IO_BACKING_DEGRADED)
		observed->degraded++;
	if (action & IS_IO_MARK_LOCAL_ONLY)
		observed->local_only++;
	if (action & IS_IO_SUBMIT_LOCAL)
		observed->local_submissions++;
}

static int test_write_outcome_matrix(void)
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

		for (local_status = 0; local_status >= -1; local_status--) {
			int remote_status;

			for (remote_status = 0; remote_status >= -1;
			     remote_status--) {
				unsigned int local_first;

				for (local_first = 0; local_first <= 1;
				     local_first++) {
					struct observed_actions observed = {0};
					struct is_io_policy state;
					int expect_success;
					int expect_local_only;

					is_io_policy_init_write(&state,
						policies[policy_index], 41);
					if (local_first) {
						observe_action(&observed,
							is_io_policy_local_complete(
								&state, local_status));
						observe_action(&observed,
							is_io_policy_remote_complete(
								&state, 41,
								remote_status));
					} else {
						observe_action(&observed,
							is_io_policy_remote_complete(
								&state, 41,
								remote_status));
						observe_action(&observed,
							is_io_policy_local_complete(
								&state, local_status));
					}

					expect_success = policies[policy_index] ==
						IS_IO_POLICY_STRICT_WRITE ?
						local_status == 0 :
						(local_status == 0 || remote_status == 0);
					expect_local_only = local_status == 0 &&
						remote_status != 0;
					if (observed.completions != 1 ||
					    observed.successes !=
						    (unsigned int)expect_success ||
					    observed.errors !=
						    (unsigned int)!expect_success ||
					    observed.degraded !=
						    (unsigned int)(local_status != 0) ||
					    observed.local_only !=
						    (unsigned int)expect_local_only ||
					    observed.local_submissions != 0 ||
					    !is_io_policy_releasable(&state)) {
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

static int test_remote_read_falls_back_once(void)
{
	struct observed_actions observed = {0};
	struct is_io_policy state;
	int failed = 0;

	is_io_policy_init_remote_read(&state, 73);
	observe_action(&observed,
		is_io_policy_remote_complete(&state, 73, -1));
	observe_action(&observed, is_io_policy_local_complete(&state, 0));
	observe_action(&observed, is_io_policy_local_complete(&state, 0));
	if (observed.local_submissions != 1 || observed.completions != 1 ||
	    observed.successes != 1 || !is_io_policy_releasable(&state)) {
		fprintf(stderr, "remote read fallback was not completed once\n");
		failed = 1;
	}
	return failed;
}

static int test_cancelled_generation_ignores_late_completion(void)
{
	struct observed_actions observed = {0};
	struct is_io_policy state;
	int failed = 0;

	is_io_policy_init_write(&state, IS_IO_POLICY_STRICT_WRITE, 101);
	observe_action(&observed, is_io_policy_local_complete(&state, 0));
	observe_action(&observed, is_io_policy_cancel_remote(&state, 101, -1));
	observe_action(&observed,
		is_io_policy_remote_complete(&state, 101, 0));
	if (observed.completions != 1 || observed.successes != 1 ||
	    observed.local_only != 1 || !state.remote_cancelled ||
	    !is_io_policy_releasable(&state)) {
		fprintf(stderr, "cancelled request accepted a late completion\n");
		failed = 1;
	}

	is_io_policy_init_remote_read(&state, 202);
	if (is_io_policy_remote_complete(&state, 201, 0) != IS_IO_NO_ACTION ||
	    state.remote_done || !state.remote_pending) {
		fprintf(stderr, "stale generation changed request state\n");
		failed = 1;
	}
	observe_action(&observed, is_io_policy_cancel_remote(&state, 202, -1));
	observe_action(&observed, is_io_policy_local_complete(&state, 0));
	if (!is_io_policy_releasable(&state)) {
		fprintf(stderr, "cancelled read fallback was not releasable\n");
		failed = 1;
	}
	return failed;
}

static int test_remote_only_never_falls_back_or_masks_failure(void)
{
	struct observed_actions observed = {0};
	struct is_io_policy state;
	int failed = 0;

	is_io_policy_init_remote_only_write(&state, 301);
	observe_action(&observed,
		is_io_policy_remote_complete(&state, 301, -1));
	if (observed.completions != 1 || observed.errors != 1 ||
	    observed.local_submissions != 0 || observed.local_only != 0 ||
	    !is_io_policy_releasable(&state)) {
		fprintf(stderr, "Remote-Only write failure was masked\n");
		failed = 1;
	}

	memset(&observed, 0, sizeof(observed));
	is_io_policy_init_remote_only_read(&state, 302);
	observe_action(&observed,
		is_io_policy_cancel_remote(&state, 302, -1));
	if (observed.completions != 1 || observed.errors != 1 ||
	    observed.local_submissions != 0 || !state.remote_cancelled ||
	    !is_io_policy_releasable(&state)) {
		fprintf(stderr, "Remote-Only read attempted a local fallback\n");
		failed = 1;
	}

	memset(&observed, 0, sizeof(observed));
	is_io_policy_init_remote_only_write(&state, 303);
	observe_action(&observed,
		is_io_policy_remote_complete(&state, 303, 0));
	if (observed.completions != 1 || observed.successes != 1 ||
	    !is_io_policy_releasable(&state)) {
		fprintf(stderr, "Remote-Only success did not complete exactly once\n");
		failed = 1;
	}
	return failed;
}

int main(void)
{
	return test_write_outcome_matrix() |
		test_remote_read_falls_back_once() |
		test_cancelled_generation_ignores_late_completion() |
		test_remote_only_never_falls_back_or_masks_failure();
}
