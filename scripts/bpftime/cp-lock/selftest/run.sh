#!/usr/bin/env bash
# Self-check for the cp-lock bpftime instrumentation.
#
# Runs the synthetic workload through start mode, attach mode,
# exiting before the first snapshot, two processes at once, each
# snapshot trigger, a stripped target (via --debug-file, build-id
# lookup, and neither), a full site table, and the collector's
# refuse-if-exists guard — asserting known call sites, counts,
# hold/wait times and fail counts, and exiting non-zero naming the
# first failed assertion. Every leg uses bpftime's default
# shared-memory name, since `bpftime attach` reads
# BPFTIME_GLOBAL_SHM_NAME from the target's own environment rather
# than from the attach command, so a target started without it (the
# common case for a real control-plane process) only ever reaches a
# collector using the default; legs run strictly one at a time, since
# they all share that one name.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1

COLLECTOR="$ROOT/collector"
WORKLOAD="$ROOT/selftest/workload"
OVERFLOW_BPF_OBJECT="$ROOT/selftest/cp_lock_overflow_test.bpf.o"
READY_TIMEOUT_SEC=10
WORKLOAD_DONE_TIMEOUT_SEC=15
SNAPSHOT_WAIT_TIMEOUT_SEC=5
PID_WAIT_TIMEOUT_SEC=10
ATTACH_TIMEOUT_SEC=30
LOAD_TIMEOUT_SEC=10

CALLER_A_COUNT=1000
CALLER_B_COUNT=500
TRY_LOCK_FAILS=20
TRY_LOCK_SUCCESS_COUNT=50
MIN_AVG_HOLD_NS=100000 # HOLD_USEC in workload.c, in nanoseconds.
# CONTENDED_WAIT_HOLD_USEC in workload.c, in nanoseconds.
CONTENDED_WAIT_HOLD_NS=150000000
# Tolerance for the scheduling delay between the holder recording its
# own hold start and the waiter noticing it.
CONTENDED_WAIT_SLACK_NS=50000000

# bpftime's own defaults for the shared memory segment every leg here
# uses, matching what the collector itself checks for.
BPFTIME_SHM_PATH="/dev/shm/bpftime_maps_shm"
BPFTIME_LOCK_PATH="/tmp/bpftime-shm-bpftime_maps_shm.lock"

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

require_tool() {
	command -v "$1" >/dev/null 2>&1 || fail "$1 not found in PATH"
}

require_tool bpftime
require_tool strip
require_tool objcopy
require_tool readelf
require_tool timeout
HAVE_JQ=0
command -v jq >/dev/null 2>&1 && HAVE_JQ=1
if [ "$HAVE_JQ" -eq 0 ]; then
	require_tool python3
fi
[ -x "$COLLECTOR" ] || fail "$COLLECTOR missing; run make first"
[ -x "$WORKLOAD" ] || fail "$WORKLOAD missing; run make selftest first"
[ -f "$OVERFLOW_BPF_OBJECT" ] ||
	fail "$OVERFLOW_BPF_OBJECT missing; run make selftest first"

# Refuses to run against a shm some other session owns.
#
# The same rule the collector itself enforces: never clean up
# something this script did not create.
if [ -e "$BPFTIME_SHM_PATH" ]; then
	fail "$BPFTIME_SHM_PATH already exists; it belongs to another" \
		"bpftime session or was left behind after a crash. Remove it" \
		"first with \`bpftimetool remove\`, then \`rm $BPFTIME_LOCK_PATH\`."
fi

WORK_DIR="$(mktemp -d /tmp/cp-lock-selftest.XXXXXX)"
CLEANUP_PIDS=()
# Set once a leg's own collector has reported ready, so the only
# segment ever removed is one this script is sure it created.
#
# Cleared again as soon as that segment is actually removed. Without
# that reset, a segment some other session creates between two legs
# would otherwise look like this script's own and get deleted by the
# next leg's teardown or by the EXIT trap.
CREATED_SHM=0

# Removes the shared default-name shm the most recently readied
# collector used.
#
# Only if CREATED_SHM says this script actually created one.
remove_shm() {
	[ "$CREATED_SHM" -eq 1 ] || return 0
	rm -f "$BPFTIME_SHM_PATH" "$BPFTIME_LOCK_PATH"
	CREATED_SHM=0
}

# Sends the given pid a signal and waits for it to actually exit.
#
# Polls with kill -0, since several of these pids are not this
# shell's own children, escalating to SIGKILL if not gone by a short
# deadline. Never fails the leg: by the time this runs, the leg's own
# assertions have already decided pass or fail, so this only has to
# keep teardown from hanging the rest of the suite. A no-op on a pid
# this script already reaped, which narrows, but does not close, the
# window for signaling an unrelated process that reused the number:
# a pid that exited on its own and was never waited for is still
# signaled at exit.
declare -A REAPED_PIDS=()
stop_pid() {
	local pid="$1"
	[ -n "$pid" ] || return 0
	[ -n "${REAPED_PIDS[$pid]:-}" ] && return 0
	kill "$pid" >/dev/null 2>&1 || true
	local deadline=$((SECONDS + 5))
	while kill -0 "$pid" 2>/dev/null; do
		if [ "$SECONDS" -ge "$deadline" ]; then
			kill -9 "$pid" >/dev/null 2>&1 || true
			break
		fi
		sleep 0.1
	done
	wait "$pid" 2>/dev/null || true
	REAPED_PIDS[$pid]=1
}

