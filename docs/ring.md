# Ring objects reference

A ring is a standalone shared-memory object holding one cache-line-isolated,
overwrite-oldest buffer of opaque records per dataplane worker. Other
configs reference a ring by name; it carries no dataplane module of its
own.

## Ownership

- A ring is a standalone `cp_object` of type `"ring"`, created and deleted
  through `RingService`, hosted by the pdump module control plane.
- There is no implicit or auto-created ring: every ring exists because a
  `create` call named it.
- `create` rejects an existing name outright. Deleting a name and creating
  it again yields a fresh object with a fresh handle — a lease or link
  taken against the old handle never matches the new one.
- Delete is refused while a consumer holds a lease on the ring (the Go
  owner's admission gate) or while a published module config still links
  it by name (the generic `cp_object` guard every shared object gets). In
  both cases the ring stays usable and the caller retries the delete once
  the blocker clears.

## Capacity

- Capacity is per worker: each worker gets its own buffer of the
  configured size, not a shared pool.
- It must be a power of two, from the 8-byte record frame up to the
  block allocator's maximum block size, 64 MiB. Under ASan, every
  allocation request is padded by a red zone that pushes it into the
  next power-of-two size class, so a capacity can pass this check and
  still fail the actual allocation with an out-of-memory error; 16 MiB
  is the practical ceiling observed in ASan builds, not a fixed limit —
  the actual cutoff also depends on how much of the owning agent's arena
  is still free.
- Capacity is fixed at `create` and never resized. A different capacity
  needs a new ring.
- The worker count is not a create-time argument: it always follows the
  dataplane's configured worker count.
- Ring storage — each worker's buffer (capacity times worker count) plus
  the metadata array — is charged against the pdump module's agent
  memory: `memory_requirements` in the controlplane config, 16 MB by
  default. A create that would exceed it fails.

## Overwrite semantics

- Each worker has exactly one writer, and that writer never blocks: a
  full buffer evicts whole oldest records to make room rather than
  stalling or failing the write.
- Eviction always drops whole records — it never truncates one —
  advancing past exactly as many as the new record needs, and publishes
  only the final position. If it ever
  finds a corrupt length at the record it means to evict next, it does
  not try to guess how far is safe to advance: it drops every record
  currently in the buffer in one step instead.
- Readers keep their own independent cursor per worker; a slow reader
  loses the records the writer evicted before it caught up, but never
  affects the writer or other readers.

## Framing and seqno

- The frame is little-endian, which the Go reader decodes it as; the C
  header refuses to compile on a big-endian target.
- Every record starts with an 8-byte frame, `{total_len: u32, seqno:
  u32}`, followed by its opaque payload. `total_len` covers the frame and
  the payload together.
- Records start on a 4-byte boundary; a private payload header (for
  example a future capture header) still has to decode itself safely,
  since only the 4-byte alignment is guaranteed.
- `seqno` is a per-worker u32 counter assigned at commit and incremented
  for every accepted record, wrapping from `0xFFFFFFFF` back to `0`
  without a gap.
- A record whose declared size is below the frame or above the buffer's
  capacity is rejected outright: nothing is written, no index moves, and
  no seqno is consumed.

## Ordering contract

The writer is the sole producer for its worker and never takes a lock or
issues a read-modify-write on either index:

1. Read its own `write_idx` and `readable_idx` with relaxed loads: nothing
   else writes them, so coherence alone returns its latest stores.
2. If the new record does not fit, walk whole oldest records locally to
   the first boundary that leaves room (or to `write_idx` on a corrupt
   length, see Overwrite semantics), then publish that final position
   with one release **store** of `readable_idx`.
3. Issue a release fence, still before touching any evicted byte. Steps 2
   and 3 run only when something was evicted.
4. Copy the frame and payload with an ordinary bulk `memcpy`.
5. Publish the record with a release store of the locally computed
   `write_idx`.

`readable_idx` advances once per eviction, not once per evicted record,
so it is not an eviction counter: a reader only needs the final boundary
visible before any byte it covers is overwritten, and intermediate
boundaries add nothing. Loss statistics need a dedicated counter.

The reader, in `objects/ring/bindings/go/cring`, keeps its own contract:
acquire-load both indices, copy the bytes into a private buffer, advance
its read cursor with `atomic.Add`, reload `readable_idx` with an acquire
load, drop whatever prefix that recheck shows was invalidated during the
copy, and only then parse what remains. Both the atomic add and the
atomic reload are load-bearing on arm64; neither may become a plain
access, and the recheck may not move above the add.

**Happens-before on arm64.** Writer: `stlr readable` → `dmb` → data
stores. The fence orders the `readable_idx` store before every later
store, so any reader that observes an overwritten byte is guaranteed to
observe the new `readable_idx` afterwards. Reader: data loads → the
release half of `atomic.Add` (`ldaddal`, or `ldaxr`/`stlxr` without LSE)
→ `ldar readable`. Go atomics are sequentially consistent (RCsc), so an
acquire load after a release operation is never reordered above it: the
copy's loads complete before the recheck. If a copied byte was
overwritten, the recheck sees the eviction that covers it and drops that
prefix.

**Happens-before on x86-64.** TSO never reorders a store with an earlier
store or a load with an earlier load, so the writer's `readable_idx`
store precedes its data stores and the reader's copy precedes its
recheck without any fence instruction; the `lock xadd` of the cursor
add is a full barrier anyway.

| Writer operation | Reader operation | x86-64 | arm64 |
|---|---|---|---|
| Relaxed load of own indices | — | `mov` | `ldr` |
| Release store of final `readable_idx` | Acquire recheck of `readable_idx` | `mov` / `mov` | `stlr` / `ldar` |
| Release fence before overwriting | Release half of cursor `atomic.Add` | none (compiler barrier) / `lock xadd` | `dmb ish` (GCC 16: `dmb ishld` + `dmb ishst`) / `ldaddal` or `ldaxr`+`stlxr` |
| Bulk `memcpy` of frame and payload | Bulk `copy` into a private buffer | plain stores / loads | plain stores / loads |
| Release store of `write_idx` | Acquire snapshot of `write_idx` | `mov` / `mov` | `stlr` / `ldar` |

**Why the fence:** a release store orders the accesses before it, not the
ones after it. Without step 3 a weakly ordered CPU (arm64) may make the
overwriting payload stores visible before the advanced readable position,
and a reader's recheck would accept bytes the writer has already started
overwriting. The fence costs one barrier per evicting prepare on arm64,
none on x86-64. pdump's own writer still lacks this fence; it is replaced
by this ring when pdump migrates.

**Compiler dependency:** GCC and clang treat `atomic_thread_fence` as a
full compiler barrier, so the plain `memcpy` stores are never hoisted
above it even on x86-64 where it emits no instruction. The arm64 check
harness (`objects/ring/tests/arm64-check.sh`) disassembles the writer and
the Go reader to guard this and the instruction selection above.

Verified instruction selection (GCC 13 x86-64 `-O2`; cross GCC 16
aarch64 `-mcpu=neoverse-n1` and `-march=armv8-a+crc`, `-O2`): x86-64
has no `mfence`/`sfence` and no `lock`-prefixed instruction in the
writer; aarch64 has one `stlr` of `readable_idx` followed by one release
fence, shared by the normal and corrupt-length eviction paths, no
`ldadd`, and the `stlr` publication of `write_idx`. This shows code
generation, not cross-core behaviour; runtime validation on arm64
hardware is the harness's stress section.

**What this does not claim:** the bulk `memcpy` and the reader's plain
byte reads are ordinary, non-atomic accesses racing the writer's
concurrent copy by design — this is not portable ISO C data-race-free
code. Correctness rests on whole-record invalidation before the copy and
the reader's copy-then-recheck discarding any prefix that invalidation
touched, not on the copy itself being race-free.

## Pdump capture status

Pdump's own packet capture still writes into its private, module-local
ring buffers. It does not yet use standalone ring objects; that migration
is a later change.

## CLI examples

`yanet-cli-ring` manages ring objects through `RingService`. `--format
json` (a global flag from the shared `ync` CLI framework) switches any of
these from human-readable output to the JSON wire response.

Create a ring with a 1 MiB per-worker capacity:

```bash
yanet-cli-ring create --name captures --capacity 1MiB
```

List every registered ring, printed as a `NAME`/`CAPACITY`/`WORKERS` table:

```bash
yanet-cli-ring list
```

Show one ring's capacity and worker count:

```bash
yanet-cli-ring show --name captures
```

Delete a ring:

```bash
yanet-cli-ring delete --name captures
```

`--name`/`-n` identifies the ring on every subcommand except `list`;
`--capacity` takes a size such as `1MiB`, `4096`, or `64KiB` and is
checked against the power-of-two range above before anything is created.
