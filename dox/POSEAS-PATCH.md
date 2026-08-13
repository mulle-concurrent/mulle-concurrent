# Replacing an existing value: why `pose` exists and `patch` had to go

`mulle_concurrent_hashmap_patch()` used to be the way to change the value of an
existing entry. It was a single CAS on the slot in the currently published
storage, retried across migrations. It reported success, and that success was a
lie: a concurrent migration could throw the write away *after* `patch` returned
`0`.

This document proves that the lie is not fixable by repair, that a general
"change this value" operation is not implementable over this data structure at
all, and that the narrow **poseAs** contract *is* implementable — but only by
building the next storage generation instead of writing into the current one.
That is `mulle_concurrent_hashmap_pose()`.

## The two facts everything follows from

**F1 — copying into a new generation is write-once.**
`_mulle_concurrent_hashmapstorage_put()` fills a destination slot with
`CAS(&entry->value, value, MULLE_CONCURRENT_NO_POINTER)`. If the slot is
already occupied it does nothing and returns success ("stored, or dst already
holds a newer value/tombstone"). Whoever writes a destination slot *first*
wins it for that generation.

**F2 — live values carry no ordering information.**
An entry is `{hash, value}`. `value` is a single atomic word holding an opaque
caller payload (or one of the three reserved sentinels). There is no version,
sequence number or timestamp anywhere in the entry. Therefore: given two
distinct live payloads `A` and `B`, **no thread can determine which one is the
newer write.** There is no input from which to compute it.

F2 is the load-bearing one. Two decisions in the code need exactly the
knowledge F2 denies:

* migration, when its `put` finds the destination slot already occupied —
  keep what is there, or overwrite it?
* a repairing writer, looking at a later generation — is this value stale, or
  did somebody legitimately move past me?

## Failure 1: the old `patch`, and why "keep" loses writes

Migration copies a slot in two separate steps: `put` the value into the
destination, then freeze the source with
`CAS(&p->value, REDIRECT_VALUE, value)`. If the freeze fails, it re-reads and
loops. Those steps are not atomic with respect to a `patch` CAS.

Key `K` holds `A`. **P** patches `A → B`. **M** is any thread helping a
migration `p → q`.

| # | Who | Action | p.slot[K] | q.slot[K] | published |
|---|-----|--------|-----------|-----------|-----------|
| 1 | M | in the copy loop, reads the slot: `value = A`, now held in a **register** | A | empty | p |
| 2 | M | is descheduled here, before its `put` | A | empty | p |
| 3 | P | `CAS(p.slot[K], B, A)` succeeds → **`patch` returns 0** | **B** | empty | p |
| 4 | M | wakes, still holding `A`, `put(q,K,A)` → slot empty → **succeeds** | B | **A** | p |
| 5 | M | freeze `CAS(p.slot[K], REDIRECT, A)` → fails (slot is B), loops with `value = B` | B | A | p |
| 6 | M | `put(q,K,B)` → slot occupied → **does nothing** (F1) | B | A | p |
| 7 | M | freeze `CAS(p.slot[K], REDIRECT, B)` → succeeds | REDIRECT | A | p |
| 8 | M | finishes the pass, `CAS(&storage, q, p)` → publishes | REDIRECT | A | **q** |
| 9 | any | `lookup(K)` → **A**. P's write is gone. | | A | q |

Step 6 is F1 in action. The destination kept the value that arrived first, and
at step 4 that was the stale one.

## Failure 2: "overwrite" loses writes too, in the other direction

The obvious fix is to let migration's `put` overwrite an occupied destination
slot with its own value. That repairs Failure 1 and immediately introduces its
mirror image, because of F2: a migrator's value is a *snapshot* read at an
arbitrary earlier time, and it cannot tell that its snapshot is the older one.

Two migrators **M1**, **M2** of the same generation, plus poser **P**:

| # | Who | Action | p.slot[K] | q.slot[K] |
|---|-----|--------|-----------|-----------|
| 1 | M1 | reads the slot: `value = A` (early), then stalls | A | empty |
| 2 | P | `CAS(p.slot[K], B, A)` succeeds | B | empty |
| 3 | M2 | reads `B`, `put(q,K,B)` → slot empty → succeeds | B | **B** |
| 4 | M1 | wakes, `put(q,K,A)`, now overwriting: `CAS(q.slot[K], A, B)` → **succeeds** | B | **A** |

`B` is destroyed by a thread whose only crime was being slow. This was measured,
not theorised: with the overwrite variant the punishing test went from ~44 lost
writes per run to ~1. A probability shift, not a fix.

## Failure 3: no amount of repair after the CAS is enough

So let the writer repair: CAS in place, then verify with `lookup()`, and
re-apply while the stale value is still visible. This is strictly better and
still wrong, for a reason that no implementation can engineer around:

**`lookup() == value` is a point-in-time observation of the published
generation. The write that reverts you need not be anywhere observable when you
look.** In Failure 1 the killer is a migrator holding `A` in a register (step
1–2). At the instant P verifies, memory is in a perfectly good state: the
published storage shows `B`. The stale `A` exists only in another thread's
register, and lands afterwards. Nothing P can read before returning reveals it.

A finite repair chase does not help either. Each repair step is separate and
non-atomic, so a delayed migrator can resurrect the stale value in a
generation the repairer has already walked past. And a writer that must
*return* can only perform finitely many steps.

Failures 1–3 together: **a general "replace this value" operation, one that
must work for arbitrary values and report success honestly, is not
implementable over this representation.** Not "not yet implemented" — not
implementable, given F1 and F2. That is why `patch` was removed rather than
fixed.

### Corollary: read-modify-write is out

A counter built from `lookup` + compare-and-set is unachievable here even in
principle, and this is worth stating separately because it is the tempting
misuse. Beyond the reverts above, F2 makes the *success test itself*
undecidable: after your CAS wins, seeing your value in the map does not mean
*your* write is the one present — another thread incrementing from the same
base produces a bit-identical value. Two threads can each legitimately win the
same transition in two different generations, both report success, and the
counter advances once. No repair logic distinguishes these cases, because the
distinguishing information does not exist in the data structure.

## What the poseAs contract changes

poseAs is narrower than "change a value":

1. `value` is **unique** to the caller — nobody else ever writes it for this hash.
2. `value` is **final** — the hash is never posed away from it again.
3. Nobody removes the hash while it is being posed.

Uniqueness plus finality is precisely the ordering information F2 says the data
structure cannot supply: the caller supplies it from outside. Consequences:

* `lookup() == value` becomes a **decidable and honest** success test. No other
  writer can produce that value, so seeing it means *your* pose is in place.
* Re-applying is idempotent, so retrying is always safe.

That makes a *convergent* implementation possible. It does **not** by itself
make a *durable* one — Failure 3 still applies to any CAS-then-verify scheme,
because the reverting write can still be sitting in a register. The contract
alone is not enough; the algorithm has to change.

## The implementation: build the next world, don't write into this one

`pose` never CASes the published slot. It makes the substitution while the new
generation is still **private**, and only then publishes it:

1. Confirm the key currently holds `expect` (tombstone or absent → `ENOENT`;
   already holds `value` → `0`, idempotent; anything else → `EEXIST`).
2. If a migration is already in flight, help it finish and start over — we can
   only substitute during a migration we own.
3. Allocate a fresh storage and seed it with `value` **while nothing can reach
   it**, using the normal claim path so the entry lands at the right probe
   position and `n_hashs` counts it.
4. `CAS(&next_storage, alloced, p)`. Lost? `abafree` the copy and retry.
5. Run the ordinary cooperative copy and publish the result.

**Step 3 before step 4 is the whole trick.** Once `next_storage` is published
the copy is cooperative and every helper races you; before it, you are alone.
Because `value` is in the destination slot first, F1 — the property that made
Failure 1 unfixable — now works *for* us: every carrier of the stale `expect`
is **refused** at its `put`, including a migrator descheduled since before the
pose began. The register-resident stale value of Failure 3 loses too, because
it must go through the same write-once `put`.

Failure 2 cannot occur because nothing overwrites an occupied destination slot;
`put` keeps its original keep-what-is-there semantics.

Contested poses resolve cleanly without needing F2: two posers seed two private
copies, only one wins the step-4 CAS. The loser frees its copy, retries, reads
the published storage, finds a value that is neither `expect` nor its own, and
correctly returns `EEXIST`. Exactly one winner.

## Costs and limits, stated plainly

* **A successful pose performs a full migration**, so it doubles the map. `N`
  poses means `2^N` growth. This is fine for what poseAs is — a handful of
  classes posing once while a library loads — and unusable as a steady-state
  mutation primitive. `model.c` had to drop pose from its randomized op mix for
  exactly this reason: 2400 poses in a hot loop is `2^2400`.
* **Not wait-free, by design.** It retries until it wins or loses outright.
  Under the quiescence of a library load it succeeds on the first attempt.
  Under sustained insert pressure it can be starved by other threads winning
  the `next_storage` CAS; it cannot abort, and it does not spin forever in
  practice.
* **Single-threaded posing does not need any of this.** A revert requires a
  concurrent migration, and migrations only start from another thread's
  insert-driven growth. With no other thread, a plain in-place CAS is durable.

## If a general value-replacement is ever needed

It requires breaking F2 by adding ordering information to the slot:

* box the value — the slot holds a pointer to an immutable `{payload, seq}`.
  Works with single-word CAS; costs an allocation per write and ABA-safe
  reclamation of the boxes.
* or a double-width CAS on `{payload, counter}`.

With either, "which of these two is newer" becomes computable, migration's
`put` simply keeps the higher sequence, and Failures 1–3 all disappear —
including read-modify-write. That is a change to the data representation and to
every reader, which is why it was not done here.

## Coverage

* `test/hashmap/pose.c` — sequential contract: wrong expect, absent, removed,
  idempotent re-pose, all reserved-argument rejections.
* `test/hashmap/pose_stress.c` — poses racing a flood of inserts on an
  undersized map, so every pose races a migration. Asserts no pose is ever
  reverted, and that a contested key has exactly one winner whose value is
  what the map finally reports. The contested check reproduced the old
  implementation's bug at roughly 1 run in 7; it now runs clean.
* `test/hashmap/race.c` — a single owner thread cycling
  remove/register/pose against dedicated migrator threads.