cleanup() {
	for pid in "${CLEANUP_PIDS[@]:-}"; do
		stop_pid "$pid"
	done
	remove_shm
	rm -rf "$WORK_DIR"
}
trap cleanup EXIT

# Waits for the given pid to exit, bounded by a deadline so a stuck
# process fails this leg instead of hanging the whole suite.
#
# Returns its real exit status once it does. Only meaningful for a
# direct child of this shell; a pid that is not one — e.g. the
# collector's own pid under `bpftime load`'s wrapper — needs the
# signal-based waiter above instead.
wait_for_pid() {
	local pid="$1" timeout_sec="$2" label="$3"
	local deadline=$((SECONDS + timeout_sec))
	while kill -0 "$pid" 2>/dev/null; do
		[ "$SECONDS" -lt "$deadline" ] ||
			fail "$label: pid $pid did not exit within ${timeout_sec}s"
		sleep 0.1
	done
	local status=0
	wait "$pid" 2>/dev/null || status=$?
	REAPED_PIDS[$pid]=1
	return "$status"
}

# Waits for the collector's readiness line in its stderr log, so a mode
# never attaches to or starts the workload before the uprobes are live.
wait_for_collector_ready() {
	local log="$1"
	local deadline=$((SECONDS + READY_TIMEOUT_SEC))
	while [ "$SECONDS" -lt "$deadline" ]; do
		grep -q "cp-lock collector ready" "$log" 2>/dev/null && return 0
		sleep 0.1
	done
	return 1
}

# Waits for the workload to print its done marker to the given log.
wait_for_workload_done() {
	local log="$1"
	local deadline=$((SECONDS + WORKLOAD_DONE_TIMEOUT_SEC))
	while [ "$SECONDS" -lt "$deadline" ]; do
		grep -q "^done$" "$log" 2>/dev/null && return 0
		sleep 0.1
	done
	return 1
}

# The given path's inode number, or empty if it does not exist yet.
snapshot_inode() {
	stat -c '%i' "$1" 2>/dev/null
}

# Polls until the snapshot at the given path becomes a new file,
# different from the one before the triggering write.
#
# The collector always renames a freshly written temp file over the
# final path, so the final path's inode changes on every write — a
# check independent of mtime's coarser, clock-resolution-dependent
# granularity, which a fast-running leg could otherwise race. Bounded
# by a deadline so a stuck collector fails the leg instead of
# hanging.
wait_for_new_snapshot() {
	local path="$1" before="$2"
	local deadline=$((SECONDS + SNAPSHOT_WAIT_TIMEOUT_SEC))
	while [ "$SECONDS" -lt "$deadline" ]; do
		if [ -s "$path" ]; then
			local now
			now=$(snapshot_inode "$path")
			[ -n "$now" ] && [ "$now" != "$before" ] && return 0
		fi
		sleep 0.05
	done
	return 1
}

# Starts the collector against the given binary and waits for it to
# become ready.
#
# Leaves its own pid in LAST_COLLECTOR_PID and the wrapping `bpftime
# load` process's pid in LAST_LOADER_PID — killing only the wrapper
# leaves the collector running. Both are added to CLEANUP_PIDS so a
# later failure in the same leg does not leak them. env_prefix, when
# non-empty, is forwarded as literal `VAR=value` text that `env`
# applies only to this collector process — used by the build-id leg
# to set CP_LOCK_DEBUG_ROOT without a raw, duplicated `bpftime load`
# invocation of its own.
start_collector() {
	local env_prefix="$1" label="$2" binary="$3" out_prefix="$4" log="$5"
	shift 5
	local -a env_args=()
	[ -n "$env_prefix" ] && env_args=("$env_prefix")
	env "${env_args[@]}" bpftime load "$COLLECTOR" --binary "$binary" \
		--out "$out_prefix" --interval 3600 "$@" >"$log" 2>&1 &
	LAST_LOADER_PID=$!
	CLEANUP_PIDS+=("$LAST_LOADER_PID")

	wait_for_collector_ready "$log" ||
		fail "$label: collector did not become ready; see $log"
	# Only now, with this collector confirmed to actually own the shm,
	# is it safe to let cleanup remove it.
	CREATED_SHM=1
	LAST_COLLECTOR_PID=$(grep -o 'pid=[0-9]*' "$log" | head -1 | cut -d= -f2)
	[ -n "$LAST_COLLECTOR_PID" ] || fail "$label: could not read collector pid"
	CLEANUP_PIDS+=("$LAST_COLLECTOR_PID")
}

# Tears down the most recently started collector, and clears the
# default shm it used.
#
# So the next leg's own refuse-if-exists check sees a clean slate
# instead of this leg's now-stale segment.
stop_collector() {
	stop_pid "$LAST_COLLECTOR_PID"
	stop_pid "$LAST_LOADER_PID"
	remove_shm
}

