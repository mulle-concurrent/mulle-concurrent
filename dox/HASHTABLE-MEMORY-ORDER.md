# Hashtable Memory Ordering Analysis

## The three modes

**seq_cst** (sequential consistency): all threads agree on a single total order
of all atomic operations. Every load/store is a full fence. Most expensive — on
ARM it's `dmb ish` + store/load, on x86 loads are free but stores use `XCHG` or
`LOCK`.

**acquire** (on loads): you see everything the releasing thread wrote before its
release-store. No reordering of later loads/stores above this load. On ARM it's
`ldar` (one instruction). On x86 it's a plain load — x86 gives acquire
semantics for free on loads.

**relaxed**: no ordering guarantees at all. You see *some* value that was
written at some point, but it might be arbitrarily stale relative to other
memory locations. Just prevents tearing (word-sized atomicity). Compiles to a
plain load everywhere.

The practical cost difference: **ARM/POWER** pay real hardware cost for
seq_cst > acquire > relaxed. On **x86**, seq_cst loads and acquire loads
generate identical code — the cost is only on stores. So downgrading from
seq_cst to acquire helps ARM but does nothing for x86 loads.


## Can lookup be relaxed?

Lookup reads two words per slot: `entry->hash` then `entry->value`.

### Hash word (first read in the probe loop)

The hash word transitions are: `NO_HASH → h → h|FROZEN`. It never goes back.

A relaxed read could see a stale `NO_HASH` when the slot is actually claimed by
a different hash `h2`. If `h2` was inserted *before* our target hash and pushed
our target further down the probe chain, we'd stop the chain at the stale
NO_HASH and miss an entry that is present → **false negative**.

This is not a valid linearization. The insert of our target happened-before (in
the release/acquire sense) from the caller's point of view — they synchronized
externally to know the entry should be there. A relaxed load doesn't participate
in that ordering, so it can't see the chain.

**Verdict**: relaxed is **unsafe** for the hash probe.

### Value word (read after finding our hash)

Same reasoning. A relaxed read could see stale EMPTY when the value is already
live, reporting "absent" for an entry that a releasing thread already published.
The caller's external synchronization (which told them to look it up) is not
honored without at least acquire.

**Verdict**: relaxed is **unsafe** for the value read.

### FROZEN re-check (after seeing value == EMPTY)

After finding our hash and reading EMPTY from the value word, lookup re-reads
the hash to check for the FROZEN bit. If a migration froze the slot, carried
the value to a newer generation, and consumed it (set value back to EMPTY), the
FROZEN bit is the only signal to retry in the newer generation.

A relaxed re-read could miss the FROZEN bit → report "absent" when the entry is
live in the newer generation. There is no point in time where the entry was
absent (it went old-gen → new-gen without a gap), so this is **not
linearizable**.

**Verdict**: relaxed is **unsafe** for the FROZEN re-check.


## What about acquire?

Downgrading from seq_cst to acquire on all lookup reads is **safe**:

1. **Hash probe**: an acquire load of the hash word synchronizes-with the
   release-store that claimed the slot (the CAS, which is at least
   release). So once a slot is claimed, all subsequent acquire-loads see
   the claimed hash. The probe chain is intact.

2. **Value read**: an acquire load synchronizes-with the release-store that
   installed the value (the CAS in insert/register). So if the value was
   published, an acquire reader sees it.

3. **FROZEN re-check**: an acquire load synchronizes-with the release-CAS
   that set the FROZEN bit. If the slot was frozen, the acquire reader sees
   the frozen hash word and reports EBUSY.

Acquire preserves all the linearizability invariants. The lookup remains
wait-free with the same algorithmic structure.


## Cost on real hardware

| Architecture | seq_cst load          | acquire load      | relaxed load |
|--------------|-----------------------|-------------------|--------------|
| x86-64       | plain `mov`           | plain `mov`       | plain `mov`  |
| ARM64        | `ldar` + barrier      | `ldar`            | `ldr`        |
| POWER        | `ld` + `cmp` + `bc` + `isync` | `ld` + `isync` | `ld`  |

On x86, the downgrade from seq_cst to acquire is invisible for loads (both
compile to a plain `mov`; the compiler fence is the only difference, and it
only prevents compiler reordering, not hardware reordering which x86 already
guarantees). The benefit is exclusively on **ARM and POWER**.


## Conclusion

- **Fully relaxed lookup**: not possible without design changes. Produces
  false negatives that are not linearizable.
- **Acquire lookup**: safe, preserves all correctness invariants, meaningful
  perf win on ARM/POWER, zero cost on x86.
- **Current (seq_cst)**: correct but over-synchronized for a read-only path.

The recommended change is seq_cst → acquire for all loads in
`_mulle_concurrent_hashtablestorage_lookup`. This requires
`_mulle_atomic_pointer_read_acquire` (or equivalent) in mulle-atomic. Currently
only `_mulle_atomic_pointer_read` (seq_cst) and
`_mulle_atomic_pointer_read_relaxed` are available.
