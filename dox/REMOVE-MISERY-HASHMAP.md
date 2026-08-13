# Remove misery (hashmap)

A design record of why `mulle_concurrent_hashmap_remove` interacts badly with
migration, what tombstones do and do not fix, and which repairs are sound.

This document supersedes the "Related API behavior" claims in
[WAIT-FREE-LOCK-FREE.md](WAIT-FREE-LOCK-FREE.md) where the two disagree.


## Verdict

`remove` can be silently undone when a migration runs concurrently. The pair is
tombstoned in the old storage, the call returns 0, and a lagging copier
installs the *old* value into the new storage afterwards. A subsequent
`lookup` finds the value again.

The defect is **pre-existing** and is not caused by tombstones. Tombstones fix
two other problems (see scenarios C and D) and never addressed this one.

Practical scope:

| concurrent with `remove`     | safe | reason
|------------------------------|------|-------------------------------------------
| `lookup`                     | yes  | never initiates a migration, only joins one
| another `remove`             | yes  | loser of the value CAS gets `ENOENT`
| `insert` / `register`        | no   | may cross the growth threshold
| `_mulle_concurrent_hashmap_pose` | no | migrates on every call

So this is not "remove is single-threaded only". Remove is safe with any number
of concurrent readers and removers. It is unsafe when something can start a
migration underneath it.


## Background: the invariants the design rests on

1. **A slot is claimed by its hash.** `claim` CASes `hash` from `NO_HASH`.
   Only operations carrying that hash may write the slot's `value`. The hash is
   never cleared, so probe chains stay intact across removals.

2. **`REDIRECT` is a freeze marker, not a "moved" marker.** `copy` writes it to
   *every* slot it passes, in all four states:

   | source slot state                      | copy's action                          | carried?
   |----------------------------------------|----------------------------------------|---------
   | `hash == NO_HASH` (virgin)             | `CAS(value, REDIRECT, NO_POINTER)`     | no
   | claimed, `value == NO_POINTER`          | `CAS(value, REDIRECT, NO_POINTER)`     | no
   | `value == TOMBSTONE`                    | `CAS(value, REDIRECT, TOMBSTONE)`      | no, dropped
   | `value == live V`                       | `put(dst,h,V)` then `CAS(REDIRECT, V)` | yes

   Freezing virgin slots too is what makes a pass airtight: after the cursor
   passes, no writer can land anything in the old storage. A racing writer's
   CAS fails, it observes `REDIRECT`, and it redoes its work in the newer
   generation.

3. **`REDIRECT` implies a strictly newer generation exists.** `put`'s
   `assert(q != p)` depends on it. This is why `remove` must never write
   `REDIRECT` as a deletion marker.

4. **`put` never overwrites.** It CASes from `NO_POINTER` only and returns 0
   either way — "stored, or dst already holds a newer value/tombstone".

5. **The value state graph is monotonic.**

   ```
   NO_POINTER -> live -> TOMBSTONE -> REDIRECT
   NO_POINTER -> live ->              REDIRECT
   NO_POINTER ->                      REDIRECT
   ```

   A failed freeze can therefore only have lost to one of finitely many
   forward transitions, which bounds `copy`'s per-slot work and makes a pass
   `O(source_size)`.

6. **The storage swap is gated on *one* completed pass, not on all copiers
   finishing.** There is no barrier. A second thread can still be scanning
   `src` after another helper has already swapped `map->storage` to `dst`, and
   its `put` will land in `dst` afterwards. This is the root of scenario A.

7. Only one migration is pending at a time. `migrate_storage` allocates only
   when `next_storage == storage`; otherwise it joins the pending migration.
   Generations are created strictly serially.


## Scenario A — remove is undone (current design, with tombstones)

`copy`'s live-slot path reads the value, puts it, then freezes.

```
copier: value = read( src[h].value )         -> V        (1)  slot is live; decision correct as of now
remove: CAS( src[h].value, TOMBSTONE, V )    -> 0        (R)  caller is told "removed"
copier: put( dst, h, V )                                 (2)  dst[h] virgin -> V is live in dst
copier: CAS( src[h].value, REDIRECT, V )     -> fails    (3)  returns TOMBSTONE
copier: loop, TOMBSTONE branch, CAS( REDIRECT, TOMBSTONE ) -> break
```

End state: `src[h]` is `REDIRECT` with the tombstone correctly dropped and
nothing carried from it — and `dst[h]` holds live `V`, installed by step (2).
After the swap, `lookup(h)` returns `V`.

