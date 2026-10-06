#!/usr/bin/env bash
# BEFORE/AFTER capture-handler benchmark for the pdump ring rewrite.
#
# Builds pdump_ab_bench.c (a mirror of the real pdump_handle_packets and of
# the production Go reader) into two scratch trees: BEFORE at a chosen
# revision, with the module's old private per-worker ring_buffer, and
# AFTER at this checkout's HEAD, with the shared ring object the handler
# now captures into. Runs both across a matrix of packet size, ring
# capacity, filter and reader presence (sizes 64/256/1500/9000, ring 1 MiB
# and 8 MiB, reader off/on, the "ip" filter accepting everything or half
# the packets), pinned with taskset to one writer and one reader CPU, and
# also runs the in-tree `meson test --benchmark pdump_bench` of the AFTER
# tree on the same CPU pair. Everything lands in one report file. See
# AB_BENCH.md.
set -euo pipefail

BEFORE_REV_DEFAULT=a3c5fbdf389542781b6b80dd7c0dce0c9ae6043f

usage() {
	cat <<EOF
Usage: modules/pdump/tests/ab/pdump-ab.sh [options]

  --cpus "W,R"    writer,reader CPU pair for taskset (default: the first
                  two CPUs this process is allowed to run on)
  --launches N    independent process launches per matrix cell (default: 5)
  --reps N        timed repetitions per launch (default: 10)
  --quick         one launch, three repetitions, a smaller matrix
  --before REV    BEFORE revision (default: $BEFORE_REV_DEFAULT)
  --evict-chunks LIST
                  eviction chunk sweep instead of the handler matrix:
                  BEFORE and AFTER at each comma-separated chunk size in
                  bytes (e.g. "4096,16384,65536"), reader off/on, both
                  ring sizes, "all" filter; skips the in-tree benchmark
  --out FILE      report path (default: pdump-ab-<host>-<date>.txt in the
                  repository root)
  -h, --help      this help

BEFORE and this checkout's HEAD (AFTER) are each built in a detached git
worktree under .pdump-ab/ in the repository root; reruns reuse them.
EOF
}

CPUS=
LAUNCHES=5
REPS=10
QUICK=0
EVICT_CHUNKS=
BEFORE_REV=$BEFORE_REV_DEFAULT
OUT=

while (($#)); do
	case "$1" in
	--cpus)
		CPUS=$2
		shift
		;;
	--launches)
		LAUNCHES=$2
		shift
		;;
	--reps)
		REPS=$2
		shift
		;;
	--quick)
		QUICK=1
		;;
	--evict-chunks)
		EVICT_CHUNKS=$2
		shift
		;;
	--before)
		BEFORE_REV=$2
		shift
		;;
	--out)
		OUT=$2
		shift
		;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		echo "pdump-ab: unknown argument: $1" >&2
		usage >&2
		exit 2
		;;
	esac
	shift
done

if ((QUICK)); then
	LAUNCHES=1
	REPS=3
fi

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)
cd "$ROOT"

AB_DIR="$ROOT/modules/pdump/tests/ab"
WORK="$ROOT/.pdump-ab"
BEFORE_DIR="$WORK/before"
AFTER_DIR="$WORK/after"
AFTER_REV=$(git rev-parse HEAD)

HOST=$(hostname -s 2>/dev/null || hostname)
OUT=${OUT:-"$ROOT/pdump-ab-$HOST-$(date +%Y%m%d-%H%M%S).txt"}

