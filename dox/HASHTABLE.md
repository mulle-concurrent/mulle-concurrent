# hashtable: freeze the hash, seal the value

> **EXPERIMENTAL.** The migration protocol has had several race bugs so far,
> The current design is soak-tested and exhaustively schedule-checked on small
> scenarios (see *Test status*), but not under weak memory ordering. Prefer
> the hashmap if you can live without a concurrent `remove`.

A resizable, wait-free concurrent hashmap with a concurrent `remove`, in
`src/hashtable/`. The hash *is* the key. It exists because the original hashmap
cannot support `remove` (see [REMOVE-MISERY-HASHMAP.md](REMOVE-MISERY-HASHMAP.md)):
it freezes a slot by overwriting the value with `REDIRECT`, which destroys the
payload and forces a speculative install that a concurrent `remove` can undo.

The design follows the state machine of Cliff Click's NonBlockingHashMap, but
marks states with reserved bits instead of boxing every value, so there is no
allocation per write, and only single-word CAS is used.

> **History.** An earlier version froze only the hash word and let writers
> "post-check" afterwards. That was wrong: a writer that read the hash before
> the freeze could still change the value after the copier read it, and a single
> global `next_storage` let a stalled writer skip a generation. Values were lost,
> removals resurrected and `register` misreported `*p_old`. See
> `BUGREPORT-hashtable-migration-races.md` and the deterministic reproducer
> `test/hashtable/migration_stall.c`.


## Modes and reserved values

Both words reserve one bit. Where that bit sits is chosen at `init`, separately
for hashes and for values:

| | positive | even |
|---|---|---|
| **hash** (FROZEN bit) | top bit; legal hashes `[1, INTPTR_MAX]` (**default**) | bit 0; legal hashes even, non-zero |
| **value** (SEALED bit, TOMB) | top bit, TOMB = `MULLE_CONCURRENT_TOMBSTONE_POINTER`; legal values `[1, INTPTR_MAX - 1]` | bit 0, TOMB = `MULLE_CONCURRENT_INVALID_POINTER`; legal values even, non-NULL, not TOMB (**default**) |

```c
mulle_concurrent_hashtable_init( &map, size, allocator);        // positive hash, even value
mulle_concurrent_hashtable_init_even( &map, size, allocator);   // even hash, even value
mulle_concurrent_hashtable_init_with_mode( &map, size,
      MULLE_CONCURRENT_HASHTABLE_EVEN_HASH | MULLE_CONCURRENT_HASHTABLE_POSITIVE_VALUE,
      allocator);
```

Use even mode for aligned pointers (full address range on every platform) and
positive mode for integers. In positive hash mode, pointer-as-hash is safe on
LP64 (user space never sets bit 63) but **not** on ILP32.

Illegal hashes and values are rejected, never folded: `insert`, `register`
and `remove` return `EINVAL`, and `lookup` returns `NULL`. Because `NULL` also
means "absent", use `mulle_concurrent_hashtable_hash_is_valid()` and
`mulle_concurrent_hashtable_value_is_valid()` when the difference matters.


## Slot states

Per generation:

| hash word | value word | meaning |
|---|---|---|
| `NO_HASH` | `EMPTY` | virgin |
| `FROZEN` | `EMPTY` | retired virgin, can no longer be claimed |
| `h` | `EMPTY` | claimed, never held a value |
| `h` | `V` | live |
| `h` | `TOMB` | removed, reusable by `insert`/`register` |
| `h(\|FROZEN)` | `x\|SEALED` | retired, `x` (`EMPTY`, `V` or `TOMB`) is final and readable |

| transition | word | operation |
|---|---|---|
| claim | hash | `CAS( hash, h, NO_HASH)` |
| insert / register | value | `CAS( value, V, EMPTY or TOMB)` |
| remove | value | `CAS( value, TOMB, V)` |
| freeze | hash | `CAS( hash, w\|FROZEN, w)` |
| seal | value | `CAS( value, x\|SEALED, x)` |
| carry | value of the successor | `CAS( value, V, EMPTY)`, only from `EMPTY` |

Each generation has its own `next` pointer to its successor. There is no
map-wide "next storage".


