# Wait-free status of the concurrent structures

This document covers the hashmap first, then the pointerset and pointerarray
sections at the end.

`mulle_concurrent_hashmap_lookup`, `_insert` and `_register` are **wait-free**:
each completes in a bounded number of its own steps, and the bound does not
depend on how many other threads are running or on who wins individual races.

`count`, `lookup_any` and enumeration are **not** covered. They are weakly
consistent snapshots and may restart without bound (see *Not covered* below).

> This document describes the hashmap in `src/hashmap/`. The value word carries
> the freeze marker here, which is what costs the hashmap its multi-threaded
> `remove`. The `src/hashtable/` variant puts the marker in the hash word
> instead and buys `remove` back for one reserved hash bit — see
> [HASHTABLE.md](HASHTABLE.md) and the design comparison at the end.


## 1. The slot protocol

Every entry is a `{ hash, value }` pair of two atomic words. The **hash word is
the claim token**, and it is the only word a slot contender writes in order to
take ownership.

```text
                claim: CAS(hash, h, NO_HASH)
  +---------------+                       +-------------------+
  | hash:  NO_HASH|---------------------->| hash:  h          |
  | value: NULL   |                       | value: NULL       |
  | (virgin)      |                       | (claimed, empty)  |
  +---------------+                       +-------------------+
                                                   |
                                                   | fill: CAS(value, V, NULL)
                                                   |
                                                   v
                                          +-------------------+
                                          | hash:  h          |
                                          | value: V   (live) |
                                          +-------------------+
```

The invariant is: **a slot advertises its owner before it holds a payload.**
Since the hash word never moves backwards, a slot can never be claimed by two
different hashes during the lifetime of one storage generation, so a thread can
never store its value under a foreign hash and can never be handed a foreign
key's value.

Winning the claim does not make that thread the exclusive value writer. If it
stalls before filling, a same-hash peer may install its value and complete; a
different-hash peer probes onward; a migration may freeze the empty value to
`REDIRECT`. **No operation ever waits for the claimer.** That is what makes the
protocol wait-free rather than merely lock-free.

`_mulle_concurrent_hashmapstorage_claim` is one hash read, at most one hash CAS
and one counter increment. `_mulle_concurrent_hashmapstorage_fill` is one value
CAS. Neither retries.

`fill` observes one of three states:

| State | Result |
|-------|--------|
| `NO_POINTER` | install the payload, report insertion |
| live value | return it (`register`) / `EEXIST` (`insert`) |
| `REDIRECT` | `EBUSY`: help migration and retry in the newer generation |

There is no `TOMBSTONE` state. See §6.


## 2. The freeze-word law

This is the constraint that decides the whole design:

> **The freeze marker must live in a word with a bounded number of forward
> transitions.** Otherwise the freeze CAS in `copy` can lose an unbounded
> number of times, and copying a generation is no longer bounded.

The hashmap freezes by writing `REDIRECT` into the **value** word, which also
holds arbitrary caller payloads. So the value word has to be made artificially
monotonic. It is:

```text
  NO_POINTER  ----->  live V  ----->  REDIRECT
```

Three states, two forward transitions, and no concurrent path moves a value
backwards:

* `fill` and `put` CAS **from `NO_POINTER` only**.
* `copy` only ever writes `REDIRECT`.
* `_mulle_concurrent_hashmap_patch` / `_remove` do write `NULL` over a live
  value, but they are **single-threaded only** (relaxed, non-atomic in effect).
  They are the only `live -> NULL` edge in the library.

Therefore a losing freeze CAS can only have lost to one of two forward
transitions, and the per-slot work in `copy` is at most three CAS attempts.

The historical route to getting this wrong is worth recording, because it is
subtle. Adding multi-threaded `remove` forces a `TOMBSTONE` state to keep the
word monotonic, and then either horn is bad:

```text
  freeze marker in the VALUE word
        |
        v
  value word must be monotonic (or copy is unbounded)
        |
        v
  a removed slot cannot return to "empty"  ==>  TOMBSTONE
        |
        +--> never reuse it        ==> removed key un-insertable until a doubling
        |
        +--> reuse it in place     ==> value word no longer monotonic
                                       ==> NOT WAIT-FREE
```

