#!/usr/bin/env bash
# One-shot hardware check of the ring writer's eviction fence.
#
# Builds the tree, runs the ring unit tests, checks in the generated code
# that an eviction is one release store plus a fence (which the test-only
# knob removes) and that the Go reader uses acquire/release atomics,
# stress-tests the Go reader against a full-speed C writer with and without
# the fence, and benchmarks both writers alone, with a full or index-only
# concurrent reader per worker, unpaced and paced to fixed record rates, and
# the Go reader. Everything is logged to
# arm64-check-<host>-<date>.txt in the repository root. See ARM64_CHECK.md.

set -euo pipefail

usage() {
	cat <<'EOF'
Usage: objects/ring/tests/arm64-check.sh [--quick]

  --quick   short run (about 4 minutes after the build) instead of the
            default one (about 12-18 minutes after the build)

Environment overrides:
  RING_CHECK_STRESS_SECONDS  target seconds per stress run
  RING_CHECK_RECORDS         records per stress run (skips calibration)
  RING_CHECK_REPS            stress repetitions per build and capacity
  RING_CHECK_CAPACITIES      space-separated ring capacities to stress
  RING_CHECK_BENCH_REPS      benchmark repetitions per build
  RING_CHECK_BENCH_CPUS      benchmark CPUs as w0,r0[,w1,r1]: the writer and
                             reader CPU of worker 0, then of worker 1
  RING_CHECK_GOBENCH_TIME    -benchtime of each Go reader benchmark run
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
	GOBENCH_TIME=${RING_CHECK_GOBENCH_TIME:-0.5s}
else
	STRESS_SECONDS=${RING_CHECK_STRESS_SECONDS:-30}
	REPS=${RING_CHECK_REPS:-3}
	CAPACITIES=${RING_CHECK_CAPACITIES:-"1024 4096 65536"}
	BENCH_REPS=${RING_CHECK_BENCH_REPS:-5}
	GOBENCH_TIME=${RING_CHECK_GOBENCH_TIME:-1s}
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
		echo "Codegen profiles (ring writer per build, Go reader):"
		cat "$WORK/codegen.txt"
	fi
	if [[ -s "$WORK/stress-summary.txt" ]]; then
		echo
		echo "Stress (torn = records the reader returned with wrong bytes):"
		cat "$WORK/stress-summary.txt"
	fi
	if [[ -s "$WORK/bench-summary.txt" ]]; then
		echo
		echo "Benchmark medians:"
		cat "$WORK/bench-summary.txt"
	fi
	if [[ -s "$WORK/gobench-summary.txt" ]]; then
		echo
		echo "Go reader benchmark medians:"
		cat "$WORK/gobench-summary.txt"
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

required=(git gcc cc meson ninja go cargo objdump taskset python3 awk nproc)
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
	# cmake, flex and bison build the libpcap subproject.
	required+=(cmake pkg-config flex bison)
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
	die preflight "missing prerequisites: ${missing[*]} (objects/ring/tests/arm64-check-nix.sh runs the check in a Nix shell that has them all)"
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
# cgo does not track headers outside a package directory, so the Go build
# cache can serve a writer compiled from an older ring header. Keying the
# flags on the headers' content recompiles the cgo packages when they change.
RING_HEADERS_SUM=$(cat common/ring.h objects/ring/api/*.h | sha256sum | cut -c1-16)
export CGO_CPPFLAGS="$CGO_CPPFLAGS -DRING_CHECK_HEADERS=$RING_HEADERS_SUM"
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
    if not entry["file"].endswith("tests/common/ring_bench.c"):
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

IS_ARM64=0
[[ $ARCH == aarch64 || $ARCH == arm64 ]] && IS_ARM64=1

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

# Summarise how a disassembled writer publishes an eviction, as
# "fences=F after_stlr=S dmb=D ldadd=L lock=K".
#
# A release fence is one "dmb ish", or "dmb ishld" directly followed by
# "dmb ishst" (newer GCC). after_stlr counts fences whose nearest preceding
# memory or branch instruction is an stlr, i.e. the single release store of
# the readable position. ldadd counts LSE adds and outline-atomic calls,
# the read-modify-write the writer no longer issues; lock counts x86-64
# lock-prefixed instructions, and dmb every data memory barrier.
writer_profile() {
	awk '
		/^[0-9a-f]+ </ { next }
		{
			line = $0
			sub(/^[ \t]*[0-9a-f]+:[ \t]*/, "", line)
			n = split(line, f, /[ \t]+/)
			if (n == 0 || f[1] == "") next
			op = f[1]; arg = f[2]
			if (op == "dmb") dmb++
			if (op ~ /^ldadd/ || line ~ /__aarch64_ldadd/) ldadd++
			if (op == "lock" || op ~ /^lock/) lock++
			fence = 0
			if (op == "dmb" && arg == "ish") {
				fence = 1; back = nhist
			} else if (op == "dmb" && arg == "ishst" && nhist > 0 && hist[nhist] == "dmb ishld") {
				fence = 1; back = nhist - 1
			}
			if (fence) {
				fences++
				for (k = back; k > 0 && k > back - 8; k--) {
					split(hist[k], h, " ")
					if (h[1] ~ /^(ld|st|b|bl|br|blr|ret|cb|tb|dmb|dsb|cas|swp)/) break
				}
				if (k > 0 && k > back - 8 && h[1] == "stlr") after_stlr++
			}
			hist[++nhist] = op " " arg
		}
		END {
			printf "fences=%d after_stlr=%d dmb=%d ldadd=%d lock=%d\n",
				fences, after_stlr, dmb, ldadd, lock
		}
	' "$1"
}

