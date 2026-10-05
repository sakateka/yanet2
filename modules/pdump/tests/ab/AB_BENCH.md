# pdump BEFORE/AFTER capture benchmark

`pdump-ab.sh` measures the cost of one call to `pdump_handle_packets`
(the real handler, through `new_module_pdump()`, not a reproduction of
it) BEFORE this story's ring rewrite, with its own private per-worker
`ring_buffer` (per-record `fetch_add` eviction and publish), and AFTER
it, with the shared ring object (batched eviction and publish). A C
mirror of the production Go reader may run concurrently on its own CPU,
busy-spinning, the same way a `pdump` client does. Run it on a real
arm64 machine for the numbers that matter: `ring_worker`'s cache-line
alignment is 128 bytes there against 64 on x86-64, and the ring's
batching changes how often the writer's stores cross CPUs.

## One command

On an idle Linux box with 2 or more CPUs (hugepages and a NIC are not
needed), with git, curl and sudo:

```bash
git clone -b test/pdump-bench-arm64 https://github.com/sakateka/yanet2.git yanet2-pdump-bench && cd yanet2-pdump-bench && modules/pdump/tests/ab/pdump-ab-nix.sh --cpus "6,7"
```

`pdump-ab-nix.sh` installs Nix if it is missing (asks first, needs
sudo), enters the devShell pinned in `devshell/flake.nix` and
`flake.lock` (gcc 13, meson 1.9.1, Go 1.25, cargo, protoc and its Go
plugins, cmake, flex and bison for the vendored libpcap submodule,
libpcap itself for DPDK's `rte_bpf_convert`, python3 with pyelftools, libyaml, numactl, rdma-core, util-linux), from
an empty environment so nothing on the host leaks into the build, and
runs `pdump-ab.sh` there with the remaining arguments. `--yes` installs
Nix without asking; `--dry-run` prints the commands instead of running
them.

If you already have a checkout, fetch the branch from the fork instead:
`git fetch https://github.com/sakateka/yanet2.git test/pdump-bench-arm64 && git checkout -B test/pdump-bench-arm64 FETCH_HEAD`.

## Without Nix

`pdump-ab.sh` also runs directly on a host that has gcc, meson, ninja,
cmake, pkg-config, flex, bison, go, cargo/rustc, protoc with its Go
plugins (only needed once, before `*.pb.go` is generated), libyaml
(`libyaml-dev`; pkg-config must find `yaml-0.1`), libpcap (`libpcap-dev`;
DPDK enables `rte_bpf_convert`, which the filter rows need, only when
pkg-config finds `libpcap`), python3, taskset
(util-linux) and git, with network access on the first run for the git
submodules:

```bash
modules/pdump/tests/ab/pdump-ab.sh [--cpus "W,R"] [--quick]
```

## Options

- `--cpus "W,R"`: writer and reader CPU. Default: the first two CPUs
  this process is allowed to run on.
- `--launches N` (default 5), `--reps N` (default 10): independent
  process launches and timed repetitions per launch, per matrix cell.
- `--quick`: one launch, three repetitions, a smaller matrix. For
  checking the harness works, not for numbers to act on.
- `--before REV` (default `a3c5fbdf389542781b6b80dd7c0dce0c9ae6043f`,
  the commit before this story): the BEFORE revision.
- `--out FILE`: report path (default `pdump-ab-<host>-<date>.txt` in the
  repository root).

BEFORE and this checkout's HEAD (AFTER) are each built in a detached git
worktree under `.pdump-ab/` in the repository root; reruns reuse them
and only rebuild `pdump_ab_bench`.

## CPU pair

Pick two cores of the same CPU cluster, so the writer and the busy-spun
reader share an L2 and move cache lines between them at local, not
cross-cluster, latency: on a Radxa CM5/Orion O6-class board the
performance cluster (Cortex-A76) is CPUs 6-7 and the efficiency cluster
(Cortex-A55) is CPUs 2-3, so `--cpus "6,7"` is the number that matters
for a production pdump deployment, and `--cpus "2,3"` shows the same
comparison on the slower cluster. On a generic x86-64 box, two
hyperthread siblings or two cores of the same socket both work; the
default (the first two allowed CPUs) is a reasonable choice there.

## Runtime

The default matrix is 20 cells (2 ring sizes x 2 reader states x 4
packet sizes, plus 4 half-filter rows) x 3 variants (BEFORE, AFTER batch
8, AFTER batch 64) x 5 launches x 10 reps x 100 ms: about 5-10 minutes of
measurement after the build, plus the in-tree `pdump_bench` run (well
under a minute). The first run on a given box also builds DPDK twice
(once per worktree), 5-15 minutes depending on core count; reruns reuse
both worktrees and their `build-ab/`, rebuilding only
`pdump_ab_bench.c`. `--quick` finishes in under a minute after the
build.

## Report

Lands in `pdump-ab-<hostname>-<date>.txt` in the repository root (never
committed; see `.gitignore`). It opens with the hostname, `uname -m`,
the `lscpu` model name, the cache line size from both `getconf
LEVEL1_DCACHE_LINESIZE` and the `YANET_CACHE_LINE_SIZE` the AFTER build
actually used, the CPU pair and both revisions, then the handler matrix
(median of launch medians, `[min..max]` of launch medians, AFTER's delta
from BEFORE in percent for each publish batch) and the in-tree
`meson test --benchmark pdump_bench` output on the same CPU pair.

## Caveat

Under some VMs and sandboxes (noisy neighbours, a hypervisor that steals
a vCPU mid-run), the reader-on cells come out bimodal: most launches
land in a tight cluster and a minority land far above it, which widens
`[min..max]` without moving the median much. That is the host's
scheduling, not a difference between BEFORE and AFTER; trust the median
and treat a `[min..max]` spread much larger than the other cells in the
same row as a sign to rerun with more launches or on quieter hardware,
not as a benchmark finding.