## The rules

1. **Freeze, then seal.** The copier freezes the hash word first. A writer that
   reads a frozen hash helps migrate and retries, so after the freeze only
   writers that are already in flight can touch the value. Then the copier
   seals the value word. That makes every later writer CAS fail.
2. **The seal is the commit point.** After it, the value is final. Every helper
   reads the same value and carries the same value.
3. **Carry only into `EMPTY`.** A destination value word that has ever held a
   value is never `EMPTY` again, because removal writes `TOMB`. So a lagging
   copier cannot reinject a value that was carried and then removed or
   replaced. Re-running `copy` over a stale generation is therefore harmless.
4. **Follow `p->next`, never "the newest".** A thread that stalls across
   several migrations helps finish the migration right after its own
   generation. It never skips a generation that readers are still using.
5. **One value CAS settles a write.** If it fails because the slot is sealed,
   the operation has not happened: return `EBUSY`, migrate and retry. If it
   succeeds, the slot was not yet sealed, so the copier will carry the result
   (or the `TOMB`). There is no post-check and no "redo the remove".
6. **Lookup strips the seal.** A sealed slot still holds its value, so a
   lookup never retries and never helps migrate. It returns `NULL` for `EMPTY`
   and for `TOMB`.


## Scenarios

**Insert or remove racing the copy of the same slot.** The writer's value CAS
and the copier's seal CAS target the same word, so they serialize:

```
writer CAS first  -> copier seals the new state and carries it (or nothing for TOMB)
seal first        -> writer CAS fails on SEALED -> EBUSY -> migrate -> retry in q
```

Either way, the result is the sequential one.

**A writer stalled across two migrations** (the old lost-insert / S11 bug).
The writer claims a slot in p and stalls. p→q completes and q→r starts. The
writer's value CAS fails, because p's slot is sealed. It then helps
`p->next == q`, which is already done, and retries in the current storage. It
never writes into r behind q's back.

**A stale copier** (the old resurrection / S6 bug). Copier A seals V in p and
stalls before carrying it. A helper finishes p→q, and the owner removes V in q
(`V → TOMB`). A then carries into q, finds `TOMB` rather than `EMPTY`, and does
nothing.

**Two copiers on one slot.** Only one seal wins, but both read the same sealed
value. Only one carry can succeed against `EMPTY`. The other is a no-op.

**Reuse after remove.** The slot keeps its hash claim and its value word is
`TOMB`, so a plain `CAS( value, V, TOMB)` reuses it. There is no migration and
`n_hashs` does not change. `test/hashtable/simple.c` runs 1000 cycles on one key
and checks the table does not grow.


## Progress

| step | bound |
|---|---|
| freeze one slot | `<= 2` CAS: the hash word only goes `NO_HASH -> h -> h\|FROZEN` |
| seal one slot | `<= 1 +` number of in-flight writers. After the freeze no new writer starts on the slot, and each in-flight writer does at most one value CAS |
| carry | probe + 1 CAS per generation, following `next` strictly forward |
| lookup | one probe, no retries |
| insert / remove | one value CAS per generation; retries only on a generation change |
| register | as insert, but on a lost CAS it re-reads in order to report the winner. Concurrent insert/remove churn on the *same* key can repeat that, so register is lock-free rather than wait-free under that churn |

Retries are triggered only by a generation change. That costs peers `Ω(size)`
work, or happens through the explicit `_mulle_concurrent_hashtable_migrate_same_size()`
test hook. The probe caveats (the occupancy check and the claim are not atomic,
so a table can overshoot) are unchanged from the earlier design. `insert`
recovers with `EBUSY`; the other probe loops still `assert`.

**Memory ordering.** All atomics used here are seq_cst: `_mulle_atomic_pointer_read`
and `__mulle_atomic_pointer_cas` under the C11 backend. The protocol needs no
cross-word ordering for correctness of the value. Freeze, seal, writer CAS and
carry are each single-word, and the seal plus the writer CAS on the same word
decide the outcome. The hash freeze only limits how many writers can race the
seal. Relaxing anything still needs the analysis in
[MEMORY-ORDER-RELAXED.md](MEMORY-ORDER-RELAXED.md).