The hashmap escapes by **not having a multi-threaded `remove`**, which is why
the value word is already monotonic without tombstones. The reason `remove` had
to go is in [REMOVE-MISERY-HASHMAP.md](REMOVE-MISERY-HASHMAP.md); it is not a
progress problem but a correctness one.

Single-threaded `patch`/`remove` leave a slot as `{ h, NULL }`. That state is
harmless under a hash-first claim: a foreign key probing it sees `h` and moves
on, and the hash owner may legally fill it again from `NULL`.


## 3. Copying a generation

`copy` walks each source slot **once** and freezes every slot it passes, in
every state. Freezing unconditionally is the only mechanism that catches a
writer still holding a retired storage pointer, because `insert`/`register`
never re-validate that their cached storage is still `map->storage`.

| source slot state | copy's action | carried? |
|-------------------|---------------|----------|
| `hash == NO_HASH` (virgin) | `CAS(value, REDIRECT, NO_POINTER)` | no |
| claimed, `value == NO_POINTER` | `CAS(value, REDIRECT, NO_POINTER)` | no |
| `value == live V` | `put(dst, h, V)`, then `CAS(value, REDIRECT, V)` | yes |
| `value == REDIRECT` | nothing, already frozen | — |

A racing writer whose slot was frozen gets `REDIRECT` back from its value CAS,
returns `EBUSY`, and redoes its work in the newer generation. Nothing is
stranded behind the cursor and nothing lands in a dead generation.

The virgin case needs care. If `CAS(value, REDIRECT, NO_POINTER)` loses to a
racing fill, the returned live value is **not** discarded; copy reloads the hash
— which hash-first publication guarantees is now visible — and runs the pair
through the normal live path.

> **Memory-order requirement.** That reload is correct only because both CASes
> are `seq_cst`. The writer's hash CAS precedes its value CAS in the total
> order; our value CAS observed the writer's value, so it follows the writer's
> hash CAS; our subsequent hash read follows our own value CAS, hence sees the
> published hash. Relaxing either CAS breaks this. See
> [MEMORY-ORDER-RELAXED.md](MEMORY-ORDER-RELAXED.md).

`put` is the only writer into the destination and it obeys two rules:

1. **Never overwrite.** It CASes from `NO_POINTER` only and returns success
   either way — "stored, or the destination already holds a newer value". A
   copier cannot tell which of two live payloads is newer
   ([POSEAS-PATCH.md](POSEAS-PATCH.md) F2), so it must not try.
2. **Chase forward on `REDIRECT`.** If the destination is itself already being
   migrated away, `put` follows `map->next_storage` and retries there. Each hop
   is to a strictly larger generation, so the recursion depth is bounded by the
   number of remaining doublings, not by contention.


## 4. Why the outer retries are bounded

Migration **always doubles** (`size * 2` in
`_mulle_concurrent_hashmap_migrate_storage`). There is no same-size migration.
At the representable limit it aborts rather than letting the size wrap. So the
number of storage generations that can ever exist is the number of power-of-two
size classes: at most 29, from the minimum of 4 up to `2^31`.

```text
insert(map, h, V)
   |
   v
 retry: <-------------------------------------+
   |  p = read(map->storage)                  |
   |                                          |
   |  n_hashs >= max ?  --- yes --> migrate(p)+
   |        | no                              |
   |        v                                 |
   |  storage_insert(p, h, V)                 |
   |        |                                 |
   |        +-- EBUSY (REDIRECT or probe  ----+
   |        |          exhausted)
   |        +-- 0 | EEXIST --> return
```

The load-bearing fact is that **every retry advances a generation**.
`migrate_storage(map, p)` ends with `CAS(&map->storage.pointer, q, p)`. Either
it succeeds, and `map->storage` is now `q != p`; or it fails, which means
`map->storage` had already moved off `p`. Since `map->storage` only ever
advances, on return `map->storage != p` — so the next `read(map->storage)` sees
a strictly newer, strictly larger generation.

