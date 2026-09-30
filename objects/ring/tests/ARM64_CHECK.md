# arm64 check of the ring writer eviction fence

`arm64-check.sh` checks the release fence the ring writer issues after it
evicts old records. Run it on a real arm64 machine. On x86-64 the fence
compiles to no instruction and stores are never reordered, so an x86 run
only checks that the harness works.

## Prerequisites

- An idle arm64 Linux box with 4 or more CPUs. Hugepages and a NIC are not
  needed.
- gcc, meson, ninja, cmake and pkg-config, plus the libraries the build
  needs: libyaml (`libyaml-dev`; pkg-config must find `yaml-0.1`), python3
  with pyelftools, and libnuma headers. Also Go 1.24+, binutils
  (`objdump`), util-linux (`taskset`, `lscpu`), git and make. The script
  lists anything missing before it starts building.
- protoc, protoc-gen-go and protoc-gen-go-grpc, but only when the
  `*.pb.go` files have not been generated yet (always true on a fresh
  clone).
- Network access on the first run: git submodules and Go modules are
  downloaded.

On a clean machine with Nix, the portable yanet2 devShell provides all of
these (its flake supports `aarch64-linux`). Copy the
`yanet2-devshell` flake directory to the box and run the commands below
inside `nix develop path:<dir>/yanet2-devshell`.

## Commands

```bash
git clone -b test/ring-arm64-check https://github.com/sakateka/yanet2.git yanet2-arm64-check
cd yanet2-arm64-check && objects/ring/tests/arm64-check.sh
```

If you already have a checkout, fetch the branch from the fork instead:
`git fetch origin test/ring-arm64-check && git checkout test/ring-arm64-check`.

Use `--quick` for a short smoke run (about 2-3 minutes after the build).
The default run takes about 10-15 minutes after the build. The first
build (DPDK and the whole tree) adds several minutes, depending on the
core count. The script reuses an existing configured `build/` and never
reconfigures it.

## What it checks

| Section | Verdict |
|---|---|
| preflight | Architecture, CPU model, tools. Fails if a tool is missing. |
| build | Builds with meson, generates protobufs and prints DPDK's cache line size. Fails if the C build and DPDK disagree on it. Every Go build gets that size through `CGO_CPPFLAGS`, like the Makefile. |
| correctness | `meson test ring ring_object pdump_ring` and `go test ./objects/ring/...`. |
| codegen | Builds the Go stress binary and `ring_bench` twice, with and without `-DRING_TEST_NO_EVICT_FENCE`, and counts barriers in the writer. On aarch64 the fence build must contain `dmb ish` and the no-fence build must have fewer barriers. |
| stress | A C writer thread overwrites a small ring at full speed while the production Go reader checks every record it returns. Runs cover both builds, several ring capacities and repetitions. The fence build must return **zero torn records**. torn > 0 in the no-fence build reproduces the original bug. It is reported, but the run never fails because of it, since a reproduction is not guaranteed. |
| performance | Runs `ring_bench` for both builds, pinned with `taskset`, and prints median ns/record. |

The script exits non-zero only when there is a real failure (build, tests,
codegen, or a torn record in the fence build).

## Reading the results

The summary at the end shows PASS, FAIL or INFO for each section, followed
by the tables:

- Barrier counts: on aarch64 the fence column is at least 1 and the
  no-fence column is lower.
- Stress: `torn` is 0 for every `fence` row. `runs_torn` for the `nofence`
  rows counts the runs that reproduced the bug.
- Benchmark: `fence%` is the cost of the fence, new(f) against new(nf).
  `new/old%` compares the new ring writer with the old pdump writer.
  `noise%` compares the old writer between the two builds. That code is
  identical in both builds, so this column shows run-to-run noise. Read a
  `fence%` smaller than the noise as "no measurable cost".

## What to send back

Send the `arm64-check-<hostname>-<date>.txt` file from the repository
root. It contains the full log and the summary.
