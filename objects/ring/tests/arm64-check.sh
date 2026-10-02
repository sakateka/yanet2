#!/usr/bin/env bash
# One-shot hardware check of the ring writer's eviction fence.
#
# Builds the tree twice: build/ as is, and build-nofence/ with the test-only
# -DRING_TEST_NO_EVICT_FENCE knob that compiles the fence out. Runs the ring
# unit tests, checks in the generated code that an eviction chunk is one
# release store plus a fence, that a batch is published with one release
# store and that the Go reader uses acquire/release atomics, stress-tests
# the Go reader against a full-speed batching C writer of both builds, and
# benchmarks the ring writer of both builds alone and with a concurrent
# reader per worker, plus the Go reader. Everything is logged to
# arm64-check-<host>-<date>.txt in the repository root. See ARM64_CHECK.md.

set -euo pipefail

usage() {
	cat <<'EOF'
Usage: objects/ring/tests/arm64-check.sh [--quick]

  --quick   short run (about 3 minutes after the build) instead of the
            default one (about 10 minutes after the build)

Environment overrides:
  RING_CHECK_STRESS_SECONDS  target seconds per stress run
  RING_CHECK_STRESS_COUNT    stress test iterations per run (-test.count);
                             skips the calibration
  RING_CHECK_REPS            stress runs per build
  RING_CHECK_BENCH_REPS      ring_bench runs per build
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
# Second meson build directory: configured like build/, plus the knob.
NOFENCE_BUILD_DIR=build-nofence
NOFENCE_DEFINE=-DRING_TEST_NO_EVICT_FENCE
WORK="$BUILD_DIR/ring-arm64-check"
HOST=$(hostname -s 2>/dev/null || hostname)
REPORT="$ROOT/arm64-check-$HOST-$(date +%Y%m%d-%H%M%S).txt"
CRING_PKG=./objects/ring/bindings/go/cring
CRING_DIR="$ROOT/objects/ring/bindings/go/cring"
STRESS_TEST='^Test_Reader_Stress_ConcurrentWriterNeverTears$'

if ((QUICK)); then
	STRESS_SECONDS=${RING_CHECK_STRESS_SECONDS:-20}
	REPS=${RING_CHECK_REPS:-1}
	BENCH_REPS=${RING_CHECK_BENCH_REPS:-3}
	GOBENCH_TIME=${RING_CHECK_GOBENCH_TIME:-0.5s}
else
	STRESS_SECONDS=${RING_CHECK_STRESS_SECONDS:-60}
	REPS=${RING_CHECK_REPS:-3}
	BENCH_REPS=${RING_CHECK_BENCH_REPS:-5}
	GOBENCH_TIME=${RING_CHECK_GOBENCH_TIME:-1s}
fi

exec > >(tee "$REPORT") 2>&1
TEE_PID=$!

# Drop the previous run's results, so a run that stops early cannot show
# them in its summary.
rm -rf "$WORK"

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
		echo "ring_bench, medians over $BENCH_REPS run(s) per build:"
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
if [[ ! -e "$BUILD_DIR" || ! -e "$NOFENCE_BUILD_DIR" ]]; then
	# cmake, flex and bison build the libpcap subproject.
	required+=(cmake pkg-config flex bison)
fi
missing=()
for tool in "${required[@]}"; do
	command -v "$tool" >/dev/null || missing+=("$tool")
done
# Libraries only a fresh meson setup needs: libyaml for the dataplane and
# pyelftools for DPDK.
if [[ ! -e "$BUILD_DIR" || ! -e "$NOFENCE_BUILD_DIR" ]]; then
	# meson runs $PKG_CONFIG when it is set.
	if command -v "${PKG_CONFIG:-pkg-config}" >/dev/null && ! "${PKG_CONFIG:-pkg-config}" --exists yaml-0.1; then
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

# The no-fence build: a second meson build directory configured with the
# options build/ was configured with, plus the knob in c_args. Only the
# targets the no-fence variants use are compiled: the ring object archive
# and the ringtest writer archive, which the Go cring test binary links,
# and ring_bench. build/ itself is never reconfigured.
nofence_options() {
	python3 - "$BUILD_DIR/meson-private/cmd_line.txt" "$NOFENCE_DEFINE" <<'EOF'
import configparser, sys
cfg = configparser.ConfigParser(interpolation=None)
cfg.read(sys.argv[1])
opts = dict(cfg["options"]) if cfg.has_section("options") else {}
opts["c_args"] = (opts.get("c_args", "") + " " + sys.argv[2]).strip()
for key, val in opts.items():
    print(f"-D{key}={val}")
EOF
}

# Check that every no-fence source is compiled with the knob and the cache
# line size of build/.
nofence_verify() {
	python3 - "$NOFENCE_BUILD_DIR/compile_commands.json" "$NOFENCE_DEFINE" "$CACHE_LINE" <<'EOF'
import json, shlex, sys
want = ("objects/ring/api/ring_object.c", "objects/ring/tests/ringtest_writer.c",
        "tests/common/ring_bench.c")
seen = set()
for entry in json.load(open(sys.argv[1])):
    for src in want:
        if entry["file"].endswith(src):
            args = entry.get("arguments") or shlex.split(entry["command"])
            if sys.argv[2] not in args:
                sys.exit(f"{src} is compiled without {sys.argv[2]}")
            if f"-DYANET_CACHE_LINE_SIZE={sys.argv[3]}" not in args:
                sys.exit(f"{src} is not compiled with a cache line of {sys.argv[3]}")
            seen.add(src)
missing = sorted(set(want) - seen)
if missing:
    sys.exit(f"not in compile_commands.json: {missing}")
EOF
}

if [[ ! -f "$NOFENCE_BUILD_DIR/build.ninja" ]]; then
	rm -rf "$NOFENCE_BUILD_DIR"
	mapfile -t nofence_opts < <(nofence_options)
	log "meson setup $NOFENCE_BUILD_DIR ${nofence_opts[*]}"
	meson setup "$NOFENCE_BUILD_DIR" "${nofence_opts[@]}" ||
		die build "meson setup $NOFENCE_BUILD_DIR failed"
fi
NOFENCE_TARGETS=(objects/ring/api/ring_objects objects/ring/tests/ringtest_writer tests/common/ring_bench)
log "meson compile -C $NOFENCE_BUILD_DIR ${NOFENCE_TARGETS[*]}"
meson compile -C "$NOFENCE_BUILD_DIR" "${NOFENCE_TARGETS[@]}" ||
	die build "meson compile -C $NOFENCE_BUILD_DIR failed"
nofence_verify ||
	die build "$NOFENCE_BUILD_DIR/ is not a no-fence build of this tree; remove it and rerun"

export CGO_CPPFLAGS="${CGO_CPPFLAGS:+$CGO_CPPFLAGS }-DYANET_CACHE_LINE_SIZE=$CACHE_LINE"
# cgo does not track headers outside a package directory, nor the archives
# it links, so the Go build cache can serve a test binary built from an
# older ring header or archive. Keying the flags on their content rebuilds
# the cgo packages when they change and keeps the two builds' Go test
# binaries apart.
RING_HEADERS_SUM=$(cat common/ring.h objects/ring/api/*.h | sha256sum | cut -c1-16)
GO_CPPFLAGS_BASE="$CGO_CPPFLAGS -DRING_CHECK_HEADERS=$RING_HEADERS_SUM"
libs_sum() {
	cat "$1/objects/ring/api/libring_objects.a" "$1/objects/ring/tests/libringtest_writer.a" |
		sha256sum | cut -c1-16
}
# The fence build: the packages' #cgo directives link the archives of
# build/.
FENCE_LIBS_SUM=$(libs_sum "$BUILD_DIR")
export CGO_CPPFLAGS="$GO_CPPFLAGS_BASE -DRING_CHECK_LIBS=$FENCE_LIBS_SUM"
# The no-fence build: go puts CGO_LDFLAGS before the packages' own #cgo
# LDFLAGS, so these -L directories win for -lring_objects and
# -lringtest_writer; -lconfig_cp still comes from build/. CGO_CFLAGS gives
# the cgo preambles the knob too.
NOFENCE_LIBS_SUM=$(libs_sum "$NOFENCE_BUILD_DIR")
NOFENCE_GO_ENV=(
	"CGO_CPPFLAGS=$GO_CPPFLAGS_BASE -DRING_CHECK_LIBS=$NOFENCE_LIBS_SUM"
	"CGO_CFLAGS=$(go env CGO_CFLAGS) $NOFENCE_DEFINE"
	"CGO_LDFLAGS=-L$ROOT/$NOFENCE_BUILD_DIR/objects/ring/tests -L$ROOT/$NOFENCE_BUILD_DIR/objects/ring/api $(go env CGO_LDFLAGS)"
)
echo "fence Go env:    CGO_CPPFLAGS=$CGO_CPPFLAGS"
printf 'no-fence Go env: %s\n' "${NOFENCE_GO_ENV[@]}"
mkdir -p "$WORK"
set_status build PASS "cache line $CACHE_LINE; $NOFENCE_BUILD_DIR/ built with $NOFENCE_DEFINE"

# ---------------------------------------------------------------------------
header "CORRECTNESS"

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

log "building the cring test binary of both builds"
go test -c -o "$WORK/cring-fence.test" "$CRING_PKG" ||
	die codegen "go test -c (fence) failed"
env "${NOFENCE_GO_ENV[@]}" go test -c -o "$WORK/cring-nofence.test" "$CRING_PKG" ||
	die codegen "go test -c (no fence) failed"
cp "$BUILD_DIR/tests/common/ring_bench" "$WORK/ring_bench-fence"
cp "$NOFENCE_BUILD_DIR/tests/common/ring_bench" "$WORK/ring_bench-nofence"

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
# the read-modify-write the writer must not issue; lock counts x86-64
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
# The stress writer thread of the ringtest writer archive, and the
# benchmark's writer thread; the benchmark's reader functions are left out.
check_writer cring '^(ringtest_stress_run|ringtest_commit_record|ring_worker_prepare|ring_worker_evict)' \
	"$WORK/cring-fence.test" "$WORK/cring-nofence.test"
check_writer bench '^(writer_thread|writer_write|ring_worker_prepare|ring_worker_evict)' \
	"$WORK/ring_bench-fence" "$WORK/ring_bench-nofence"

# The stress writer of each cring test binary must be the code of its own
# build's ringtest writer archive: this catches a no-fence binary that
# linked the archive of build/ (or the reverse). Addresses and branch
# targets are masked; if the linker rewrote an instruction, neither archive
# matches and the link is only reported as unverified.
stress_writer_sum() {
	local text
	text=$(objdump -d --no-show-raw-insn "$1" | awk '
		/^[0-9a-f]+ <ringtest_stress_run>:$/ { keep = 1; next }
		keep && !NF { exit }
		keep {
			sub(/^[ \t]*[0-9a-f]+:[ \t]*/, "")
			gsub(/[0-9a-f]+ <[^>]*>/, "ADDR")
			gsub(/0x[0-9a-f]+/, "IMM")
			print
		}
	')
	# A missing function matches nothing.
	if [[ -z $text ]]; then
		echo "missing-$1"
		return
	fi
	sha256sum <<<"$text" | cut -c1-16
}
declare -A ARCHIVE_SUM=(
	[fence]=$(stress_writer_sum "$BUILD_DIR/objects/ring/tests/libringtest_writer.a")
	[nofence]=$(stress_writer_sum "$NOFENCE_BUILD_DIR/objects/ring/tests/libringtest_writer.a")
)
printf '\nStress writer of each cring binary against the ringtest writer archives:\n' >>"$WORK/codegen.txt"
for variant in fence nofence; do
	other=nofence
	[[ $variant == nofence ]] && other=fence
	sum=$(stress_writer_sum "$WORK/cring-$variant.test")
	if [[ $sum == "${ARCHIVE_SUM[$variant]}" ]]; then
		verdict="matches its own build's archive"
	elif [[ $sum == "${ARCHIVE_SUM[$other]}" ]]; then
		verdict="matches the $other build's archive"
		codegen_fail+=("cring ($variant): linked the $other build's ringtest writer archive")
	else
		verdict="unverified, matches neither archive"
	fi
	printf '%-8s %s\n' "$variant" "$verdict" >>"$WORK/codegen.txt"
