# hashmap2: migration state in the hash word

An experimental resizable concurrent hashmap in `src/hashmap2/`, written to see
whether the problems in [REMOVE-MISERY-HASHMAP.md](REMOVE-MISERY-HASHMAP.md)
are inherent or an artifact of one design decision.

They are an artifact. The original hashmap freezes a slot by overwriting its
**value** with `REDIRECT`, which destroys the payload. A helper therefore cannot
finish someone else's carry, so the copier is forced to install the value into
the destination *before* it can validate that the value is still live, and that
ordering is what allows a successful `remove` to be silently undone.

hashmap2 puts the migration state in the **hash** word instead, so freezing
marks a slot without erasing it. Single-word CAS only, no double-width CAS, no
allocation, no tombstones.


## Slot encoding

The topmost bit of the hash word is the `FROZEN` flag. Hashes are folded into
the remaining bits (a hash is a hash, so this is harmless, but it is not the
identity). `0` stays reserved as the unclaimed token.

| hash word    | value word | meaning
|--------------|------------|------------------------------------------------
| `NO_HASH`    | `EMPTY`    | virgin
| `FROZEN`     | `EMPTY`    | retired virgin, nothing was ever here
| `h`          | `EMPTY`    | claimed, empty or removed — **immediately reusable**
| `h`          | `V`        | live entry
| `h\|FROZEN`  | `V`        | frozen, `V` is final and still readable
| `h\|FROZEN`  | `EMPTY`    | frozen and drained

No transition ever touches both words, so nothing needs to be atomic across
them:

| transition               | word  | operation
|--------------------------|-------|-------------------------------------
| claim                    | hash  | `CAS(hash, h, NO_HASH)`
| insert / register / fill | value | `CAS(value, V, EMPTY)`
| remove                   | value | `CAS(value, EMPTY, V)`
| freeze                   | hash  | `CAS(hash, w\|FROZEN, w)`
| consume (after carry)    | value | `CAS(value, EMPTY, V)`

The value word has exactly two states, empty or live, so `NULL` is the only
reserved payload. `MULLE_CONCURRENT_INVALID_POINTER` and
`MULLE_CONCURRENT_TOMBSTONE_POINTER` are ordinary values here, which
`test/hashmap2/validation.c` checks.


## The five rules

Three of these were not in the original sketch. They were forced by
`test/hashmap2/model.c` failing, or found by reading the code afterwards.

1. **Freeze before reading the value.** The freeze CAS is the commit point.
   After it, no writer may modify the value, so the copier reads a value that
   is provably final. There is no speculative install, so a removal can never
   be undone. This is the whole point.

2. **Consume the value after carrying it.** Because a frozen slot *preserves*
   its payload, "frozen" cannot also mean "already migrated". So after a carry
   succeeds the source value is CASed back to `EMPTY`, giving the drained state
   an explicit representation. Without this, re-running `copy` over a stale
   generation reinjects every value that was legitimately removed in a newer
   generation. The original gets this for free precisely because `REDIRECT`
   destroys the payload. *This is what `model.c` caught.*

3. **Never chase forward with a value the destination already holds.** If
   `carry`'s CAS fails because the destination slot is occupied, stop. Chasing
   on would push a stale value past a newer removal in an even newer
   generation.

4. **Post-check after every value write.** A writer reads the hash word and
   then CASes the value word, so a freeze can slip in between. After a
   successful value CAS the writer re-reads the hash word; if it turned frozen
   it carries its own value forward and consumes it.

5. **Re-check the gate when a reader observes `EMPTY`.** Rule 2 makes `EMPTY`
   ambiguous: genuinely absent, or drained by a migration. `lookup` and
   `remove` therefore re-read the hash word on `EMPTY` and treat frozen as
   "retry in the newer generation".


## Scenarios: competing inserts and removes on the same key

### Without a migration in flight

