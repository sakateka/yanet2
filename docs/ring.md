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
  advancing past exactly as many as the new record needs. If it ever
  finds a corrupt length at the record it means to evict next, it does
  not try to guess how far is safe to advance: it drops every record
  currently in the buffer in one step instead.
- Readers keep their own independent cursor per worker; a slow reader
  loses the records the writer evicted before it caught up, but never
  affects the writer or other readers.

## Framing and seqno

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

The writer is the sole producer for its worker and never takes a lock:

1. Invalidate whole oldest records by advancing `readable_idx` with a
   release read-modify-write (see Overwrite semantics for how far: exactly
   as far as the new record needs, or all at once on a corrupt length).
2. If anything was invalidated, issue a release fence before touching the
   evicted bytes.
3. Copy the frame and payload with an ordinary bulk `memcpy`.
4. Publish the record with a release **store** of the locally computed
   `write_idx` — not a fetch-and-add, since there is only one writer and
   nothing else can race the update.

The reader, in `objects/ring/bindings/go/cring`, keeps its own contract:
snapshot the published indices, copy the bytes into a private buffer,
advance its read cursor with an atomic add, recheck `readable_idx`, drop
whatever prefix that recheck shows was invalidated during the copy, and
only then parse what remains.

**Why the fence:** a release operation orders the accesses before it, not
the ones after it. Without step 2 a weakly ordered CPU (arm64) may make
the overwriting payload stores visible before the advanced readable
position, and a reader's recheck would accept bytes the writer has already
started overwriting. x86-64 never reorders stores with other stores, so
the fence compiles to no instruction there (only a compiler barrier); on
arm64 GCC and clang emit one `dmb ish`, executed only by records that
evicted something. pdump's own writer still lacks this fence; it is
replaced by this ring when pdump migrates.

Verified instruction selection (GCC 13 x86-64, cross GCC aarch64
`-march=armv8-a+crc` and `-mcpu=neoverse-n1`, `-O2`): x86-64 contains no
`mfence`/`sfence`, only the pre-existing `lock add` of the invalidation;
aarch64 adds exactly one `dmb ish` per eviction path, next to the
existing release read-modify-write invalidation and the `stlr` publication. This
shows code generation, not cross-core behaviour; runtime validation on
arm64 hardware is a separate check.

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