# Read one "key=value" field out of a profile line.
field() {
	sed -n "s/.*\b$2=\([0-9]*\).*/\1/p" <<<"$1"
}

codegen_fail=()
{
	printf 'Writer (functions that inline the eviction):\n'
	printf '%-10s %-8s %s\n' binary build profile
} >"$WORK/codegen.txt"
check_writer() {
	local label=$1 funcs=$2 fence_bin=$3 nofence_bin=$4
	local variant bin asm profile
	for variant in fence nofence; do
		bin=$fence_bin
		[[ $variant == nofence ]] && bin=$nofence_bin
		asm="$WORK/$label-$variant.asm"
		disasm_funcs "$bin" "$funcs" >"$asm"
		if [[ ! -s $asm ]]; then
			codegen_fail+=("$label ($variant): functions /$funcs/ not found")
			continue
		fi
		profile=$(writer_profile "$asm")
		printf '%-10s %-8s %s\n' "$label" "$variant" "$profile" >>"$WORK/codegen.txt"
		echo "--- $label ($variant): functions:"
		grep -E '^[0-9a-f]+ <' "$asm" | sed 's/^/    /'
		echo "--- $label ($variant): $profile; barriers and atomics with context:"
		grep -E -B3 -A1 '[[:space:]](dmb|dsb|stlr|ldadd[a-z]*|mfence|sfence|lock)[[:space:]]|__aarch64_ldadd' \
			"$asm" | head -60 || true
		((IS_ARM64)) || continue
		local fences after_stlr dmb ldadd
		fences=$(field "$profile" fences)
		after_stlr=$(field "$profile" after_stlr)
		dmb=$(field "$profile" dmb)
		ldadd=$(field "$profile" ldadd)
		if ((ldadd != 0)); then
			codegen_fail+=("$label ($variant): $ldadd ldadd on the readable position, expected one release store")
		fi
		if [[ $variant == fence ]]; then
			if ((fences == 0)); then
				codegen_fail+=("$label (fence): no release fence (dmb ish)")
			elif ((after_stlr != fences)); then
				codegen_fail+=("$label (fence): $((fences - after_stlr)) of $fences fences not right after an stlr")
			fi
		elif ((dmb != 0)); then
			codegen_fail+=("$label (nofence): $dmb dmb left, the knob must remove the fence")
		fi
	done
}
check_writer cring '^(ring_stress_run|ring_worker_prepare)' \
	"$WORK/cring-fence.test" "$WORK/cring-nofence.test"