The tombstone was never copied. What reached the new generation is a **stale
read** taken at (1), live then, dead by (2). `copy` commits at (2) but only
validates at (3).

The window is exactly (1)…(3), and it is not narrow: (2) is a full probe walk
in `dst` and may follow a `REDIRECT` chain into a newer generation.

Other orderings are fine:

- remove before (1) — copier reads `TOMBSTONE`, drops it, never puts.
- remove after (3) — `src[h]` is `REDIRECT`, remove returns `EBUSY`, the map
  level migrates and retries, and the remove lands in `dst`. This is the
  designed path.

`remove` gets no signal in the broken ordering: it observed a live `V`, its CAS
succeeded, and nothing tells it a copier had already latched that value.


## Scenario B — same failure without tombstones

With `remove` doing `CAS(value, NO_POINTER, V)` instead:

```
copier: value = read( src[h].value )         -> V        (1)
remove: CAS( src[h].value, NO_POINTER, V )   -> 0        (R)  "removed"
copier: put( dst, h, V )                                 (2)  dst[h] = V
copier: CAS( src[h].value, REDIRECT, V )     -> fails    (3)  returns NO_POINTER
copier: loop, NO_POINTER branch, CAS( REDIRECT, NO_POINTER ) -> break
```

Identical outcome. This is the proof that tombstones are orthogonal to
scenario A: they only change which sentinel the failed freeze observes.


## Scenario C — lost update (prevented by tombstones)

Reachable only if a removed slot is immediately reusable, i.e. without
tombstones:

```
copier: value = read( src[h].value )         -> V        (1)
copier: put( dst, h, V )                                 (2)  dst[h] = V
remove: CAS( src[h].value, NO_POINTER, V )   -> 0             src[h] empty
insert: CAS( src[h].value, V', NO_POINTER )  -> 0             src[h] = V'  (legal, slot reusable)
copier: CAS( src[h].value, REDIRECT, V )     -> fails    (3)  returns V'
copier: loop, live branch: put( dst, h, V' )             (4)  dst[h] already V
                                                              CAS from NO_POINTER fails,
                                                              put returns 0, V' is NOT carried
copier: CAS( src[h].value, REDIRECT, V' )    -> succeeds      src frozen
```

`dst[h] == V` and `V'` — the last successful insert — is gone. Invariant 4
(`put` never overwrites) makes the newer value lose to the stale one already
installed.

With tombstones this is unreachable: after the remove the slot holds
`TOMBSTONE`, so `insert(h,V')` cannot succeed in that generation.


## Scenario D — unbounded copy (prevented by tombstones)

Extend scenario C. Every time a peer does `remove(h,V); insert(h,V'')`, the
copier's freeze fails, it re-enters the live branch, and pays another probe
walk in `dst`:

```
peer:   remove + insert         O(1)
copier: put + failed freeze     O(probe in dst, may follow a REDIRECT chain)
        ... repeats indefinitely
```

`live -> NO_POINTER -> live` is a cycle, so a failed freeze carries no
progress information and per-slot work has no bound. `copy` stops being
`O(source_size)` and migration stops being wait-free.

Scenarios C and D are what tombstones bought. That is the whole of it.


## The freeze-return lemma, and why a copier-side repair still fails

If `CAS(src[h].value, REDIRECT, V)` returns `NO_POINTER` or `TOMBSTONE` rather
than `REDIRECT`, then `src[h]` is **still unfrozen at that instant**. Hence no
thread has completed a pass over `src`, hence the swap has not happened, hence
`map->storage` is still `src`, hence **no live operation can have written into
`dst` yet**. At that moment `dst` is copier-private and the only value any
copier could have put for `h` is one read from `src[h]`. A retraction
`CAS(dst[h].value, NO_POINTER, V)` would therefore be unambiguous.

The lemma is true but instantaneous, not durable:

```
copier: CAS( src[h], REDIRECT, V )       -> NO_POINTER   (3)  dst provably copier-private here
                        <-- copier descheduled -->
helper: scans src[h], CAS( REDIRECT, NO_POINTER )  -> ok       src[h] now frozen
helper: finishes its pass, CAS( &storage, dst, src ) -> ok     dst is now live storage
live:   insert( h, V )                                         legitimately lands in dst[h] = V
copier: CAS( dst[h], NO_POINTER, V )     -> succeeds            kills the legitimate insert
```

The copier cannot hold the swap back, and it cannot re-verify `src[h]` and
clear `dst[h]` atomically. Freezing first instead of retracting first does not
help: once frozen, a helper can swap immediately and the same insert can land
before the retraction.

**Therefore a fixup inside `copy` is unsound**, and this is why the obvious
"undo the put" repair must not be used.


