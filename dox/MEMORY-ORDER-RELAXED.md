# Why mulle-concurrent uses relaxed memory ordering

## Background

mulle-thread historically used `memory_order_relaxed` for all atomic
operations. A recent change made `memory_order_seq_cst` the default, adding
explicit `_relaxed` suffixed variants for cases where relaxed is appropriate.

mulle-concurrent was always designed around relaxed atomics (Preshing-style
concurrent map) and has been tested extensively under relaxed ordering. This
document explains why relaxed ordering is correct here and why we use the
`_relaxed` variants explicitly.


## Atomic operations in mulle-concurrent

All atomic usage falls into these categories:

1. **CAS on slots** — `__mulle_atomic_pointer_cas` on `entry->hash`,
   `entry->value`, `p->entries[]`. These are the core lock-free protocol
   operations that claim, fill, and freeze slots.

2. **Reads of slot contents** — `_mulle_atomic_pointer_read` on
   `entry->hash`, `entry->value`, `p->entries[]`. These load slot state to
   decide what operation to perform.

3. **Storage pointer reads** — `_mulle_atomic_pointer_read` on
   `map->storage.pointer`, `map->next_storage.pointer`, `set->storage.pointer`,
   etc. These load the current/next generation pointers.

4. **Counter increments** — `_mulle_atomic_pointer_increment` on `p->n_hashs`,
   `p->n_used`, and pointerarray's `p->n`. These track
   occupancy.

5. **Counter reads** — `_mulle_atomic_pointer_read` on `p->n_hashs`,
   `p->n_used`, `p->n`. These decide whether to trigger migration.


## Why relaxed is correct

### CAS operations are self-synchronizing

A CAS is inherently atomic — the old value returned is the actual state at the
point of the CAS. On x86-64, CAS always has full barrier semantics regardless
of the memory order specified. On ARM/POWER, relaxed CAS means no barrier
around the operation, but the CAS itself is still atomic: you never get a torn
value. The algorithm's correctness depends on CAS atomicity, not on ordering
with respect to other memory locations.

### The slot protocol is self-contained

The hashmap correctness argument: hash is claimed first (CAS on hash), then
value is written (CAS on value). A reader reads hash, then reads value. On
ARM, could a reader see the value before the hash? No — the reader only
accesses `entry->value` after observing `entry->hash == target_hash` in the
probe loop. The data dependency (address dependency) prevents hardware
reordering.

### Counters are heuristic

`n_hashs`, `n_used`, and `n` gate migration decisions. Reading a stale count
means migration triggers slightly early or slightly late. Both are safe:
- Early migration wastes memory but is correct
- Late migration means the actual slot CAS encounters a full table, gets
  EBUSY/ENOSPC, and retries after migration — the designed fallback

### Storage pointer staleness is handled by design

Getting a slightly stale `map->storage.pointer` means operating on a storage
that is being migrated. The operation encounters REDIRECT values and retries
after helping finish migration. This is the normal code path, not an error.

### Value visibility after hash publication

The hash-first publication protocol (claim hash → fill value) on ARM with
relaxed ordering: could a reader see `entry->hash == H` but read a stale
`entry->value` (NO_POINTER)? This does not break correctness:
- If the reader sees NO_POINTER, it returns "not found" — safe
- If the reader sees REDIRECT, it migrates and retries — safe
- The fill is a CAS from NO_POINTER → value, so the value state machine is
  monotonic (NO_POINTER → live → REDIRECT)
- A subsequent read will see the filled value

### The algorithm was designed for relaxed semantics

The Preshing concurrent map design that mulle-concurrent is based on was
specifically designed to work without memory barriers between operations.
Correctness comes from the CAS protocol (slot state machines with monotonic
transitions), not from inter-operation ordering.


## The one place this reasoning does not reach

Every argument above is about a **reader** that loads `entry->hash` and then
loads `entry->value`. The address dependency carries the ordering, so relaxed is
fine. The hashmap's `copy` has one step that runs in the *opposite* direction and
has no such dependency:

```c
   hash = read( &p->hash);                 // observed NO_HASH: looks virgin
   if( hash == MULLE_CONCURRENT_NO_HASH)
   {
      actual = cas( &p->value, REDIRECT_VALUE, NO_POINTER);
      if( actual is a live value)          // lost the freeze to a racing fill
      {
         hash = read( &p->hash);           // <-- MUST see the published hash
         assert( hash != MULLE_CONCURRENT_NO_HASH);
         ...carry (hash, actual) into the destination...
      }
   }
```

The writer does `CAS(hash)` then `CAS(value)`. Copy's `CAS(value)` observed the
writer's value, so it follows the writer's value CAS; copy's *later* hash read
must therefore also follow the writer's *earlier* hash CAS. Under `seq_cst`
that is immediate from the single total order. Under relaxed there is no
dependency between copy's value CAS and copy's hash read at all, so on
ARM/POWER the load can be satisfied from before the writer's hash store and
return `NO_HASH` — tripping the assert in a debug build and carrying a value
under the wrong hash in a release build.

So this read, and the value CAS it follows, need at least acquire/release
ordering, not relaxed. It is not covered by "CAS is self-synchronizing" (the
problem is the plain load, not the CAS) and not covered by the address-dependency
argument (there is no address dependency in this direction). Any relaxation
programme has to treat it as an explicit exception. See
[WAIT-FREE-LOCK-FREE.md](WAIT-FREE-LOCK-FREE.md) §3.


## Conclusion

Most atomic operations in mulle-concurrent can use the `_relaxed` suffixed
variants: correctness derives from CAS atomicity and the slot state protocol
rather than from memory ordering between different locations. The exception
above is real and must be carried explicitly — it is the reason the library
currently relies on the `seq_cst` defaults of its atomics dependency rather than
having been audited operation by operation.