check_writer bench '^(new_bench_thread|new_write_record|ring_worker_prepare)' \
	"$WORK/ring_bench-fence" "$WORK/ring_bench-nofence"

# The Go reader: the index snapshot and recheck load through the shared
# memory source, and the cursor add between the copy and the recheck.
# Bracket expressions, not backslashes: awk -v would eat the escapes.
READER_PKG='github[.]com/yanet-platform/yanet2/objects/ring/bindings/go/cring'
READ_FUNC="^${READER_PKG}[.][(][*]Reader[)][.]Read\$"
INDICES_FUNC="^${READER_PKG}[.][(][*]shmSource[)][.]Indices\$"
disasm_funcs "$WORK/cring-fence.test" "$READ_FUNC" >"$WORK/reader-read.asm"
disasm_funcs "$WORK/cring-fence.test" "$INDICES_FUNC" >"$WORK/reader-indices.asm"
count_ops() {
	awk -v re="$2" '
		/^[0-9a-f]+ </ { next }
		{
			line = $0
			sub(/^[ \t]*[0-9a-f]+:[ \t]*/, "", line)
			split(line, f, /[ \t]+/)
			if (f[1] ~ re) n++
		}
		END { print n + 0 }
	' "$1"
}
if [[ ! -s $WORK/reader-read.asm || ! -s $WORK/reader-indices.asm ]]; then
	codegen_fail+=("reader: (*Reader).Read or (*shmSource).Indices not found in the cring test binary")
else
	ldar=$(count_ops "$WORK/reader-indices.asm" '^ldar$')
	ldaddal=$(count_ops "$WORK/reader-read.asm" '^ldaddal$')
	ldaxr=$(count_ops "$WORK/reader-read.asm" '^ldaxr$')
	stlxr=$(count_ops "$WORK/reader-read.asm" '^stlxr$')
	xadd=$(grep -cE '[[:space:]]lock[[:space:]]+xadd' "$WORK/reader-read.asm" || true)
	{
		printf '\nGo reader (cring fence build):\n'
		printf '(*shmSource).Indices  ldar=%s\n' "$ldar"
		printf '(*Reader).Read        ldaddal=%s ldaxr=%s stlxr=%s lock_xadd=%s\n' \
			"$ldaddal" "$ldaxr" "$stlxr" "$xadd"
	} >>"$WORK/codegen.txt"
	echo "--- reader: atomics in (*Reader).Read and (*shmSource).Indices:"
	grep -hE '[[:space:]](ldar|ldaddal|ldaxr|stlxr|stlr|xadd|lock)[[:space:]]' \
		"$WORK/reader-indices.asm" "$WORK/reader-read.asm" | head -20 || true
	if ((IS_ARM64)); then
		if ((ldar < 2)); then
			codegen_fail+=("reader: (*shmSource).Indices has $ldar ldar, expected acquire loads of both indices")
		fi
		if ((ldaddal == 0)) && ((ldaxr == 0 || stlxr == 0)); then
			codegen_fail+=("reader: (*Reader).Read has no ldaddal or ldaxr/stlxr pair for the cursor add")
		fi
	fi
fi
cat "$WORK/codegen.txt"