# Reads one top-level field out of a snapshot's JSON file, via jq if
# installed, else python3 — both are reasonable to assume present.
json_field() {
	local json="$1" field="$2"
	if [ "$HAVE_JQ" -eq 1 ]; then
		jq -r ".$field" "$json"
	else
		python3 -c \
			"import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])" \
			"$json" "$field"
	fi
}

# Reads one field of the JSON row whose "site" starts with the given prefix.
#
# Ties a JSON row back to the same symbol a .txt row was grepped
# for, so the two formats can be checked against each other.
json_site_field() {
	local json="$1" site_prefix="$2" field="$3"
	if [ "$HAVE_JQ" -eq 1 ]; then
		jq -r --arg p "$site_prefix" --arg f "$field" \
			'[.sites[] | select(.site | startswith($p))][0][$f]' \
			"$json"
	else
		python3 -c '
import json, sys
data = json.load(open(sys.argv[1]))
prefix, field = sys.argv[2], sys.argv[3]
for row in data["sites"]:
	if row["site"].startswith(prefix):
		print(row[field])
		break
' "$json" "$site_prefix" "$field"
	fi
}

# The caller_a row's histogram, reduced to two numbers.
#
# The sum of every bucket below floor(log2(HOLD_USEC in ns)) = 16
# (should be 0, since nothing at this site holds for less than
# HOLD_USEC), and the sum of the whole histogram (should equal the
# row's own count).
caller_a_hist_check_values() {
	local json="$1"
	if [ "$HAVE_JQ" -eq 1 ]; then
		jq -r '
			([.sites[] | select(.site | startswith("caller_a"))][0].hist) as $h
			| "\($h[0:16] | add) \($h | add)"
		' "$json"
	else
		python3 -c '
import json, sys
data = json.load(open(sys.argv[1]))
for row in data["sites"]:
	if row["site"].startswith("caller_a"):
		hist = row["hist"]
		print(sum(hist[0:16]), sum(hist))
		break
' "$json"
	fi
}

# Fails unless the JSON snapshot next to the given .txt path parses
# cleanly.
#
# A snapshot ever observed mid-write would fail this the same way a
# genuinely corrupt one would, so calling this on every snapshot this
# script reads checks one is never half-written.
check_json_valid() {
	local label="$1" txt="$2"
	local json="${txt%.txt}.json"
	[ -s "$json" ] || fail "$label: $json missing or empty"
	if [ "$HAVE_JQ" -eq 1 ]; then
		jq empty "$json" >/dev/null 2>&1 ||
			fail "$label: $json did not parse (jq)"
	else
		python3 -m json.tool "$json" >/dev/null 2>&1 ||
			fail "$label: $json did not parse (python3)"
	fi
}

# Fails unless the caller_a row's file:line annotation in the .txt
# snapshot names workload.c.
#
# That's the source file every row in this selftest should resolve
# back to, whether via the target's own debug info or a separate
# --debug-file/build-id image.
check_file_line() {
	local label="$1" snapshot="$2"
	local matched
	matched=$(grep -A1 -E '^caller_a(\+0x[0-9a-f]+)? ' "$snapshot") ||
		fail "$label: no caller_a row in $snapshot"
	local annotation
	annotation=$(tail -n1 <<<"$matched")
	case "$annotation" in
	*workload.c*) ;;
	*)
		fail "$label: caller_a row's file:line ($annotation) does not" \
			"name workload.c"
		;;
	esac
}

