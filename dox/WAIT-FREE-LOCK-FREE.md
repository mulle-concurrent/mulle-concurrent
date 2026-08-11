# Wait-free status of `mulle_concurrent_hashmap_register`

`mulle_concurrent_hashmap_register` is wait-free. A call completes in a bounded
number of its own steps regardless of concurrent activity. The bound is set by
the storage geometry and the finite number of remaining capacity doublings,
not by which threads win individual races.

## Hash-first registration

A virgin entry starts as:

```text
{ hash = NO_HASH, value = NO_POINTER }
```

Registration first claims the hash with one CAS. Only operations with that
matching hash may then CAS a payload into the value. Consequently, a thread
that encounters a claimed slot can immediately decide whether it found its
hash or a collision.

The hash claim does not make its thread the exclusive value writer. If that
thread pauses before filling the value, a same-hash peer may install its value
and complete. A different-hash peer probes onward, while migration may freeze
the empty value to `REDIRECT`. No operation waits for the original claimer.

## Bounded work in one storage

`_mulle_concurrent_hashmapstorage_claim` performs one hash read, at most one
hash CAS, and one counter increment. `_mulle_concurrent_hashmapstorage_fill`
performs one value CAS.

The table begins migration at half claimed capacity. The unused half is the
reserve for operations that passed the occupancy check concurrently, preserving
a virgin slot. Linear probing therefore examines at most `mask + 1` slots.

Fill observes one of four states:

| State | Result |
|-------|--------|
| `NO_POINTER` | install the payload and report insertion |
| live value | return the already registered value |
| `TOMBSTONE` | report `EEXIST`; this generation does not reuse removed hashes |
| `REDIRECT` | help migration and retry in the newer generation |

## Monotonic slot states

Hashmap point operations move a value through an acyclic state graph:

```text
NO_POINTER -> live -> TOMBSTONE -> REDIRECT
NO_POINTER -> live -> REDIRECT
NO_POINTER -> REDIRECT
```

A tombstone never becomes live again in the same storage generation. Migration
drops tombstones, after which their hashes can be registered in the newer
storage.

The monotonic state graph bounds migration work. If a freeze CAS loses, it can
only have lost to one of the finite forward transitions shown above.

## Copying a generation

Copy scans each source slot once. For a live slot it first puts the pair into
the destination, then freezes the source value to `REDIRECT`.

A slot whose hash initially reads as `NO_HASH` is frozen with:

```text
CAS(value, REDIRECT, NO_POINTER)
```

If this CAS loses to a racing fill, the returned live value is retained rather
than discarded. Because hash publication precedes value publication, copy can
reload the now-visible hash and process the pair through the normal live-slot
path. Thus an insertion cannot be stranded behind the copy cursor.

Removal may beat a live-slot freeze and change the value to `TOMBSTONE`; copy
then freezes and drops that tombstone on its next iteration. Each source slot
requires only a fixed number of state-dependent CAS attempts, so copying is
`O(source_size)` with a contention-independent bound.

## Nested migration

Migration always doubles storage capacity. It never creates a same-sized
successor. At the representable size limit it aborts instead of allowing the
capacity calculation to wrap.

If `put` reaches a destination slot already marked `REDIRECT`, that destination
is itself migrating. `put` follows `map->next_storage` and continues in the
newer generation. Each recursion advances to a strictly larger storage, so its
depth is bounded by the number of remaining capacity doublings.

## Register retries

Register retries for two reasons:

1. the current generation reached its claimed-slot threshold; or
2. its target value was already redirected by migration.

A retry may encounter another generation that concurrent threads have already
filled or begun migrating. This cannot continue without bound: `map->storage`
only advances, every successor is twice as large, and ABA reclamation prevents
an old generation from reappearing as a current one.

For every encountered generation, probing, copying and redirect following are
bounded. The number of generations is also bounded, so the complete register
operation is wait-free.

## Related API behavior

A removed hash remains claimed by a tombstone until migration. During that
generation:

- `mulle_concurrent_hashmap_insert` returns `EEXIST`;
- `mulle_concurrent_hashmap_register` returns
  `MULLE_CONCURRENT_INVALID_POINTER` and sets `errno` to `EEXIST`.

`count` and enumeration are weakly consistent observations. They may be
canceled or restarted by migration and are not part of the point-operation
wait-free guarantee.
