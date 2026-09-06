# mulle-concurrent Library Documentation for AI
<!-- Keywords: lockfree, waitfree, hashtable, hashmap, pointerarray, pointerset, C -->

## 1. Introduction & Purpose

- mulle-concurrent provides wait-free, lock-free concurrent data structures in C: two resizable hash maps (`mulle_concurrent_hashmap` and its redesigned successor `mulle_concurrent_hashtable`), a grow-only pointer array, and a pointer set with single-word-per-slot linear probing.
- Solves contention in multithreaded environments where low-latency, non-blocking operations are required.
- The new `mulle_concurrent_hashtable` moves migration state from the *value* word into the *hash* word (a FROZEN bit), which makes removed hashes immediately reusable — no tombstones, no migration-triggered reinsert, no reserved payloads other than NULL.
- The older `mulle_concurrent_hashmap` still exists; it freezes slots during migration by overwriting the value with a REDIRECT sentinel and uses tombstones for removal. It has retained only single-threaded value replacement/removal (`_patch`/`_remove`); its former multi-threaded `remove` is gone.
- Key features: wait-free register/insert/lookup/remove for all structures, lock-free add/get/find and enumerators, optional custom allocator, ABA handling via mulle-aba.
- A component of mulle-core; depends on mulle-aba.

## 2. Key Concepts & Design Philosophy

- Wait-free designs: operations aim to complete in a finite number of steps regardless of other threads. Growth and migration are cooperative — any thread observing a migration marker helps finish the move and then retries its own operation.
- Both hash maps are inspired by Preshing's resizable concurrent map but implemented to be wait-free.
- **hashmap (legacy):** a slot is claimed by CASing the `hash` word from `MULLE_CONCURRENT_NO_HASH` to the key's hash; once claimed only the claiming thread may CAS that slot's `value`. Migration freezes a slot by overwriting its value with `MULLE_CONCURRENT_INVALID_POINTER` (the REDIRECT marker), which destroys the payload. Removal writes a TOMBSTONE value while keeping the hash claim, so probe chains stay intact; a removed hash cannot be re-inserted in the same storage generation (`EEXIST`) and reuse triggers a same-size migration.
- **hashtable (redesigned):** freezing marks the `hash` word with a reserved FROZEN bit and leaves the value intact, so a frozen slot still holds its payload and any helper can complete the carry. The value word has exactly two states — empty or live — so NULL is the only reserved payload (`MULLE_CONCURRENT_INVALID_POINTER` and `MULLE_CONCURRENT_TOMBSTONE_POINTER` are ordinary payloads here). Removed slots keep their hash claim (probe chains stay intact) but are immediately reusable by a plain value CAS: no tombstones, no intermediate error state, no migration for re-insert. The price is one reserved bit in the hash space (the topmost bit, or bit 0 in "even" mode), so hashes are folded into the low bits — this is not an identity mapping.
- The pointer array grows but never shrinks — this limitation simplifies concurrent access.
- Pointerset uses linear probing with tombstones for removal; tombstones are dropped during migration.
- Growing/migration is triggered at 50% load. Migration is atomic and non-blocking for callers.
- ABA problem management via mulle-aba; each thread must register/unregister with the ABA system before accessing structures.
- Enumerators are "limited multi-threaded": safe for single-threaded use or when no concurrent removals/growth occur; mutation/migration is signaled via error codes (ECANCELED).

## 3. Core API & Data Structures

### 3.1. `mulle-concurrent-types.h`

Sentinel values and constants (user code must not store these as payload where noted):

| Constants                         | Value                 | Meaning
| ----------------------------------|-----------------------|---------
| `MULLE_CONCURRENT_NO_HASH`        | `0`                   | Invalid hash sentinel, never use as actual hash
| `MULLE_CONCURRENT_INVALID_POINTER`| `((void *) INTPTR_MIN)`| REDIRECT marker during hashmap/pointerset migration
| `MULLE_CONCURRENT_NO_POINTER`     | `((void *) 0)`        | Internal "no value" sentinel (NULL)
| `MULLE_CONCURRENT_TOMBSTONE_POINTER`| `((void *) INTPTR_MAX)`| Removed slot marker in hashmap and pointerset

Reserved-value rules differ per structure:
- **hashmap** and **pointerset**: reject NULL, `INTPTR_MIN`, `INTPTR_MAX` as payloads (return `EINVAL`).
- **hashtable**: only NULL is reserved as a payload; `INTPTR_MIN` and `INTPTR_MAX` are valid payloads.

### 3.2. `mulle-concurrent-hashmap.h`

#### `struct mulle_concurrent_hashmap`

**Purpose:** Wait-free, resizable hash table mapping full `sizeof(intptr_t)` hashes to `void *` pointer values. The hash is the key (you can use a pointer cast to `intptr_t`).

**Key Fields (opaque):**
- `storage` — current storage (`union mulle_concurrent_atomichashmapstorage_t`)
- `next_storage` — next storage during resize
- `allocator` — memory allocator (`mulle_atomic_pointer_t`)

**Internal Structures:**
- `struct _mulle_concurrent_hashvaluepair` — hash/value pair; `hash` and `value` are both `mulle_atomic_pointer_t`
- `struct _mulle_concurrent_hashmapstorage` — backing storage with `n_hashs` count and `mask` (capacity-1)

**Lifecycle Functions (single-threaded only):**

