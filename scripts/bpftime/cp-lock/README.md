# cp_config_lock bpftime instrumentation

An opt-in diagnostic for `cp_config_lock` / `cp_config_try_lock` /
`cp_config_unlock` (`lib/controlplane/config/zone.c`): per-call-site
acquisition count, wait and hold time, and a hold-time histogram,
measured with userspace uprobes via [bpftime](https://eunomia.dev/bpftime)
rather than any change to YANET itself. Not part of the default build,
not wired into meson or debian, and not a production metrics path.

## Scope

A single-operator diagnostic, run by hand on a dev, lab, or
maintenance host — not hardened against a hostile local user.
bpftime allows one session per host (the default shared-memory
segment), so run one collector at a time. Point `--out` at a
directory private to the operator, never a world-writable one such
as `/tmp`.

## Prerequisites

- `clang`, `libelf`/`zlib` dev headers, `libbpf` 1.3+ headers and a
  static `libbpf.a` (`libbpf-dev` on Debian/Ubuntu ships both — link it
  dynamically instead and bpftime can fail at runtime on a glibc symbol
  mismatch). No `libbpf-dev`? Point the build at one with `LIBBPF_DIR`.
- `bpftime` built and in `PATH`. `addr2line` (binutils) is optional,
  for `file:line` annotations.
- `bpftime attach` needs `/proc/sys/kernel/yama/ptrace_scope` to allow
  a non-ancestor ptrace (`sysctl kernel.yama.ptrace_scope=0`), or root
  / `CAP_SYS_PTRACE`. `bpftime start` does not need this.

## Build

```sh
cd scripts/bpftime/cp-lock && make
# or, with LIBBPF_DIR pointing at include/bpf/libbpf.h + lib[64]/libbpf.a:
make LIBBPF_DIR=/path/to/libbpf/install
```

## Run

Do not set `BPFTIME_GLOBAL_SHM_NAME`; use bpftime's default. `bpftime
attach` reads that variable from the *target's* environment, not from
the attach command's — a real control-plane process never has it set,
so a non-default name would only make `attach` miss it. The collector
refuses to start if `/dev/shm/bpftime_maps_shm` already exists (clean
it up with `bpftimetool remove`), and warns if the variable is set in
its own environment. That refusal is a best-effort guard against a
leftover session, not a lock: two collectors started together can
both pass it, so start one at a time on a host.

```sh
# Start the collector first.
mkdir -p "$HOME/cp-lock"
bpftime load ./collector --binary /path/to/yanet-controlplane \
  --out "$HOME/cp-lock/snap" --interval 10

# Then, in another shell, either start a fresh instance:
bpftime start /path/to/yanet-controlplane ...
# or attach to one already running:
bpftime attach <pid>
```

A stripped `--binary` needs `--debug-file /path/.../binary.debug`, or
the collector finds one itself via the binary's GNU build-id under
`/usr/lib/debug/.build-id/`.

`SIGUSR1` to the collector (pid on the `cp-lock collector ready` line)
writes an out-of-cycle snapshot; `SIGINT`/`SIGTERM` write a final one
before exit. Output is `<out>.txt` (sorted by total hold time) and
`<out>.json`.

## Teardown

`bpftime start`/`bpftime load` exit with their subprocess, but
`bpftime attach` injects an agent that outlives this collector: the
target keeps its probes and keeps running them until `bpftime detach`
(which detaches every agent on the host, not just this one) or the
target restarts.

The shared memory segment the collector refuses to recreate
(`/dev/shm/bpftime_maps_shm`) also outlives the collector process;
while any agent anywhere is still attached, that segment is still in
use, and removing it from under a live agent leaves that agent's
later map accesses undefined. Tear down in this order: detach every
attached target first, then stop the collector, then run `bpftimetool
remove` only once no agent is left attached.

## Reading the output

One row per `{process, call site}`: `site` is the calling function
(plus a byte offset, and `file:line` when available); `fail_count` only
applies to `try_lock` rows; `hist_total` (the hold-time histogram's
sum) equals `count` once the target is quiescent — a snapshot reads
every map without pausing the target, so a row read mid-update can
briefly show the two disagree; `overflow_events` counts any call — an
acquisition or a failed `try_lock` — not recorded because the site
table was full; `drops` counts other events this tool declines to
fold into any site's stats (see `cp_lock.h`'s `cp_lock_drop_counters`
for what each one means). A process's symbol resolves correctly even
after it has exited (its load address is recorded, independent of
`/proc`, from its first probed call) — a row falls back to
`pid:0xaddress` only if more than 256 distinct processes have been
instrumented by one collector.

## Self-test

```sh
make selftest
```

Builds a synthetic target and runs every scenario this tool commits
to: start mode, attach mode, exiting before the first snapshot, two
processes at once, each snapshot trigger (interval, SIGINT, SIGTERM),
a stripped target (via `--debug-file`, via build-id, and via neither),
a full site table, and the collector's refuse-if-exists guard. Cleans
up every process and shared-memory segment it starts.

Measured attach pause:
- Host, 64-thread synthetic workload: ~35–65 ms all threads stopped.
- QEMU 4 vCPU/4 GiB, live `yanet-controlplane`: attach command 605 ms, injection ~340 ms.

## Limitations

- x86-64 only: the caller's return address is read directly off the
  stack at function entry.
- `bpftime attach` briefly stops every thread in the target while
  patching it in memory; pause scales with thread count.
- Go control-plane binaries link the lock through cgo; exercised in
  the QEMU lab by attaching to a live `yanet-controlplane` and driving
  a config update plus a counters read, which produced 15 named rows
  (`cp_config_update_*`, `yanet_get_counters_by_tags_per_worker`,
  `route_snapshot_open`/`close`, `cp_{module,device,object}_try_destroy`).
  The default 1 GiB lab guest stopped responding with the collector and
  agent loaded; start the lab with
  `YANET_VM_CPUS=4 YANET_VM_MEMORY=4G just lab up`. Adjust
  `--binary`/`--debug-file` to the binary actually staged in your target.
- Sites are keyed only by tgid, and a process's load address is
  recorded once and kept for the life of the collector: if a pid is
  reused by a new process during a long collector session, that new
  process's rows are symbolized against the previous, unrelated
  process's load address — reporting the wrong function names, not
  merely merging the two processes' rows.
