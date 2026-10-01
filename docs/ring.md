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

The writer (`common/ring.h`) is the sole mutator of both indices and
uses no lock and no read-modify-write:

1. Read its own `write_idx` and `readable_idx` with relaxed loads.
2. If the record does not fit, walk whole oldest records locally (to
   `write_idx` on a corrupt length) and publish the final boundary with
   one release store of `readable_idx`.
3. Issue a release fence before touching any evicted byte. Steps 2 and 3
   run only when something was evicted.
4. `memcpy` the frame and payload.
5. Publish the record with a release store of `write_idx`.

`readable_idx` moves once per eviction, not once per evicted record: it
is not an eviction counter.

The reader (`objects/ring/bindings/go/cring`) acquire-loads both
indices, copies into a private buffer, advances its cursor with
`atomic.Add`, acquire-reloads `readable_idx` and drops the prefix that
the recheck shows was invalidated before parsing. The add and the reload
must stay atomic and in this order.

**Why it is correct.** If the reader copied any overwritten byte, its
recheck must see the eviction that covers it. arm64: the writer's fence
orders the `readable_idx` store before the data stores; on the reader
side the release half of the add followed by the acquire reload keeps
the copy's loads before the recheck (release→acquire is never
reordered). x86-64: TSO keeps stores after earlier stores and loads
after earlier loads, so no fence instruction is needed.

| Writer | Reader | x86-64 | arm64 |
|---|---|---|---|
| Release store of `readable_idx` | Acquire recheck | `mov` / `mov` | `stlr` / `ldar` |
| Release fence | Release half of cursor add | none / `lock xadd` | `dmb ish` (or `dmb ishld` + `dmb ishst`) / `ldaddal` or `ldaxr`+`stlxr` |
| `memcpy` | `copy` | plain | plain |
| Release store of `write_idx` | Acquire snapshot | `mov` / `mov` | `stlr` / `ldar` |

**Compiler dependency.** GCC and clang treat `atomic_thread_fence` as a
full compiler barrier, so the `memcpy` stores are not hoisted above it
even where it emits no instruction.

**Not claimed:** the bulk copies are ordinary accesses that race by
design; this is not ISO C data-race-free code. Correctness rests on
invalidating whole records before overwriting them and on the reader's
copy-then-recheck.

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

List every registered ring, sorted by name, as a `NAME`/`CAPACITY` table:

```bash
yanet-cli-ring list
```

Show one ring's capacity:

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
before anything is created.