# Checks the snapshot for one mode.
#
# The two sequential callers at their exact counts and a hold average
# at or above the workload's known critical section, the try_lock
# caller with zero acquisitions and every attempt counted as a
# failure, an uncontended try_lock/unlock site with every attempt
# succeeding, and a contended cp_config_lock wait whose measured time
# tracks the other thread's known hold.
check_snapshot() {
	local label="$1"
	local snapshot="$2"
	local json="${snapshot%.txt}.json"

	[ -s "$snapshot" ] || fail "$label: $snapshot missing or empty"
	check_json_valid "$label" "$snapshot"

	local row
	row=$(grep -E '^caller_a(\+0x[0-9a-f]+)? ' "$snapshot") ||
		fail "$label: no caller_a row in $snapshot"
	local count hold_sum fail_count tgid
	count=$(awk '{print $3}' <<<"$row")
	tgid=$(awk '{print $2}' <<<"$row")
	hold_sum=$(awk '{print $6}' <<<"$row")
	fail_count=$(awk '{print $8}' <<<"$row")
	[ "$count" = "$CALLER_A_COUNT" ] ||
		fail "$label: caller_a count=$count want=$CALLER_A_COUNT"
	[ "$fail_count" = "0" ] ||
		fail "$label: caller_a fail_count=$fail_count want=0"
	[ "$((hold_sum / count))" -ge "$MIN_AVG_HOLD_NS" ] ||
		fail "$label: caller_a avg hold $((hold_sum / count))ns" \
			"below ${MIN_AVG_HOLD_NS}ns"

	local json_count json_fail json_tgid
	json_count=$(json_site_field "$json" "caller_a" count)
	json_fail=$(json_site_field "$json" "caller_a" fail_count)
	json_tgid=$(json_site_field "$json" "caller_a" tgid)
	[ "$json_count" = "$count" ] ||
		fail "$label: caller_a JSON count=$json_count disagrees with" \
			"txt count=$count"
	[ "$json_fail" = "$fail_count" ] ||
		fail "$label: caller_a JSON fail_count=$json_fail disagrees" \
			"with txt fail_count=$fail_count"
	[ "$json_tgid" = "$tgid" ] ||
		fail "$label: caller_a JSON tgid=$json_tgid disagrees with txt" \
			"tgid=$tgid"

	local below total
	read -r below total < <(caller_a_hist_check_values "$json")
	[ "$below" = "0" ] ||
		fail "$label: caller_a hist buckets below 16 sum to $below, want 0"
	[ "$total" = "$count" ] ||
		fail "$label: caller_a hist total=$total disagrees with count=$count"

	row=$(grep -E '^caller_b(\+0x[0-9a-f]+)? ' "$snapshot") ||
		fail "$label: no caller_b row in $snapshot"
	count=$(awk '{print $3}' <<<"$row")
	hold_sum=$(awk '{print $6}' <<<"$row")
	fail_count=$(awk '{print $8}' <<<"$row")
	[ "$count" = "$CALLER_B_COUNT" ] ||
		fail "$label: caller_b count=$count want=$CALLER_B_COUNT"
	[ "$fail_count" = "0" ] ||
		fail "$label: caller_b fail_count=$fail_count want=0"
	[ "$((hold_sum / count))" -ge "$MIN_AVG_HOLD_NS" ] ||
		fail "$label: caller_b avg hold $((hold_sum / count))ns" \
			"below ${MIN_AVG_HOLD_NS}ns"

	row=$(grep -E '^caller_try_lock(\+0x[0-9a-f]+)? ' "$snapshot") ||
		fail "$label: no caller_try_lock row in $snapshot"
	count=$(awk '{print $3}' <<<"$row")
	fail_count=$(awk '{print $8}' <<<"$row")
	[ "$count" = "0" ] ||
		fail "$label: caller_try_lock count=$count want=0 (lock was held" \
			"for every attempt)"
	[ "$fail_count" = "$TRY_LOCK_FAILS" ] ||
		fail "$label: caller_try_lock fail_count=$fail_count" \
			"want=$TRY_LOCK_FAILS"

	row=$(grep -E '^caller_try_lock_success(\+0x[0-9a-f]+)? ' "$snapshot") ||
		fail "$label: no caller_try_lock_success row in $snapshot"
	count=$(awk '{print $3}' <<<"$row")
	hold_sum=$(awk '{print $6}' <<<"$row")
	fail_count=$(awk '{print $8}' <<<"$row")
	[ "$count" = "$TRY_LOCK_SUCCESS_COUNT" ] ||
		fail "$label: caller_try_lock_success count=$count" \
			"want=$TRY_LOCK_SUCCESS_COUNT"
	[ "$fail_count" = "0" ] ||
		fail "$label: caller_try_lock_success fail_count=$fail_count want=0"
	[ "$((hold_sum / count))" -ge "$MIN_AVG_HOLD_NS" ] ||
		fail "$label: caller_try_lock_success avg hold" \
			"$((hold_sum / count))ns below ${MIN_AVG_HOLD_NS}ns"

	row=$(grep -E '^caller_contended_wait(\+0x[0-9a-f]+)? ' "$snapshot") ||
		fail "$label: no caller_contended_wait row in $snapshot"
	local wait_sum wait_max
	wait_sum=$(awk '{print $4}' <<<"$row")
	wait_max=$(awk '{print $5}' <<<"$row")
	[ "$wait_sum" -gt 0 ] ||
		fail "$label: caller_contended_wait wait_sum_ns=$wait_sum want > 0"
	local min_wait=$((CONTENDED_WAIT_HOLD_NS - CONTENDED_WAIT_SLACK_NS))
	[ "$wait_max" -ge "$min_wait" ] ||
		fail "$label: caller_contended_wait wait_max_ns=$wait_max below" \
			"$min_wait (the other thread's known hold time, minus slack)"

	# hist_total = count for every row: the histogram and the running
	# count must never disagree, successes or failures alike.
	while read -r hist_count hist_total; do
		[ "$hist_total" = "$hist_count" ] ||
			fail "$label: a row's hist_total=$hist_total" \
				"disagrees with its count=$hist_count"
	done < <(awk 'NF == 9 && $3 ~ /^[0-9]+$/ {print $3, $9}' "$snapshot")

	echo "ok: $label"
}

run_start_mode() {
	local out_prefix="$WORK_DIR/start"
	local collector_log="$WORK_DIR/start-collector.log"
	start_collector "" "start mode" "$WORKLOAD" "$out_prefix" "$collector_log"

	local workload_log="$WORK_DIR/start-workload.log"
	bpftime start "$WORKLOAD" >"$workload_log" 2>&1 &
	local start_pid=$!
	CLEANUP_PIDS+=("$start_pid")

	# Snapshot while the workload is still in its post-run grace sleep,
	# so this leg covers a live target; the exited-target case has a
	# leg of its own.
	wait_for_workload_done "$workload_log" ||
		fail "start mode: workload did not finish; see $workload_log"
	local before
	before=$(snapshot_inode "$out_prefix.txt")
	kill -USR1 "$LAST_COLLECTOR_PID" 2>/dev/null ||
		fail "start mode: collector (pid $LAST_COLLECTOR_PID) is gone"
	wait_for_new_snapshot "$out_prefix.txt" "$before" ||
		fail "start mode: no new snapshot after SIGUSR1"

	check_snapshot "start mode" "$out_prefix.txt"
	check_file_line "start mode" "$out_prefix.txt"

	wait_for_pid "$start_pid" "$PID_WAIT_TIMEOUT_SEC" "start mode" ||
		fail "start mode: bpftime start exited non-zero"

	stop_collector
}