Retries are therefore capped by the generation count, **independent of thread
count**. Each retry is also peer-funded: a generation transition requires peers
to fill half a table, `Omega(size)` work at a geometrically rising price.

ABA reclamation matters here: it prevents a retired generation from being
recycled and reappearing as a current one, which would break the "only
advances" argument.


## 5. The bound, construct by construct

| Construct | Bound | Why |
|---|---|---|
| `_hashmapstorage_claim` | 1 read + 1 CAS | hash word is write-once `NO_HASH -> h` |
| `_hashmapstorage_fill` | 1 CAS | no retry |
| probe loop in `lookup`, `register`, `insert`, `put` | `mask + 1` | migration starts at half capacity, so the reserve preserves a virgin slot; and the sentinel exit **returns a value** (see §6) |
| `copy` per-slot loop | ≤ 3 CAS attempts | value word has 3 states and 2 forward transitions (§2) |
| `copy` outer loop | `mask + 1` | one pass, each slot visited once |
| `put` `REDIRECT` recursion | ≤ generations left | `next_storage` only advances; each hop is strictly larger |
| `migrate_storage` | 1 alloc + 2 CAS + 1 `copy` | no loop of its own |
| outer `retry` in `lookup`, `register`, `insert` | ≤ 29 | every retry advances a generation (§4) |

Composed: `O(generations * size_max)` steps, with no term in the number of
threads. That is wait-freedom.


## 6. Probe exhaustion

The occupancy gate is deliberately **not** atomic with the claim — making it
atomic would put a contended reservation counter on the fast path:

```text
  read:       n_hashs >= max ?
                  | no
                  v
  claim:      CAS the hash word, then increment n_hashs
              ^
              +--- these two are not one atomic step
```

With `k` threads past the gate, `n_hashs` can reach `max - 1 + k`. Since
`max = size - size/2`, more than about `size/2` concurrent writers can fill a
table completely. A full table plus an absent key satisfies neither probe exit
condition, so **every probe loop must have a real exit, not an `assert`.** They
do:

| function | on probe exhaustion | caller's response |
|---|---|---|
| `_hashmapstorage_lookup` | returns `REDIRECT_VALUE` | migrate and retry — the degenerate full-table reader *grows the table* instead of spinning |
| `_hashmapstorage_register` | returns `INVALID_POINTER` | migrate and retry |
| `_hashmapstorage_insert` | returns `EBUSY` | migrate and retry |
| `_hashmapstorage_put` | returns `EBUSY` | **see the known defect below** |
| `_hashmap_patch` | returns `ENOENT` | single-threaded, no retry |

### Known defect: `copy` drops a value when `put` exhausts its probe

`copy` ignores `put`'s return value and freezes the source slot regardless. If
`put` returned `EBUSY` because the destination was full, the value is in neither
generation — a **silent lost insert**. This is a correctness defect, not a
progress defect; the step bound above still holds.

Reachability is narrow. Direct writers can only reach `dst` after
`map->storage == dst`, which requires a completed copy pass, so
`n_hashs(dst) >= live(src)` before any of them arrive. The gate then admits at
most `size(src) - live(src) - 1 + k` more claims, for a total of
`size(src) - 1 + k`. Filling all `2 * size(src)` slots therefore needs
`k > size(dst)/2` threads simultaneously past the occupancy gate — plausible on
a 4-slot table with 8 threads, unreachable once the table has grown.

The fix is for `put` to grow the destination and chase, rather than for `copy`
to drop. Until then, treat the probe bound as thread-count dependent: it is
`mask + 1` steps, but *correct* only while concurrent writers stay below
`size/2`.


## 7. Not covered by the guarantee

`mulle_concurrent_hashmap_count`, `lookup_any` and the enumerator are weakly
consistent snapshots. `count` restarts its whole enumeration on `ECANCELED`,
with no bound on the number of restarts, and an enumeration may be cancelled by
any concurrent mutation. They are diagnostics and heuristics, not point
operations, and they are explicitly outside the wait-free claim.