```c
void  _mulle_concurrent_hashmap_init( struct mulle_concurrent_hashmap *map,
                                      unsigned int size,
                                      struct mulle_allocator *allocator);
static inline void
   mulle_concurrent_hashmap_init( struct mulle_concurrent_hashmap *map,
                                  unsigned int size,
                                  struct mulle_allocator *allocator);
```
Initializes `map` with a starting `size`. `allocator` may be `NULL` for the default. `init` returns void; the static inline asserts `map != NULL`. Allocation is fail-fast (success or abort).

```c
void  _mulle_concurrent_hashmap_done( struct mulle_concurrent_hashmap *map);
static inline void
   mulle_concurrent_hashmap_done( struct mulle_concurrent_hashmap *map);
```
Frees internal resources. The static inline is safe to call with NULL.

**Core Operations (multi-threaded safe):**

```c
void   *_mulle_concurrent_hashmap_register( struct mulle_concurrent_hashmap *map,
                                            intptr_t hash,
                                            void *value);
void   *mulle_concurrent_hashmap_register( struct mulle_concurrent_hashmap *map,
                                           intptr_t hash,
                                           void *value);
```
Insert-or-get. Returns `MULLE_CONCURRENT_NO_POINTER` if it inserted, `MULLE_CONCURRENT_INVALID_POINTER` on error (check `errno`, which is `EINVAL` or `EEXIST`), or the value that was already registered. Do not use `hash == 0`, `value == NULL`, or `value == INTPTR_MIN`.

```c
int  _mulle_concurrent_hashmap_insert( struct mulle_concurrent_hashmap *map,
                                       intptr_t hash,
                                       void *value);
int   mulle_concurrent_hashmap_insert( struct mulle_concurrent_hashmap *map,
                                       intptr_t hash,
                                       void *value);
```
Insert hash/value pair. Returns `0` on success, `EEXIST` on duplicate, `EINVAL` for invalid arguments. Do not use `hash == 0`, `value == NULL`, or `value == INTPTR_MIN`.

```c
void  *_mulle_concurrent_hashmap_lookup( struct mulle_concurrent_hashmap *map,
                                         intptr_t hash);
static inline void
   *mulle_concurrent_hashmap_lookup( struct mulle_concurrent_hashmap *map,
                                     intptr_t hash);
```
Look up value by hash. Returns the value pointer or NULL if not found.

**Single-threaded value manipulation (setup/teardown phases only):**

```c
int  _mulle_concurrent_hashmap_patch( struct mulle_concurrent_hashmap *map,
                                      intptr_t hash,
                                      void *value);
static inline int
   _mulle_concurrent_hashmap_remove( struct mulle_concurrent_hashmap *map,
                                     intptr_t hash)
{
   return( _mulle_concurrent_hashmap_patch( map, hash, NULL));
}
```
`_patch` unconditionally replaces the value of an existing entry (no CAS, no migration concern). Returns `0` on success, `ENOENT` if the hash is absent. `_remove` (static inline) clears the value to NULL; the hash claim stays in place so probe chains remain intact and lookup returns NULL afterwards; the slot can be repopulated later with `_patch`. **Both are single-threaded only** — calling while another thread accesses the map is undefined behavior.

**Inspection Functions:**

```c
unsigned int  _mulle_concurrent_hashmap_get_size( struct mulle_concurrent_hashmap *map);
static inline unsigned int
   mulle_concurrent_hashmap_get_size( struct mulle_concurrent_hashmap *map);
```
Returns current storage capacity (not count). Snapshot only.

```c
unsigned int   mulle_concurrent_hashmap_count( struct mulle_concurrent_hashmap *map);
```
Counts live entries via enumeration. Expensive. Snapshot only.

```c
void           *mulle_concurrent_hashmap_lookup_any( struct mulle_concurrent_hashmap *map);
```
Returns any value from the map, or NULL if empty.

**Enumeration (limited multi-threaded):**

```c
struct mulle_concurrent_hashmapenumerator
{
   struct mulle_concurrent_hashmap   *map;
   unsigned int                      index;
   unsigned int                      mask;
};

struct mulle_concurrent_hashmapenumerator
   mulle_concurrent_hashmap_enumerate( struct mulle_concurrent_hashmap *map);

int  _mulle_concurrent_hashmapenumerator_next( struct mulle_concurrent_hashmapenumerator *rover,
                                               intptr_t *hash,
                                               void **value);
static inline int
  mulle_concurrent_hashmapenumerator_next( struct mulle_concurrent_hashmapenumerator *rover,
                                           intptr_t *hash,
                                           void **value)
```
Enumeration return codes: `1` OK, `0` nothing left, `ECANCELLED` mutation alert, `ENOMEM`, `EINVAL`. Enumerating a NULL map produces an empty enumerator. The enumerator is only usable by the calling thread.

```c
static inline void
   mulle_concurrent_hashmapenumerator_done( struct mulle_concurrent_hashmapenumerator *rover);
```
Cleanup (currently no-op, call for forward compatibility).

**Convenience Macros:**

```c
mulle_concurrent_hashmap_for( name, hash, value)
mulle_concurrent_hashmap_for_rval( name, hash, value, rval)
```
`for`-loop style enumeration; the `_rval` variant exposes the return value of `_next` (e.g. for ECANCELED detection).

### 3.3. `mulle-concurrent-hashtable.h`

#### `struct mulle_concurrent_hashtable`

**Purpose:** Wait-free, resizable hash map that is the redesigned replacement for `mulle_concurrent_hashmap`. Migration state lives in the *hash* word (FROZEN bit) instead of the value word, so values survive migration, a successful remove cannot be silently undone, and removed hashes are immediately reusable.