## Why the identical fixup *is* sound inside `remove`

Linearization. A fixup performed inside `remove`, before it returns, may
clobber a concurrent `insert(h,V)` because that insert can be ordered *before*
the remove and the history stays valid:

```
insert(h,V) ; remove(h,V)   ->   absent
```

A fixup performed inside `copy` happens arbitrarily long after `remove`
already returned, so no ordering exists that makes clobbering a later insert
legal.

A fixup must also only ever match the exact value being removed. A concurrent
`register(h,V')` with `V' != V` must survive, since it has to linearize after
our remove.


## Candidate repairs

### R1 — remove plants forward (chase the frontier)

After the source CAS succeeds, walk forward and plant a tombstone in every
generation that could still receive a carry, repeating while the frontier
advances:

```
r = storage_remove( p, h, V )
if r == 0:
    seen = p
    q    = next_storage
    while q != seen:
        plant_tombstone( q, h, V )     // NO_POINTER->TOMBSTONE, or V->TOMBSTONE
        seen = q                        // follow REDIRECT down the chain
        q    = next_storage             // re-read: did the frontier move?
```

Sound by the linearization argument above. Planting pre-empts future carries,
because `put` never overwrites a non-virgin slot.

A single plant is **not** sufficient: one plant blocks one generation, and a
sufficiently lagging copier can inject into a newer one after that generation's
tombstone has been dropped by its own migration. Hence the loop.

Bounded by the number of generations, which is the same argument the rest of
the container already uses. Contained, and does not touch `copy`'s contract.

### R2 — descriptor-based freeze (the structural cure)

Make the carry and the validation the same atomic step: freeze the source slot
by publishing a descriptor holding `(hash, value, dst)` that any helper can
install into `dst`, then retire the slot to plain `REDIRECT`. Then a `put` can
only ever carry a value that was live at freeze time.

Consequences:

- Scenario A disappears; `remove` becomes a single local CAS with no forward
  work at all.
- `copy`'s per-slot work becomes one successful CAS regardless of value churn,
  so scenario D disappears **without** needing tombstones — which means
  removed slots could become immediately reusable again.
- Cost: a descriptor per frozen live slot (ABA reclamation is already
  available) and a rewrite of `copy` plus every `REDIRECT` consumer. Value
  tagging is not an option, since the API accepts arbitrary `void *`.

### R3 — restrict usage

Document remove as unsafe against concurrent migration and leave the code
alone. Callers either size the map so it never grows, quiesce around removes,
or never remove in multi-threaded phases.

Note that R3 interacts badly with the same-size tombstone migration below,
which makes remove-then-reinsert itself a migration trigger.


## Related: same-size migration breaks the bounded-wait-free claim

`insert`/`register` currently respond to a tombstoned slot by running a
*same-size* migration to drop the tombstone, then retrying. That restores
immediate reuse of a removed hash, but it does not consume a size class, so
the "bounded by remaining capacity doublings" argument does not cover it:

| invalidation                | peer cost              | consumes a size class
|-----------------------------|------------------------|----------------------
| growth threshold reached    | `O(size)`, doubling    | yes
| our slot frozen by a copier | `O(size)`, doubling    | yes
| tombstone hit               | `O(1)` insert + remove | **no**

A single peer looping `insert(h); remove(h)` pays `O(1)` per cycle to make our
`insert(h)` pay `O(size)` and retry, and the table never grows, so the `~31`
generation ceiling never engages. That path is lock-free but not *bounded*
wait-free.

Cheap repair: escalate. First tombstone hit in a call → same-size migration;
any subsequent hit in the same call → growing migration. Every retry after the
first then consumes a size class and the operation is bounded again, with the
memory cost paid only under the pattern that provokes it.


## Bound sources, for the record

The number of generations an operation can traverse is bounded by the size
classes, not by the thread count:

- Concurrently pending migrations are bounded by **1** (invariant 7), which is
  stronger than the thread count.
- A *single* thread can nonetheless create arbitrarily many generations by
  inserting and migrating repeatedly. What limits it is that generation `k`
  costs `Ω(2^k)` work to fill, plus the hard `abort()` above `UINT_MAX/2` in
  `_mulle_concurrent_hashmapstorage_get_migration_size`.
- An operation does not walk the chain on retry: it re-reads `map->storage`
  and lands on the newest generation, skipping intermediates.

So plain wait-freedom rests on "each hop is peer-funded at a geometrically
increasing price". *Bounded* wait-freedom additionally rests on the size cap,
which is enforced by aborting rather than by degrading.