run_attach_mode() {
	local out_prefix="$WORK_DIR/attach"
	local collector_log="$WORK_DIR/attach-collector.log"
	start_collector "" "attach mode" "$WORKLOAD" "$out_prefix" "$collector_log"

	local workload_log="$WORK_DIR/attach-workload.log"
	# Explicitly unset, not merely left unexported.
	#
	# This reproduces a real control-plane process, which is never
	# started with this variable set. `bpftime attach` reads it from
	# the target's own environment, not from the attach command, so
	# this target must rely on the default the same way the collector
	# above does.
	env -u BPFTIME_GLOBAL_SHM_NAME \
		"$WORKLOAD" --wait-for-signal >"$workload_log" 2>&1 &
	local workload_shell_pid=$!
	CLEANUP_PIDS+=("$workload_shell_pid")

	local deadline=$((SECONDS + READY_TIMEOUT_SEC))
	local workload_pid=""
	while [ "$SECONDS" -lt "$deadline" ] && [ -z "$workload_pid" ]; do
		workload_pid=$(grep -o 'pid [0-9]*' "$workload_log" 2>/dev/null |
			head -1 | awk '{print $2}')
		[ -n "$workload_pid" ] || sleep 0.1
	done
	[ -n "$workload_pid" ] || fail "attach mode: workload never printed its pid"

	local attach_start attach_end
	attach_start=$(date +%s.%N)
	timeout "$ATTACH_TIMEOUT_SEC" bpftime attach "$workload_pid" ||
		fail "attach mode: bpftime attach $workload_pid failed (see" \
			"README prerequisites: ptrace_scope must allow a" \
			"non-child ptrace, e.g. CAP_SYS_PTRACE or root)"
	attach_end=$(date +%s.%N)
	echo "attach mode: injection took $(awk "BEGIN{printf \"%.3f\", $attach_end - $attach_start}")s"

	kill -USR1 "$workload_pid" || fail "attach mode: workload (pid $workload_pid) is gone"

	wait_for_workload_done "$workload_log" ||
		fail "attach mode: workload did not finish; see $workload_log"

	local before
	before=$(snapshot_inode "$out_prefix.txt")
	kill -USR1 "$LAST_COLLECTOR_PID" 2>/dev/null ||
		fail "attach mode: collector (pid $LAST_COLLECTOR_PID) is gone"
	wait_for_new_snapshot "$out_prefix.txt" "$before" ||
		fail "attach mode: no new snapshot after SIGUSR1"

	check_snapshot "attach mode" "$out_prefix.txt"

	# The attached workload is still in its post-run grace sleep here,
	# with the injected agent alive.
	#
	# Wait for it to actually exit before tearing down the collector
	# and its shm: removing that segment out from under a live agent
	# is undefined.
	wait_for_pid "$workload_shell_pid" "$PID_WAIT_TIMEOUT_SEC" "attach mode" ||
		fail "attach mode: workload exited non-zero"

	stop_collector
}

# Verifies symbolization when the first snapshot request arrives
# after the instrumented process has already exited.
#
# The start-mode leg above avoids this by snapshotting during the
# workload's post-run grace sleep; this leg instead runs `bpftime
# start` to full, synchronous completion first, so the process is
# provably gone before any snapshot is requested. Rows must still
# carry their symbol names, not just raw addresses.
run_exited_before_snapshot_mode() {
	local out_prefix="$WORK_DIR/exited"
	local collector_log="$WORK_DIR/exited-collector.log"
	start_collector "" "exited-before-snapshot mode" "$WORKLOAD" \
		"$out_prefix" "$collector_log"

	timeout "$WORKLOAD_DONE_TIMEOUT_SEC" bpftime start "$WORKLOAD" \
		>"$WORK_DIR/exited-workload.log" 2>&1 ||
		fail "exited-before-snapshot mode: bpftime start exited non-zero"

	local before
	before=$(snapshot_inode "$out_prefix.txt")
	kill -USR1 "$LAST_COLLECTOR_PID" 2>/dev/null ||
		fail "exited-before-snapshot mode: collector (pid" \
			"$LAST_COLLECTOR_PID) is gone"
	wait_for_new_snapshot "$out_prefix.txt" "$before" ||
		fail "exited-before-snapshot mode: no new snapshot after SIGUSR1"

	check_snapshot "exited-before-snapshot mode" "$out_prefix.txt"

	stop_collector
}