**Key Fields (opaque):**
- `storage` — current storage (`union mulle_concurrent_atomichashtablestorage_t`)
- `next_storage` — next storage during migration
- `allocator` — memory allocator (`mulle_atomic_pointer_t`)
- `frozen_bit` — the reserved FROZEN flag bit (top bit in "positive" mode, bit 0 in "even" mode)
- `hash_mask` — mask that clears the FROZEN bit to recover the stored hash

**Internal Structures:**
- `struct _mulle_concurrent_hashtablepair` — `hash` (intptr_t with FROZEN bit, 0 == unclaimed) and `value` (payload or NULL), both `mulle_atomic_pointer_t`
- `struct _mulle_concurrent_hashtablestorage` — backing storage with `n_hashs` count and `mask` (capacity-1)

**Hash width restriction (critical):** legal hash range is `[1, INTPTR_MAX]` in the default "positive" mode (topmost bit must be clear, 0 is `NO_HASH`). On LP64 this is 63 usable bits; user-space pointers never set bit 63, so pointer-as-hash is safe. On ILP32 this is 31 usable bits — pointers above `0x80000000` CANNOT be used as hashes (they alias their `& 0x7FFFFFFF` counterpart). Use `mulle_concurrent_hashtable_init_even` for the "even" mode (FROZEN = bit 0, hashes must be even and non-zero, full address range, safe for aligned pointers on any platform).

**Lifecycle Functions (single-threaded only):**

```c
MULLE_C_NONNULL_FIRST
void   _mulle_concurrent_hashtable_init_positive( struct mulle_concurrent_hashtable *map,
                                                  size_t size,
                                                  struct mulle_allocator *allocator);
MULLE_C_NONNULL_FIRST
void   _mulle_concurrent_hashtable_init_even( struct mulle_concurrent_hashtable *map,
                                              size_t size,
                                              struct mulle_allocator *allocator);
static inline
void   mulle_concurrent_hashtable_init( struct mulle_concurrent_hashtable *map,
                                        size_t size,
                                        struct mulle_allocator *allocator);
static inline
void   mulle_concurrent_hashtable_init_even( struct mulle_concurrent_hashtable *map,
                                             size_t size,
                                             struct mulle_allocator *allocator);
static inline
void   mulle_concurrent_hashtable_init_positive( struct mulle_concurrent_hashtable *map,
                                                 size_t size,
                                                 struct mulle_allocator *allocator);
```
`mulle_concurrent_hashtable_init` defaults to "positive" mode. `allocator` may be NULL (default). Allocation is fail-fast.

```c
void  mulle_concurrent_hashtable_done( struct mulle_concurrent_hashtable *map);
```
Frees internal resources. `map` must be a valid pointer (no NULL guard).

**Core Operations (multi-threaded safe):**

```c
int   mulle_concurrent_hashtable_insert( struct mulle_concurrent_hashtable *map,
                                       intptr_t hash,
                                       void *value);
```
Insert hash/value pair. Returns `0` did insert, `EEXIST` a live value is already registered for `hash`, `EINVAL` invalid argument. A previously removed `hash` is immediately insertable again — no migration and no intermediate error state.

```c
int   mulle_concurrent_hashtable_register( struct mulle_concurrent_hashtable *map,
                                         intptr_t hash,
                                         void *value,
                                         void **p_old);
```
Insert `value` if `hash` is absent. `*p_old` is set to the value registered afterwards: NULL if we inserted, otherwise the pre-existing value (in which case `value` was not stored). `p_old` may be NULL. Returns `0` OK, `EINVAL` invalid argument.

```c
int   mulle_concurrent_hashtable_remove( struct mulle_concurrent_hashtable *map,
                                       intptr_t hash,
                                       void *value);
```
Remove the `(hash, value)` pair (both must match). Returns `0` removed, `ENOENT` pair not present, `EINVAL` invalid argument.

```c
void  *mulle_concurrent_hashtable_lookup( struct mulle_concurrent_hashtable *map,
                                         intptr_t hash);
```
NULL if absent, otherwise the registered value.

**Inspection Functions:**

```c
size_t   mulle_concurrent_hashtable_get_size( struct mulle_concurrent_hashtable *map);
```
Current storage capacity (number of slots). Snapshot only.

```c
size_t   mulle_concurrent_hashtable_count( struct mulle_concurrent_hashtable *map);
```
Number of live entries, counted via enumeration. Expensive; snapshot only.

**Test/Internal Hook:**

```c
void   _mulle_concurrent_hashtable_migrate_same_size( struct mulle_concurrent_hashtable *map);
```
Retires the current generation into a fresh one of the same size. Primarily a test hook to force continuous generation changes without doubling memory (used to reproduce the remove-versus-copy race). Not intended for production use.

**Enumeration (limited multi-threaded):**

```c
struct mulle_concurrent_hashtableenumerator
{
   struct mulle_concurrent_hashtable   *map;
   void                               *storage;   // compared only, never dereferenced
   unsigned int                       index;
};

int   _mulle_concurrent_hashtableenumerator_next( struct mulle_concurrent_hashtableenumerator *rover,
                                                 intptr_t *p_hash,
                                                 void **p_value);
```
Enumeration return codes: `1` OK, `0` nothing left, `ECANCELED` the storage migrated (restart the enumeration), `EINVAL` wrong parameter value.

