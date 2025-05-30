#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

#include <cmdline.h>
#include <cmdline_parse.h>
#include <cmdline_rdline.h>
#include <cmdline_socket.h>
extern cmdline_parse_ctx_t main_ctx[];

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_string_fns.h>
#ifdef RTE_LIB_TIMER
#include <rte_timer.h>
#endif

#include "test.h"

#define RTE_LOGTYPE_APP RTE_LOGTYPE_USER7

#define FOR_EACH_SUITE_TESTCASE(iter, suite, case)                             \
	for (iter = 0, case = suite->unit_test_cases[0];                       \
	     suite->unit_test_cases[iter].testcase ||                          \
	     suite->unit_test_cases[iter].testcase_with_data;                  \
	     iter++, case = suite->unit_test_cases[iter])

#define FOR_EACH_SUITE_TESTSUITE(iter, suite, sub_ts)                          \
	for (iter = 0,                                                         \
	    sub_ts = suite->unit_test_suites ? suite->unit_test_suites[0]      \
					     : NULL;                           \
	     sub_ts && suite->unit_test_suites[iter]->suite_name != NULL;      \
	     iter++, sub_ts = suite->unit_test_suites[iter])

const char *prgname; /* to be set to argv[0] */

int last_test_result;

#define MAX_EXTRA_ARGS 32

// ugly, standart non compatible hack for sigsev
void
handler(int sig) {
	void *array[10];
	size_t size;

	size = backtrace(array, 10);

	fprintf(stderr, "Error: signal %d:\n", sig);
	backtrace_symbols_fd(array, size, STDERR_FILENO);
	exit(1);
}

int
main(int argc, char **argv) {
	signal(SIGSEGV, handler); // install our handler
	struct cmdline *cl;
	char *tests[argc]; /* store an array of tests to run */
	int test_count = 0;
	int i;
	char *extra_args;
	int ret;

	extra_args = getenv("DPDK_TEST_PARAMS") ?: "--no-huge";
	if (extra_args != NULL && strlen(extra_args) > 0) {
		char **all_argv;
		char *eargv[MAX_EXTRA_ARGS];
		int all_argc;
		int eargc;
		int i;

		RTE_LOG(INFO,
			APP,
			"Using additional DPDK_TEST_PARAMS: '%s'\n",
			extra_args);
		eargc = rte_strsplit(
			extra_args,
			strlen(extra_args),
			eargv,
			MAX_EXTRA_ARGS,
			' '
		);

		/* merge argc/argv and the environment args */
		all_argc = argc + eargc;
		all_argv = malloc(sizeof(*all_argv) * (all_argc + 1));
		if (all_argv == NULL) {
			ret = -1;
			goto out;
		}

		for (i = 0; i < argc; i++)
			all_argv[i] = argv[i];
		for (i = 0; i < eargc; i++)
			all_argv[argc + i] = eargv[i];
		all_argv[all_argc] = NULL;

		/* call eal_init with combined args */
		ret = rte_eal_init(all_argc, all_argv);
		free(all_argv);
	} else
		ret = rte_eal_init(argc, argv);
	if (ret < 0) {
		ret = -1;
		goto out;
	}

	argv += ret;
	argc -= ret;

	prgname = argv[0];

#ifdef RTE_LIB_TIMER
	ret = rte_timer_subsystem_init();
	if (ret < 0 && ret != -EALREADY) {
		ret = -1;
		goto out;
	}
#endif

	if (commands_init() < 0) {
		ret = -1;
		goto out;
	}

#ifdef RTE_LIBEAL_USE_HPET
	if (rte_eal_hpet_init(1) < 0)
#endif
		RTE_LOG(INFO,
			APP,
			"HPET is not enabled, using TSC as default timer\n");

	char *dpdk_test = getenv("YANET_TEST");

	if (dpdk_test && strlen(dpdk_test) > 0)
		tests[test_count++] = dpdk_test;
	for (i = 1; i < argc; i++)
		tests[test_count++] = argv[i];

	if (test_count > 0) {
		char buf[1024];
		char *dpdk_test_skip = getenv("YANET_TEST_SKIP");
		char *skip_tests[128] = {0};
		size_t n_skip_tests = 0;

		if (dpdk_test_skip != NULL && strlen(dpdk_test_skip) > 0) {
			int split_ret;
			char *dpdk_test_skip_cp = strdup(dpdk_test_skip);
			if (dpdk_test_skip_cp == NULL) {
				ret = -1;
				goto out;
			}
			dpdk_test_skip = dpdk_test_skip_cp;
			split_ret = rte_strsplit(
				dpdk_test_skip,
				strlen(dpdk_test_skip),
				skip_tests,
				RTE_DIM(skip_tests),
				','
			);
			if (split_ret > 0)
				n_skip_tests = split_ret;
			else
				free(dpdk_test_skip);
		}

		cl = cmdline_new(main_ctx, "YANET>>", 0, 1);
		if (cl == NULL) {
			ret = -1;
			goto out;
		}

		for (i = 0; i < test_count; i++) {
			/* check if test is to be skipped */
			for (size_t j = 0; j < n_skip_tests; j++) {
				if (strcmp(tests[i], skip_tests[j]) == 0) {
					fprintf(stderr,
						"Skipping %s "
						"[YANET_TEST_SKIP]\n",
						tests[i]);
					ret = TEST_SKIPPED;
					goto end_of_cmd;
				}
			}

			snprintf(buf, sizeof(buf), "%s\n", tests[i]);
			if (cmdline_parse_check(cl, buf) < 0) {
				printf("Error: invalid test command: '%s'\n",
				       tests[i]);
				ret = -1;
			} else if (cmdline_in(cl, buf, strlen(buf)) < 0) {
				printf("error on cmdline input\n");
				ret = -1;
			} else
				ret = last_test_result;

		end_of_cmd:
			if (ret != 0 && ret != TEST_SKIPPED)
				break;
		}
		if (n_skip_tests > 0)
			free(dpdk_test_skip);

		cmdline_free(cl);
		goto out;
	} else {
		/* if no YANET_TEST env variable, go interactive */
		cl = cmdline_stdin_new(main_ctx, "YANET>>");
		if (cl == NULL) {
			ret = -1;
			goto out;
		}

		cmdline_interact(cl);
		cmdline_stdin_exit(cl);
	}
	ret = 0;

out:
#ifdef RTE_LIB_TIMER
	rte_timer_subsystem_finalize();
#endif
	rte_eal_cleanup();
	return ret;
}

