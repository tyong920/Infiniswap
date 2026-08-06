#include "../is_io_policy.h"

#include <stdio.h>

static int expect_action(const char *name, enum is_io_action actual,
                         enum is_io_action expected)
{
	if (actual == expected)
		return 0;
	fprintf(stderr, "%s: got action %d, expected %d\n", name, actual,
		expected);
	return 1;
}

static int test_remote_first_remote_then_backing(void)
{
	struct is_io_policy state;
	int failed = 0;

	is_io_policy_init_remote_first_write(&state);
	failed |= expect_action("remote completion",
		is_io_policy_remote_complete(&state, 0),
		IS_IO_COMPLETE_SUCCESS);
	failed |= expect_action("late backing completion",
		is_io_policy_local_complete(&state, 0), IS_IO_NO_ACTION);
	if (!is_io_policy_releasable(&state)) {
		fprintf(stderr, "late backing resources were not releasable\n");
		failed = 1;
	}
	return failed;
}

static int test_remote_first_backing_then_remote(void)
{
	struct is_io_policy state;
	int failed = 0;

	is_io_policy_init_remote_first_write(&state);
	failed |= expect_action("early backing completion",
		is_io_policy_local_complete(&state, 0), IS_IO_NO_ACTION);
	failed |= expect_action("remote completion",
		is_io_policy_remote_complete(&state, 0),
		IS_IO_COMPLETE_SUCCESS);
	if (!is_io_policy_releasable(&state)) {
		fprintf(stderr, "completed write resources were not releasable\n");
		failed = 1;
	}
	return failed;
}

static int test_remote_failure_falls_back_to_backing_result(void)
{
	struct is_io_policy state;
	int failed = 0;

	is_io_policy_init_remote_first_write(&state);
	failed |= expect_action("remote failure",
		is_io_policy_remote_complete(&state, -1), IS_IO_NO_ACTION);
	failed |= expect_action("backing success",
		is_io_policy_local_complete(&state, 0),
		IS_IO_COMPLETE_SUCCESS);
	if (!is_io_policy_releasable(&state))
		failed = 1;

	is_io_policy_init_remote_first_write(&state);
	failed |= expect_action("backing failure",
		is_io_policy_local_complete(&state, -1), IS_IO_NO_ACTION);
	failed |= expect_action("remote failure after backing failure",
		is_io_policy_remote_complete(&state, -1),
		IS_IO_COMPLETE_ERROR);
	return failed;
}

static int test_remote_read_falls_back_once(void)
{
	struct is_io_policy state;
	int failed = 0;

	is_io_policy_init_remote_read(&state);
	failed |= expect_action("remote read failure",
		is_io_policy_remote_complete(&state, -1),
		IS_IO_SUBMIT_LOCAL);
	failed |= expect_action("backing read completion",
		is_io_policy_local_complete(&state, 0),
		IS_IO_COMPLETE_SUCCESS);
	failed |= expect_action("duplicate backing completion",
		is_io_policy_local_complete(&state, 0), IS_IO_NO_ACTION);
	return failed;
}

int main(void)
{
	return test_remote_first_remote_then_backing() |
		test_remote_first_backing_then_remote() |
		test_remote_failure_falls_back_to_backing_result() |
		test_remote_read_falls_back_once();
}