# Two instrumented processes produce one snapshot, with rows
# attributed to both tgids and both pids listed.
run_multi_process_mode() {
	local out_prefix="$WORK_DIR/multi"
	local collector_log="$WORK_DIR/multi-collector.log"
	start_collector "" \
		"multi-process mode" "$WORKLOAD" "$out_prefix" "$collector_log"

	local log_a="$WORK_DIR/multi-a.log" log_b="$WORK_DIR/multi-b.log"
	bpftime start "$WORKLOAD" >"$log_a" 2>&1 &
	local pid_a=$!
	CLEANUP_PIDS+=("$pid_a")
	bpftime start "$WORKLOAD" >"$log_b" 2>&1 &
	local pid_b=$!
	CLEANUP_PIDS+=("$pid_b")

	wait_for_workload_done "$log_a" ||
		fail "multi-process mode: first workload did not finish; see $log_a"
	wait_for_workload_done "$log_b" ||
		fail "multi-process mode: second workload did not finish; see $log_b"

	local before
	before=$(snapshot_inode "$out_prefix.txt")
	kill -USR1 "$LAST_COLLECTOR_PID" 2>/dev/null ||
		fail "multi-process mode: collector (pid $LAST_COLLECTOR_PID) is gone"
	wait_for_new_snapshot "$out_prefix.txt" "$before" ||
		fail "multi-process mode: no new snapshot after SIGUSR1"

	wait_for_pid "$pid_a" "$PID_WAIT_TIMEOUT_SEC" "multi-process mode" ||
		fail "multi-process mode: first bpftime start exited non-zero"
	wait_for_pid "$pid_b" "$PID_WAIT_TIMEOUT_SEC" "multi-process mode" ||
		fail "multi-process mode: second bpftime start exited non-zero"

	local snapshot="$out_prefix.txt"
	[ -s "$snapshot" ] || fail "multi-process mode: $snapshot missing or empty"
	check_json_valid "multi-process mode" "$snapshot"

	local pids_line
	pids_line=$(grep '^pids:' "$snapshot") ||
		fail "multi-process mode: no pids: line in $snapshot"
	local pid_count
	pid_count=$(awk '{print NF - 1}' <<<"$pids_line")
	[ "$pid_count" = "2" ] ||
		fail "multi-process mode: pids: line lists $pid_count pids," \
			"want 2 ($pids_line)"

	local tgid row count
	while read -r tgid; do
		row=$(awk -v p="$tgid" '$1 ~ /^caller_a/ && $2 == p' "$snapshot")
		[ -n "$row" ] ||
			fail "multi-process mode: no caller_a row for pid $tgid"
		count=$(awk '{print $3}' <<<"$row")
		[ "$count" = "$CALLER_A_COUNT" ] ||
			fail "multi-process mode: pid $tgid caller_a count=$count" \
				"want=$CALLER_A_COUNT"

		row=$(awk -v p="$tgid" '$1 ~ /^caller_b/ && $2 == p' "$snapshot")
		[ -n "$row" ] ||
			fail "multi-process mode: no caller_b row for pid $tgid"
		count=$(awk '{print $3}' <<<"$row")
		[ "$count" = "$CALLER_B_COUNT" ] ||
			fail "multi-process mode: pid $tgid caller_b count=$count" \
				"want=$CALLER_B_COUNT"
	done < <(awk '{for (i=2;i<=NF;i++) print $i}' <<<"$pids_line")

	echo "ok: multi-process mode"
	stop_collector
}

# A short --interval produces a snapshot on its own, with no signal
# ever sent.
run_snapshot_interval_mode() {
	local out_prefix="$WORK_DIR/interval"
	local collector_log="$WORK_DIR/interval-collector.log"
	local before
	before=$(snapshot_inode "$out_prefix.txt")
	start_collector "" \
		"snapshot-interval mode" "$WORKLOAD" "$out_prefix" "$collector_log" \
		--interval 1

	local workload_log="$WORK_DIR/interval-workload.log"
	bpftime start "$WORKLOAD" >"$workload_log" 2>&1 &
	local start_pid=$!
	CLEANUP_PIDS+=("$start_pid")
	wait_for_workload_done "$workload_log" ||
		fail "snapshot-interval mode: workload did not finish; see $workload_log"
	wait_for_pid "$start_pid" "$PID_WAIT_TIMEOUT_SEC" "snapshot-interval mode" ||
		fail "snapshot-interval mode: bpftime start exited non-zero"

	# No signal sent here on purpose: the point under test is that
	# --interval alone produces a snapshot.
	wait_for_new_snapshot "$out_prefix.txt" "$before" ||
		fail "snapshot-interval mode: $out_prefix.txt never appeared"

	check_snapshot "snapshot-interval mode" "$out_prefix.txt"
	stop_collector
}

