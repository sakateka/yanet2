// Shared ABI between the BPF program and the collector: map key/value
// layouts and bounds both sides compile against.
//
// A field added on one side without the other silently misreads the
// map. Plain fixed-width types only, no pointers: this header is
// included unmodified by a clang -target bpf translation unit, which
// has no libc.

#pragma once

#include <linux/types.h>

// Per-site table size: one row per distinct call site that has
// acquired the lock at least once.
//
// Comfortably covers every call site a real control-plane process
// makes several times over; insertion past this bound is counted as
// an overflow event, never silently dropped. Overridable at compile
// time (-DCP_LOCK_MAX_SITES=n) so the selftest can build a copy of
// the BPF program whose table fills after only a handful of call
// sites, to exercise that overflow path.
#ifndef CP_LOCK_MAX_SITES
#define CP_LOCK_MAX_SITES 1024
#endif

// Bound on threads with an acquisition in flight at once, across
// every instrumented process.
#define CP_LOCK_MAX_THREADS 4096

// Log2-bucketed hold-time histogram, one bucket per bit position of a
// 64-bit nanosecond duration.
//
// Bucket i holds [2^i, 2^(i+1)) ns, bucket 0 also absorbs 0 — the
// full range a u64 duration can express, so a hold time is never
// clamped away.
#define CP_LOCK_HIST_BUCKETS 64

// Bound on distinct instrumented processes whose load address gets
// recorded, well above any realistic number sharing one collector.
#define CP_LOCK_MAX_PROCESSES 256

// Retry bound for the running-max compare-and-swap loop: concurrent
// updates to the same max could in principle retry indefinitely.
//
// The bound exists because bpftime's userspace execution gives no
// verifier guarantee against an unbounded loop here.
#define CP_LOCK_MAX_CAS_RETRIES 16

// Index into the per-run table of probed functions' link-time
// addresses, set before any uprobe can fire.
//
// A probe turns its own entry address into the calling process's
// load address using this index, independent of /proc, so that
// address stays available even after the process exits.
#define CP_LOCK_TARGET_LOCK 0
#define CP_LOCK_TARGET_TRY_LOCK 1
#define CP_LOCK_TARGET_UNLOCK 2
#define CP_LOCK_TARGET_COUNT 3

// Identifies a call site: the process that made the call and the
// return address its call instruction pushed.
//
// That address is the instruction right after the call, in the
// calling function.
struct cp_lock_site_key {
	__u64 tgid;
	__u64 ret;
};

// Cumulative stats for one call site, aggregated at unlock return
// (after release) so accounting never extends the next waiter's wait.
struct cp_lock_site_stats {
	__u64 count;
	__u64 wait_sum_ns;
	__u64 wait_max_ns;
	__u64 hold_sum_ns;
	__u64 hold_max_ns;
	// A failed attempt is not an acquisition and leaves no wait or
	// hold sample; it is tallied here instead.
	__u64 fail_count;
	__u64 hist[CP_LOCK_HIST_BUCKETS];
};

// Counters for events this tool chooses not to fold into any site's
// stats.
//
// Folding one in would record a sample that does not mean what every
// other sample at that site means. Reported next to overflow_events,
// so a snapshot's totals are never silently short.
struct cp_lock_drop_counters {
	// A lock/try_lock entry found in-flight state already recorded
	// for its thread.
	//
	// The lock discipline every site this tool targets follows means
	// a thread's own acquisition is never still in flight at a new
	// entry, so this can only be a kernel tid reused by a new thread
	// after the old one died between its own entry and matching
	// unlock, abandoning its state. That old state cannot describe
	// the new call, so it is discarded before it can reach an
	// unlock under this entry's name.
	__u64 stale_state;
	// The per-thread in-flight state table had no room for a new
	// entry.
	__u64 state_insert_fail;
	// An unlock with no matching in-flight state for that thread.
	__u64 unlock_no_state;
	// The per-process load-address table had no room for a new tgid.
	__u64 bias_insert_fail;
	// Reading the return address off the stack at entry failed.
	__u64 ret_read_fail;
	// The acquiring timestamp was still unset, or preceded the call's
	// own start, at unlock return.
	//
	// Folding either in would show a nonsensical duration.
	__u64 bad_timestamp;
	// A per-site table insert failed for a reason other than the
	// table being full (see overflow_events for that case).
	__u64 site_insert_fail;
};

// In-flight state for one thread between a lock/try_lock entry and
// the matching unlock return.
//
// Keyed by the kernel tid, unique system-wide at any instant, since
// every lock/unlock pair in the sites this tool targets runs
// start-to-finish on one OS thread.
struct cp_lock_state {
	__u64 start_ns;
	__u64 acquired_ns;
	__u64 hold_ns;
	// Return address captured at the lock/try_lock call, carried
	// through to the unlock return where the site's stats live.
	__u64 ret;
};