# The first two CPUs this process is allowed to run on, from
# /proc/self/status's Cpus_allowed_list (e.g. "0-3,8"), so a default run
# never pins to a CPU a container or cpuset has taken away.
default_cpu_pair() {
	local list nums=()
	list=$(sed -n 's/^Cpus_allowed_list:[[:space:]]*//p' /proc/self/status)
	local part
	for part in ${list//,/ }; do
		if [[ $part == *-* ]]; then
			local lo=${part%-*} hi=${part#*-}
			local i
			for ((i = lo; i <= hi; i++)); do
				nums+=("$i")
				((${#nums[@]} >= 2)) && break
			done
		else
			nums+=("$part")
		fi
		((${#nums[@]} >= 2)) && break
	done
	if ((${#nums[@]} < 2)); then
		echo "pdump-ab: cannot find two allowed CPUs; pass --cpus" >&2
		exit 1
	fi
	echo "${nums[0]},${nums[1]}"
}

CPUS=${CPUS:-$(default_cpu_pair)}
W=${CPUS%,*}
R=${CPUS#*,}

# ---------------------------------------------------------------------------
# Build one side (BEFORE or AFTER) in its own detached worktree.
#
# A rerun resets the worktree to REV with a forced checkout, which
# discards and then reappends the meson.build block below; the reset is a
# local, networkless operation, since the commit is already in this
# clone's history.
dpdk_has_libpcap() {
	grep -q '^#define RTE_HAS_LIBPCAP' \
		"$1/build-ab/subprojects/dpdk/rte_build_config.h" 2>/dev/null
}

setup_tree() {
	local dir=$1 rev=$2 after=$3

	if [[ -e $dir/.git ]]; then
		echo "pdump-ab: resetting $dir to $rev"
		git -C "$dir" checkout --force --detach "$rev"
	else
		echo "pdump-ab: creating worktree $dir at $rev"
		mkdir -p "$(dirname "$dir")"
		git worktree add --detach "$dir" "$rev"
	fi
	# The counters library's build references subprojects/regex's
	# include directory unconditionally, so a plain `meson setup` needs
	# it even though this benchmark never links that library.
	git -C "$dir" submodule update --init -- \
		subprojects/dpdk subprojects/libpcap subprojects/regex

	cp "$AB_DIR/pdump_ab_bench.c" "$dir/modules/pdump/tests/pdump_ab_bench.c"

	local extra_args=
	if ((after)); then
		extra_args=" + ['-DPDUMP_BENCH_AFTER']"
	fi
	cat >>"$dir/modules/pdump/tests/meson.build" <<EOF

# A/B capture bench (modules/pdump/tests/ab/pdump-ab.sh), not part of any
# production target or commit in this tree.
executable(
  'pdump_ab_bench',
  ['pdump_ab_bench.c'],
  c_args: yanet_c_args${extra_args},
  link_args: yanet_link_args,
  dependencies: [
    lib_dataplane_ut_dep,
    lib_logging_dep,
    lib_errors_dep,
    lib_ring_objects_dep,
    lib_pdump_dp_dep,
    libdpdk_dep,
    libpcap_dep,
  ],
  include_directories: [yanet_rootdir],
)
EOF

	# DPDK enables rte_bpf_convert only when it finds a system libpcap at
	# setup time; a build configured without one is redone so a later
	# libpcap install takes effect.
	if [[ -f $dir/build-ab/build.ninja ]] && ! dpdk_has_libpcap "$dir"; then
		echo "pdump-ab: $dir/build-ab was configured without libpcap; reconfiguring"
		rm -rf "$dir/build-ab"
	fi
	# build.ninja, not just the directory, is the signal that a previous
	# setup finished: meson creates the directory before it can fail.
	if [[ ! -f $dir/build-ab/build.ninja ]]; then
		rm -rf "$dir/build-ab"
		echo "pdump-ab: meson setup $dir/build-ab"
		if ! meson setup "$dir/build-ab" "$dir" --buildtype=release \
			>"$dir/setup.log" 2>&1; then
			grep -iE 'error' -A5 "$dir/setup.log" | head -60 || true
			echo "pdump-ab: meson setup failed for $dir; see $dir/setup.log" >&2
			exit 1
		fi
		if ! dpdk_has_libpcap "$dir"; then
			echo "pdump-ab: DPDK in $dir/build-ab was configured without libpcap, so rte_bpf_convert is a stub and the filter rows cannot run; install libpcap with its pkg-config file (libpcap-dev) and rerun" >&2
			exit 1
		fi
	fi
	echo "pdump-ab: building pdump_ab_bench in $dir"
	if ! meson compile -C "$dir/build-ab" pdump_ab_bench \
		>"$dir/compile.log" 2>&1; then
		grep -iE 'error|FAILED' -A5 "$dir/compile.log" |
			grep -v '^ccache' | head -60 || true
		echo "pdump-ab: build failed for $dir; see $dir/compile.log" >&2
		exit 1
	fi
}

setup_tree "$BEFORE_DIR" "$BEFORE_REV" 0
setup_tree "$AFTER_DIR" "$AFTER_REV" 1

BEFORE_BIN="$BEFORE_DIR/build-ab/modules/pdump/tests/pdump_ab_bench"
AFTER_BIN="$AFTER_DIR/build-ab/modules/pdump/tests/pdump_ab_bench"

# The YANET_CACHE_LINE_SIZE the AFTER build actually used, read back from
# the compiler invocations meson recorded, not assumed from the host arch.
#
# -m1 stops grep after the first match, so a downstream reader that also
# stops early cannot SIGPIPE it into a nonzero exit that `pipefail` would
# turn into a silent, unrelated failure.
AFTER_BUILD_CACHE_LINE=$(
	grep -m1 -o -- '-DYANET_CACHE_LINE_SIZE=[0-9]*' \
		"$AFTER_DIR/build-ab/compile_commands.json" 2>/dev/null |
		cut -d= -f2
) || true

# ---------------------------------------------------------------------------
# Run the matrix: every case is BEFORE, then AFTER at publish batch 8, then
# AFTER at publish batch 64, so a cell never always follows the same
# neighbour. --quick keeps one ring size, two packet sizes and one
# half-filter row.
run_matrix() {
	local out=$1
	: >"$out"
	local -a cases=()
	if ((QUICK)); then
		local sz
		for sz in 64 1500; do
			cases+=("$sz 1048576 all 0")
			cases+=("$sz 1048576 all 1")
		done
		cases+=("64 1048576 half 0")
	else
		local ring rd sz
		for ring in 1048576 8388608; do
			for rd in 0 1; do
				for sz in 64 256 1500 9000; do
					cases+=("$sz $ring all $rd")
				done
			done
		done
		for sz in 64 256 1500 9000; do
			cases+=("$sz 1048576 half 0")
		done
	fi

	local l c
	for ((l = 1; l <= LAUNCHES; l++)); do
		for c in "${cases[@]}"; do
			# shellcheck disable=SC2086
			set -- $c
			taskset -c "$W,$R" "$BEFORE_BIN" \
				"$1" "$2" "$3" "$4" 8 "$REPS" 100 "$W" "$R" >>"$out"
			taskset -c "$W,$R" "$AFTER_BIN" \
				"$1" "$2" "$3" "$4" 8 "$REPS" 100 "$W" "$R" >>"$out"
			taskset -c "$W,$R" "$AFTER_BIN" \
				"$1" "$2" "$3" "$4" 64 "$REPS" 100 "$W" "$R" >>"$out"
		done
	done
}

# ---------------------------------------------------------------------------
# Eviction chunk sweep: BEFORE once per case, AFTER at both publish
# batches for every chunk, in the same interleaved order as the matrix.
run_chunk_sweep() {
	local out=$1
	: >"$out"
	local -a cases=() chunks=()
	IFS=, read -r -a chunks <<<"$EVICT_CHUNKS"
	local ring rd sz
	for ring in 1048576 8388608; do
		for rd in 0 1; do
			for sz in 64 256 1500 9000; do
				cases+=("$sz $ring all $rd")
			done
		done
	done

	local l c chunk batch
	for ((l = 1; l <= LAUNCHES; l++)); do
		for c in "${cases[@]}"; do
			# shellcheck disable=SC2086
			set -- $c
			taskset -c "$W,$R" "$BEFORE_BIN" \
				"$1" "$2" "$3" "$4" 8 "$REPS" 100 "$W" "$R" >>"$out"
			for chunk in "${chunks[@]}"; do
				for batch in 8 64; do
					taskset -c "$W,$R" "$AFTER_BIN" \
						"$1" "$2" "$3" "$4" "$batch" "$REPS" 100 \
						"$W" "$R" "$chunk" >>"$out"
				done
			done
		done
	done
}

RAW="$WORK/raw.txt"
if [[ -n $EVICT_CHUNKS ]]; then
	echo "pdump-ab: running the eviction chunk sweep (chunks=$EVICT_CHUNKS launches=$LAUNCHES reps=$REPS cpus=$W,$R)"
	run_chunk_sweep "$RAW"
else
	echo "pdump-ab: running the handler matrix (launches=$LAUNCHES reps=$REPS cpus=$W,$R)"
	run_matrix "$RAW"
fi

# ---------------------------------------------------------------------------
# Everything from here on goes to the terminal and to the report file.
exec > >(tee "$OUT") 2>&1

echo "=== pdump A/B capture benchmark ==="
echo "date:          $(date -Is)"
echo "host:          $(hostname)"
echo "uname -m:      $(uname -m)"
# The model of each pinned CPU, not of CPU 0: big.LITTLE boards mix
# core types, and lscpu's first "Model name" is the first cluster's.
cpu_model() {
	lscpu -e=CPU,MODELNAME 2>/dev/null |
		awk -v cpu="$1" '$1 == cpu { $1 = ""; sub(/^ /, ""); print; exit }'
}
echo "cpu model:     writer $(cpu_model "$W"), reader $(cpu_model "$R")"
echo "L1 line:       $(getconf LEVEL1_DCACHE_LINESIZE 2>/dev/null || echo unknown) bytes (getconf)"
echo "build cache:   YANET_CACHE_LINE_SIZE=${AFTER_BUILD_CACHE_LINE:-unknown} (AFTER build)"
echo "cpu pair:      writer=$W reader=$R"
echo "before rev:    $BEFORE_REV"
echo "after rev:     $AFTER_REV"
if command -v "${CC:-cc}" >/dev/null; then
	echo "compiler:      $("${CC:-cc}" --version | head -1)"
fi
echo

if [[ -n $EVICT_CHUNKS ]]; then
	echo "--- eviction chunk sweep: median of $LAUNCHES launch medians, AFTER vs BEFORE delta % per chunk and publish batch ---"
	python3 "$AB_DIR/summarize.py" --chunks "$RAW"
	echo
	echo "report: $OUT"
	exit 0
fi

echo "--- handler matrix: median of $LAUNCHES launch medians, [min..max] of launch medians, AFTER vs BEFORE delta % ---"
python3 "$AB_DIR/summarize.py" "$RAW"
echo

echo "--- in-tree benchmark: meson test --benchmark pdump_bench (AFTER, PDUMP_BENCH_CPUS=$W,$R) ---"
PDUMP_BENCH_CPUS="$W,$R" meson test -C "$AFTER_DIR/build-ab" \
	--benchmark -v pdump_bench || true

echo
echo "report: $OUT"
