// Synthetic target for the cp-lock selftest.
//
// Defines cp_config_lock/cp_config_try_lock/cp_config_unlock with the
// same CAS-spin semantics as lib/controlplane/config/zone.c, called
// from distinctly named functions so a report can be checked against
// known call sites, counts and a known critical-section length. Every
// such function is kept noinline, since at -O2 in a single
// translation unit the compiler would otherwise fold a tiny CAS
// wrapper into its caller and leave no call for a uprobe to catch.

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#define CALLER_A_ITERATIONS 1000
#define CALLER_B_ITERATIONS 500
#define HOLD_USEC 100
#define TRY_LOCK_HOLD_USEC 200000
#define TRY_LOCK_ATTEMPTS 20
#define TRY_LOCK_ATTEMPT_INTERVAL_USEC 2000
#define TRY_LOCK_SUCCESS_ITERATIONS 50
#define CONTENDED_WAIT_HOLD_USEC 150000
#define POST_RUN_GRACE_USEC 1000000
#define ATTACH_PAUSE_DEFAULT_THREADS 64
#define ATTACH_PAUSE_DEFAULT_DURATION_SEC 5

struct workload_lock {
	atomic_int owner;
};

static struct workload_lock g_lock;

__attribute__((noinline)) bool
cp_config_try_lock(struct workload_lock *lock) {
	int zero = 0;
	return atomic_compare_exchange_strong(&lock->owner, &zero, 1);
}

__attribute__((noinline)) void
cp_config_lock(struct workload_lock *lock) {
	for (;;) {
		int zero = 0;
		if (atomic_compare_exchange_strong(&lock->owner, &zero, 1)) {
			return;
		}
		while (atomic_load(&lock->owner) != 0) {
			sched_yield();
		}
	}
}

__attribute__((noinline)) void
cp_config_unlock(struct workload_lock *lock) {
	atomic_store(&lock->owner, 0);
}

__attribute__((noinline)) void
caller_a(void) {
	cp_config_lock(&g_lock);
	usleep(HOLD_USEC);
	cp_config_unlock(&g_lock);
}

__attribute__((noinline)) void
caller_b(void) {
	cp_config_lock(&g_lock);
	usleep(HOLD_USEC);
	cp_config_unlock(&g_lock);
}

// Every attempt against this call site is expected to fail: the lock
// is held by another thread for the whole budget of the attempt loop.
//
// Unlocking immediately if an attempt unexpectedly succeeds keeps a
// flaky acquisition from holding the lock for the rest of the run,
// instead of just failing once.
__attribute__((noinline)) void
caller_try_lock(void) {
	if (cp_config_try_lock(&g_lock)) {
		cp_config_unlock(&g_lock);
	}
}

// Uncontended: nothing else holds g_lock while this runs, so every call
// here is expected to succeed, unlike caller_try_lock above.
__attribute__((noinline)) void
caller_try_lock_success(void) {
	if (cp_config_try_lock(&g_lock)) {
		usleep(HOLD_USEC);
		cp_config_unlock(&g_lock);
	}
}

static void
run_try_lock_success(void) {
	for (int i = 0; i < TRY_LOCK_SUCCESS_ITERATIONS; ++i) {
		caller_try_lock_success();
	}
}

// Distinct call site from every other cp_config_lock() call in this
// file.
//
// The selftest checks this site's own wait stats against the holder
// thread's known hold time below.
__attribute__((noinline)) void
caller_contended_wait(void) {
	cp_config_lock(&g_lock);
	cp_config_unlock(&g_lock);
}

struct contended_wait_state {
	atomic_bool holder_ready;
};

static void *
contended_wait_holder_thread(void *arg) {
	struct contended_wait_state *state = arg;
	cp_config_lock(&g_lock);
	atomic_store(&state->holder_ready, true);
	usleep(CONTENDED_WAIT_HOLD_USEC);
	cp_config_unlock(&g_lock);
	return NULL;
}

static void *
contended_wait_waiter_thread(void *arg) {
	struct contended_wait_state *state = arg;
	while (!atomic_load(&state->holder_ready)) {
		sched_yield();
	}
	caller_contended_wait();
	return NULL;
}

// Blocks on cp_config_lock while the holder thread keeps it for a
// known duration.
//
// So the selftest can check this site's wait time against that
// duration.
static void
run_contended_wait(void) {
	struct contended_wait_state state = {0};
	pthread_t holder, waiter;
	if (pthread_create(
		    &holder, NULL, contended_wait_holder_thread, &state
	    ) != 0) {
		fprintf(stderr, "pthread_create(holder) failed\n");
		exit(1);
	}
	if (pthread_create(
		    &waiter, NULL, contended_wait_waiter_thread, &state
	    ) != 0) {
		fprintf(stderr, "pthread_create(waiter) failed\n");
		exit(1);
	}
	pthread_join(holder, NULL);
	pthread_join(waiter, NULL);
}

struct try_contention_state {
	atomic_bool holder_ready;
};

static void *
holder_thread(void *arg) {
	struct try_contention_state *state = arg;
	cp_config_lock(&g_lock);
	atomic_store(&state->holder_ready, true);
	usleep(TRY_LOCK_HOLD_USEC);
	cp_config_unlock(&g_lock);
	return NULL;
}

// Every attempt here lands while the lock is provably still held by
// the other thread.
//
// This loop only starts once that hold is confirmed, and its entire
// attempt budget comfortably fits inside how long the other thread
// holds the lock, so each attempt here is a guaranteed failure.
static void *
try_thread(void *arg) {
	struct try_contention_state *state = arg;
	while (!atomic_load(&state->holder_ready)) {
		sched_yield();
	}
	for (int i = 0; i < TRY_LOCK_ATTEMPTS; ++i) {
		caller_try_lock();
		usleep(TRY_LOCK_ATTEMPT_INTERVAL_USEC);
	}
	return NULL;
}