# SIGINT and SIGTERM (the signal name is the only argument) each write
# a final, complete snapshot before the collector exits.
run_snapshot_signal_exit_mode() {
	local signal="$1"
	local out_prefix="$WORK_DIR/exit-$signal"
	local collector_log="$WORK_DIR/exit-$signal-collector.log"
	start_collector "" \
		"snapshot-$signal mode" "$WORKLOAD" "$out_prefix" "$collector_log"

	local workload_log="$WORK_DIR/exit-$signal-workload.log"
	bpftime start "$WORKLOAD" >"$workload_log" 2>&1 &
	local start_pid=$!
	CLEANUP_PIDS+=("$start_pid")
	wait_for_workload_done "$workload_log" ||
		fail "snapshot-$signal mode: workload did not finish; see $workload_log"
	wait_for_pid "$start_pid" "$PID_WAIT_TIMEOUT_SEC" "snapshot-$signal mode" ||
		fail "snapshot-$signal mode: bpftime start exited non-zero"

	kill -s "$signal" "$LAST_COLLECTOR_PID" 2>/dev/null ||
		fail "snapshot-$signal mode: collector (pid $LAST_COLLECTOR_PID) is gone"
	# LAST_LOADER_PID, not LAST_COLLECTOR_PID: only a direct child's
	# exit status is available to `wait`.
	#
	# `bpftime load` is a supervising wrapper around the collector,
	# exiting with its status once it does.
	local collector_rc
	wait_for_pid "$LAST_LOADER_PID" "$PID_WAIT_TIMEOUT_SEC" "snapshot-$signal mode"
	collector_rc=$?
	[ "$collector_rc" -eq 0 ] ||
		fail "snapshot-$signal mode: collector exited $collector_rc after" \
			"SIG$signal, want 0"

	check_snapshot "snapshot-$signal mode" "$out_prefix.txt"

	remove_shm
}

# A stripped copy of the workload resolves named rows via an explicit
# --debug-file, and again via the build-id convention.
#
# Uses a test-only debug root (CP_LOCK_DEBUG_ROOT); the real
# /usr/lib/debug needs root to write into, which this script does
# not have.
run_stripped_target_mode() {
	local stripped="$WORK_DIR/workload-stripped"
	local debug_file="$WORK_DIR/workload.debug"
	cp "$WORKLOAD" "$stripped"
	objcopy --only-keep-debug "$stripped" "$debug_file" ||
		fail "stripped-target mode: objcopy --only-keep-debug failed"
	strip --strip-all "$stripped" ||
		fail "stripped-target mode: strip --strip-all failed"

	# (a) --debug-file
	local out_prefix="$WORK_DIR/stripped-a"
	local collector_log="$WORK_DIR/stripped-a-collector.log"
	start_collector "" "stripped-target mode (a)" "$stripped" "$out_prefix" \
		"$collector_log" --debug-file "$debug_file"

	local workload_log="$WORK_DIR/stripped-a-workload.log"
	bpftime start "$stripped" >"$workload_log" 2>&1 &
	local start_pid=$!
	CLEANUP_PIDS+=("$start_pid")
	wait_for_workload_done "$workload_log" ||
		fail "stripped-target mode (a): workload did not finish; see $workload_log"
	wait_for_pid "$start_pid" "$PID_WAIT_TIMEOUT_SEC" "stripped-target mode (a)" ||
		fail "stripped-target mode (a): bpftime start exited non-zero"

	local before
	before=$(snapshot_inode "$out_prefix.txt")
	kill -USR1 "$LAST_COLLECTOR_PID" 2>/dev/null ||
		fail "stripped-target mode (a): collector (pid $LAST_COLLECTOR_PID) is gone"
	wait_for_new_snapshot "$out_prefix.txt" "$before" ||
		fail "stripped-target mode (a): no new snapshot after SIGUSR1"
	check_snapshot "stripped-target mode (a: --debug-file)" "$out_prefix.txt"
	check_file_line "stripped-target mode (a: --debug-file)" "$out_prefix.txt"
	stop_collector

	# (b) build-id lookup, debug file placed under a temporary root
	local build_id
	build_id=$(readelf -n "$stripped" 2>/dev/null |
		awk '/Build ID:/ {print $NF}')
	[ -n "$build_id" ] ||
		fail "stripped-target mode (b): stripped binary has no build-id"
	local debug_root="$WORK_DIR/debug-root"
	local build_id_dir="$debug_root/.build-id/${build_id:0:2}"
	mkdir -p "$build_id_dir"
	cp "$debug_file" "$build_id_dir/${build_id:2}.debug"

	out_prefix="$WORK_DIR/stripped-b"
	collector_log="$WORK_DIR/stripped-b-collector.log"
	start_collector "CP_LOCK_DEBUG_ROOT=$debug_root" "stripped-target mode (b)" \
		"$stripped" "$out_prefix" "$collector_log"

	workload_log="$WORK_DIR/stripped-b-workload.log"
	bpftime start "$stripped" >"$workload_log" 2>&1 &
	start_pid=$!
	CLEANUP_PIDS+=("$start_pid")
	wait_for_workload_done "$workload_log" ||
		fail "stripped-target mode (b): workload did not finish; see $workload_log"
	wait_for_pid "$start_pid" "$PID_WAIT_TIMEOUT_SEC" "stripped-target mode (b)" ||
		fail "stripped-target mode (b): bpftime start exited non-zero"

	before=$(snapshot_inode "$out_prefix.txt")
	kill -USR1 "$LAST_COLLECTOR_PID" 2>/dev/null ||
		fail "stripped-target mode (b): collector (pid $LAST_COLLECTOR_PID) is gone"
	wait_for_new_snapshot "$out_prefix.txt" "$before" ||
		fail "stripped-target mode (b): no new snapshot after SIGUSR1"
	check_snapshot "stripped-target mode (b: build-id)" "$out_prefix.txt"
	stop_collector
}