**Claimed slots never shrink** within a generation, because a removed slot keeps
its claim. Migration carries live values only, so the claim count collapses to
the live count on the way out.


## Measurements

Linux, release, 4 threads, `test/bench/compare.c`. Throughput goes to stderr;
stdout is kept deterministic so it can serve as the test baseline.

| phase | workload | result |
|---|---|---|
| mixed | insert 1000 / lookup 10000 / remove 100 per round, keys recycled | hashtable **3.2x to 4.6x faster** |
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
  and the extra gate read happens only on the `EMPTY` branch. Caveat: hashtable's
  loads are seq_cst against the original's `_relaxed`, which is free on x86 but
  measurable on weakly ordered targets, so parity here should not be assumed for
  ARM.
* The grow deficit is the extra per-slot CAS in `copy` (the consume) plus the
  ordered atomics. Migration is essentially the whole cost of that phase.



> These numbers predate the seal redesign (copy now does a seal CAS per slot
> instead of a consume CAS) and `test/bench/compare.c` currently compares two
> hashtables, so treat them as historical until re-measured against the hashmap.


## Reproducing the races

Each `hashtable_race_point( HASHTABLE_RACE_*)` in the source defaults to
`hashtable_race_yield()`. That compiles to nothing unless
`MULLE_CONCURRENT_HASHTABLE_RACE_YIELD` is defined, in which case it becomes
`MULLE_THREAD_UNPLEASANT_RACE_YIELD()` (a random yield or nanosleep).

| point | where |
|---|---|
| `LOOKUP` | lookup, between hash read and value read |
| `INSERT`, `REGISTER` | after the claim, before the value CAS |
| `REMOVE` | after the hash read, before the value CAS |
| `COPY_FROZEN` | copy, frozen, not sealed yet |
| `COPY_READ` | copy, sealed, not carried yet |
| `COPY_CARRIED` | copy, carried |

`test/hashtable/migration_stall.c` `#include`s the source with
`hashtable_race_point()` redefined, parks threads at exact points, and so forces
the failing interleavings deterministically, on any number of cores.

The probabilistic tests force continuous generation changes with
`_mulle_concurrent_hashtable_migrate_same_size()`. Soak them with
`MULLE_TEST_ITERATIONS=<n>`. Every thread that stays registered with mulle-aba
must check in often. A registered thread that only waits (e.g. main in
`join`) blocks all reclamation, and memory grows by one storage per migration.


## Test status

| test | what it establishes |
|---|---|
| `simple.c` | basics, register, 1000 reuse cycles without growth, enumeration |
| `validation.c` | argument checks, both hash modes, both value modes, reserved values |
| `model.c` | strict 4-thread model, disjoint key ranges |
| `migration_stall.c` | the three bug-report interleavings, deterministically (failed on the old design) |
| `remove_race.c` | removes under threshold-driven migration |
| `remove_migrate_race.c` | insert/lookup/remove/lookup by an exclusive owner under forced migration |
| `register_race.c` | `register` never reports `*p_old == NULL` while V1 is live, under forced migration |
| `lookup_race.c` | lookups under insert/remove churn and migration |
| `probe_overflow.c` | 8 threads × 50k inserts into a 4-slot table |

Soak on a 64-core box: 4 + 4 processes of `remove_migrate_race` and
`register_race`.

* **Old design** (`RACE_YIELD`): 50 of 64 processes failed within 60 s.
* **New design:** clean with `RACE_YIELD` for 300 s, clean on a plain gcc `-O2`
  build for 2·10⁹ iterations, and clean on tcc (mintomic backend) for 300 s.

### Exhaustive schedule exploration

`test/hashtable/explore.c` (and `test/hashmap/explore.c` for the hashmap) run
small scenarios, 2–3 threads with a few operations each on a 4 slot table,
under *every* interleaving of the atomic operations up to a preemption bound.
The scheduler is `test/explore/explore.h`, a small stateless model checker in
the style of CHESS: every atomic operation is a scheduling point, only one
thread runs at a time, and a depth-first search over the scheduling decisions
replays the scenario until all alternatives are exhausted. After each
execution it checks that the history of results is linearizable against a
sequential map, that `count` and the enumerator agree with `lookup` at
quiescence, and that no block leaked or was freed twice. A failure prints the
history and the schedule.