```c
static inline struct mulle_concurrent_hashtableenumerator
   mulle_concurrent_hashtable_enumerate( struct mulle_concurrent_hashtable *map);

static inline int
   mulle_concurrent_hashtableenumerator_next( struct mulle_concurrent_hashtableenumerator *rover,
                                             intptr_t *p_hash,
                                             void **p_value);

static inline void
   mulle_concurrent_hashtableenumerator_done( struct mulle_concurrent_hashtableenumerator *rover);
```
`enumerate` on a NULL map produces an empty enumerator. Enumerator is only usable by the calling thread.

**Convenience Macros:**

```c
mulle_concurrent_hashtable_for( name, hash, value)
mulle_concurrent_hashtable_for_rval( name, hash, value, rval)
```

### 3.4. `mulle-concurrent-pointerarray.h`

#### `struct mulle_concurrent_pointerarray`

**Purpose:** Wait-free, lock-free, grow-only pointer array. Safe for concurrent reads and appends. Cannot shrink or overwrite elements.

**Key Fields:**
- `storage` — current array storage
- `next_storage` — next storage during growth
- `allocator` — memory allocator (plain pointer, not atomic)

**Lifecycle Functions (single-threaded only):**

```c
void  _mulle_concurrent_pointerarray_init( struct mulle_concurrent_pointerarray *array,
                                           unsigned int size,
                                           struct mulle_allocator *allocator);
static inline int  mulle_concurrent_pointerarray_init( struct mulle_concurrent_pointerarray *array,
                                                       unsigned int size,
                                                       struct mulle_allocator *allocator);
```
The static inline returns `0` on success, `EINVAL` for NULL array. Allocation is fail-fast (success or abort).

```c
void  _mulle_concurrent_pointerarray_done( struct mulle_concurrent_pointerarray *array);
static inline void  mulle_concurrent_pointerarray_done( struct mulle_concurrent_pointerarray *array);
```
Frees internal resources; the static inline is safe with NULL.

**Core Operations (multi-threaded safe):**

```c
void  _mulle_concurrent_pointerarray_add( struct mulle_concurrent_pointerarray *array,
                                          void *value);
static inline int  mulle_concurrent_pointerarray_add( struct mulle_concurrent_pointerarray *array,
                                                      void *value);
```
Append `value` to the end of the array. The static inline returns `0` on success, `EINVAL` if `array` is NULL or `value` is `MULLE_CONCURRENT_NO_POINTER`/`MULLE_CONCURRENT_INVALID_POINTER`.

```c
void  *_mulle_concurrent_pointerarray_get( struct mulle_concurrent_pointerarray *array,
                                           unsigned int index);
static inline void  *mulle_concurrent_pointerarray_get( struct mulle_concurrent_pointerarray *array,
                                          unsigned int i);
```
Get value at index. Returns NULL when `array` is NULL or `i` is outside the current count.

```c
int   _mulle_concurrent_pointerarray_find( struct mulle_concurrent_pointerarray *array,
                                           void *search);
static inline int  mulle_concurrent_pointerarray_find( struct mulle_concurrent_pointerarray *array,
                                                       void *value);
```
Linear search. Returns the index on found, `EINVAL` on error (NULL array / reserved value). An element found at index 0 also returns 0 — treat `>= 0` as found.

**Inspection Functions:**

```c
unsigned int  _mulle_concurrent_pointerarray_get_size( struct mulle_concurrent_pointerarray *array);
static inline unsigned int  mulle_concurrent_pointerarray_get_size( struct mulle_concurrent_pointerarray *array);
```
Current capacity. Snapshot only.

```c
unsigned int  _mulle_concurrent_pointerarray_get_count( struct mulle_concurrent_pointerarray *array);
static inline unsigned int  mulle_concurrent_pointerarray_get_count( struct mulle_concurrent_pointerarray *array);
```
Current element count. Reliable because the array is grow-only.

**Mapping:**

```c
int   mulle_concurrent_pointerarray_map( struct mulle_concurrent_pointerarray *list,
                                        void (*f)( void *, void *),
                                        void *userinfo);
```
Apply `f(value, userinfo)` to each element.

**Enumeration:**

```c
struct mulle_concurrent_pointerarrayenumerator
{
   struct mulle_concurrent_pointerarray   *array;
   unsigned int                            index;
};

struct mulle_concurrent_pointerarrayreverseenumerator
{
   struct mulle_concurrent_pointerarray   *array;
   unsigned int                           index;
};

void   *_mulle_concurrent_pointerarrayenumerator_next( struct mulle_concurrent_pointerarrayenumerator *rover);

void   *_mulle_concurrent_pointerarrayreverseenumerator_next( struct mulle_concurrent_pointerarrayreverseenumerator *rover);

struct mulle_concurrent_pointerarrayenumerator
   mulle_concurrent_pointerarray_enumerate( struct mulle_concurrent_pointerarray *array);

struct mulle_concurrent_pointerarrayreverseenumerator
   mulle_concurrent_pointerarray_reverseenumerate( struct mulle_concurrent_pointerarray *array, unsigned int n);

static inline void  *mulle_concurrent_pointerarrayenumerator_next( struct mulle_concurrent_pointerarrayenumerator *rover);

static inline void  *mulle_concurrent_pointerarrayreverseenumerator_next( struct mulle_concurrent_pointerarrayreverseenumerator *rover);

static inline void  mulle_concurrent_pointerarrayenumerator_done( struct mulle_concurrent_pointerarrayenumerator *rover);

static inline void  mulle_concurrent_pointerarrayreverseenumerator_done( struct mulle_concurrent_pointerarrayreverseenumerator *rover);
```
`enumerator_next` returns the next pointer or NULL when exhausted. Safe even if the array grows during iteration (append-only). `reverseenumerate` starts at index `n` and counts down.