static void
run_try_lock_contention(void) {
	struct try_contention_state state = {0};
	pthread_t holder, tryer;
	if (pthread_create(&holder, NULL, holder_thread, &state) != 0) {
		fprintf(stderr, "pthread_create(holder) failed\n");
		exit(1);
	}
	if (pthread_create(&tryer, NULL, try_thread, &state) != 0) {
		fprintf(stderr, "pthread_create(tryer) failed\n");
		exit(1);
	}
	pthread_join(holder, NULL);
	pthread_join(tryer, NULL);
}

static void
run_workload(void) {
	for (int i = 0; i < CALLER_A_ITERATIONS; ++i) {
		caller_a();
	}
	for (int i = 0; i < CALLER_B_ITERATIONS; ++i) {
		caller_b();
	}
	run_try_lock_contention();
	run_try_lock_success();
	run_contended_wait();
}

static double
timespec_diff_sec(struct timespec a, struct timespec b) {
	return (double)(a.tv_sec - b.tv_sec) +
	       (double)(a.tv_nsec - b.tv_nsec) / 1e9;
}

// One spinning thread's contribution to the attach-pause measurement:
// the largest gap it ever saw between two consecutive timestamps.
//
// A ptrace-based injection stops every thread in the process while it
// patches code in memory, so that stop shows up here as one outlying
// gap, letting a driver script derive the pause from however long this
// runs without needing to coordinate with the injection's own timing.
struct spin_result {
	double max_gap_sec;
};

static void *
spin_thread(void *arg) {
	struct spin_result *result = arg;
	struct timespec start, prev, now;
	clock_gettime(CLOCK_MONOTONIC, &start);
	prev = start;
	result->max_gap_sec = 0;
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		double gap = timespec_diff_sec(now, prev);
		if (gap > result->max_gap_sec) {
			result->max_gap_sec = gap;
		}
		prev = now;
		if (timespec_diff_sec(now, start) >=
		    ATTACH_PAUSE_DEFAULT_DURATION_SEC) {
			break;
		}
	}
	return NULL;
}

// Runs the given number of spinning threads for a fixed duration and
// prints the largest per-thread gap seen.
//
// Used as a proxy for how long a concurrent `bpftime attach` stopped
// every thread in this process.
static void
run_attach_pause_measurement(int thread_count) {
	pthread_t *threads = calloc((size_t)thread_count, sizeof(*threads));
	struct spin_result *results =
		calloc((size_t)thread_count, sizeof(*results));
	if (!threads || !results) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	for (int i = 0; i < thread_count; ++i) {
		if (pthread_create(
			    &threads[i], NULL, spin_thread, &results[i]
		    ) != 0) {
			fprintf(stderr, "pthread_create failed\n");
			exit(1);
		}
	}
	for (int i = 0; i < thread_count; ++i) {
		pthread_join(threads[i], NULL);
	}

	double max_gap_sec = 0;
	for (int i = 0; i < thread_count; ++i) {
		if (results[i].max_gap_sec > max_gap_sec) {
			max_gap_sec = results[i].max_gap_sec;
		}
	}
	printf("max_gap_sec %f\n", max_gap_sec);
	fflush(stdout);

	free(threads);
	free(results);
}

// Parses a strictly positive integer with no trailing garbage. Returns
// 1 and writes it on success, 0 on any parse error.
static int
parse_positive_int(const char *s, int *out) {
	if (!s || s[0] == '\0') {
		return 0;
	}
	errno = 0;
	char *end = NULL;
	long val = strtol(s, &end, 10);
	if (*end != '\0' || errno == ERANGE || val <= 0 || val > INT_MAX) {
		return 0;
	}
	*out = (int)val;
	return 1;
}

// Runs immediately by default, or waits for a go-ahead signal first.
//
// `bpftime start` wants the immediate run; a driver script using
// `bpftime attach` sends the signal after reading this process's
// printed pid. Either way, it then stays alive for a grace period
// before exiting, so a driver script has a guaranteed window to
// snapshot it while it is still running.
int
main(int argc, char **argv) {
	// Test-only: opts this process out of the default ptrace_scope
	// restriction.
	//
	// So `bpftime attach` works here without requiring root,
	// CAP_SYS_PTRACE, or a host-wide ptrace_scope change just to run
	// the selftest. A real instrumented target does not get this and
	// needs one of those instead — see the README.
	prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);

	// Blocked before the pid line below is printed, not just before
	// the wait.
	//
	// A driver script can send SIGUSR1 the instant it reads that
	// line, and this process must not be exposed to the default
	// terminate-on-SIGUSR1 disposition in that window.
	sigset_t usr1_set;
	sigemptyset(&usr1_set);
	sigaddset(&usr1_set, SIGUSR1);
	sigprocmask(SIG_BLOCK, &usr1_set, NULL);

	printf("pid %d\n", getpid());
	fflush(stdout);

	if (argc > 1 && strcmp(argv[1], "--measure-attach-pause") == 0) {
		int thread_count = ATTACH_PAUSE_DEFAULT_THREADS;
		if (argc > 2 && !parse_positive_int(argv[2], &thread_count)) {
			fprintf(stderr, "invalid thread count: %s\n", argv[2]);
			return 1;
		}
		run_attach_pause_measurement(thread_count);
		return 0;
	}

	bool wait_for_signal =
		argc > 1 && strcmp(argv[1], "--wait-for-signal") == 0;
	if (wait_for_signal) {
		int sig;
		sigwait(&usr1_set, &sig);
	}

	run_workload();

	printf("done\n");
	fflush(stdout);
	usleep(POST_RUN_GRACE_USEC);
	return 0;
}