| scenario | covers |
|---|---|
| `insert_vs_2_migrations`, `register_vs_2_migrations` | RC1: writer stalled across p→q→r |
| `remove_vs_copier` | RC2: stale copier vs. remove |
| `remove_reinsert_churn`, `remove_remove_migrate` | remove/insert/register vs. migration |
| `insert_insert_migrate`, `register_register_remove` | same-key contention |
| `growth_vs_remove`, `growth_vs_growth` | real growth 4→8(→16) with collisions |

Results (default bound runs in `mulle-sde test`, about 4 s):

* **Old design** (git HEAD before the fix): 7 of 9 scenarios fail, among them
  RC1 with 2 preemptions and RC2 with 2. `growth_vs_growth` loses a removal
  with a single preemption.
* **New design:** all pass at the default bound of 2–3 preemptions (90k executions), with
  ≤ 3 everywhere (1.4M executions, 53 s), and `remove_vs_copier` with ≤ 5.

`EXPLORE_PREEMPTIONS=n` deepens the search, `EXPLORE_SCENARIO=name` selects
one scenario.

Known gaps:

1. Exploration assumes sequential consistency. All atomics used are `seq_cst`
   except the acquire loads in lookup, which no store precedes, so nothing is
   lost for the current code, but weak memory (GenMC) is not checked and any
   future relaxation of orders must be.
2. The scenarios are small and the preemption bound is finite: it is a proof
   for those scenarios, not for all programs.
3. `register` is lock-free, not wait-free, under same-key churn (see *Progress*).

## Enumerator

`next()` re-reads `map->storage` on each call, so it never dereferences a stale
generation. The defect was subtler: `rover->index` is an offset into the
generation the enumeration started on, and applying it to a newer generation
silently skips and duplicates entries *without* reporting `ECANCELED`, because
the newer generation is not frozen. It now records the generation and compares
(pointer comparison only, never a dereference), returning `ECANCELED` when it
changes. A recycled allocation at the same address would defeat that, which is
one reason the enumerator stays "limited multi-threaded".



## What is the worst case number to exhaustion ?
---------------------------

The number

~2³⁰ = 1,073,741,824 distinct-key inserts to force the  abort() , arriving after 29 successful migrations (the 30th attempt aborts).

Why distinct keys: migration triggers when  n_hashs >= size/2 , and  n_hashs  only grows when a new hash claims a slot ( claim  CASes  NO_HASH → h , then increments). Re-inserting an already-claimed hash — even after a remove — does not increment, because remove only sets the value word to TOMB, leaving the hash claim in place. So within any one generation you must feed it fresh hashes to keep growing.

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

The count is a hard floor regardless of strategy: keep keys live or insert/remove churn them, you still need 2³⁰ different hash values, because migration drops removed slots (copy only carries live values (not EMPTY, not TOMB), so a churned generation collapses its claim count back to the live count on the way out — meaning the final 2³¹ table needs 2³⁰ distinct hashes claimed within it, full stop).

Why this is theoretical, not reachable

- The final table is 2³¹ slots × 16 bytes = 32 GiB (48 GiB peak during the copy, old + new). Any real machine OOMs —  calloc  fails or the OOM killer fires — long before the size cap. The  abort()  is a safety ceiling, not a sequence you can actually run.
- The "29 doublings" is the 2²→2³¹ span;  get_migration_size  aborts when  size > UINT_MAX/2 , i.e. at 2³¹, so 2³¹ is the largest table ever allocated and the doubling off it is the one that dies.
- The irony: growth triggers at half capacity, so the container aborts with a table that's only half full — 2³⁰ claimed of 2³¹ slots. It never gets to fill itself.

So: bounded at ~2³⁰ user-level inserts and 29 migrations by the arithmetic, but bounded at "however much RAM you have" in practice — the memory wall hits first, usually around size 2²⁸–2³⁰ depending on the machine.