done

# A producer call committing records and publishing at its end: on aarch64
# it holds the release stores of the write position (the explicit
# publication, and the commit's and the full batch limit's own
# publications, which the compiler may merge) and exactly one release store
# of the readable position, the eviction chunk's, with exactly one fence
# right after it.
cat >"$WORK/batch-probe.c" <<'EOF'
#include "common/ring.h"

void
ring_check_produce_batch(
	struct ring_worker *ring,
	uint8_t *data,
	const uint8_t *payload,
	uint32_t payload_len,
	uint32_t count
) {
	uint32_t total_len = RING_RECORD_FRAME_SIZE + payload_len;
	for (uint32_t i = 0; i < count; ++i) {
		if (ring_worker_prepare(ring, data, total_len) != 0) {
			break;
		}
		ring_worker_write(
			ring, data, RING_RECORD_FRAME_SIZE, payload, payload_len
		);
		ring_worker_commit(ring, data, total_len);
	}
	ring_worker_publish(ring);
}
EOF
{
	printf '\nBatch probe (one producer call, whole object):\n'
	printf '%-8s %s\n' build profile
} >>"$WORK/codegen.txt"
# The bench flags of build/ minus its source file compile the probe.
probe_args=()
for arg in "${BENCH_ARGS[@]}"; do
	[[ $arg == *ring_bench.c ]] && continue
	probe_args+=("$arg")
