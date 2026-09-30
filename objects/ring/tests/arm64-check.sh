#!/usr/bin/env bash
# One-shot hardware check of the ring writer's eviction fence.
#
# Builds the tree, runs the ring unit tests, proves the fence is (and the
# test-only knob removes) a barrier in the generated code, stress-tests the
# Go reader against a full-speed C writer with and without the fence, and
# benchmarks both writers. Everything is logged to
# arm64-check-<host>-<date>.txt in the repository root. See ARM64_CHECK.md.

set -euo pipefail

usage() {
	cat <<'EOF'
Usage: objects/ring/tests/arm64-check.sh [--quick]

  --quick   short run (about 2-3 minutes after the build) instead of the
            default one (about 10-15 minutes after the build)

Environment overrides:
  RING_CHECK_STRESS_SECONDS  target seconds per stress run
  RING_CHECK_RECORDS         records per stress run (skips calibration)
  RING_CHECK_REPS            stress repetitions per build and capacity
  RING_CHECK_CAPACITIES      space-separated ring capacities to stress
  RING_CHECK_BENCH_REPS      benchmark repetitions per build
  RING_CHECK_BENCH_CPUS      taskset CPU list for the benchmark (e.g. 2,3)
EOF
}

QUICK=0
while (($#)); do
	case "$1" in
	--quick) QUICK=1 ;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		echo "unknown argument: $1" >&2
		usage >&2
		exit 2
		;;
	esac
	shift
done

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
cd "$ROOT"

BUILD_DIR=build
WORK="$BUILD_DIR/ring-arm64-check"
HOST=$(hostname -s 2>/dev/null || hostname)
REPORT="$ROOT/arm64-check-$HOST-$(date +%Y%m%d-%H%M%S).txt"
CRING_PKG=./objects/ring/bindings/go/cring
CRING_DIR="$ROOT/objects/ring/bindings/go/cring"
STRESS_TEST='^Test_Reader_Stress_ConcurrentWriterNeverTears$'

if ((QUICK)); then
	STRESS_SECONDS=${RING_CHECK_STRESS_SECONDS:-20}
	REPS=${RING_CHECK_REPS:-1}
	CAPACITIES=${RING_CHECK_CAPACITIES:-"4096 65536"}
	BENCH_REPS=${RING_CHECK_BENCH_REPS:-3}
else
	STRESS_SECONDS=${RING_CHECK_STRESS_SECONDS:-30}
	REPS=${RING_CHECK_REPS:-3}
	CAPACITIES=${RING_CHECK_CAPACITIES:-"1024 4096 65536"}
	BENCH_REPS=${RING_CHECK_BENCH_REPS:-5}
fi

exec > >(tee "$REPORT") 2>&1
TEE_PID=$!

START_TS=$(date +%s)
ARCH=$(uname -m)
declare -a SEC_ORDER=()
declare -A SEC_STATUS=() SEC_NOTE=()
FAILED=0

# Record a section verdict; the summary prints sections in first-seen order.
set_status() {
	local name=$1 status=$2 note=${3:-}
	if [[ ! -v "SEC_STATUS[$name]" ]]; then
		SEC_ORDER+=("$name")
	fi
	SEC_STATUS[$name]=$status
	SEC_NOTE[$name]=$note
	if [[ $status == FAIL ]]; then
		FAILED=1
	fi
}

header() {
	printf '\n==================== %s ====================\n' "$*"
}

log() {
	printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"
}

# Print the verdicts and tables, then flush the report file.
summary() {
	local rc=$?
	set +e
	header "SUMMARY"
	echo "host:    $HOST ($ARCH)"
	echo "mode:    $([[ $QUICK == 1 ]] && echo quick || echo full)"
	echo "elapsed: $(($(date +%s) - START_TS)) s"
	echo
	local name
	for name in "${SEC_ORDER[@]}"; do
		printf '%-12s %-5s %s\n' "$name" "${SEC_STATUS[$name]}" "${SEC_NOTE[$name]}"
	done
	if [[ -s "$WORK/codegen.txt" ]]; then
		echo
		echo "Barrier instructions in the ring writer (fence vs no-fence build):"
		cat "$WORK/codegen.txt"
	fi
	if [[ -s "$WORK/stress-summary.txt" ]]; then
		echo
		echo "Stress (torn = records the reader returned with wrong bytes):"
		cat "$WORK/stress-summary.txt"
	fi
	if [[ -s "$WORK/bench-summary.txt" ]]; then
		echo
		echo "Benchmark medians, ns/record (lower is better):"
		cat "$WORK/bench-summary.txt"
	fi
	echo
	if ((FAILED)) || { ((rc != 0)) && ((${#SEC_ORDER[@]} == 0)); }; then
		echo "OVERALL: FAIL"
		rc=1
	elif ((rc != 0)); then
		echo "OVERALL: FAIL (aborted, exit code $rc)"
	else
		echo "OVERALL: PASS"
	fi
	echo "report:  $REPORT"
	exec 1>&- 2>&-
	wait "$TEE_PID" 2>/dev/null
	exit "$rc"
}
trap summary EXIT

# Abort the run: later sections cannot proceed without this one.
die() {
	set_status "$1" FAIL "$2"
	log "FATAL: $2"
	exit 1
}

# ---------------------------------------------------------------------------
header "PREFLIGHT"

echo "date:     $(date -Is)"
echo "uname:    $(uname -a)"
echo "arch:     $ARCH"
if command -v lscpu >/dev/null; then
	lscpu | grep -E '^(Model name|Vendor ID|CPU\(s\)|Thread|Core|Socket|NUMA node\(s\)|L1d|L2|L3|Flags)' || true
fi
echo "cpus:     $(nproc) online for this process"
echo "loadavg:  $(cut -d' ' -f1-3 /proc/loadavg)"
if [[ -r /sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size ]]; then
	echo "L1 line:  $(cat /sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size) bytes (sysfs)"
fi
echo "git:      $(git rev-parse --abbrev-ref HEAD 2>/dev/null) $(git rev-parse --short HEAD 2>/dev/null)"
git status --short 2>/dev/null | sed 's/^/          /' | head -20 || true

required=(git gcc cc meson ninja go objdump taskset python3 awk nproc)
proto_missing=()
while IFS= read -r proto; do
	if [[ ! -f "${proto%.proto}.pb.go" ]]; then
		proto_missing+=("$proto")
	fi
done < <(find . \( -path ./subprojects -o -path './build*' -o -type d -name '.?*' \) -prune \
	-o -name '*.proto' -print)
if ((${#proto_missing[@]})); then
	required+=(make protoc protoc-gen-go protoc-gen-go-grpc)
fi
if [[ ! -e "$BUILD_DIR" ]]; then
	required+=(cmake pkg-config)
fi
missing=()
for tool in "${required[@]}"; do
	command -v "$tool" >/dev/null || missing+=("$tool")
done
# Libraries only a fresh meson setup needs: libyaml for the dataplane and
# pyelftools for DPDK.
if [[ ! -e "$BUILD_DIR" ]]; then
	if command -v pkg-config >/dev/null && ! pkg-config --exists yaml-0.1; then
		missing+=("yaml-0.1 (libyaml-dev, or PKG_CONFIG_PATH does not reach its .pc file)")
	fi
	if command -v python3 >/dev/null && ! python3 -c 'import elftools' 2>/dev/null; then
		missing+=("python3 elftools (python3-pyelftools)")
	fi
fi

for tool in gcc meson ninja go protoc objdump; do
	if command -v "$tool" >/dev/null; then
		case "$tool" in
		go) printf '%-9s %s\n' "$tool:" "$(go version)" ;;
		ninja) printf '%-9s %s\n' "$tool:" "$(ninja --version)" ;;
		*) printf '%-9s %s\n' "$tool:" "$("$tool" --version 2>&1 | head -1)" ;;
		esac
	fi
done

if ((${#missing[@]})); then
	die preflight "missing prerequisites: ${missing[*]}"
fi
if [[ $ARCH != aarch64 && $ARCH != arm64 ]]; then
	log "WARNING: not aarch64; the barrier checks only report, and the stress cannot reproduce a store reordering on this CPU"
fi
if (($(nproc) < 2)); then
	die preflight "the stress needs at least 2 CPUs, this process has $(nproc)"
fi
if awk -v n="$(nproc)" '{ exit !($1 > n / 4 && $1 > 1) }' /proc/loadavg; then
	log "WARNING: the box is busy (loadavg $(cut -d' ' -f1 /proc/loadavg)); benchmark numbers will be noisy"
fi
set_status preflight PASS "$ARCH, $(nproc) cpus"

# ---------------------------------------------------------------------------
header "BUILD"

if [[ ! -e "$BUILD_DIR" ]]; then
	log "no $BUILD_DIR/: initialising submodules and configuring"
	git submodule update --init || die build "git submodule update failed"
	meson setup "$BUILD_DIR" || die build "meson setup failed"
elif [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
	die build "$BUILD_DIR/ exists but is not a configured meson build directory"
fi
log "meson compile -C $BUILD_DIR"
meson compile -C "$BUILD_DIR" || die build "meson compile failed"

if ((${#proto_missing[@]})); then
	log "generating ${#proto_missing[@]} missing protobuf Go files (make proto-go)"
	make proto-go || die build "make proto-go failed"
fi

python3 - "$BUILD_DIR" <<'EOF' || true
import json, subprocess, sys
opts = json.loads(subprocess.check_output(
    ["meson", "introspect", sys.argv[1], "--buildoptions"]))
show = {"buildtype", "b_sanitize", "optimization", "cpu_arch"}
for o in opts:
    if o["name"] in show and not o.get("subproject"):
        print(f"meson {o['name']}: {o['value']}")
EOF

# The dataplane lays out shared structs with DPDK's cache line size, and cgo
# must see the same value (the Makefile passes it the same way).
RTE_CONFIG="$BUILD_DIR/subprojects/dpdk/rte_build_config.h"
CACHE_LINE=$(awk '$2 == "RTE_CACHE_LINE_SIZE" { print $3; exit }' "$RTE_CONFIG" 2>/dev/null || true)
if [[ -z $CACHE_LINE ]]; then
	die build "RTE_CACHE_LINE_SIZE not found in $RTE_CONFIG"
fi
MESON_CACHE_LINE=$(grep -o -- '-DYANET_CACHE_LINE_SIZE=[0-9]*' "$BUILD_DIR/compile_commands.json" |
	sort -u | cut -d= -f2 | paste -sd, -)
echo "cache line: DPDK RTE_CACHE_LINE_SIZE=$CACHE_LINE, meson C code uses $MESON_CACHE_LINE"
if [[ $MESON_CACHE_LINE != "$CACHE_LINE" ]]; then
	die build "cache line mismatch between DPDK ($CACHE_LINE) and the meson C flags ($MESON_CACHE_LINE)"
fi
export CGO_CPPFLAGS="${CGO_CPPFLAGS:+$CGO_CPPFLAGS }-DYANET_CACHE_LINE_SIZE=$CACHE_LINE"
echo "CGO_CPPFLAGS=$CGO_CPPFLAGS"
rm -rf "$WORK"
mkdir -p "$WORK"
set_status build PASS "cache line $CACHE_LINE"

# ---------------------------------------------------------------------------
header "CORRECTNESS"

unset RING_STRESS_RECORDS RING_STRESS_CAPACITY
correct_fail=()
log "meson test ring ring_object pdump_ring"
meson test -C "$BUILD_DIR" --print-errorlogs ring ring_object pdump_ring || correct_fail+=("meson test")
log "go test -count=1 ./objects/ring/..."
go test -count=1 ./objects/ring/... || correct_fail+=("go test")
if ((${#correct_fail[@]})); then
	set_status correctness FAIL "failed: ${correct_fail[*]}"
else
	set_status correctness PASS "meson ring/ring_object/pdump_ring, go ./objects/ring/..."
fi

# ---------------------------------------------------------------------------
header "CODEGEN"

GO_CFLAGS_BASE=$(go env CGO_CFLAGS)
NOFENCE_DEFINE=-DRING_TEST_NO_EVICT_FENCE

log "building the cring test binary with and without the fence"
go test -c -o "$WORK/cring-fence.test" "$CRING_PKG" ||
	die codegen "go test -c (fence) failed"
CGO_CFLAGS="$GO_CFLAGS_BASE $NOFENCE_DEFINE" go test -c -o "$WORK/cring-nofence.test" "$CRING_PKG" ||
	die codegen "go test -c (no fence) failed"

# Emit meson's ring_bench compile command, minus its object/dependency
# outputs, as NUL-separated words preceded by its working directory.
bench_command() {
	python3 - "$BUILD_DIR/compile_commands.json" <<'EOF'
import json, shlex, shutil, sys
for entry in json.load(open(sys.argv[1])):
    if not entry["file"].endswith("objects/ring/tests/ring_bench.c"):
        continue
    args = entry.get("arguments") or shlex.split(entry["command"])
    if args[0] == "ccache" and shutil.which("ccache") is None:
        args = args[1:]
    out, skip = [], False
    for arg in args:
        if skip:
            skip = False
        elif arg in ("-o", "-MQ", "-MF"):
            skip = True
        elif arg not in ("-c", "-MD"):
            out.append(arg)
    sys.stdout.write("\0".join([entry["directory"]] + out) + "\0")
    sys.exit(0)
sys.exit("ring_bench.c not found in compile_commands.json")
EOF
}

mapfile -d '' BENCH_CMD < <(bench_command) || true
if ((${#BENCH_CMD[@]} < 2)); then
	die codegen "cannot extract the ring_bench compile command"
fi
BENCH_DIR=${BENCH_CMD[0]}
BENCH_ARGS=("${BENCH_CMD[@]:1}")
echo "ring_bench flags (from compile_commands.json): ${BENCH_ARGS[*]}"
for variant in fence nofence; do
	extra=()
	[[ $variant == nofence ]] && extra=("$NOFENCE_DEFINE")
	(cd "$BENCH_DIR" && "${BENCH_ARGS[@]}" "${extra[@]}" -Wl,--as-needed -Wl,--no-undefined \
		-o "$ROOT/$WORK/ring_bench-$variant") || die codegen "ring_bench ($variant) build failed"
done

if [[ $ARCH == aarch64 || $ARCH == arm64 ]]; then
	BARRIER_RE='[[:space:]](dmb|dsb)[[:space:]]'
else
	BARRIER_RE='[[:space:]](mfence|sfence|lfence|lock)[[:space:]]|[[:space:]]lock '
fi

# Disassemble the functions of a binary whose names match a regex.
disasm_funcs() {
	objdump -d --no-show-raw-insn "$1" | awk -v re="$2" '
		/^[0-9a-f]+ <.*>:$/ {
			name = $2
			gsub(/[<>:]/, "", name)
			keep = (name ~ re)
			if (keep) print
			next
		}
		keep && NF { print }
	'
}

codegen_fail=()
printf '%-10s %-56s %8s %8s\n' binary functions fence nofence >"$WORK/codegen.txt"
check_barriers() {
	local label=$1 funcs=$2 fence_bin=$3 nofence_bin=$4
	local fence_asm="$WORK/$label-fence.asm" nofence_asm="$WORK/$label-nofence.asm"
	disasm_funcs "$fence_bin" "$funcs" >"$fence_asm"
	disasm_funcs "$nofence_bin" "$funcs" >"$nofence_asm"
	if [[ ! -s $fence_asm || ! -s $nofence_asm ]]; then
		codegen_fail+=("$label: functions /$funcs/ not found")
		return
	fi
	local fence_n nofence_n
	fence_n=$(grep -cE "$BARRIER_RE" "$fence_asm" || true)
	nofence_n=$(grep -cE "$BARRIER_RE" "$nofence_asm" || true)
	printf '%-10s %-56s %8s %8s\n' "$label" "$funcs" "$fence_n" "$nofence_n" >>"$WORK/codegen.txt"
	echo "--- $label: functions in the fence build:"
	grep -E '^[0-9a-f]+ <' "$fence_asm" | sed 's/^/    /'
	echo "--- $label: barrier instructions with context (fence build):"
	grep -E -B4 -A1 "$BARRIER_RE" "$fence_asm" | head -60 || true
	echo "--- $label: barrier instructions (no-fence build):"
	grep -E "$BARRIER_RE" "$nofence_asm" | head -20 || true
	if [[ $ARCH == aarch64 || $ARCH == arm64 ]]; then
		if ! grep -qE '[[:space:]]dmb[[:space:]]+ish$' "$fence_asm"; then
			codegen_fail+=("$label: no 'dmb ish' in the fence build")
		fi
		if ((fence_n <= nofence_n)); then
			codegen_fail+=("$label: the no-fence knob removed no barrier")
		fi
	fi
}
check_barriers cring '^(ring_stress_run|ring_worker_prepare)' \
	"$WORK/cring-fence.test" "$WORK/cring-nofence.test"
check_barriers ring_bench '^(new_bench_thread|new_write_record|ring_worker_prepare)' \
	"$WORK/ring_bench-fence" "$WORK/ring_bench-nofence"
cat "$WORK/codegen.txt"

if ((${#codegen_fail[@]})); then
	set_status codegen FAIL "$(printf '%s; ' "${codegen_fail[@]}")"
elif [[ $ARCH == aarch64 || $ARCH == arm64 ]]; then
	set_status codegen PASS "fence build has 'dmb ish', no-fence build drops it"
else
	set_status codegen INFO "not aarch64: counts reported only (x86-64 emits no fence)"
fi

# ---------------------------------------------------------------------------
header "STRESS"

GOMAXPROCS=${GOMAXPROCS:-$(nproc)}
((GOMAXPROCS < 2)) && GOMAXPROCS=2
export GOMAXPROCS
echo "GOMAXPROCS=$GOMAXPROCS; the writer runs on its own pthread, unpinned"

STRESS_TSV="$WORK/stress.tsv"
: >"$STRESS_TSV"
stress_fail=()

# Run one stress pass and append "variant capacity records written
# returned torn seconds exit" to the stress table; prints the parsed counts.
run_stress() {
	local variant=$1 capacity=$2 records=$3
	local out="$WORK/stress-$variant-$capacity-$RANDOM.log"
	local timeout=$((STRESS_SECONDS * 20 + 300))
	local t0 t1 rc=0
	t0=$(date +%s.%N)
	(cd "$CRING_DIR" && RING_STRESS_RECORDS=$records RING_STRESS_CAPACITY=$capacity \
		"$ROOT/$WORK/cring-$variant.test" -test.run "$STRESS_TEST" -test.v -test.count=1 \
		-test.timeout "${timeout}s") >"$out" 2>&1 || rc=$?
	t1=$(date +%s.%N)
	local counts written returned torn
	counts=$(grep -oE 'written=[0-9]+ returned=[0-9]+ torn=[0-9]+' "$out" | tail -1 || true)
	written=$(sed -n 's/.*written=\([0-9]*\).*/\1/p' <<<"$counts")
	returned=$(sed -n 's/.*returned=\([0-9]*\).*/\1/p' <<<"$counts")
	torn=$(sed -n 's/.*torn=\([0-9]*\).*/\1/p' <<<"$counts")
	local secs
	secs=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.1f", b - a }')
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$variant" "$capacity" "$records" \
		"${written:--}" "${returned:--}" "${torn:--}" "$secs" "$rc" >>"$STRESS_TSV"
	log "stress $variant cap=$capacity records=$records: ${counts:-no counts} (${secs}s, exit $rc)"
	grep -E 'torn record' "$out" | head -5 | sed 's/^/    /' || true
	if [[ -z $counts ]]; then
		sed 's/^/    | /' "$out" | tail -30
		stress_fail+=("$variant cap=$capacity: no result (exit $rc)")
	elif [[ $written != "$records" ]]; then
		stress_fail+=("$variant cap=$capacity: writer committed $written of $records")
	elif [[ $variant == fence ]] && ((torn != 0 || rc != 0)); then
		sed 's/^/    | /' "$out" | tail -30
		stress_fail+=("fence cap=$capacity: torn=$torn exit=$rc")
	elif [[ $variant == nofence ]] && ((torn == 0 && rc != 0)); then
		sed 's/^/    | /' "$out" | tail -30
		stress_fail+=("nofence cap=$capacity: exit $rc without torn records")
	fi
	LAST_SECS=$secs
}

if [[ -n ${RING_CHECK_RECORDS:-} ]]; then
	RECORDS=$RING_CHECK_RECORDS
	log "records per run: $RECORDS (RING_CHECK_RECORDS)"
else
	# Grow the calibration run until it lasts long enough to time reliably.
	CAL_RECORDS=2000000
	while :; do
		log "calibrating: $CAL_RECORDS records, fence build, capacity 4096"
		run_stress fence 4096 "$CAL_RECORDS"
		if awk -v s="$LAST_SECS" 'BEGIN { exit !(s < 2) }' && ((CAL_RECORDS < 500000000)); then
			CAL_RECORDS=$((CAL_RECORDS * 8))
		else
			break
		fi
	done
	RECORDS=$(awk -v n="$CAL_RECORDS" -v s="$LAST_SECS" -v t="$STRESS_SECONDS" 'BEGIN {
		if (s < 0.1) s = 0.1
		r = n / s * t
		r = int(r / 1000000) * 1000000
		if (r < 1000000) r = 1000000
		if (r > 2000000000) r = 2000000000
		print r
	}')
	log "records per run: $RECORDS (about ${STRESS_SECONDS}s each)"
fi

for ((rep = 1; rep <= REPS; rep++)); do
	for capacity in $CAPACITIES; do
		if ((rep % 2)); then order=(fence nofence); else order=(nofence fence); fi
		for variant in "${order[@]}"; do
			run_stress "$variant" "$capacity" "$RECORDS"
		done
	done
done

awk -F'\t' '
	{
		key = $1 "\t" $2
		if (!(key in runs)) keys[++nk] = key
		runs[key]++
		if ($4 != "-") written[key] += $4
		if ($5 != "-") returned[key] += $5
		if ($6 != "-") { torn[key] += $6; if ($6 > 0) hit[key]++ }
		secs[key] += $7
	}
	END {
		printf "%-8s %9s %5s %14s %14s %10s %11s %8s\n",
			"build", "capacity", "runs", "written", "returned", "torn",
			"runs_torn", "secs"
		for (i = 1; i <= nk; i++) {
			split(keys[i], k, "\t")
			printf "%-8s %9s %5d %14d %14d %10d %11d %8.0f\n",
				k[1], k[2], runs[keys[i]], written[keys[i]], returned[keys[i]],
				torn[keys[i]], hit[keys[i]], secs[keys[i]]
		}
	}
' "$STRESS_TSV" >"$WORK/stress-summary.txt"
cat "$WORK/stress-summary.txt"

NOFENCE_TORN=$(awk -F'\t' '$1 == "nofence" && $6 != "-" { t += $6 } END { print t + 0 }' "$STRESS_TSV")
if ((${#stress_fail[@]})); then
	set_status stress FAIL "$(printf '%s; ' "${stress_fail[@]}")"
elif ((NOFENCE_TORN > 0)); then
	set_status stress PASS "fence torn=0; no-fence REPRODUCED the bug: torn=$NOFENCE_TORN"
else
	set_status stress PASS "fence torn=0; no-fence torn=0 (not reproduced, which is allowed)"
fi

# ---------------------------------------------------------------------------
header "PERFORMANCE"

if [[ -n ${RING_CHECK_BENCH_CPUS:-} ]]; then
	BENCH_CPUS=$RING_CHECK_BENCH_CPUS
else
	BENCH_CPUS=$(python3 -c '
import os
cpus = sorted(os.sched_getaffinity(0))
pick = cpus[2:4] if len(cpus) >= 4 else cpus[:2]
print(",".join(map(str, pick)))
')
fi
echo "ring_bench pinned to CPUs $BENCH_CPUS, $BENCH_REPS runs per build, builds alternate"

BENCH_TSV="$WORK/bench.tsv"
: >"$BENCH_TSV"
bench_fail=()
for ((rep = 1; rep <= BENCH_REPS; rep++)); do
	if ((rep % 2)); then order=(fence nofence); else order=(nofence fence); fi
	for variant in "${order[@]}"; do
		out="$WORK/bench-$variant-$rep.txt"
		log "ring_bench $variant run $rep"
		if taskset -c "$BENCH_CPUS" "$WORK/ring_bench-$variant" >"$out" 2>&1; then
			cat "$out"
			awk -v v="$variant" -v r="$rep" 'NR > 1 && NF == 5 {
				print v "\t" r "\t" $1 "\t" $2 "\t" $3 "\t" $4 "\t" $5
			}' "$out" >>"$BENCH_TSV"
		else
			cat "$out"
			bench_fail+=("$variant run $rep")
		fi
	done
done

python3 - "$BENCH_TSV" >"$WORK/bench-summary.txt" <<'EOF'
import statistics, sys
from collections import defaultdict

vals = defaultdict(list)
keys = []
for line in open(sys.argv[1]):
    variant, _, size, scen, workers, old, new = line.split("\t")
    key = (int(size), scen, int(workers))
    if key not in keys:
        keys.append(key)
    vals[key + (variant, "old")].append(float(old))
    vals[key + (variant, "new")].append(float(new))

def med(key, variant, side):
    xs = vals.get(key + (variant, side))
    return statistics.median(xs) if xs else float("nan")

def pct(a, b):
    return (a - b) / b * 100 if b else float("nan")

print("old = pdump writer, new = ring writer; f = fence build, nf = no-fence build")
print("fence% = new(f) vs new(nf); new/old% = new(f) vs old(f);")
print("noise% = old(f) vs old(nf), the same code in both builds")
print(f"{'size':>5} {'scenario':<11} {'wrk':>3} {'old(f)':>8} {'old(nf)':>8} "
      f"{'new(f)':>8} {'new(nf)':>8} {'fence%':>7} {'new/old%':>8} {'noise%':>7}")
for key in keys:
    of, onf = med(key, "fence", "old"), med(key, "nofence", "old")
    nf_, nnf = med(key, "fence", "new"), med(key, "nofence", "new")
    print(f"{key[0]:>5} {key[1]:<11} {key[2]:>3} {of:8.2f} {onf:8.2f} "
          f"{nf_:8.2f} {nnf:8.2f} {pct(nf_, nnf):+7.1f} {pct(nf_, of):+8.1f} "
          f"{pct(of, onf):+7.1f}")
EOF
cat "$WORK/bench-summary.txt"

if ((${#bench_fail[@]})); then
	set_status performance FAIL "ring_bench failed: ${bench_fail[*]}"
else
	set_status performance INFO "no threshold; see the table"
fi