**S1 — insert vs insert.** Both threads end up seeing `h` in the slot (one
claimed it, the other observed the claim). Both then CAS the value from
`EMPTY`. Exactly one wins and returns 0; the loser sees a live value and
returns `EEXIST`. Sequentially equivalent.

**S2 — insert vs remove.** `remove` CASes `V -> EMPTY`, `insert` CASes
`EMPTY -> V`. Both are single-word CASes on the *same* location, so they
serialize and the outcomes are exactly the sequential ones. Note this makes the
value word **cyclic** (`live -> empty -> live -> ...`), which the original
design had to forbid. It is safe here because `copy`'s per-slot work no longer
depends on the value word at all.

**S3 — remove vs remove.** One CAS wins and returns 0. The other observes
`EMPTY`, re-reads the gate (rule 5), finds it unfrozen, and returns `ENOENT`.

**S4 — remove then insert (reuse).** The slot keeps its hash claim, so probe
chains through it still work, and the insert is a plain `CAS(value, V, EMPTY)`.
No migration, no tombstone, no intermediate error state, and `n_hashs` is
unchanged. `test/hashmap2/simple.c` runs 1000 such cycles on one key and
asserts the table does not grow at all.

### With a migration in flight

**S5 — insert racing the freeze.** The two-word hazard.

```
writer: read hash -> h, unfrozen, proceed
copier: CAS( hash, h|FROZEN, h )   -> ok      freeze commits
copier: read value -> EMPTY                  nothing to carry
writer: CAS( value, V, EMPTY )     -> ok      lands in a retired slot
writer: post-check: hash is frozen -> carry V into next_storage, then consume
```

Nothing is lost. In the other order the copier reads `V`, carries and consumes
it, and the writer's post-check carries again — idempotent, because `carry`
never overwrites — and its consume CAS simply fails.

**S6 — remove racing the freeze.** This is the original's resurrection
scenario, and it cannot happen here.

```
remover: read hash -> h, unfrozen
copier:  CAS( hash, h|FROZEN, h )  -> ok      freeze commits
copier:  read value -> V, carry V to dst, consume  (src value -> EMPTY)
remover: CAS( value, EMPTY, V )    -> fails, old == EMPTY
remover: re-read gate -> frozen -> EBUSY -> migrate, retry, remove V in dst
```

And in the other order:

```
remover: CAS( value, EMPTY, V )    -> ok      removed
copier:  CAS( hash, h|FROZEN, h )  -> ok
copier:  read value -> EMPTY                 nothing to carry, nothing carried
remover: post-check -> frozen -> EAGAIN -> migrate, retry
         retry finds the key absent in dst -> ENOENT, but the "removed" flag
         makes the call return 0
```

Both converge on "removed". The copier can never carry a value it read before
the removal, because it only reads after committing the freeze.

**S7 — lookup racing freeze, carry and consume.**

```
reader: read hash -> h, unfrozen
copier: freeze, carry V to dst, consume  (src value -> EMPTY)
reader: read value -> EMPTY
reader: re-read gate -> frozen -> EBUSY -> migrate, retry, finds V in dst
```

Without rule 5 this reports "absent" for a live entry. **This is the invariant
the whole design rests on, and it is the price of deleting tombstones**, so it
deserves to be stated as a trade rather than a free win: a tombstone made
`EMPTY` unambiguous, consuming carried values made it ambiguous again, and rule
5 buys the distinction back with a second read on the empty branch.

It is reproducible. Defining `MULLE_CONCURRENT_HASHMAP2_RACE_YIELD` widens the
window, after which `test/hashmap2/lookup_race.c` fails at around iteration 88
against a deliberately unfixed lookup, and passes with the fix. See *Reproducing
the races* below.

**S8 — carry vs a live insert in the destination.** The destination becomes
live storage as soon as *one* thread completes a pass, so a lagging copier can
still be carrying into a generation that live threads are already writing.
`carry` CASes from `EMPTY` only, so a live insert that got there first wins and
the carry does nothing, which is correct because the live insert is newer. Rule
3 is what stops the carry from chasing on with its stale value.

