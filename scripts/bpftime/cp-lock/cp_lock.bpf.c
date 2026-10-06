// Uprobe program for cp_config_lock / cp_config_try_lock / cp_config_unlock.
//
// Six probes (entry and return of each function) turn a call/return pair
// into a wait and hold duration, keyed by the calling thread while the
// call is in flight and folded into a per-call-site total at unlock
// return. x86-64 only: the caller's return address is read off the top
// of the stack at function entry, which only holds at the entry
// instruction on this architecture.

#if !defined(__TARGET_ARCH_x86)
#error "cp_lock.bpf.c reads the return address off the x86-64 stack layout; build with -D__TARGET_ARCH_x86"
#endif

#include <linux/bpf.h>
#include <linux/ptrace.h>

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#include "cp_lock.h"

char LICENSE[] SEC("license") = "GPL";

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, CP_LOCK_MAX_SITES);
	__type(key, struct cp_lock_site_key);
	__type(value, struct cp_lock_site_stats);
} cp_lock_sites SEC(".maps");

// Evicts stale entries: a thread that never reaches its matching
// unlock would otherwise hold its slot forever.
//
// This can happen if the thread is killed mid-call, or takes one of
// the drop paths below. Verified empirically that this bpftime
// build's LRU eviction actually runs under pressure here, rather
// than just capping inserts once the table fills.
struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, CP_LOCK_MAX_THREADS);
	__type(key, __u64);
	__type(value, struct cp_lock_state);
} cp_lock_state_map SEC(".maps");

// Single counter: an acquisition that was not recorded because the
// per-site table was full.
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u64);
} cp_lock_overflow_events SEC(".maps");

// A permanently zeroed row, used as the initial value for a new site.
//
// A per-site stats row is too large for a zeroed instance of it to
// live on the BPF stack — its histogram alone exceeds the stack limit
// — so the all-zero value a freshly allocated map entry already holds
// is read from here instead.
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct cp_lock_site_stats);
} cp_lock_site_zero SEC(".maps");

// The three probed functions' link-time addresses, one per function,
// written before any uprobe can fire.
//
// Used to derive a calling process's load address independent of
// /proc, so a report can still name a process's call sites after
// that process has exited.
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, CP_LOCK_TARGET_COUNT);
	__type(key, __u32);
	__type(value, __u64);
} cp_lock_targets SEC(".maps");

// One process's load address, recorded from whichever probed function
// it calls first.
//
// Kept for as long as the collector runs, well past that process
// exiting.
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, CP_LOCK_MAX_PROCESSES);
	__type(key, __u64);
	__type(value, __u64);
} cp_lock_bias SEC(".maps");

// Counts for events this tool declines to fold into a site's stats.
// See struct cp_lock_drop_counters for what each one means.
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct cp_lock_drop_counters);
} cp_lock_drops SEC(".maps");

static __always_inline struct cp_lock_drop_counters *
cp_lock_drops_ptr(void) {
	__u32 zero_key = 0;
	return bpf_map_lookup_elem(&cp_lock_drops, &zero_key);
}

// Reads the caller's return address off the top of the stack.
//
// Returns 1 and writes it on success, 0 if the read failed (the
// stack page was not resident, or similar) — a zero address is never
// substituted in that case, so a failed read never masquerades as a
// real call site.
static __always_inline int
cp_lock_read_ret_addr(struct pt_regs *ctx, __u64 *out) {
	return bpf_probe_read_user(
		       out, sizeof(*out), (void *)PT_REGS_SP(ctx)
	       ) == 0;
}