**Convenience Macros:**

```c
mulle_concurrent_pointerarray_for( name, item)
mulle_concurrent_pointerarray_for_reverse( name, n, item)
```

### 3.5. `mulle-concurrent-pointerset.h`

#### `struct mulle_concurrent_pointerset`

**Purpose:** Wait-free set of `void *` pointers using linear probing with a single-word-per-slot layout. The pointer value itself is the key — it is hashed internally; no separate hash key is needed.

**Key Fields (opaque):**
- `storage` — current storage
- `next_storage` — next storage during migration
- `allocator` — memory allocator (`mulle_atomic_pointer_t`)

**Slot States (linear probing):**
- `NULL` (`MULLE_CONCURRENT_NO_POINTER`) — empty; probe chain stops
- `INTPTR_MIN` (`MULLE_CONCURRENT_INVALID_POINTER`) — REDIRECT, migration in progress, retry
- `INTPTR_MAX` (`MULLE_CONCURRENT_TOMBSTONE_POINTER`) — removed slot, probe chain continues
- Anything else — live pointer

**Tombstone rationale:** With linear probing, pointers hashing to the same slot form chains. Removing by writing NULL would break the chain; a tombstone preserves correctness. Tombstones are NOT reused for new inserts (to avoid races) but are dropped during migration.

**Lifecycle Functions (single-threaded only):**

```c
int  _mulle_concurrent_pointerset_init( struct mulle_concurrent_pointerset *set,
                                        size_t size,
                                        struct mulle_allocator *allocator);
static inline int
   mulle_concurrent_pointerset_init( struct mulle_concurrent_pointerset *set,
                                     size_t size,
                                     struct mulle_allocator *allocator);
```
Returns `0` on success, `EINVAL` for NULL set. Allocation is fail-fast.

```c
void  _mulle_concurrent_pointerset_done( struct mulle_concurrent_pointerset *set);
static inline void
   mulle_concurrent_pointerset_done( struct mulle_concurrent_pointerset *set);
```
Frees internal resources; the static inline is safe with NULL.

```c
static inline void
   mulle_concurrent_pointerset_reset( struct mulle_concurrent_pointerset *set);
```
Clears the set by combining `done` + `init` with the same size and allocator. Efficient way to drop accumulated tombstones.

**Core Operations (multi-threaded safe):**

```c
void  *_mulle_concurrent_pointerset_register( struct mulle_concurrent_pointerset *set,
                                              void *ptr);
void  *mulle_concurrent_pointerset_register( struct mulle_concurrent_pointerset *set,
                                             void *ptr);
```
Insert-or-get. Returns `MULLE_CONCURRENT_NO_POINTER` if it inserted, `MULLE_CONCURRENT_INVALID_POINTER` on error (check `errno`), or the pointer if already present. Do not pass NULL, `INTPTR_MIN`, or `(void *) -1`.

```c
int  _mulle_concurrent_pointerset_insert( struct mulle_concurrent_pointerset *set,
                                          void *ptr);
int   mulle_concurrent_pointerset_insert( struct mulle_concurrent_pointerset *set,
                                          void *ptr);
```
Insert pointer. Returns `0` inserted, `EEXIST` already present, `EINVAL` invalid argument.

```c
int  _mulle_concurrent_pointerset_member( struct mulle_concurrent_pointerset *set,
                                          void *ptr);
static inline int
   mulle_concurrent_pointerset_member( struct mulle_concurrent_pointerset *set,
                                       void *ptr);
```
Returns `1` if `ptr` is in the set, `0` if not. NULL `set`/`ptr` return `0` silently without setting `errno`.

```c
int  _mulle_concurrent_pointerset_remove( struct mulle_concurrent_pointerset *set,
                                          void *ptr);
int   mulle_concurrent_pointerset_remove( struct mulle_concurrent_pointerset *set,
                                          void *ptr);
```
Remove pointer. Returns `0` removed, `ENOENT` not found, `EINVAL` invalid argument.

**Inspection Functions:**

```c
size_t  _mulle_concurrent_pointerset_get_size( struct mulle_concurrent_pointerset *set);
static inline size_t
   mulle_concurrent_pointerset_get_size( struct mulle_concurrent_pointerset *set);
```
Current storage capacity. Snapshot only.

```c
static inline struct mulle_allocator *
   mulle_concurrent_pointerset_get_allocator( struct mulle_concurrent_pointerset *set);
```
Returns the allocator used by the set, or NULL if `set` is NULL. Uses a relaxed atomic read, so it is safe to call concurrently.

```c
size_t   mulle_concurrent_pointerset_count( struct mulle_concurrent_pointerset *set);
```
Counts live entries via enumeration (with retry on ECANCELED). Expensive; snapshot only.

```c
void          *mulle_concurrent_pointerset_lookup_any( struct mulle_concurrent_pointerset *set);
```
Returns any pointer from the set, or NULL if empty.

**Enumeration (limited multi-threaded):**

```c
struct mulle_concurrent_pointerset_enumerator
{
   struct mulle_concurrent_pointerset   *set;
   uintptr_t                            index;
   uintptr_t                            mask;
};

int  _mulle_concurrent_pointerset_enumerator_next( struct mulle_concurrent_pointerset_enumerator *rover,
                                                   void **ptr);

static inline struct mulle_concurrent_pointerset_enumerator
   mulle_concurrent_pointerset_enumerate( struct mulle_concurrent_pointerset *set);

static inline int
   mulle_concurrent_pointerset_enumerator_next( struct mulle_concurrent_pointerset_enumerator *rover,
                                                void **ptr);

static inline void
   mulle_concurrent_pointerset_enumerator_done( struct mulle_concurrent_pointerset_enumerator *rover);
```
Return codes: `1` OK (`*ptr` filled), `0` done, `ECANCELED` mutation detected, `EINVAL` invalid argument. NULL set produces an empty enumerator; the enumerator is only usable by the calling thread.