done
for variant in fence nofence; do
	extra=()
	[[ $variant == nofence ]] && extra=("$NOFENCE_DEFINE")
	obj="$ROOT/$WORK/batch-probe-$variant.o"
	if ! (cd "$BENCH_DIR" && "${probe_args[@]}" "${extra[@]}" -c "$ROOT/$WORK/batch-probe.c" -o "$obj"); then
		codegen_fail+=("batch probe ($variant): build failed")
		continue
	fi
	objdump -d --no-show-raw-insn "$obj" >"$WORK/batch-probe-$variant.asm"
	profile=$(writer_profile "$WORK/batch-probe-$variant.asm")
	stlr=$(grep -cE '[[:space:]]stlr[[:space:]]' "$WORK/batch-probe-$variant.asm" || true)
	printf '%-8s %s stlr=%s\n' "$variant" "$profile" "$stlr" >>"$WORK/codegen.txt"
	echo "--- batch probe ($variant): $profile stlr=$stlr"
	grep -E -B3 -A2 '[[:space:]](dmb|stlr)[[:space:]]' "$WORK/batch-probe-$variant.asm" | head -40 || true
	((IS_ARM64)) || continue
	if ((stlr < 2 || stlr > 4)); then
		codegen_fail+=("batch probe ($variant): $stlr stlr, expected one to three publication stores and one eviction store")
	fi
	if [[ $variant == fence ]]; then
		if [[ $(field "$profile" fences) != 1 || $(field "$profile" after_stlr) != 1 ]]; then
			codegen_fail+=("batch probe (fence): expected exactly one fence, right after the eviction stlr: $profile")
		fi
	elif [[ $(field "$profile" dmb) != 0 ]]; then
		codegen_fail+=("batch probe (nofence): dmb left: $profile")
	fi
