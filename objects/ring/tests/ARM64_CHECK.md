# arm64 check of the ring writer eviction fence

`arm64-check.sh` checks how the ring writer publishes an eviction chunk
(one release store of the readable position, then a release fence) and a
batch of records (one release store of the write position), and the Go
reader's matching atomics. Run it on a real arm64 machine. On x86-64 the
fence compiles to no instruction and stores are never reordered, so an x86 run
only checks that the harness works.

## One command

On an idle arm64 Linux box with 4 or more CPUs (hugepages and a NIC are
not needed), with git, curl and sudo:

```bash
git clone -b test/ring-arm64-check https://github.com/sakateka/yanet2.git yanet2-arm64-check && cd yanet2-arm64-check && objects/ring/tests/arm64-check-nix.sh
```

`arm64-check-nix.sh` does three things:

1. If `nix` is not installed, it explains what the official multi-user
   installer does and asks before running
   `sh <(curl --proto '=https' --tlsv1.2 -L https://nixos.org/nix/install) --daemon`.
   The installer needs sudo, creates `/nix`, the `nixbld` users and the
   `nix-daemon` service, and asks its own questions. The script then
   loads `/nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh` and
   continues in the same run. An existing Nix install is used as is.
2. It enters the devShell pinned in `devshell/flake.nix` and
   `devshell/flake.lock` (gcc 13 as in CI, meson 1.9.1, Go 1.25, cargo,
   protoc and its Go plugins, python3 with pyelftools, libyaml, numactl,
   rdma-core, binutils, util-linux). Flakes are enabled with
   `--extra-experimental-features 'nix-command flakes'` for this command
   only; `nix.conf` is not edited. The shell starts from an empty
   environment, so nothing from the host toolchain or library paths leaks
   into the build.
3. It runs `arm64-check.sh` in that shell with the remaining arguments.

Options:

- `--quick` (passed to `arm64-check.sh`): a short smoke run, about 3
  minutes after the build. The default run takes about 10 minutes after
  the build.
- `--yes`: install Nix without asking (for unattended runs; without a
  terminal the script refuses to install unless `--yes` is given).
- `--dry-run`: print the installer and `nix develop` commands instead of
  running them.

First-run time on top of the check: the Nix install takes 1-3 minutes;
the first devShell entry downloads the pinned nixpkgs source and about
600 MiB of binaries from cache.nixos.org (a few minutes, nothing is
compiled); the first build fetches the git submodules and Go modules and
compiles DPDK and the tree (several minutes, depending on the core
count). Later runs reuse all of it.

The report lands in `arm64-check-<hostname>-<date>.txt` in the repository
root.

If you already have a checkout, fetch the branch from the fork instead:
`git fetch https://github.com/sakateka/yanet2.git test/ring-arm64-check && git checkout -B test/ring-arm64-check FETCH_HEAD`.

## Without Nix

`arm64-check.sh` also runs directly on a host that has the tools:

- gcc, meson, ninja, cmake, pkg-config, flex and bison, plus the libraries
  the build needs: libyaml (`libyaml-dev`; pkg-config must find
  `yaml-0.1`), python3 with pyelftools, libnuma headers and rdma-core
  (`libibverbs-dev`, for the DPDK mlx5 drivers). cargo and rustc build the
  regex archive `lib/counters` links. Also Go
  1.24.13+, binutils (`objdump`), util-linux (`taskset`, `lscpu`), git and
  make. The script lists anything missing before it starts building.
- protoc, protoc-gen-go and protoc-gen-go-grpc, but only when the
  `*.pb.go` files have not been generated yet (always true on a fresh
  clone).
- Network access on the first run: git submodules and Go modules are
  downloaded.

```bash
objects/ring/tests/arm64-check.sh [--quick]
```

The script reuses an existing configured `build/` and never reconfigures
it.

## How the no-fence build is made

The writer's fence lives in `ring_evict_fence()` in `common/ring.h`. On
this branch only, `-DRING_TEST_NO_EVICT_FENCE` compiles it out to
reproduce the pre-fence writer. The ring code reaches Go through two
meson-built archives, `libring_objects.a` and `libringtest_writer.a` (the
C writer of the Go tests), so the no-fence variant needs its own meson
build:

- `build-nofence/` (gitignored) is configured once with the options
  `build/` was configured with (`build/meson-private/cmd_line.txt`) plus
  the knob in `c_args`, by the same meson. Only `ring_objects`,
  `ringtest_writer` and `ring_bench` are compiled there. The script then
  checks in its `compile_commands.json` that their sources carry the knob
  and the cache line size of `build/`; remove `build-nofence/` if it
  refuses one left from another configuration.
- The no-fence Go test binary is built with
  `CGO_LDFLAGS=-L$PWD/build-nofence/objects/ring/tests -L$PWD/build-nofence/objects/ring/api`:
  go puts `CGO_LDFLAGS` before the packages' own `#cgo LDFLAGS`, so these
  archives win over the ones in `build/` (`libconfig_cp.a` still comes from
  `build/`). `CGO_CFLAGS` gets the knob too, for the cgo preambles.
- Each Go build's `CGO_CPPFLAGS` carries a hash of the ring headers and of
  the two archives it links, so the Go build cache never serves a test
  binary linked against another build's or an older archive.

By hand, for example:

