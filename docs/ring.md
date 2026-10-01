# Ring objects reference

A ring is a standalone shared-memory object holding one cache-line-isolated,
overwrite-oldest buffer of opaque records per dataplane worker. A module
config can link a ring by name; the ring carries no dataplane module of
its own.

## Ownership

- A ring is a standalone `cp_object` of type `"ring"`, created and deleted
  through `RingService`, hosted by the pdump module control plane.
- There is no implicit or auto-created ring: every ring exists because a
  `create` call named it.
- `create` rejects an existing name outright. Deleting a name and creating
  it again yields a fresh object with a fresh handle — a lease taken
  against the old handle never matches the new one.
- Delete is refused while a consumer holds a lease on the ring (the Go
  owner's admission gate) or while a published module config still links
  it by name (the generic `cp_object` guard every shared object gets). In
  both cases the ring stays usable and the caller retries the delete once
  the blocker clears. No production module links a ring yet, so the
  link refusal is currently reachable only from tests.

## Limitations

- After a control-plane restart, rings still published in shared memory
  are not re-adopted by `RingService`: `list` and `show` miss them,
  `delete` reports not found and `create` of the same name reports that
  it already exists. Module shutdown does not free them either. The
  fwstate map service has the same limitation; both are tracked as
  follow-up work.

## Capacity

- Capacity is per worker: each worker gets its own buffer of the
  configured size, not a shared pool.
- It must be a power of two, from the 8-byte record frame up to the
  block allocator's maximum block size, 64 MiB. Under ASan that maximum
  is 64 MiB minus two red zones, so the largest accepted capacity is
  32 MiB. Red-zone padding also pushes each allocation into the next
  size class, so in practice ASan builds already fail with an
  out-of-memory error above about 16 MiB, depending on how much of the
  owning agent's arena is still free.
- Capacity is fixed at `create` and never resized. A different capacity
  needs a new ring.
- The number of per-worker buffers is not a create-time argument and is
  not reported: it always follows the dataplane's configured worker count.
- Ring storage — each worker's buffer (capacity times worker count) plus
  the metadata array — is charged against the pdump module's agent
  memory: `memory_requirements` in the controlplane config, 16 MiB by
  default. A create that would exceed it fails.

## Overwrite semantics

- Each worker has exactly one writer, and that writer never blocks: a
  full buffer evicts whole oldest records to make room rather than
  stalling or failing the write.
- Eviction always drops whole records — it never truncates one — and
  frees space in chunks: once a record does not fit, the writer drops at
  least a chunk of the oldest records, up to the first record boundary
  past it, so at least a chunk (or the record's own length, if larger)
  is free afterwards, and publishes only the final position. The chunk
  is a sixteenth of the capacity, at most 4 KiB, rounded down to a
  multiple of 4 bytes, fixed at `create`; the records that fit in the
  freed space then write without evicting again. The writer walks the
  oldest records' frames ahead of time, one per write that needs no
  eviction, up to a chunk past the readable position, so an eviction
  finds its chunk already walked. If it ever finds a corrupt length at
  the record it means to evict next, it does not try to guess how far is
  safe to advance: it drops every published record in one step instead.
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
- A record whose declared size is below the frame or above the batch
  limit (the capacity minus the eviction chunk) is rejected outright:
  nothing is written, no index moves, and no seqno is consumed.

## Batch publication

- The writer separates committing a record from publishing it. A commit
  frames the record, stamps its seqno and advances the writer's private
  write position; readers see nothing of it yet. A publication makes
  every record committed since the previous one visible with one store
  of the published write position, and does nothing when there is none.
- Each ring has a publish batch, 8 records unless `create` asks for
  another, from 1 to 1024, fixed at `create`. The commit that brings the
  unpublished records to the publish batch publishes them itself, so a
  steady producer pays one store of the published line per batch rather
  than per record, and a reader polling that line takes it away from the
  writer's CPU at most once per batch.
- A producer still publishes once at the end of each call or burst. The
  publish batch only bounds how many records wait unpublished while the
  producer keeps writing; it never holds back the tail of a call, so a
  slow producer's last records reach readers without waiting for the
  batch to fill.
- A batch also totals at most the batch limit, the capacity minus the
  eviction chunk, in aligned record lengths. A record that would take the
  batch past it makes the writer publish the records committed so far
  first, exactly as an explicit publication does, and start a new batch
  with that record, however few records the batch holds; exceeding the
  limit is never an error. The writer reports the room left in the
  current batch, so a producer may still size its batches by it.
- A batch never evicts itself: eviction only drops published records,
  and within the limit evicting all of them always frees the whole chunk,
  so no record is lost to its own batch and seqnos stay contiguous.
- Everything below the published write position is whole records,
  complete in memory, so a reader reads right up to it.

## Metadata layout

Each worker's metadata (`struct ring_worker`) spans four cache lines of
`YANET_CACHE_LINE_SIZE` (L) bytes, so a reader polling the indices never
contends with the writer's per-record bookkeeping:

| Line (offset) | Fields (offset within the line, bytes) | Written by | Read by |
|---|---|---|---|
| Writer-private `local` (0) | `write_idx` (0), `readable_idx` (8), `published_write_idx` (16), `evict_idx` (24), `data` (32), `next_seqno` (40), `size` (44), `mask` (48), `evict_chunk` (52), `publish_batch` (56), `batch_records` (60) | writer, every record | writer; readers load `size`, `mask`, `data` once at attach |
| Guard (L) | unused | — | — |
| Published `published` (2L) | `write_idx` (0), `readable_idx` (8) | writer, release stores only | readers |
| Guard (3L) | unused | — | — |

The writer keeps its authoritative positions in the private line and
never loads the published one; the published positions are copies it
release-stores after updating its own. `published_write_idx` is its
private record of the last write position it published, the start of
the unpublished batch, `batch_records` counts that batch's records
against `publish_batch`, and `evict_idx` is the record boundary its
eviction walk has reached ahead of the next eviction. The guard lines
stop a CPU that prefetches lines in adjacent pairs from pulling a line a
reader polls together with one the writer stores to, of the same worker
or the next one in the array.

## Ordering contract

The writer (`common/ring.h`) is the sole mutator of both indices and
uses no lock and no read-modify-write. Per record:

1. If the record would take the unpublished batch past the batch limit,
   publish the batch first, exactly as step 6, before any eviction and
   any byte of the record. Rare: at most once per batch limit of bytes.
   Then read its private `write_idx` and `readable_idx` with plain loads.
   If the record fits, walk the eviction cursor over at most one more
   published record's frame (reads only) and skip to step 4.
2. Otherwise continue the walk over whole oldest published records
   locally until a chunk is free (to the batch start on a corrupt
   length), update the private `readable_idx` and publish the final
   boundary with one release store of the published `readable_idx`.
3. Issue a release fence before touching any evicted byte. Steps 2 and 3
   run only when something was evicted: once per chunk, not per record.
4. `memcpy` the frame and payload.
5. Advance the private `write_idx` (commit). If the unpublished records
   now make up the publish batch, publish them as step 6.

Per batch, on a full publish batch and at the end of each producer call:

6. Publish every committed record with one release store of the
   published `write_idx`, skipped when nothing was committed.

`readable_idx` moves once per eviction chunk, not once per evicted
record: it is not an eviction counter.

The reader (`objects/ring/bindings/go/cring`) acquire-loads both
published indices, copies into a private buffer up to the published
`write_idx`, advances its cursor with `atomic.Add`,
acquire-reloads `readable_idx` and drops the prefix that the recheck
shows was invalidated before parsing. The add and the reload must stay
atomic and in this order.

**Why it is correct.** Publication: the release store of `write_idx`
follows every frame and payload store of the batch (and of any earlier
batch) in program order, so a reader whose acquire load sees it also
sees every byte below it. The publications of steps 1 and 5 are the
same store at a point where every committed record is whole and the next
one has no byte written, so the same argument covers them.
Unpublished records lie at or past the published `write_idx`, which no
reader copies. Eviction: if the reader copied any overwritten byte, its
recheck must see the eviction that covers it. One readable store and
one fence cover a whole chunk, since all of the chunk's evicted bytes
are overwritten only after them, and the walk stops at the batch start,
so an overwrite inside the current batch only ever lands on bytes a
published eviction already invalidated. arm64: the writer's fence
orders the published `readable_idx` store before the data stores; on
the reader side the release half of the add followed by the acquire
reload keeps the copy's loads before the recheck (release→acquire is
never reordered). x86-64: TSO keeps stores after earlier stores and
loads after earlier loads, so no fence instruction is needed.

| Writer | Reader | x86-64 | arm64 |
|---|---|---|---|
| Release store of published `readable_idx`, once per chunk | Acquire recheck | `mov` / `mov` | `stlr` / `ldar` |
| Release fence, once per chunk | Release half of cursor add | none / `lock xadd` | `dmb ish` (or `dmb ishld` + `dmb ishst`) / `ldaddal` or `ldaxr`+`stlxr` |
| `memcpy` of every record of the batch | `copy` | plain | plain |
| Release store of published `write_idx`, once per batch | Acquire snapshot | `mov` / `mov` | `stlr` / `ldar` |

**Compiler dependency.** GCC and clang treat `atomic_thread_fence` as a
full compiler barrier, so the `memcpy` stores are not hoisted above it
even where it emits no instruction.

**Not claimed:** the bulk copies are ordinary accesses that race by
design; this is not ISO C data-race-free code. Correctness rests on
invalidating whole records before overwriting them and on the reader's
copy-then-recheck.

## Performance

Writer cost per record and reader throughput at 64-byte records, each
design step on top of the previous one, against the pdump writer it
replaces. "Alone" is the writer with no reader; "full reader" is one
concurrent reader per writer copying and parsing every record.

| Design step | x86-64 alone, ns | x86-64 full reader, ns | arm64 alone, ns | arm64 full reader, ns | x86-64 reader, Mrec/s | arm64 reader, Mrec/s |
|---|---|---|---|---|---|---|
| Baseline: pdump writer | 34.5 | 151 | 36.2 | 307 | 6.6 | 3.25 |
| 1. Single-writer publication, cache-line isolated workers, single readable store per eviction, eviction fence | 13.0 | 59 | 22.3 | 130 | ~17 | ~7.7 |
| 2. Writer-private and published cache lines with guard lines | 13.3 | 45 | 22.7 | 40 | — | 24.8 |
| 3. Batch publication (default 8) and chunked eviction | 12.8 | 24.6 | 23.0 | 31.6 | 40.6 | 31.6 |

x86-64: Xeon Gold 6230 under KVM, pinned to 4 vCPUs; arm64: Cortex-A76,
128-byte cache lines. 1 MiB ring, 64-byte records, unpaced, one worker;
reproduce with `build/tests/common/ring_bench` (`RING_BENCH_CPUS`,
`RING_BENCH_REPS`, `RING_BENCH_QUICK`). The numbers are indicative and
will drift with hardware, compiler and load.

## Pdump capture status

Pdump's own packet capture still writes into its private, module-local
ring buffers. It does not yet use standalone ring objects; that migration
is a later change.

## CLI examples

`yanet-cli-ring` manages ring objects through `RingService`. `--format
json` (a global flag from the shared `ync` CLI framework) switches any of
these from human-readable output to the JSON wire response.

Create a ring with a 1 MiB per-worker capacity and the default publish
batch, or one publishing every 32 records:

```bash
yanet-cli-ring create --name captures --capacity 1MiB
yanet-cli-ring create --name bursts --capacity 1MiB --publish-batch 32
```

List every registered ring, sorted by name, as a
`NAME`/`CAPACITY`/`PUBLISH BATCH` table:

```bash
yanet-cli-ring list
```

Show one ring's capacity and publish batch:

```bash
yanet-cli-ring show --name captures
```

Delete a ring:

```bash
yanet-cli-ring delete --name captures
```

`--name`/`-n` identifies the ring on every subcommand except `list`;
`--capacity` takes a size in bytes or IEC units, such as `4096`, `64KiB`
or `1MiB`; the CLI rejects a value that is not a power of two (including
decimal units such as `1MB`), and the service checks the range above
before anything is created. `--publish-batch` takes 1 to 1024 records and
defaults to the service's 8.