done

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
	set_status codegen PASS "writer: stlr then one fence per eviction chunk, release-store publications, no ldadd, knob drops the fence; reader: ldar + ldaddal/ldaxr-stlxr"
else
	set_status codegen INFO "not aarch64: profiles reported only (x86-64 emits no fence)"
fi

# ---------------------------------------------------------------------------
header "STRESS"

GOMAXPROCS=${GOMAXPROCS:-$(nproc)}
((GOMAXPROCS < 2)) && GOMAXPROCS=2
export GOMAXPROCS
echo "GOMAXPROCS=$GOMAXPROCS; each test iteration runs an unpinned C writer thread over a 4 KiB ring with the default publish batch while the Go reader checks every record; a run repeats the test with -test.count"

STRESS_TSV="$WORK/stress.tsv"
: >"$STRESS_TSV"
stress_fail=()

# Run the stress test count times in one process and append "variant count
# iterations written returned torn iterations_torn seconds exit" to the
# stress table.
run_stress() {
	local variant=$1 count=$2
	local out="$WORK/stress-$variant-$RANDOM.log"
	local timeout=$((STRESS_SECONDS * 20 + 300))
	local t0 t1 rc=0
	t0=$(date +%s.%N)
	(cd "$CRING_DIR" && "$ROOT/$WORK/cring-$variant.test" -test.run "$STRESS_TEST" -test.v \
		-test.count "$count" -test.timeout "${timeout}s") >"$out" 2>&1 || rc=$?
	t1=$(date +%s.%N)
	LAST_SECS=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.1f", b - a }')
	local counts
	counts=$(grep -oE 'written=[0-9]+ returned=[0-9]+ torn=[0-9]+' "$out" |
		awk -F'[= ]' '
			{ n++; w += $2; r += $4; t += $6; if ($6 > 0) hit++
			  if (n == 1) first = $2; else if ($2 != first) uneven = 1 }
			END { printf "%d %d %d %d %d %d", n, w, r, t, hit, uneven }' || true)
	local iters written returned torn hit uneven
	read -r iters written returned torn hit uneven <<<"$counts"
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$variant" "$count" "$iters" \
		"$written" "$returned" "$torn" "$hit" "$LAST_SECS" "$rc" >>"$STRESS_TSV"
	log "stress $variant x$count: iterations=$iters written=$written returned=$returned torn=$torn (${LAST_SECS}s, exit $rc)"
	grep -E 'torn record' "$out" | head -5 | sed 's/^/    /' || true
	if ((iters != count)); then
		sed 's/^/    | /' "$out" | tail -30
		stress_fail+=("$variant: $iters of $count iterations reported a result (exit $rc)")
	elif ((uneven)); then
		stress_fail+=("$variant: the writer committed a different record count in some iterations")
	elif [[ $variant == fence ]] && ((torn != 0 || rc != 0)); then
		sed 's/^/    | /' "$out" | tail -30
		stress_fail+=("fence: torn=$torn exit=$rc")
	elif [[ $variant == nofence ]] && ((torn == 0 && rc != 0)); then
		sed 's/^/    | /' "$out" | tail -30
		stress_fail+=("nofence: exit $rc without torn records")
	fi
}

if [[ -n ${RING_CHECK_STRESS_COUNT:-} ]]; then
	COUNT=$RING_CHECK_STRESS_COUNT
	log "iterations per run: $COUNT (RING_CHECK_STRESS_COUNT)"
else
	log "calibrating: one iteration of the fence build"
	run_stress fence 1
	COUNT=$(awk -v s="$LAST_SECS" -v t="$STRESS_SECONDS" 'BEGIN {
		if (s < 0.05) s = 0.05
		c = int(t / s + 0.5)
		if (c < 1) c = 1
		print c
	}')
	log "iterations per run: $COUNT (about ${STRESS_SECONDS}s each)"
	# The calibration run is not one of the runs in the table.
	: >"$STRESS_TSV"
fi

for ((rep = 1; rep <= REPS; rep++)); do
	if ((rep % 2)); then order=(fence nofence); else order=(nofence fence); fi
	for variant in "${order[@]}"; do
		run_stress "$variant" "$COUNT"
	done
done

awk -F'\t' '
	{
		if (!($1 in runs)) keys[++nk] = $1
		runs[$1]++
		iters[$1] += $3; written[$1] += $4; returned[$1] += $5
		torn[$1] += $6; hit[$1] += $7; secs[$1] += $8
	}
	END {
		printf "%-8s %5s %10s %14s %14s %10s %11s %8s\n",
			"build", "runs", "iterations", "written", "returned", "torn",
			"iters_torn", "secs"
		for (i = 1; i <= nk; i++) {
			k = keys[i]
			printf "%-8s %5d %10d %14d %14d %10d %11d %8.0f\n",
				k, runs[k], iters[k], written[k], returned[k], torn[k],
				hit[k], secs[k]
		}
	}
' "$STRESS_TSV" >"$WORK/stress-summary.txt"
cat "$WORK/stress-summary.txt"

NOFENCE_TORN=$(awk -F'\t' '$1 == "nofence" { t += $6 } END { print t + 0 }' "$STRESS_TSV")
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
echo "ring_bench CPUs (w0,r0[,w1,r1]): $BENCH_CPUS; $BENCH_REPS single-run invocations per build, builds alternate"

# Each invocation prints ring_bench's own tables for one run; the summary
# takes the median of every cell over the invocations of a build.
bench_fail=()
bench_outputs=()
for ((rep = 1; rep <= BENCH_REPS; rep++)); do
	if ((rep % 2)); then order=(fence nofence); else order=(nofence fence); fi
	for variant in "${order[@]}"; do
		out="$WORK/bench-$variant-$rep.txt"
		log "ring_bench $variant run $rep"
		if RING_BENCH_REPS=1 taskset -c "$BENCH_CPUS" "$WORK/ring_bench-$variant" "$BENCH_CPUS" >"$out" 2>&1; then
			cat "$out"
			bench_outputs+=("$variant" "$out")
		else
			cat "$out"
			bench_fail+=("$variant run $rep")
		fi
	done
done

if ((${#bench_outputs[@]})); then
	python3 - "${bench_outputs[@]}" >"$WORK/bench-summary.txt" <<'EOF'
import re, statistics, sys
from collections import defaultdict

# Cells by (table, size, workers, label, batch index, variant).
vals = defaultdict(list)
batches = []
rows = {"writer": [], "reader": []}
bad = defaultdict(int)
args = sys.argv[1:]
for variant, path in zip(args[::2], args[1::2]):
    table = None
    for line in open(path):
        if line.startswith("Table 1."):
            table = "writer"
        elif line.startswith("Table 2."):
            table = "reader"
        m = re.search(r"must be 0\): (\d+)", line)
        if m:
            bad[variant] += int(m.group(1))
            continue
        if table is None or "|" not in line:
            continue
        left, right = line.rstrip("\n").split("|", 1)
        words = left.split()
        if not words[0].isdigit():
            found = [int(b) for b in re.findall(r"batch (\d+)", right)]
            if found and not batches:
                batches = found
            continue
        row = (int(words[0]), int(words[1]), " ".join(words[2:]))
        if row not in rows[table]:
            rows[table].append(row)
        for bi, val in enumerate(right.split()):
            if val != "-":
                vals[(table, row, bi, variant)].append(float(val))

def med(table, row, bi, variant):
    xs = vals.get((table, row, bi, variant))
    return statistics.median(xs) if xs else float("nan")

def num(v, width, prec):
    return f"{'-':>{width}}" if v != v else f"{v:>{width}.{prec}f}"

def pct(a, b):
    return float("nan") if a != a or b != b or b == 0 else (a - b) / b * 100

print()
print("Writer cost, ns/record (lower is better): fence build, no-fence build and")
print("fence % = (fence - no-fence) / no-fence. ring: no-overflow = positions reset")
print("before the ring fills (no eviction, a control); 1 MiB = evicting ring, no")
print("reader; 1 MiB+reader = evicting ring with a full reader per writer.")
head = "size, B  workers  ring         |"
head += "".join(f" {'b' + str(b) + ' fence':>10} {'no-fence':>9} {'fence %':>8} |" for b in batches)
print(head)
for row in rows["writer"]:
    line = f"{row[0]:>7}  {row[1]:>7}  {row[2]:<12} |"
    for bi in range(len(batches)):
        f_, nf = med("writer", row, bi, "fence"), med("writer", row, bi, "nofence")
        p = pct(f_, nf)
        line += f" {num(f_, 10, 2)} {num(nf, 9, 2)} {'-' if p != p else f'{p:+.1f}':>8} |"
    print(line)

if rows["reader"]:
    print()
    print("Full reader on the 1 MiB ring: Mrec/s per reader (higher is better) and")
    print("lost records, % of committed (lower is better), fence and no-fence build.")
    head = "size, B  workers  metric       |"
    head += "".join(f" {'b' + str(b) + ' fence':>10} {'no-fence':>9} |" for b in batches)
    print(head)
    for row in rows["reader"]:
        line = f"{row[0]:>7}  {row[1]:>7}  {row[2]:<12} |"
        for bi in range(len(batches)):
            line += f" {num(med('reader', row, bi, 'fence'), 10, 2)} {num(med('reader', row, bi, 'nofence'), 9, 2)} |"
        print(line)

print()
print("Bad records or corrupt reads, all cells and runs: "
      f"fence {bad['fence']} (must be 0), no-fence {bad['nofence']}")
print(f"FENCE_BAD={bad['fence']}")
EOF
	grep -v '^FENCE_BAD=' "$WORK/bench-summary.txt" >"$WORK/bench-summary.tmp"
	FENCE_BAD=$(sed -n 's/^FENCE_BAD=//p' "$WORK/bench-summary.txt")
	mv "$WORK/bench-summary.tmp" "$WORK/bench-summary.txt"
	cat "$WORK/bench-summary.txt"
	if ((${FENCE_BAD:-0} > 0)); then
		bench_fail+=("the fence build's reader returned $FENCE_BAD bad records")
	fi
fi

# The Go reader benchmark runs from the package directory, as the stress
# does, on the benchmark CPUs. It reads a prefilled ring with no writer, so
# only the fence build is run.
GOBENCH_COUNT=$BENCH_REPS
GOBENCH_OUT="$WORK/gobench-fence.txt"
echo
echo "Go reader benchmark: -benchtime $GOBENCH_TIME, $GOBENCH_COUNT runs"
log "Go reader benchmark"
if (cd "$CRING_DIR" && taskset -c "$BENCH_CPUS" "$ROOT/$WORK/cring-fence.test" \
	-test.run '^$' -test.bench 'Reader' -test.benchtime "$GOBENCH_TIME" \
	-test.count "$GOBENCH_COUNT" -test.timeout 30m) >"$GOBENCH_OUT" 2>&1; then
	cat "$GOBENCH_OUT"
	python3 - "$GOBENCH_OUT" >"$WORK/gobench-summary.txt" <<'EOF'
import re, statistics, sys
from collections import defaultdict

vals = defaultdict(list)
keys = []
for line in open(sys.argv[1]):
    if not line.startswith("Benchmark"):
        continue
    fields = line.split()
    name = re.sub(r"-\d+$", "", fields[0]).replace("Benchmark_Reader_Read_", "")
    if name not in keys:
        keys.append(name)
    # After the name and iteration count, fields come in value/unit pairs.
    for val, unit in zip(fields[2::2], fields[3::2]):
        vals[(name, unit)].append(float(val))

def med(name, unit):
    xs = vals.get((name, unit))
    return statistics.median(xs) if xs else float("nan")

print("Prefilled = the production reader over a filled shared-memory ring, no writer")
print(f"{'case':<28} {'ns/record':>10} {'Mrec/s':>8} {'MB/s':>8}")
for name in keys:
    print(f"{name:<28} {med(name, 'ns/record'):10.1f} "
          f"{med(name, 'records/s') / 1e6:8.2f} {med(name, 'MB/s'):8.0f}")
EOF
	cat "$WORK/gobench-summary.txt"
else
	cat "$GOBENCH_OUT"
	bench_fail+=("Go reader benchmark")
fi

if ((${#bench_fail[@]})); then
	set_status performance FAIL "$(printf '%s; ' "${bench_fail[@]}")"
else
	set_status performance INFO "no threshold; see the tables"
fi