# No .symtab and no debug file reachable by any route: the collector
# exits non-zero, naming the binary.
run_stripped_no_debug_mode() {
	local stripped="$WORK_DIR/workload-nodebug"
	cp "$WORKLOAD" "$stripped"
	strip --strip-all "$stripped" ||
		fail "stripped-no-debug mode: strip --strip-all failed"

	local log="$WORK_DIR/nodebug-collector.log"
	CP_LOCK_DEBUG_ROOT="$WORK_DIR/no-such-debug-root" \
		timeout "$LOAD_TIMEOUT_SEC" \
		bpftime load "$COLLECTOR" --binary "$stripped" \
		--out "$WORK_DIR/nodebug" --interval 3600 >"$log" 2>&1
	local rc=$?
	[ "$rc" -ne 0 ] ||
		fail "stripped-no-debug mode: collector did not refuse a stripped" \
			"binary with no debug file"
	grep -q "$stripped" "$log" ||
		fail "stripped-no-debug mode: error message did not name the" \
			"binary (see $log)"

	echo "ok: stripped-no-debug mode"
}

# A site table with no room for every call site one workload run makes
# still counts, rather than drops, the sites that did not fit.
run_table_full_mode() {
	local out_prefix="$WORK_DIR/overflow"
	local collector_log="$WORK_DIR/overflow-collector.log"
	start_collector "" "table-full mode" "$WORKLOAD" "$out_prefix" \
		"$collector_log" --bpf-object "$OVERFLOW_BPF_OBJECT"

	local workload_log="$WORK_DIR/overflow-workload.log"
	bpftime start "$WORKLOAD" >"$workload_log" 2>&1 &
	local start_pid=$!
	CLEANUP_PIDS+=("$start_pid")
	wait_for_workload_done "$workload_log" ||
		fail "table-full mode: workload did not finish; see $workload_log"
	wait_for_pid "$start_pid" "$PID_WAIT_TIMEOUT_SEC" "table-full mode" ||
		fail "table-full mode: bpftime start exited non-zero"

	local before
	before=$(snapshot_inode "$out_prefix.txt")
	kill -USR1 "$LAST_COLLECTOR_PID" 2>/dev/null ||
		fail "table-full mode: collector (pid $LAST_COLLECTOR_PID) is gone"
	wait_for_new_snapshot "$out_prefix.txt" "$before" ||
		fail "table-full mode: no new snapshot after SIGUSR1"

	local snapshot="$out_prefix.txt"
	[ -s "$snapshot" ] || fail "table-full mode: $snapshot missing or empty"
	check_json_valid "table-full mode" "$snapshot"

	local overflow
	overflow=$(awk -F': ' '/^overflow_events:/ {print $2}' "$snapshot")
	[ -n "$overflow" ] && [ "$overflow" -gt 0 ] ||
		fail "table-full mode: overflow_events=$overflow in $snapshot, want > 0"

	local json_overflow
	json_overflow=$(json_field "$out_prefix.json" overflow_events)
	[ "$json_overflow" = "$overflow" ] ||
		fail "table-full mode: overflow_events txt=$overflow json=$json_overflow" \
			"disagree"

	echo "ok: table-full mode"
	stop_collector
}

# Verifies a second collector refuses to start while the first's
# shm still exists, naming it.
#
# Leaves the first collector's own shm usable afterward.
run_refuse_if_exists_mode() {
	local out_prefix="$WORK_DIR/refuse1"
	local collector_log="$WORK_DIR/refuse1-collector.log"
	start_collector "" \
		"refuse-if-exists mode" "$WORKLOAD" "$out_prefix" "$collector_log"

	local second_log="$WORK_DIR/refuse2-collector.log"
	timeout "$LOAD_TIMEOUT_SEC" \
		bpftime load "$COLLECTOR" --binary "$WORKLOAD" --out "$WORK_DIR/refuse2" \
		--interval 3600 >"$second_log" 2>&1
	local second_rc=$?
	[ "$second_rc" -ne 0 ] ||
		fail "refuse-if-exists mode: a second collector did not refuse" \
			"the already-existing shm"
	grep -q "already exists" "$second_log" ||
		fail "refuse-if-exists mode: second collector's message did not" \
			"name the existing shm (see $second_log)"

	# The first collector's own shm must be untouched by the refused
	# attempt: it can still produce a fresh snapshot.
	local before
	before=$(snapshot_inode "$out_prefix.txt")
	kill -USR1 "$LAST_COLLECTOR_PID" ||
		fail "refuse-if-exists mode: first collector (pid" \
			"$LAST_COLLECTOR_PID) is gone"
	wait_for_new_snapshot "$out_prefix.txt" "$before" ||
		fail "refuse-if-exists mode: first collector's own snapshot" \
			"never appeared after the second collector's refused attempt"

	echo "ok: refuse-if-exists mode"
	stop_collector
}

run_start_mode
run_attach_mode
run_exited_before_snapshot_mode
run_multi_process_mode
run_snapshot_interval_mode
run_snapshot_signal_exit_mode INT
run_snapshot_signal_exit_mode TERM
run_stripped_target_mode
run_stripped_no_debug_mode
run_table_full_mode
run_refuse_if_exists_mode

echo "cp-lock selftest: all legs passed"