**Convenience Macro:**

```c
mulle_concurrent_pointerset_for( name, ptr)
```

### 3.6. `mulle-concurrent.h`

Umbrella header including all: `mulle-concurrent-types.h`, `mulle-concurrent-hashmap.h`, `mulle-concurrent-hashtable.h`, `mulle-concurrent-pointerarray.h`, `mulle-concurrent-pointerset.h`, plus the generated version check.

```c
#define MULLE__CONCURRENT_VERSION  ((4UL << 20) | (0 << 8) | 0)
```
Encoded version: (major << 20) | (minor << 8) | revision — currently 4.0.0.

## 4. Performance Characteristics

### mulle_concurrent_hashtable

- **Insert/Register/Lookup/Remove:** O(1) average, wait-free with atomic CAS.
- **Remove:** no migration and no tombstone; the slot is immediately reusable. Remove-heavy workloads do not degrade probe chains and do not trigger extra migrations.
- **Migration (growth):** Amortized O(n), non-blocking for concurrent callers, triggered at 50% load. A frozen slot preserves its payload, so copy performs at most two hash-word CAS attempts per slot regardless of value churn (the carrying thread re-reads the hash word after a successful value CAS — the "post-check").
- **Enumeration/Count:** O(capacity), scans entire storage; ECANCELED if the storage migrates during iteration.
- **Space:** O(capacity), capacity >= count. No tombstone overhead; value word is one word per slot.
- **Thread-safety:** Wait-free for point operations. Enumeration returns ECANCELED on migration.

### mulle_concurrent_hashmap

- **Lookup:** O(1) average, wait-free with bounded retry.
- **Insert/Register:** O(1) average, wait-free with atomic CAS.
- **Re-insert of a removed hash:** correctness requires dropping the tombstone, which triggers a same-size migration — O(capacity) copy. Avoid tight remove/reinsert cycles on the same hash.
- **Patch/Remove:** single-threaded only; O(1) but undefined behavior if called concurrently.
- **Enumeration/Count:** O(capacity); ECANCELED on mutation.
- **Space:** O(capacity); removed slots stay claimed until the next migration.
- **Thread-safety:** Wait-free for point operations; the former multi-threaded `remove` is no longer public — removal is single-threaded only.

### mulle_concurrent_pointerarray

- **Add:** O(1) amortized, wait-free with occasional resize.
- **Get:** O(1), direct index with atomic load.
- **Find:** O(n), linear search.
- **Enumeration:** O(count), safe even during concurrent additions.
- **Map:** O(count).
- **Space:** O(capacity), capacity >= count, doubles on growth.
- **Thread-safety:** Wait-free for all operations. No removal operations exist.

### mulle_concurrent_pointerset

- **Insert/Register/Member/Remove:** O(1) average, wait-free with linear probing and atomic CAS.
- **Migration:** Amortized O(n), triggered when (tombstones + live entries) >= 50% capacity.
- **Enumeration/Count:** O(capacity); ECANCELED on mutation.
- **Space:** O(capacity), capacity >= count + tombstones. Tombstones are dropped during migration; remove-heavy workloads trigger earlier migration.
- **Thread-safety:** Wait-free for core operations. Enumeration returns ECANCELED on mutation.

### General

- No locks or mutexes — all synchronization via atomic operations (CAS, atomic loads/stores).
- Memory reclamation of retired storage is deferred via mulle-aba (freed once all threads quiesce).
- Allocation is fail-fast: a conforming mulle allocator succeeds or aborts; `ENOMEM` paths are defensive and cannot occur with a conforming allocator.
- Hash quality heavily impacts performance; avoid clustering. For the hashtable, the topmost hash bit is reserved (or bit 0 in "even" mode).

## 5. AI Usage Recommendations & Patterns

### Best Practices

1. **Always initialize ABA:** call `mulle_aba_init( allocator)` once per process; each thread MUST call `mulle_aba_register()` before accessing structures and `mulle_aba_unregister()` when done.
2. **Single-threaded lifecycle:** `_init` and `_done` (all structures) and `_mulle_concurrent_hashmap_patch`/`_mulle_concurrent_hashmap_remove` (hashmap) must only be called when no other thread accesses the structure.
3. **Prefer the hashtable for new code** unless you need full-size hashes mapping to `void *` keys (hashmap) or you are tied to the legacy hashmap contract. The hashtable allows immediate reuse of removed hashes and has no tombstone accumulation.
4. **Respect hash width restrictions:** hashtable hashes must be in `[1, INTPTR_MAX]` ("positive" mode) or even and non-zero ("even" mode). With the hashmap, `hash` is a full `intptr_t` (NULL hash is `MULLE_CONCURRENT_NO_HASH`); never use `hash == 0`.
5. **Match hash AND value:** hashtable and (legacy) hashmap `remove` require the exact `(hash, value)` pair so a newer value cannot be removed by a stale caller.
6. **Avoid sentinel payloads:** hashmap and pointerset reject NULL/`INTPTR_MIN`/`INTPTR_MAX`; hashtable only reserves NULL. Never store a reserved value as a payload in the structure that reserves it.
7. **Handle enumeration mutations:** check for `ECANCELED` and retry the whole enumeration from the start; retries may duplicate or miss entries changed in between — that is expected.
8. **Use convenience macros** (`mulle_concurrent_hashtable_for`, `mulle_concurrent_hashmap_for`, `mulle_concurrent_pointerarray_for`, `mulle_concurrent_pointerset_for`) for correct enumerator lifecycle; use `_rval` variants when you need to detect `ECANCELED`.
9. **Accept snapshot semantics:** `count`, `get_size`, `get_count` and `lookup_any` return potentially-stale snapshots. Use them for diagnostics/heuristics, not correctness decisions.
10. **Pointerset reset for tombstone cleanup:** use `mulle_concurrent_pointerset_reset()` in remove-heavy workloads.
11. **Connect a custom allocator to ABA:** use `mulle_allocator_set_aba()` so retired storage is freed safely.
12. **Use `_prefixed` functions for hot paths** only when parameters are guaranteed valid — they skip NULL checks.