```bash
meson setup build-nofence -Dc_args=-DRING_TEST_NO_EVICT_FENCE
meson compile -C build-nofence objects/ring/api/ring_objects objects/ring/tests/ringtest_writer tests/common/ring_bench
CGO_CFLAGS="$(go env CGO_CFLAGS) -DRING_TEST_NO_EVICT_FENCE" \
CGO_LDFLAGS="-L$PWD/build-nofence/objects/ring/tests -L$PWD/build-nofence/objects/ring/api" \
	go test -count=1 -run 'Stress' ./objects/ring/bindings/go/cring
```

## What it checks

| Section | Verdict |
|---|---|
| preflight | Architecture, CPU model, tools. Fails if a tool is missing. |
| build | Builds with meson, generates protobufs and prints DPDK's cache line size. Fails if the C build and DPDK disagree on it. Every Go build gets that size through `CGO_CPPFLAGS`, like the Makefile. |
| correctness | `meson test ring ring_object pdump_ring` and `go test ./objects/ring/...`. |
| codegen | Builds the cring Go test binary and takes `ring_bench` from both builds, and disassembles the writer and the Go reader. The writer functions are `ringtest_stress_run`/`ringtest_commit_record` (cring) and `writer_thread`/`writer_write` (ring_bench), plus `ring_worker_prepare`/`ring_worker_evict` when not inlined. On every architecture, the stress writer of each cring binary must match its own build's `libringtest_writer.a` and not the other build's, which proves the no-fence binary linked the no-fence archive. On aarch64 the writer must publish each eviction chunk with one `stlr` of the readable position directly followed by one release fence (`dmb ish`, or `dmb ishld` + `dmb ishst` from newer GCC), with no `ldadd`; the no-fence build must have no `dmb`. A probe of one producer call (commit records, publish at the end) must hold one to three publication `stlr` (explicit, full publish batch, full batch limit) plus the eviction's, and one fence right after the eviction's. The reader's `(*shmSource).Indices` must use `ldar` for both indices, and `(*Reader).Read` must do the cursor add with `ldaddal` or an `ldaxr`/`stlxr` pair. On x86-64 everything is reported only. |
| stress | `Test_Reader_Stress_ConcurrentWriterNeverTears`: a C writer thread overwrites a 4 KiB ring with the default publish batch at full speed while the production Go reader checks every record it returns; one iteration writes a fixed record count. The test has no knobs, so a run repeats it with `-test.count`, calibrated to `RING_CHECK_STRESS_SECONDS` (or `RING_CHECK_STRESS_COUNT`), for both builds and several runs. The fence build must return **zero torn records**. torn > 0 in the no-fence build reproduces the original bug. It is reported, but the run never fails because of it, since a reproduction is not guaranteed. |
| performance | Runs `ring_bench` of both builds, alternating, one run per invocation, with every writer and reader thread pinned to its own CPU (`RING_CHECK_BENCH_CPUS`, passed as `RING_BENCH_CPUS`), parses its two tables and prints medians over the invocations: writer ns/record per publish batch (1, 8, 32) for a no-overflow ring, a 1 MiB evicting ring, and the 1 MiB ring with a full reader per writer, fence against no-fence; and the full reader's Mrec/s and lost % per batch. Then runs the Go reader benchmark (prefilled ring, no writer) from the fence build. Fails only if a benchmark fails or the fence build's reader returns a bad record. |

The script exits non-zero only when there is a real failure (build, tests,
codegen on aarch64, or a torn record in the fence build).

## Reading the results

The summary at the end shows PASS, FAIL or INFO for each section, followed
by the tables:

- Writer profile, one line per binary and build:
  `fences=F after_stlr=S dmb=D ldadd=L lock=K`. On aarch64 the `fence`
  rows need `F >= 1`, `S == F` (every fence right after the single
  release store of the readable position) and `L == 0` (no
  read-modify-write on it); the `nofence` rows need `D == 0`. `lock`
  counts x86-64 lock-prefixed instructions and is 0 there too.
- Stress writer against the archives: each build reads `matches its own
  build's archive`. `unverified` means the linker rewrote an instruction;
  `matches the ... build's archive` is a harness failure.
- Go reader: `(*shmSource).Indices` shows `ldar=2` on aarch64 (the
  acquire loads the snapshot and the recheck use). `(*Reader).read`
  shows `ldaddal >= 1`, or `ldaxr` and `stlxr` without LSE; Go builds
  for plain ARMv8.0 contain both and pick one at run time. On x86-64
  expect `lock_xadd=1` and zeros elsewhere.
- Stress: `torn` is 0 for the `fence` row. `iters_torn` for the
  `nofence` row counts the test iterations that reproduced the bug.
- Writer cost: per publish batch `bN`, the fence build, the no-fence build
  and `fence %`, the fence build against the no-fence build of the same
  cell. The `no-overflow` rows never evict, so their fence % is noise and
  shows the measurement's spread; `1 MiB` is the eviction cost alone, and
  `1 MiB+reader` adds a reader sharing the published index and data lines.
- Reader: Mrec/s per reader and `lost %`, the share of committed records
  overwritten before the reader reached them, for both builds. The
  bad-record line counts returned records with a wrong length or
  out-of-order sequence number and corrupt reads: it must be 0 in the
  fence build.
- Go reader: ns/record, Mrecords/s and MB/s of the production reader over
  a prefilled shared-memory ring.

## What to send back

Send the `arm64-check-<hostname>-<date>.txt` file from the repository
root. It contains the full log and the summary.