**S9 — re-scanning a stale generation.** A thread may call `migrate` with a
stale `p`, in which case `copy` runs over a generation that was drained long
ago. Every slot is already frozen and every value already consumed, so the pass
is a genuine no-op. Rule 2 is what makes this true.

**S10 — two copiers on one slot.** Only one freeze CAS wins, but both read the
same final value and both carry it. Carrying is idempotent, and one of the two
consume CASes fails harmlessly.


**S11 — a writer's value loses to an already-carried one.** This was a
linearizability violation, not a cosmetic wart, and it is now fixed.

If the copier has already carried and consumed a value `V`, a writer that read
the hash word before the freeze can still CAS its own `V2` into the retired
slot. Its post-check then carries `V2` and finds the destination already holds
`V`, so `V2` is correctly dropped — but the call used to report success anyway:

```
insert( h, V2) -> 0        while lookup( h) == V     // no valid sequential history
register( h, V2, &old) -> *old == NULL ("I inserted") while V stays registered
```

Both broke their documented contracts. The fix makes `carry` return the value
that is registered afterwards, `post_check` propagate it, and the callers honour
it: `insert` returns `EEXIST`, `register` reports the real `*p_old`.

Note the returned value is the occupant **observed at the failing CAS**, not a
fresh re-read. That matters: `insert` linearizes at that observation, so
`EEXIST` is sound even if `V` is removed immediately afterwards — that removal
simply linearizes after our insert. A re-read would have needed its own
post-check; returning the observed occupant does not.

Untested until now. `test/hashmap2/register_race.c` covers it: V1 is inserted
once and never removed, a migrator forces same-size migrations, and a registrant
calls `register(KEY, V2)` in a loop. With the fix reverted, it reports
`*p_old == NULL` at iteration ~50 — a clear linearizability violation. With the
fix, it passes 200k iterations. The window is easy to hit because the consumed
slot is EMPTY for the full duration of the copier's carry-to-next-gen phase.


## Wait-freedom

Every step bound below is independent of contention.

| step | bound |
|---|---|
| probe within a generation | `<= mask + 1`, but **conditionally** — see below |
| claim | 1 read, `<= 1` CAS, 1 increment |
| freeze one slot | `<= 2` CAS attempts — the hash word only ever goes `NO_HASH -> h -> h\|FROZEN`, so at most two forward transitions can beat us |
| copy one slot | freeze + 1 read + 1 carry + 1 consume CAS = `O(1)` |
| copy one generation | `O(size)`, with no dependence on value churn |
| carry | probe + `<= 1` CAS per generation hop, hops strictly forward |
| operation retries | one generation transition per retry |

**The probe bound is not unconditional.** Migration starts at half claimed
capacity and the unused half is the reserve for threads that passed the
occupancy check concurrently, which is what normally guarantees a virgin slot
and therefore a terminating probe. But the check (`n_hashs >= max`) and the
claim's increment are *not* atomic, so with more than about `size/2` threads in
flight past the check, `n_hashs` overshoots, the table can fill completely, and
a probe for an absent hash finds neither its own hash nor a virgin slot. The
bound is then thread-count dependent, and in the worst case a thread makes no
progress at all.

This is inherited from the original hashmap, which has the same
read-then-increment gate, so it is not a hashmap2 regression — but it means the
"bounded by word width, not by thread count" claim below holds only while
`size/2` comfortably exceeds the number of concurrent writers. Small tables with
many threads are the danger zone. All five probe loops (`insert`, `register`,
`lookup`, `remove` and `carry`) carry an `assert( index != sentinel)` so that
this fails diagnosably in debug builds instead of spinning forever. Nothing
tests the overshoot itself.