static void
unit_test_suite_count_tcs_on_setup_fail(
	struct unit_test_suite *suite,
	int test_success,
	unsigned int *sub_ts_failed,
	unsigned int *sub_ts_skipped,
	unsigned int *sub_ts_total
) {
	struct unit_test_case tc;
	struct unit_test_suite *ts;
	int i;

	FOR_EACH_SUITE_TESTSUITE(i, suite, ts) {
		unit_test_suite_count_tcs_on_setup_fail(
			ts,
			test_success,
			sub_ts_failed,
			sub_ts_skipped,
			sub_ts_total
		);
		suite->total += ts->total;
		suite->failed += ts->failed;
		suite->skipped += ts->skipped;
		if (ts->failed)
			(*sub_ts_failed)++;
		else
			(*sub_ts_skipped)++;
		(*sub_ts_total)++;
	}
	FOR_EACH_SUITE_TESTCASE(i, suite, tc) {
		suite->total++;
		if (!tc.enabled || test_success == TEST_SKIPPED)
			suite->skipped++;
		else
			suite->failed++;
	}
}

static void
unit_test_suite_reset_counts(struct unit_test_suite *suite) {
	struct unit_test_suite *ts;
	int i;

	FOR_EACH_SUITE_TESTSUITE(i, suite, ts)
	unit_test_suite_reset_counts(ts);
	suite->total = 0;
	suite->executed = 0;
	suite->succeeded = 0;
	suite->skipped = 0;
	suite->failed = 0;
	suite->unsupported = 0;
}