// Records tgid's load address on its first call to any probed
// function.
//
// Computed from that call's own entry address and the matching
// link-time address set up ahead of time. A later call from the same
// process leaves the first recording in place: the load address does
// not change for the life of a process.
static __always_inline void
cp_lock_record_bias(__u64 tgid, __u64 entry_ip, __u32 target_index) {
	__u64 *target_vaddr =
		bpf_map_lookup_elem(&cp_lock_targets, &target_index);
	if (!target_vaddr || *target_vaddr == 0) {
		return;
	}
	__u64 bias = entry_ip - *target_vaddr;
	if (bpf_map_update_elem(&cp_lock_bias, &tgid, &bias, BPF_NOEXIST) ==
	    0) {
		return;
	}
	// A concurrent insert from another thread of the same process is
	// the usual reason this one fails.
	//
	// Only count it as a drop when the entry is genuinely still
	// missing afterward.
	if (!bpf_map_lookup_elem(&cp_lock_bias, &tgid)) {
		struct cp_lock_drop_counters *drops = cp_lock_drops_ptr();
		if (drops) {
			__sync_fetch_and_add(&drops->bias_insert_fail, 1);
		}
	}
}

// Raises *slot to value if it is not already at least that high,
// retrying under concurrent writers up to a fixed bound.
//
// This runs under bpftime's userspace execution, not a kernel
// verifier, so tolerating an unbounded loop here would rely on a
// guarantee this tool does not actually have.
static __always_inline void
cp_lock_raise_max(__u64 *slot, __u64 value) {
	__u64 old = *slot;
	for (int i = 0; i < CP_LOCK_MAX_CAS_RETRIES && old < value; ++i) {
		__u64 seen = __sync_val_compare_and_swap(slot, old, value);
		if (seen == old) {
			break;
		}
		old = seen;
	}
}

// Position of the highest set bit, branchless (BPF has no clz
// instruction).
//
// 0 when the input is zero, matching bucket 0 absorbing a zero
// duration.
static __always_inline __u32
cp_lock_log2_32(__u32 v) {
	__u32 shift, r;

	r = (__u32)(v > 0xFFFF) << 4;
	v >>= r;
	shift = (__u32)(v > 0xFF) << 3;
	v >>= shift;
	r |= shift;
	shift = (__u32)(v > 0xF) << 2;
	v >>= shift;
	r |= shift;
	shift = (__u32)(v > 0x3) << 1;
	v >>= shift;
	r |= shift;
	r |= (v >> 1);
	return r;
}

static __always_inline __u32
cp_lock_log2_64(__u64 v) {
	__u32 hi = (__u32)(v >> 32);
	if (hi) {
		return cp_lock_log2_32(hi) + 32;
	}
	return cp_lock_log2_32((__u32)v);
}

// Finds the site's running stats, creating a zeroed row on first
// sight.
//
// Returns NULL when the site is genuinely new, not a concurrent
// insert by another thread, and the table had no room for it. A
// failed insert's return code alone cannot tell a full table apart
// from this bpftime build's behavior: an insert into a full HASH
// table reports success while the entry still does not exist,
// instead of the -E2BIG a kernel BPF hash map would return. The
// re-lookup below is what actually catches that; a genuine nonzero
// return code is a different, unexpected failure, counted separately
// so it is never confused with an ordinary full table.
static __always_inline struct cp_lock_site_stats *
cp_lock_site(__u64 tgid, __u64 ret) {
	struct cp_lock_site_key key = {.tgid = tgid, .ret = ret};

	struct cp_lock_site_stats *stats =
		bpf_map_lookup_elem(&cp_lock_sites, &key);
	if (stats) {
		return stats;
	}

	__u32 zero_key = 0;
	struct cp_lock_site_stats *zeroed =
		bpf_map_lookup_elem(&cp_lock_site_zero, &zero_key);
	if (!zeroed) {
		return NULL;
	}
	long rc =
		bpf_map_update_elem(&cp_lock_sites, &key, zeroed, BPF_NOEXIST);
	stats = bpf_map_lookup_elem(&cp_lock_sites, &key);
	if (stats) {
		return stats;
	}

	if (rc != 0) {
		struct cp_lock_drop_counters *drops = cp_lock_drops_ptr();
		if (drops) {
			__sync_fetch_and_add(&drops->site_insert_fail, 1);
		}
		return NULL;
	}
	__u64 *overflow =
		bpf_map_lookup_elem(&cp_lock_overflow_events, &zero_key);
	if (overflow) {
		__sync_fetch_and_add(overflow, 1);
	}
	return NULL;
}