`init` and `done` are single-threaded. `_hashmap_patch` and `_hashmap_remove`
are single-threaded. Allocation is fail-fast: `_mulle_allocator_abort` ends the
process, so a failed allocation never produces retries.


## 8. The two designs, for reference

```text
  hashmap                              hashtable
  -------                              ---------
  freeze by overwriting the VALUE      freeze by setting a bit in the HASH
  with REDIRECT (INTPTR_MIN)           word (top bit, or bit 0 in even mode)

  => payload is DESTROYED by the       => payload SURVIVES the freeze
     freeze
  => a helper cannot finish someone    => any helper can complete the carry
     else's carry
  => the copier must install into      => the copier reads the value AFTER
     the destination BEFORE it can        committing the freeze, so the value
     validate that the value is           it carries is provably final
     still live
  => that ordering is what lets a      => a removal can never be undone
     successful remove be undone

  cost: none                           cost: one reserved hash bit
  hash: full intptr_t                        (63 usable bits on LP64)
  no multi-threaded remove             multi-threaded remove
```

Both satisfy the freeze-word law of §2; they differ only in which word carries
the marker, and therefore in what they can offer on top. See
[HASHTABLE.md](HASHTABLE.md).


# Pointerset (EXPERIMENTAL, insert/register/member only)

`mulle_concurrent_pointerset_insert`, `_register` and `_member` are
wait-free under the same allocation/reclamation assumption as above.

- **Probe loops have a real exit.** The occupancy gate is not atomic with the
  slot claim, so an arbitrary number of in-flight claimers can fill the storage
  while `n_used` is still below the threshold. Every probe loop is bounded by
  `mask + 1`: absent `member` returns 0, full `register` returns
  `INVALID_POINTER` (migrate and retry). There is no `assert` used for
  termination.
- **Copy freezes virgin slots.** An insert holding a retired storage observes
  `REDIRECT`, returns `EBUSY` and retries in the newer generation. A copy pass
  is one scan of `size` slots with a bounded inner loop.
- **Retries advance a generation.** Migration always doubles (`assert` plus an
  explicit overflow abort) and a stale `migrate_storage(set, p)` returns
  immediately when `storage != p`. Every `EBUSY` or load-threshold retry
  therefore observes a strictly newer, strictly larger generation; the retry
  count is bounded by the number of size classes.
- **Memory order.** Pointers are published with the seq_cst pointer primitives
  for slot claims, carries, redirects, slot reads and for
  `next_storage`/`storage`. Publication of a successor therefore orders every
  helper's slot write before a reader that acquire-loads `storage`. A tuned
  release/acquire chain is possible but must keep that property.

The set is **insert-only**: there is no `remove` and no tombstone state.
`count`/enumeration are snapshots outside the guarantee; `init`/`done`/`reset`
are single-threaded.


# Pointerarray

`mulle_concurrent_pointerarray_add`, `_get` and `_get_count` are wait-free
under the allocation/reclamation assumption.

- **Helping publication.** A writer claims `entries[i]` and then publishes the
  count. A peer that finds the slot occupied advances the count itself
  (`_publish()` advances over the whole contiguous filled prefix). The count
  CAS is the gate, so it is never bumped twice. Each `add` iteration either
  returns or leaves `n` strictly higher, bounding the loop by capacity.
- **Copy** processes source slots in increasing order, freezes each one,
  carries live values, and publishes the destination prefix before the
  `storage` CAS. A stale `migrate_storage(array, p)` returns immediately when
  `storage != p`, and migration always doubles with an explicit overflow
  abort, so retries are bounded by the size classes.
- **Destination retirement.** `put` follows `next_storage` if the destination
  slot is frozen, so a slow copier cannot write into a dead generation.
- **Memory order.** Slot and `n` CASes are seq_cst; `_publish` and the
  destination observations in `copy` acquire the values they publish; `get`
  and `get_count` acquire-load `n` before reading entries; storage publication
  and consumption are release/acquire.

The in-table algorithm is wait-free; growth is wait-free only if allocation and
reclamation are. Pre-size the array to avoid growth if that matters.