int
unit_test_suite_runner(struct unit_test_suite *suite) {
	int test_success, i, ret;
	const char *status;
	struct unit_test_case tc;
	struct unit_test_suite *ts;
	unsigned int sub_ts_succeeded = 0, sub_ts_failed = 0;
	unsigned int sub_ts_skipped = 0, sub_ts_total = 0;

	unit_test_suite_reset_counts(suite);

	if (suite->suite_name) {
		printf(" + "
		       "-------------------------------------------------------"
		       " +\n");
		printf(" + Test Suite : %s\n", suite->suite_name);
	}

	if (suite->setup) {
		test_success = suite->setup();
		if (test_success != 0) {
			/*
			 * setup did not pass, so count all enabled tests and
			 * mark them as failed/skipped
			 */
			unit_test_suite_count_tcs_on_setup_fail(
				suite,
				test_success,
				&sub_ts_failed,
				&sub_ts_skipped,
				&sub_ts_total
			);
			goto suite_summary;
		}
	}

	printf(" + ------------------------------------------------------- +\n"
	);

	FOR_EACH_SUITE_TESTCASE(suite->total, suite, tc) {
		if (!tc.enabled) {
			suite->skipped++;
			continue;
		} else {
			suite->executed++;
		}

		/* run test case setup */
		if (tc.setup)
			test_success = tc.setup();
		else
			test_success = TEST_SUCCESS;

		if (test_success == TEST_SUCCESS) {
			/* run the test case */
			if (tc.testcase)
				test_success = tc.testcase();
			else if (tc.testcase_with_data)
				test_success = tc.testcase_with_data(tc.data);
			else
				test_success = -ENOTSUP;

			if (test_success == TEST_SUCCESS)
				suite->succeeded++;
			else if (test_success == TEST_SKIPPED) {
				suite->skipped++;
				suite->executed--;
			} else if (test_success == -ENOTSUP) {
				suite->unsupported++;
				suite->executed--;
			} else
				suite->failed++;
		} else if (test_success == -ENOTSUP) {
			suite->unsupported++;
		} else if (test_success == TEST_SKIPPED) {
			suite->skipped++;
		} else {
			suite->failed++;
		}

		/* run the test case teardown */
		if (tc.teardown)
			tc.teardown();

		if (test_success == TEST_SUCCESS)
			status = "succeeded";
		else if (test_success == TEST_SKIPPED)
			status = "skipped";
		else if (test_success == -ENOTSUP)
			status = "unsupported";
		else
			status = "failed";

		printf(" + TestCase [%2d] : %s %s\n",
		       suite->total,
		       tc.name,
		       status);
	}
	FOR_EACH_SUITE_TESTSUITE(i, suite, ts) {
		ret = unit_test_suite_runner(ts);
		if (ret == TEST_SUCCESS)
			sub_ts_succeeded++;
		else if (ret == TEST_SKIPPED)
			sub_ts_skipped++;
		else
			sub_ts_failed++;
		sub_ts_total++;

		suite->total += ts->total;
		suite->succeeded += ts->succeeded;
		suite->failed += ts->failed;
		suite->skipped += ts->skipped;
		suite->unsupported += ts->unsupported;
		suite->executed += ts->executed;
	}

	/* Run test suite teardown */
	if (suite->teardown)
		suite->teardown();

	goto suite_summary;

suite_summary:
	printf(" + ------------------------------------------------------- +\n"
	);
	printf(" + Test Suite Summary : %s\n", suite->suite_name);
	printf(" + ------------------------------------------------------- +\n"
	);

	FOR_EACH_SUITE_TESTSUITE(i, suite, ts)
	printf(" + %s : %d/%d passed, %d/%d skipped, "
	       "%d/%d failed, %d/%d unsupported\n",
	       ts->suite_name,
	       ts->succeeded,
	       ts->total,
	       ts->skipped,
	       ts->total,
	       ts->failed,
	       ts->total,
	       ts->unsupported,
	       ts->total);

	if (suite->unit_test_suites) {
		printf(" + "
		       "-------------------------------------------------------"
		       " +\n");
		printf(" + Sub Testsuites Total :     %2d\n", sub_ts_total);
		printf(" + Sub Testsuites Skipped :   %2d\n", sub_ts_skipped);
		printf(" + Sub Testsuites Passed :    %2d\n", sub_ts_succeeded);
		printf(" + Sub Testsuites Failed :    %2d\n", sub_ts_failed);
		printf(" + "
		       "-------------------------------------------------------"
		       " +\n");
	}

	printf(" + Tests Total :       %2d\n", suite->total);
	printf(" + Tests Skipped :     %2d\n", suite->skipped);
	printf(" + Tests Executed :    %2d\n", suite->executed);
	printf(" + Tests Unsupported:  %2d\n", suite->unsupported);
	printf(" + Tests Passed :      %2d\n", suite->succeeded);
	printf(" + Tests Failed :      %2d\n", suite->failed);
	printf(" + ------------------------------------------------------- +\n"
	);

	last_test_result = suite->failed;

	if (suite->failed)
		return TEST_FAILED;
	if (suite->total == suite->skipped)
		return TEST_SKIPPED;
	return TEST_SUCCESS;
}