The important improvement is the "copy one slot" row. In the original, the
freeze is a CAS on the *value* word, so a peer churning that value can make the
freeze lose repeatedly — scenario D of the misery document — which is why the
original needs tombstones to keep the value word monotonic. Here the freeze
targets the hash word, which has only two forward transitions ever, so value
churn cannot extend copy's work. Copy is `O(source_size)` **without** requiring
a monotonic value word, and that is precisely what allows tombstones and their
blocked reuse to be deleted.

Retries in `insert`, `register`, `remove` and `lookup` are triggered by only
two things: the claimed-slot threshold, or encountering a frozen slot. Both mean
a generation transition, and a generation transition requires peers to fill half
a table — `Ω(size)` work at a geometrically increasing price. So each retry is
peer-funded, and the number of generations is capped by the size classes: at
most **29 doublings**, from the minimum size of `2^2` up to the `2^31` ceiling
where `_mulle_concurrent_hashmap2storage_get_migration_size` aborts.

**There is no tombstone-triggered retry, and that is the point.** The
same-size-migration workaround bolted onto the original creates an invalidation
that costs a peer `O(1)` (one insert plus one remove) while costing us `O(size)`
and consuming no size class, which demotes those paths from bounded wait-free to
merely lock-free. hashmap2 has nothing analogous, because reuse is a plain value
CAS. Every retry here consumes a size class.

So: **bounded wait-free, subject to the probe caveat above**, by the same
argument as the original, minus the tombstone hole. As with the original, the
constant comes from the size cap, which is enforced by aborting rather than by
degrading.

### Memory ordering

The protocol spans two words — the freeze on the hash word must be ordered
against the value read or write — which the original's single-word protocol does
not require. That ordering is currently supplied by using the sequentially
consistent primitives throughout: `_mulle_atomic_pointer_read` is
`memory_order_seq_cst` and `__mulle_atomic_pointer_cas` is `seq_cst/seq_cst`
(verified in `mulle-thread/mulle-atomic-c11.h`).

Under a single total order over all seq_cst operations, the ordering the design
needs follows directly. The copier's freeze precedes its own value read in
program order and therefore in that total order, so the value it reads is the
post-freeze one. And a writer's post-check load of the hash word follows the
freeze in that order whenever the writer's value CAS did — which is exactly the
case the post-check exists for — so it must observe the freeze. (The post-check
does not need to be ordered against the copier's *value read*, only against the
freeze.) No ordering fence is missing, so the remaining risk here is a protocol
error, not a memory-model error.

Caveat on the evidence: the seq_cst claim rests on `mulle-atomic-c11.h` in
`mulle-thread`, an external dependency. If a platform backend ever supplies
weaker primitives under the same names, this argument fails silently.

What is genuinely open is whether these can be *relaxed* the way the rest of the
library does. That is a performance question — it is where the grow-phase
deficit lives — and it needs the analysis in
[MEMORY-ORDER-RELAXED.md](MEMORY-ORDER-RELAXED.md) before anything is loosened,
because unlike the original, relaxing here can break a cross-word invariant
rather than just a single-location one.

### Claimed slots never shrink

A removed slot keeps its hash claim, so `n_hashs` never decreases. The tombstone
cost did not vanish, it moved: from "a removed hash is unusable until migration"
to "claimed slots grow forever under key churn". A workload cycling through many
distinct keys therefore still grows the table even when the live count stays
near zero. The original behaves the same way, since its tombstones also retain
the claim.


## Measurements

Linux, release, 4 threads, `test/bench/compare.c`. Throughput goes to stderr;
stdout is kept deterministic so it can serve as the test baseline.

| phase | workload | result |
|---|---|---|
| mixed | insert 1000 / lookup 10000 / remove 100 per round, keys recycled | hashmap2 **3.2x to 4.6x faster** |
| read-heavy | fill once, then lookups only | **0.82x to 1.10x**, parity within noise |
| grow | insert only from initial size 4 | **~0.89x** |