### Common Pitfalls

1. **Forgetting ABA registration** causes crashes or memory corruption.
2. **Using zero hash** — treated as invalid sentinel by both hash maps.
3. **Hashmap: concurrent `patch`/`remove`** — these are single-threaded only; concurrent use is undefined behavior.
4. **Hashmap: tight remove/reinsert cycles** on the same hash trigger repeated same-size migrations (each is a full table copy).
5. **Hashtable: hash with the FROZEN bit set** (e.g. pointer on ILP32 `>= 0x80000000`) — aliases silently; debug builds assert.
6. **Storing sentinel pointers** in a structure that reserves them.
7. **Concurrent init/done** while other threads access the structure.
8. **Ignoring ECANCELED** during enumeration leads to incomplete iteration.
9. **Sharing enumerators** between threads — each thread must create its own.
10. **Pointerset member with NULL** returns 0 silently without setting errno.
11. **Pointerset tombstone accumulation** — heavy remove usage triggers early migration; use `reset`.

### Idiomatic Usage

Initialize ABA at startup, register each thread before use:

```c
mulle_aba_init( NULL);          // once per process
mulle_aba_register();           // each thread, before touching any structure
// ... use structures ...
mulle_aba_unregister();         // each thread, before exit
mulle_aba_done();               // once per process
```

Error handling: check return values against `0` (success) or errno codes (`EINVAL`, `ENOMEM`, `ENOENT`, `EEXIST`, `ECANCELED`).

## 6. Integration Examples

### Example 1: Hashtable — Insert, Lookup, Remove, Immediate Reuse

```c
#include <mulle-concurrent/mulle-concurrent.h>
#include <errno.h>
#include <assert.h>

int   main( int argc, char *argv[])
{
   struct mulle_concurrent_hashtable   map;
   void                                *old;
   int                                 rval;

   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_hashtable_init( &map, 0, NULL);

   rval = mulle_concurrent_hashtable_insert( &map, 0x1848, (void *) 1848);
   assert( rval == 0);
   // duplicate insert fails
   rval = mulle_concurrent_hashtable_insert( &map, 0x1848, (void *) 9999);
   assert( rval == EEXIST);

   assert( mulle_concurrent_hashtable_lookup( &map, 0x1848) == (void *) 1848);

   // remove requires hash AND value to match
   rval = mulle_concurrent_hashtable_remove( &map, 0x1848, (void *) 9999);
   assert( rval == ENOENT);
   rval = mulle_concurrent_hashtable_remove( &map, 0x1848, (void *) 1848);
   assert( rval == 0);
   assert( mulle_concurrent_hashtable_lookup( &map, 0x1848) == NULL);

   // register: insert if absent, otherwise report the existing value via *p_old
   rval = mulle_concurrent_hashtable_register( &map, 0x1848, (void *) 1849, &old);
   assert( rval == 0 && old == NULL);
   rval = mulle_concurrent_hashtable_register( &map, 0x1848, (void *) 7777, &old);
   assert( rval == 0 && old == (void *) 1849);
   assert( mulle_concurrent_hashtable_lookup( &map, 0x1848) == (void *) 1849);

   mulle_concurrent_hashtable_done( &map);

   mulle_aba_unregister();
   mulle_aba_done();
   return( 0);
}
```

### Example 2: Hashtable Enumeration with Migration Retry

```c
#include <mulle-concurrent/mulle-concurrent.h>
#include <assert.h>
#include <stdio.h>

int   main( int argc, char *argv[])
{
   struct mulle_concurrent_hashtable             map;
   struct mulle_concurrent_hashtableenumerator   rover;
   intptr_t                                      hash;
   void                                          *value;
   unsigned int                                  i;
   int                                           rval;

   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_hashtable_init( &map, 0, NULL);
   for( i = 1; i <= 10; i++)
      assert( mulle_concurrent_hashtable_insert( &map, i, (void *) (intptr_t) (i * 100)) == 0);

retry:
   rover = mulle_concurrent_hashtable_enumerate( &map);
   while( (rval = mulle_concurrent_hashtableenumerator_next( &rover, &hash, &value)) == 1)
      printf( "hash=%ld value=%p\n", (long) hash, value);
   mulle_concurrent_hashtableenumerator_done( &rover);
   if( rval == ECANCELED)
      goto retry;                       // storage migrated — restart enumeration

   mulle_concurrent_hashtable_done( &map);
   mulle_aba_unregister();
   mulle_aba_done();
   return( 0);
}
```

### Example 3: Hashtable Enumeration via Macro

