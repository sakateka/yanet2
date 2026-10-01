# arm64 check of the ring writer eviction fence

`arm64-check.sh` checks how the ring writer publishes an eviction (one
release store of the readable position, then a release fence) and the Go
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

- `--quick` (passed to `arm64-check.sh`): a short smoke run, about 4
  minutes after the build. The default run takes about 12-18 minutes after
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

## What it checks

| Section | Verdict |
|---|---|
| preflight | Architecture, CPU model, tools. Fails if a tool is missing. |
| build | Builds with meson, generates protobufs and prints DPDK's cache line size. Fails if the C build and DPDK disagree on it. Every Go build gets that size through `CGO_CPPFLAGS`, like the Makefile. |
| correctness | `meson test ring ring_object pdump_ring` and `go test ./objects/ring/...`. |
| codegen | Builds the Go stress binary and `ring_bench` twice, with and without `-DRING_TEST_NO_EVICT_FENCE`, and disassembles the writer and the Go reader. On aarch64 the writer must publish each eviction with one `stlr` of the readable position directly followed by one release fence (`dmb ish`, or `dmb ishld` + `dmb ishst` from newer GCC), with no `ldadd`; the no-fence build must have no `dmb`. The reader's `(*shmSource).Indices` must use `ldar` for both indices, and `(*Reader).Read` must do the cursor add with `ldaddal` or an `ldaxr`/`stlxr` pair. On x86-64 everything is reported only. |
| stress | A C writer thread overwrites a small ring at full speed while the production Go reader checks every record it returns. Runs cover both builds, several ring capacities and repetitions. The fence build must return **zero torn records**. torn > 0 in the no-fence build reproduces the original bug. It is reported, but the run never fails because of it, since a reproduction is not guaranteed. |
| performance | Runs `ring_bench` for both builds with every writer and reader thread pinned to its own CPU, and prints median ns/record of both writers alone and with one concurrent reader per worker, unpaced and paced to fixed record rates (`--quick` runs a smaller size and rate matrix). The full reader is the production read protocol transcribed to C; the index-only reader issues the same index loads and cursor atomics without touching the data area. The reader table gives each reader's rate, loss and bad records. Then runs the Go reader benchmarks from both cring test binaries: a prefilled ring with no writer, and a full-speed C writer. Fails only if a benchmark fails or the fence build's reader returns a bad record. |

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
- Go reader: `(*shmSource).Indices` shows `ldar=2` on aarch64 (the
  acquire loads the snapshot and the recheck use). `(*Reader).Read`
  shows `ldaddal >= 1`, or `ldaxr` and `stlxr` without LSE; Go builds
  for plain ARMv8.0 contain both and pick one at run time. On x86-64
  expect `lock_xadd=1` and zeros elsewhere.
- Stress: `torn` is 0 for every `fence` row. `runs_torn` for the `nofence`
  rows counts the runs that reproduced the bug.
- Benchmark, writer alone: the `fence` change is the cost of the fence,
  new against new across the two builds. `new vs old` compares the new
  ring writer with the old pdump writer. `noise` compares the old writer
  between the two builds. That code is identical in both builds, so this
  column shows run-to-run noise. Read a fence change smaller than the
  noise as "no measurable cost".
- Benchmark, writer by reader: the writer's cost with no reader, a full
  reader and an index-only reader, unpaced and paced. When the index-only
  column is close to the full-reader one, the cost comes from sharing the
  index cache line with the reader, not the data lines. A paced writer
  starts one record per period of the target rate and is timed inside the
  write only, so both writers are compared while their readers keep up;
  `*` marks a writer that could not reach the rate. The last columns give
  the new writer with a full reader in the no-fence build and the fence's
  cost against it.
- Benchmark, reader: `lost` is the share of records overwritten before
  the reader reached them, and `bad` counts returned records with a wrong
  length, magic or out-of-order sequence number: it must be 0 for the new
  ring in the fence build. The old column sums both builds; the old pdump
  writer and reader have no fence in either.
- Go reader: ns/record, Mrecords/s and MB/s of the production reader over
  the real shared-memory ring, for each build; `lost%` only for the
  concurrent-writer case.

## What to send back

Send the `arm64-check-<hostname>-<date>.txt` file from the repository
root. It contains the full log and the summary.
