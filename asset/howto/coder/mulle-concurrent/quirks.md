<!-- Keywords: sentinels, constraints, enum, lifecycle, thread-safety -->

# Quirks — coder bundle for mulle-concurrent

## ABA is mandatory

- `mulle_aba_init(allocator)` **must** be called once per process before any
  concurrent structure use.
- **Every thread** that touches any mulle-concurrent structure **must** call
  `mulle_aba_register()` on entry and `mulle_aba_unregister()` on exit.
- Forgetting ABA registration causes crashes — not warnings, not errno.
- `mulle_concurrent_*_done` frees old storage via the ABA system; all
  registered threads must quiesce (call `mulle_aba_unregister`) before the
  deferred free can complete.

## Sentinel values are forbidden as user data

- `MULLE_CONCURRENT_NO_HASH` (0) — never use as a hashmap key.
- `MULLE_CONCURRENT_NO_POINTER` (NULL/0) — never store as a value.
- `MULLE_CONCURRENT_INVALID_POINTER` (INTPTR_MIN) — never store as a value.
  Used internally as a REDIRECT marker during migration.
- `MULLE_CONCURRENT_TOMBSTONE_POINTER` (INTPTR_MAX) — never store as a value.
  Used in pointerset as a removal marker; including it as user data corrupts
  the set.

These values are defined in `src/mulle-concurrent-types.h:44-48`. You
**can** redefine them before including the header if your use-case requires
different sentinels, but the library's internal collision-checking logic
depends on them being distinct from valid user data.

## Init/done are single-threaded only

- `mulle_concurrent_*_init` and `mulle_concurrent_*_done` must be called in
  a single-threaded context.
- Doing `_init` while other threads are accessing the structure is undefined
  behavior.
- The `_done` functions free internal storage through mulle-aba; calling
  `_done` while other threads are using the structure will cause use-after-free.

## Hashmap: hash 0 is reserved

- `mulle_concurrent_hashmap_insert` / `_register` / `_lookup` / `_remove`
  treat `hash == 0` as an invalid sentinel (`MULLE_CONCURRENT_NO_HASH`).
- Use a non-zero hash function. If your natural keys can be 0, add an offset
  or use `hash = key + 1`.

## Hashmap: value must be non-NULL and not INTPTR_MIN

- Both `value = NULL` and `value = (void *) INTPTR_MIN` are treated as
  sentinel slots internally.
- Passing them as user data leads to silent misbehavior: values not stored,
  lookups returning sentinels instead of user data.

## Hashmap: remove matches both hash and value

```c
// hash AND value must both match — this is not a simple key-match
int rval = mulle_concurrent_hashmap_remove( &map, hash, value);
```

- If a concurrent insert updated the value for a hash, your remove with the
  old value returns `ENOENT`.
- This is intentional: it prevents removing an entry a different thread just
  placed.

## Hashmap: `patch` is experimental

- `mulle_concurrent_hashmap_patch` (atomically update an existing entry's
  value) is marked experimental in `src/hashmap/mulle-concurrent-hashmap.h:191-192`.
- `expect` must be `!= value`.
- Returns `EEXIST` if the entry has a different value, `ENOENT` if not found.
- Prefer remove+insert for production code.

## Pointerarray: grow-only, no removals

- `mulle_concurrent_pointerarray` has no remove or overwrite operation.
- The array grows but never shrinks. Capacity doubles on each growth.
- `mulle_concurrent_pointerarray_find` returns the index; index 0 is
  legitimate, so check for `!= EINVAL` / `!= ENOENT` rather than `!= 0`
  (the test code at `test/array/empty.c:16-18` does `? "" : "not "` on the
  result).

## Pointerarray: sentinel values rejected silently

- `mulle_concurrent_pointerarray_add` returns `EINVAL` for NULL or
  INTPTR_MIN values.
- The header checks at `src/pointerarray/mulle-concurrent-pointerarray.h:155-156`
  reject both `MULLE_CONCURRENT_NO_POINTER` and
  `MULLE_CONCURRENT_INVALID_POINTER`.

## Pointerarray: `get_size` vs `get_count`

- `get_size` returns current capacity (a snapshot).
- `get_count` returns current element count — more reliable than the
  hashmap equivalent because the array is grow-only.

## Pointerset: linear probing with tombstones

- Pointerset uses linear probing with single-word-per-slot storage.
- Slot states: `NULL` = empty (probe chain stops), `INTPTR_MIN` = REDIRECT
  (migration underway, retry), `INTPTR_MAX` = TOMBSTONE (removed, probe
  continues), anything else = live pointer.
- Tombstones are **never reused** for new inserts — they accumulate until
  migration drops them.
- A remove-heavy workload triggers earlier migration because tombstones
  count toward the 50% load threshold (tombstones + live >= 50% capacity).
- Use `mulle_concurrent_pointerset_reset` to clear tombstone accumulation
  in remove-heavy scenarios.

## Pointerset: `member` with NULL returns 0 silently

```c
mulle_concurrent_pointerset_member( &set, NULL);  // returns 0, errno unchanged
```

- Unlike other functions, `member` with `NULL` is not an error and does not
  set errno. It just returns "not found."

## Pointerset: no `get_count` equivalent using snapshot

- `mulle_concurrent_pointerset_count` counts via enumeration with automatic
  retry on `ECANCELED`. It is expensive and returns a snapshot only.

## Enumerators are thread-local

- Each enumerator is a stack-allocated struct; do not pass it to another
  thread.
- The enumerator captures a snapshot of the map/set storage. If the
  structure grows or entries are removed, `_next` returns `ECANCELLED`
  (hashmap/pointerset) or `EBUSY` (hashmap enumeration in some implementations).
- Both test files (`test/hashmap/hashmap.c:140-154`,
  `test/hashmap/example.c:55-67`) use `retry:` / `goto retry` on ECANCELED
  or EBUSY.

## Enumeration: `ECANCELLED` vs `EBUSY`

- Hashmap enumeration returns `ECANCELLED` on mutation detected by the
  migration state change, or `EBUSY` when the storage is found to be under
  migration. Both require retry.
- Pointerarray enumeration is safe during concurrent `add` (no removals
  exist), so no `ECANCELLED`/`EBUSY` handling is needed.

## `_prefixed` vs safe functions

- `_mulle_concurrent_hashmap_init(...)` — no NULL check, assumes map is
  valid. Only `mulle_concurrent_hashmap_init(...)` returns `EINVAL` for NULL.
- The same pattern holds for `_done`, `_get_size`, `_lookup`, `_register`,
  `_insert`, `_remove`.
- Use `_prefixed` variants in hot loops when parameters are guaranteed valid.
- The safe wrappers are small static inlines in the headers (e.g.
  `src/hashmap/mulle-concurrent-hashmap.h:134-159`).

## Custom allocator with test allocator

When using a test allocator, you must:
1. Initialize it before use: `mulle_testallocator_initialize()` or
   `mulle_default_allocator = mulle_testallocator`.
2. Reset it between test runs: `mulle_testallocator_reset()`.
3. Wire it to ABA: `mulle_allocator_set_aba(allocator, aba, _mulle_aba_free)`
   before any concurrent operations. Multi-threaded tests use
   `_mulle_aba_free_owned_pointer` instead (see `test/hashmap/hashmap.c:217`,
   `test/array/pointerarray.c:132`).
4. Unwire it after: `mulle_allocator_set_aba(allocator, NULL, NULL)` before
   calling `mulle_aba_done()`.

See `test/array/simple.c` (lines 35-47) and `test/hashmap/hashmap.c`
(lines 253-317) for the single-threaded test pattern.