static __always_inline int
cp_lock_entry(struct pt_regs *ctx, __u32 target_index) {
	__u64 pid_tgid = bpf_get_current_pid_tgid();
	cp_lock_record_bias(pid_tgid >> 32, PT_REGS_IP(ctx), target_index);

	if (bpf_map_lookup_elem(&cp_lock_state_map, &pid_tgid)) {
		// This lock discipline never nests on one thread, so state
		// already in flight at a fresh entry can only be stale.
		//
		// Left by an earlier call on this tid that never reached its
		// unlock — most likely because that thread died mid-call and
		// the tid was reused. Counted here; cleared one way or
		// another before this function returns, so the old call's
		// wait, hold, and site can never attach to this acquisition.
		struct cp_lock_drop_counters *drops = cp_lock_drops_ptr();
		if (drops) {
			__sync_fetch_and_add(&drops->stale_state, 1);
		}
	}

	__u64 ret_addr;
	if (!cp_lock_read_ret_addr(ctx, &ret_addr)) {
		struct cp_lock_drop_counters *drops = cp_lock_drops_ptr();
		if (drops) {
			__sync_fetch_and_add(&drops->ret_read_fail, 1);
		}
		// Any state already held for this tid is cleared here too,
		// since this entry never goes on to record its own below.
		bpf_map_delete_elem(&cp_lock_state_map, &pid_tgid);
		return 0;
	}

	struct cp_lock_state state = {
		.start_ns = bpf_ktime_get_ns(),
		.ret = ret_addr,
	};
	// Replaces whatever was already recorded for this tid, so this
	// acquisition's own record can never carry over a stale one.
	if (bpf_map_update_elem(
		    &cp_lock_state_map, &pid_tgid, &state, BPF_ANY
	    ) != 0) {
		bpf_map_delete_elem(&cp_lock_state_map, &pid_tgid);
		struct cp_lock_drop_counters *drops = cp_lock_drops_ptr();
		if (drops) {
			__sync_fetch_and_add(&drops->state_insert_fail, 1);
		}
	}
	return 0;
}

SEC("uprobe/cp_config_lock")
int
cp_lock_on_lock_entry(struct pt_regs *ctx) {
	return cp_lock_entry(ctx, CP_LOCK_TARGET_LOCK);
}

SEC("uprobe/cp_config_try_lock")
int
cp_lock_on_try_lock_entry(struct pt_regs *ctx) {
	return cp_lock_entry(ctx, CP_LOCK_TARGET_TRY_LOCK);
}

// cp_config_lock blocks until it owns the lock, so its return always
// marks a successful acquisition.
SEC("uretprobe/cp_config_lock")
int
cp_lock_on_lock_return(struct pt_regs *ctx) {
	(void)ctx;
	__u64 pid_tgid = bpf_get_current_pid_tgid();
	struct cp_lock_state *state =
		bpf_map_lookup_elem(&cp_lock_state_map, &pid_tgid);
	if (!state) {
		return 0;
	}
	state->acquired_ns = bpf_ktime_get_ns();
	return 0;
}

// cp_config_try_lock never blocks; its return value decides whether
// this was an acquisition or a failed attempt.
//
// Only the low 8 bits are significant: it returns bool, and the rest
// of the register is not guaranteed to be clear.
SEC("uretprobe/cp_config_try_lock")
int
cp_lock_on_try_lock_return(struct pt_regs *ctx) {
	__u64 pid_tgid = bpf_get_current_pid_tgid();
	struct cp_lock_state *state =
		bpf_map_lookup_elem(&cp_lock_state_map, &pid_tgid);
	if (!state) {
		return 0;
	}

	__u8 acquired = (__u8)PT_REGS_RC(ctx);
	if (!acquired) {
		// Failed: no unlock will follow, so the site is credited the
		// failure right here instead of at unlock return.
		__u64 tgid = pid_tgid >> 32;
		struct cp_lock_site_stats *stats =
			cp_lock_site(tgid, state->ret);
		if (stats) {
			__sync_fetch_and_add(&stats->fail_count, 1);
		}
		bpf_map_delete_elem(&cp_lock_state_map, &pid_tgid);
		return 0;
	}

	state->acquired_ns = bpf_ktime_get_ns();
	return 0;
}