if ((${#codegen_fail[@]})); then
	set_status codegen FAIL "$(printf '%s; ' "${codegen_fail[@]}")"
elif ((IS_ARM64)); then
	set_status codegen PASS "writer: stlr then one fence per eviction, no ldadd, knob drops the fence; reader: ldar + ldaddal/ldaxr-stlxr"
else
	set_status codegen INFO "not aarch64: profiles reported only (x86-64 emits no fence)"
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

# Four distinct CPUs, past the first one when enough are allowed: writers on
# the first two, their readers on the next two. With fewer, one writer and
# its reader.
if [[ -n ${RING_CHECK_BENCH_CPUS:-} ]]; then
	BENCH_CPUS=$RING_CHECK_BENCH_CPUS
else
	BENCH_CPUS=$(python3 -c '
import os
cpus = sorted(os.sched_getaffinity(0))
if len(cpus) >= 5:
    cpus = cpus[1:]
w0, w1, r0, r1 = (cpus + [None] * 4)[:4]
pick = [w0, r0, w1, r1] if r1 is not None else cpus[:2]
print(",".join(map(str, pick)))
')
fi
echo "ring_bench CPUs (w0,r0[,w1,r1]): $BENCH_CPUS; $BENCH_REPS runs per build, builds alternate"

# Each run appends its cells' values as tab-separated rows: ring, size,
# workers, rate, reader, side, writer ns/record, writer Mrec/s, reader
# Mrec/s, lost %, backlog records, bad records.
BENCH_TSV="$WORK/bench.tsv"
: >"$BENCH_TSV"
bench_fail=()
for ((rep = 1; rep <= BENCH_REPS; rep++)); do
	if ((rep % 2)); then order=(fence nofence); else order=(nofence fence); fi
	for variant in "${order[@]}"; do
		out="$WORK/bench-$variant-$rep.txt"
		tsv="$WORK/bench-$variant-$rep.tsv"
		log "ring_bench $variant run $rep"
		if RING_BENCH_REPS=1 RING_BENCH_QUICK=$QUICK RING_BENCH_TSV="$tsv" \
			taskset -c "$BENCH_CPUS" "$WORK/ring_bench-$variant" "$BENCH_CPUS" >"$out" 2>&1; then
			cat "$out"
			awk -v v="$variant" -v r="$rep" '{ print v "\t" r "\t" $0 }' "$tsv" >>"$BENCH_TSV"
		else
			cat "$out"
			bench_fail+=("$variant run $rep")
		fi
	done
done

python3 - "$BENCH_TSV" >"$WORK/bench-summary.txt" <<'EOF'
import statistics, sys
from collections import defaultdict

METRICS = ["writer_ns", "writer_mrps", "reader_mrps", "lost", "backlog", "bad"]
MODES = ["none", "full", "index-only"]
PACED_RATE_MET = 0.95

vals = defaultdict(list)
for line in open(sys.argv[1]):
    f = line.rstrip("\n").split("\t")
    variant, ring, size, workers, rate, reader, side = (
        f[0], f[2], int(f[3]), int(f[4]), float(f[5]), f[6], f[7])
    for name, val in zip(METRICS, f[8:]):
        vals[(ring, size, workers, rate, reader, side, variant, name)].append(float(val))

def keys(pred):
    seen = []
    for k in vals:
        cell = k[:5]
        if pred(*cell) and cell not in seen:
            seen.append(cell)
    return seen

def med(cell, side, variant, name):
    xs = vals.get(cell + (side, variant, name))
    return statistics.median(xs) if xs else float("nan")

def total(cell, side, variant, name):
    return int(sum(vals.get(cell + (side, variant, name), [])))

def pct(a, b):
    return (a - b) / b * 100 if b else float("nan")

def cost(cell, side, variant="fence"):
    ns = med(cell, side, variant, "writer_ns")
    if ns != ns:
        return f"{'-':>10} "
    rate = cell[3]
    missed = rate > 0 and med(cell, side, variant, "writer_mrps") < rate * PACED_RATE_MET
    return f"{ns:10.1f}{'*' if missed else ' '}"

print("Writer cost, ns/record (lower is better) - unpaced, no reader")
print(f"{'':<29}|{'old writer, ns/rec':^19}|{'new writer, ns/rec':^19}|{'change, %':^26}")
print(f"{'size, B':>7}  {'ring':<11}  {'workers':>7}|{'fence':>9}{'no-fence':>9} |"
      f"{'fence':>9}{'no-fence':>9} |{'fence':>8}{'new vs old':>11}{'noise':>7}")
for cell in keys(lambda ring, size, w, rate, reader: rate == 0 and reader == "none"):
    of, onf = med(cell, "old", "fence", "writer_ns"), med(cell, "old", "nofence", "writer_ns")
    nf_, nnf = med(cell, "new", "fence", "writer_ns"), med(cell, "new", "nofence", "writer_ns")
    print(f"{cell[1]:>7}  {cell[0]:<11}  {cell[2]:>7}|{of:9.2f}{onf:9.2f} |{nf_:9.2f}{nnf:9.2f} |"
          f"{pct(nf_, nnf):+8.1f}{pct(nf_, of):+11.1f}{pct(of, onf):+7.1f}")
print("fence = the new writer's fence build against its no-fence build; new vs old =")
print("new against old, fence build; noise = old against old across builds, the same")
print("code, so a fence change smaller than it is not measurable. ring: no-overflow =")
print("indices reset before the ring fills; overflow = 64 KiB ring evicting on every")
print("write; 1m-ring = 1 MiB ring evicting once full, the ring of the rows below.")

def by_reader(title, key_header, cells, row_key):
    print()
    print(title)
    print(f"{'':<21}|{'old writer, ns/record':^36}|{'new writer, ns/record':^36}|{'new + full reader':^23}")
    print(f"{key_header:<21}|{'no reader':>11}{'full reader':>13}{'index-only':>12}|"
          f"{'no reader':>11}{'full reader':>13}{'index-only':>12}|"
          f"{'no-fence, ns':>13}{'fence, %':>10}")
    for base in cells:
        line = row_key(base) + "|"
        for side in ("old", "new"):
            for mode, width in zip(MODES, (11, 13, 12)):
                line += f"{cost(base[:4] + (mode,), side):>{width}}"
            line += "|"
        full = base[:4] + ("full",)
        f_, nf = med(full, "new", "fence", "writer_ns"), med(full, "new", "nofence", "writer_ns")
        line += f"{cost(full, 'new', 'nofence'):>13}{pct(f_, nf):+10.1f}"
        print(line)

unpaced = keys(lambda ring, size, w, rate, reader: ring == "1m-ring" and rate == 0 and reader == "full")
if unpaced:
    by_reader("Writer cost, ns/record (lower is better) - unpaced, 1 MiB ring, by reader per writer, fence build",
              "size, B   workers", unpaced, lambda c: f"{c[1]:>7}   {c[2]:>7}    ")
    print("full reader = copy-then-recheck reader copying and parsing every record; index-only =")
    print("the same index loads and cursor atomics, never touching the data area; fence, % =")
    print("new writer with a full reader, fence build against no-fence build.")

paced = keys(lambda ring, size, w, rate, reader: rate > 0 and reader == "none")
if paced:
    by_reader("Writer cost, ns/record (lower is better) - paced, 1 MiB ring, 1 worker, fence build",
              "size, B  rate, Mrec/s", paced, lambda c: f"{c[1]:>7}  {c[3]:>12g}")
    print("rate = target records/s per writer, in millions; cost = time inside one record's")
    print("prepare, write and commit, timer overhead subtracted, pacing wait excluded;")
    print(f"* = the writer reached below {PACED_RATE_MET:.0%} of the target rate (records ran back to back).")

readers = keys(lambda ring, size, w, rate, reader: reader == "full")
readers.sort(key=lambda c: (c[3] > 0, c[1], c[3], c[2]))
if readers:
    print()
    print("Reader throughput, Mrec/s, and loss, % - full reader, 1 MiB ring")
    print(f"{'':<25}|{'writer, Mrec/s':^14}|{'reader, Mrec/s':^21}|{'lost, %':^21}|{'bad records':^20}")
    print(f"{'size, B':>7}  {'workers':>7}  {'rate':<7}|{'old':>7}{'new':>7}|{'old':>7}{'new':>7}{'new nf':>7}|"
          f"{'old':>7}{'new':>7}{'new nf':>7}|{'old':>6}{'new':>6}{'new nf':>8}")
    for c in readers:
        rate = "unpaced" if c[3] == 0 else f"{c[3]:g}"
        print(f"{c[1]:>7}  {c[2]:>7}  {rate:<7}|"
              f"{med(c, 'old', 'fence', 'writer_mrps'):7.2f}{med(c, 'new', 'fence', 'writer_mrps'):7.2f}|"
              f"{med(c, 'old', 'fence', 'reader_mrps'):7.2f}{med(c, 'new', 'fence', 'reader_mrps'):7.2f}"
              f"{med(c, 'new', 'nofence', 'reader_mrps'):7.2f}|"
              f"{med(c, 'old', 'fence', 'lost'):7.1f}{med(c, 'new', 'fence', 'lost'):7.1f}"
              f"{med(c, 'new', 'nofence', 'lost'):7.1f}|"
              f"{total(c, 'old', 'fence', 'bad') + total(c, 'old', 'nofence', 'bad'):6d}"
              f"{total(c, 'new', 'fence', 'bad'):6d}{total(c, 'new', 'nofence', 'bad'):8d}")
    print("Fence build unless marked nf (no-fence build). rate = target writer rate, Mrec/s;")
    print("writer = rate achieved; lost = committed records the reader never returned;")
    print("bad = returned records with a wrong length, magic or sequence order, summed over")
    print("runs: must be 0 for new in the fence build; old sums both builds (no fence in either).")
EOF
cat "$WORK/bench-summary.txt"

FENCE_BAD=$(awk -F'\t' '$1 == "fence" && $8 == "new" { b += $14 } END { print b + 0 }' "$BENCH_TSV")
if ((FENCE_BAD > 0)); then
	bench_fail+=("the fence build's reader returned $FENCE_BAD bad records")
fi

# The Go reader benchmarks run from the package directory, as the stress
# does, on the benchmark CPUs.
GOBENCH_COUNT=$BENCH_REPS
GOBENCH_TSV="$WORK/gobench.tsv"
: >"$GOBENCH_TSV"
echo
echo "Go reader benchmark: -benchtime $GOBENCH_TIME, $GOBENCH_COUNT runs per build"
for variant in fence nofence; do
	out="$WORK/gobench-$variant.txt"
	log "Go reader benchmark ($variant)"
	if (cd "$CRING_DIR" && taskset -c "$BENCH_CPUS" "$ROOT/$WORK/cring-$variant.test" \
		-test.run '^$' -test.bench 'Reader' -test.benchtime "$GOBENCH_TIME" \
		-test.count "$GOBENCH_COUNT" -test.timeout 30m) >"$out" 2>&1; then
		cat "$out"
		awk -v v="$variant" '/^Benchmark/ { print v "\t" $0 }' "$out" >>"$GOBENCH_TSV"
	else
		cat "$out"
		bench_fail+=("Go reader benchmark ($variant)")
	fi
done

python3 - "$GOBENCH_TSV" >"$WORK/gobench-summary.txt" <<'EOF'
import re, statistics, sys
from collections import defaultdict

vals = defaultdict(list)
keys = []
for line in open(sys.argv[1]):
    variant, rest = line.rstrip("\n").split("\t", 1)
    fields = rest.split()
    name = re.sub(r"-\d+$", "", fields[0]).replace("Benchmark_Reader_Read_", "")
    if name not in keys:
        keys.append(name)
    # After the name and iteration count, fields come in value/unit pairs.
    for val, unit in zip(fields[2::2], fields[3::2]):
        vals[(name, variant, unit)].append(float(val))

def med(name, variant, unit):
    xs = vals.get((name, variant, unit))
    return statistics.median(xs) if xs else float("nan")

print("Prefilled = no writer; ConcurrentWriter = full-speed C writer, 1 MiB ring")
print(f"{'case':<28} {'build':<8} {'ns/record':>10} {'Mrec/s':>8} {'MB/s':>8} {'lost%':>6}")
for name in keys:
    for variant in ("fence", "nofence"):
        lost = med(name, variant, "lost%")
        print(f"{name:<28} {variant:<8} {med(name, variant, 'ns/record'):10.1f} "
              f"{med(name, variant, 'records/s') / 1e6:8.2f} {med(name, variant, 'MB/s'):8.0f} "
              f"{'-' if lost != lost else f'{lost:6.1f}':>6}")
EOF
cat "$WORK/gobench-summary.txt"

if ((${#bench_fail[@]})); then
	set_status performance FAIL "$(printf '%s; ' "${bench_fail[@]}")"
else
	set_status performance INFO "no threshold; see the tables"
fi