```c
#include <mulle-concurrent/mulle-concurrent.h>
#include <stdio.h>

int   main( int argc, char *argv[])
{
   struct mulle_concurrent_hashtable   map;
   intptr_t                            hash;
   void                                *value;

   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_hashtable_init( &map, 0, NULL);
   mulle_concurrent_hashtable_insert( &map, 0xABCD, (void *) 0x1111);
   mulle_concurrent_hashtable_insert( &map, 0xDEAD, (void *) 0x2222);
   mulle_concurrent_hashtable_insert( &map, 0xBEEF, (void *) 0x3333);

   mulle_concurrent_hashtable_for( &map, hash, value)
      printf( "hash=0x%lx, value=%p\n", (unsigned long) hash, value);

   mulle_concurrent_hashtable_done( &map);
   mulle_aba_unregister();
   mulle_aba_done();
   return( 0);
}
```

### Example 4: Pointerset — Insert, Member, Remove, Reset

```c
#include <mulle-concurrent/mulle-concurrent.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>

int   main( int argc, char *argv[])
{
   struct mulle_concurrent_pointerset   set;
   void                                 *p1    = (void *) 0x1000;
   void                                 *p2    = (void *) 0x2000;
   void                                 *p3    = (void *) 0x3000;
   void                                 *result;
   unsigned int                         i;

   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_pointerset_init( &set, 0, NULL);

   assert( mulle_concurrent_pointerset_insert( &set, p1) == 0);
   assert( mulle_concurrent_pointerset_insert( &set, p2) == 0);
   assert( mulle_concurrent_pointerset_insert( &set, p1) == EEXIST);

   assert( mulle_concurrent_pointerset_member( &set, p1) == 1);
   assert( mulle_concurrent_pointerset_member( &set, p3) == 0);

   result = mulle_concurrent_pointerset_register( &set, p3);
   assert( result == MULLE_CONCURRENT_NO_POINTER);
   result = mulle_concurrent_pointerset_register( &set, p3);
   assert( result == p3);

   assert( mulle_concurrent_pointerset_remove( &set, p1) == 0);
   assert( mulle_concurrent_pointerset_remove( &set, p1) == ENOENT);

   // heavy remove workload: drop tombstones explicitly
   for( i = 0x4000; i <= 0x6000; i += 0x10)
      mulle_concurrent_pointerset_insert( &set, (void *) (uintptr_t) i);
   mulle_concurrent_pointerset_reset( &set);

   mulle_concurrent_pointerset_done( &set);
   mulle_aba_unregister();
   mulle_aba_done();
   return( 0);
}
```

### Example 5: Pointer Array — Add, Get, Find, Map

```c
#include <mulle-concurrent/mulle-concurrent.h>
#include <assert.h>
#include <stdio.h>

static void   print_value( void *value, void *userinfo)
{
   MULLE_C_UNUSED( userinfo);
   printf( "%p\n", value);
}

int   main( int argc, char *argv[])
{
   struct mulle_concurrent_pointerarray   array;
   unsigned int                           i;

   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_pointerarray_init( &array, 0, NULL);
   for( i = 1; i <= 5; i++)
      mulle_concurrent_pointerarray_add( &array, (void *) (uintptr_t) (i * 10));

   assert( mulle_concurrent_pointerarray_get( &array, 2) == (void *) 30);
   assert( mulle_concurrent_pointerarray_find( &array, (void *) 40) == 3);
   assert( mulle_concurrent_pointerarray_get_count( &array) == 5);

   mulle_concurrent_pointerarray_map( &array, print_value, NULL);

   mulle_concurrent_pointerarray_done( &array);
   mulle_aba_unregister();
   mulle_aba_done();
   return( 0);
}
```

### Example 6: Legacy Hashmap — Insert, Lookup, Single-Threaded Patch/Remove

```c
#include <mulle-concurrent/mulle-concurrent.h>
#include <errno.h>
#include <assert.h>

int   main( int argc, char *argv[])
{
   struct mulle_concurrent_hashmap   map;
   int                               rval;

   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_hashmap_init( &map, 0, NULL);

   rval = mulle_concurrent_hashmap_insert( &map, 0x1848, (void *) 1848);
   assert( rval == 0);
   assert( mulle_concurrent_hashmap_lookup( &map, 0x1848) == (void *) 1848);

   // patch: single-threaded unconditional value replacement
   rval = _mulle_concurrent_hashmap_patch( &map, 0x1848, (void *) 2048);
   assert( rval == 0);
   assert( mulle_concurrent_hashmap_lookup( &map, 0x1848) == (void *) 2048);
   rval = _mulle_concurrent_hashmap_patch( &map, 0xDEAD, (void *) 1);
   assert( rval == ENOENT);            // absent key

   // remove: single-threaded, clears the value, keeps the hash claim
   _mulle_concurrent_hashmap_remove( &map, 0x1848);
   assert( mulle_concurrent_hashmap_lookup( &map, 0x1848) == NULL);

   mulle_concurrent_hashmap_done( &map);
   mulle_aba_unregister();
   mulle_aba_done();
   return( 0);
}
```

## 7. Dependencies

- **mulle-aba** — Lock-free ABA problem solution. Provides automatic deferred memory reclamation for the concurrent data structures. Essential for safe freeing of old storage during resize/migration. Every participating thread must be registered.

Transitive dependencies of mulle-aba:
- `mulle-c11` — C11 compatibility and atomic operations
- `mulle-thread` — Cross-platform threading primitives
- `mulle-allocator` — Memory allocation abstraction

All resolved automatically by the mulle-sde build system.