// Hold ends here, at unlock entry, not at unlock return.
//
// The native release happens inside the function body, so entry is
// the closest this probe pair gets to the moment the critical
// section's work finished.
SEC("uprobe/cp_config_unlock")
int
cp_lock_on_unlock_entry(struct pt_regs *ctx) {
	__u64 pid_tgid = bpf_get_current_pid_tgid();
	// Belt and suspenders: a process whose very first probed call was
	// unlock still gets its load address recorded here.
	//
	// Should not happen given the lock discipline this tool targets,
	// but costs nothing to cover.
	cp_lock_record_bias(
		pid_tgid >> 32, PT_REGS_IP(ctx), CP_LOCK_TARGET_UNLOCK
	);
	struct cp_lock_state *state =
		bpf_map_lookup_elem(&cp_lock_state_map, &pid_tgid);
	if (!state) {
		// Counted here, not at unlock return too: both probes fire
		// for the same call, on the same thread.
		//
		// Nothing else can delete this thread's state in between,
		// so a second check at return would double-count the same
		// event.
		struct cp_lock_drop_counters *drops = cp_lock_drops_ptr();
		if (drops) {
			__sync_fetch_and_add(&drops->unlock_no_state, 1);
		}
		return 0;
	}
	state->hold_ns = bpf_ktime_get_ns() - state->acquired_ns;
	return 0;
}

// Aggregation happens at unlock return, strictly after the internal
// release, so it never extends the next waiter's measured wait.
SEC("uretprobe/cp_config_unlock")
int
cp_lock_on_unlock_return(struct pt_regs *ctx) {
	(void)ctx;
	__u64 pid_tgid = bpf_get_current_pid_tgid();
	struct cp_lock_state *state =
		bpf_map_lookup_elem(&cp_lock_state_map, &pid_tgid);
	if (!state) {
		return 0;
	}

	// The acquiring timestamp never got set, or is somehow earlier
	// than the call's own start.
	//
	// Folding either into a site's stats would show a nonsensical
	// wait or hold duration, so the sample is dropped and counted
	// instead.
	if (state->acquired_ns == 0 || state->acquired_ns < state->start_ns) {
		struct cp_lock_drop_counters *drops = cp_lock_drops_ptr();
		if (drops) {
			__sync_fetch_and_add(&drops->bad_timestamp, 1);
		}
		bpf_map_delete_elem(&cp_lock_state_map, &pid_tgid);
		return 0;
	}

	__u64 tgid = pid_tgid >> 32;
	struct cp_lock_site_stats *stats = cp_lock_site(tgid, state->ret);
	if (stats) {
		__u64 wait_ns = state->acquired_ns - state->start_ns;
		__u64 hold_ns = state->hold_ns;

		__sync_fetch_and_add(&stats->count, 1);
		__sync_fetch_and_add(&stats->wait_sum_ns, wait_ns);
		__sync_fetch_and_add(&stats->hold_sum_ns, hold_ns);
		cp_lock_raise_max(&stats->wait_max_ns, wait_ns);
		cp_lock_raise_max(&stats->hold_max_ns, hold_ns);

		__u32 bucket = cp_lock_log2_64(hold_ns);
		if (bucket >= CP_LOCK_HIST_BUCKETS) {
			bucket = CP_LOCK_HIST_BUCKETS - 1;
		}
		__sync_fetch_and_add(&stats->hist[bucket], 1);
	}

	bpf_map_delete_elem(&cp_lock_state_map, &pid_tgid);
	return 0;
}