Both containers are asserted to agree on the resulting element count, and both
end the grow phase at the same size (32768).

Reading the numbers, with the attribution kept honest:

* The mixed win comes from eliminating migration-on-reuse, but that workload is
  only *partly* the reuse path: 1000 inserts against 100 removes per round, with
  random removal targets, so a given round only re-hits some of the keys it
  removed. The effect is real, the 3.2x–4.6x is not a clean isolation of it. A
  targeted reuse microbenchmark would attribute it properly.
* read-heavy is at parity because `lookup` adds only a bit test on the hot path
  and the extra gate read happens only on the `EMPTY` branch. Caveat: hashmap2's
  loads are seq_cst against the original's `_relaxed`, which is free on x86 but
  measurable on weakly ordered targets, so parity here should not be assumed for
  ARM.
* The grow deficit is the extra per-slot CAS in `copy` (the consume) plus the
  ordered atomics. Migration is essentially the whole cost of that phase.


## Reproducing the races

The dangerous windows are two adjacent instructions wide — read the hash word,
then touch the value word — so no test hits them by luck. `hashmap2.c` therefore
carries yield points at each window, in the spirit of
`MULLE_THREAD_UNPLEASANT_RACE_YIELD`, compiled out unless
`MULLE_CONCURRENT_HASHMAP2_RACE_YIELD` is defined:

| window | scenario |
|---|---|
| `lookup`, between the hash read and the value read | S7 |
| `insert`, after the claim, before the value CAS | S5 |
| `register`, after the claim, before the value CAS | S5, S11 |
| `remove`, after the hash read, before the value CAS | S6 |
| `copy`, after the freeze and after the carry | lets writers and readers slip in |

The decision is a per-thread xorshift, deliberately **not** `rand()`: glibc's
`rand()` takes a process-global lock, which would inject a lock and a barrier at
exactly the window under observation, and would have every thread draw from one
unseeded shared sequence in lock-acquisition order. Independent lock-free
decisions perturb the timing far less. (The same criticism applies to
`MULLE_THREAD_UNPLEASANT_RACE_YIELD` upstream.)

With the yields enabled, S7 becomes a hard failure within ~100 iterations
against an unfixed `lookup`.

**Widening the window is not always the binding constraint.** For S6 it is not:
migrations there are threshold-driven, so once the table is large they become
rare and almost never coincide with a removal. Yields alone do not reproduce it.
`_mulle_concurrent_hashmap2_migrate_same_size()` exists for that reason — it
retires the current generation into a fresh one of the same size, so a probe
thread can force continuous generation changes at bounded memory cost.
`test/hashmap2/remove_migrate_race.c` uses it and reports a lost removal within
about 1500 iterations against an unfixed `remove`, which both covers S6 and
confirms that frequency rather than window width was the obstacle.


## Enumerator

`next()` re-reads `map->storage` on each call, so it never dereferences a stale
generation. The defect was subtler: `rover->index` is an offset into the
generation the enumeration started on, and applying it to a newer generation
silently skips and duplicates entries *without* reporting `ECANCELED`, because
the newer generation is not frozen. It now records the generation and compares
(pointer comparison only, never a dereference), returning `ECANCELED` when it
changes. A recycled allocation at the same address would defeat that, which is
one reason the enumerator stays "limited multi-threaded".


## Test status

| test | what it establishes |
|---|---|
| `test/hashmap2/simple.c` | basics, plus 1000 reuse cycles that do not grow the table |
| `test/hashmap2/validation.c` | argument checks, and formerly reserved pointers usable as payload |
| `test/hashmap2/model.c` | strict 4-thread model, disjoint key ranges, zero leniency |
| `test/hashmap2/remove_race.c` | S6 under threshold-driven migration, no resurrection |
| `test/hashmap2/remove_migrate_race.c` | S6 under *forced* migration — fails against an unfixed `remove` within ~1500 iterations |
| `test/hashmap2/lookup_race.c` | S7 — with race yields enabled it fails against an unfixed `lookup` within ~100 iterations |
| `test/hashmap2/register_race.c` | S11 — fails against an unfixed `register` at iteration ~50: reports *p_old==NULL while V1 is permanently live |
| `test/bench/compare.c` | the measurements above, with count agreement asserted |

For contrast, `test/hashmap/model.c` — the equivalent test against the original
— fails intermittently on `model lookup`: the model removed a key and the
container still returns its value. That is scenario A of the misery document,
observed directly.

Known gaps, in rough order of how much they should bother you:

1. **The probe bound is conditional** on `size/2` exceeding the concurrent writer
   count, and nothing tests the overshoot case. The asserts only turn it into a
   diagnosable failure.
2. **Relaxed atomics are unexplored**, so the grow-phase cost is unattributed
   between the extra CAS and the ordering. Correctness currently depends on the
   seq_cst primitives of an external dependency.
3. No `pose`/`patch` equivalent, no enumerator stress test, and no
   single-threaded teardown fuzzing.



## What is the worst case number to exhaustion ?
---------------------------

The number

~2³⁰ = 1,073,741,824 distinct-key inserts to force the  abort() , arriving after 29 successful migrations (the 30th attempt aborts).

Why distinct keys: migration triggers when  n_hashs >= size/2 , and  n_hashs  only grows when a new hash claims a slot ( claim  CASes  NO_HASH → h , then increments). Re-inserting an already-claimed hash — even after a remove — does not increment, because remove only clears the value word, leaving the hash claim in place. So within any one generation you must feed it fresh hashes to keep growing.

The sequence

┌──────────────┬──────────────────────┬───────────────┐
│ current size │ trigger (n_hashs at) │ → migrates to │
├──────────────┼──────────────────────┼───────────────┤
│ 4 (2²)       │ 2                    │ 8             │
│ 8 (2³)       │ 4                    │ 16            │
│ …            │ …                    │ …             │
│ 2³⁰          │ 2²⁹                  │ 2³¹           │
│ 2³¹          │ 2³⁰                  │ abort         │
└──────────────┴──────────────────────┴───────────────┘

- Reaching size 2³¹ (completing the 2³⁰→2³¹ migration) needs 2²⁹ = 536,870,912 distinct claims.
- Then the 2³¹ table itself must reach  n_hashs = 2³⁰  before the next migration is attempted — 2²⁹ more distinct hashes.
- Total: 2³⁰ ≈ 1.07 × 10⁹ distinct-key inserts, then the next insert/register (even a duplicate — it just reads  n >= max  and calls  migrate_storage ) hits  get_migration_size(2³¹)  →  abort() .

The count is a hard floor regardless of strategy: keep keys live or insert/remove churn them, you still need 2³⁰ different hash values, because migration drops removed slots (copy only carries live  value != EMPTY  entries, so a churned generation collapses its claim count back to the live count on the way out — meaning the final 2³¹ table needs 2³⁰ distinct hashes claimed within it, full stop).

Why this is theoretical, not reachable

- The final table is 2³¹ slots × 16 bytes = 32 GiB (48 GiB peak during the copy, old + new). Any real machine OOMs —  calloc  fails or the OOM killer fires — long before the size cap. The  abort()  is a safety ceiling, not a sequence you can actually run.
- The "29 doublings" is the 2²→2³¹ span;  get_migration_size  aborts when  size > UINT_MAX/2 , i.e. at 2³¹, so 2³¹ is the largest table ever allocated and the doubling off it is the one that dies.
- The irony: growth triggers at half capacity, so the container aborts with a table that's only half full — 2³⁰ claimed of 2³¹ slots. It never gets to fill itself.

So: bounded at ~2³⁰ user-level inserts and 29 migrations by the arithmetic, but bounded at "however much RAM you have" in practice — the memory wall hits first, usually around size 2²⁸–2³⁰ depending on the machine.