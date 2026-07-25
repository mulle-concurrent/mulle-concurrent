# mulle-concurrent Library Documentation for AI
<!-- Keywords: lockfree, waitfree, hashmap, pointerarray, pointerset, concurrent, C -->

## 1. Introduction & Purpose

- mulle-concurrent provides wait-free, lock-free concurrent data structures in C: a resizable hashmap, a grow-only pointer array, and a pointer set with single-word-per-slot linear probing.
- Solves contention in multithreaded environments where low-latency, non-blocking operations are required.
- Key features: wait-free register/insert/lookup/remove for all structures, lock-free add/get/find and enumerators, optional custom allocator, ABA handling via mulle-aba.
- A component of mulle-core; depends on mulle-aba and mulle-allocator conventions.

## 2. Key Concepts & Design Philosophy

- Wait-free designs: operations aim to complete in a finite number of steps regardless of other threads.
- Resizable concurrent map inspired by Preshing's resizable concurrent map but implemented to be wait-free.
- The pointer array grows but never shrinks — this limitation simplifies concurrent access.
- Pointerset uses linear probing with tombstones for removal; tombstones are cleaned up during migration.
- Growing/migration is triggered at 50% load (including tombstones for pointerset). Migration is atomic and non-blocking for callers.
- ABA problem management via mulle-aba; each thread must register/unregister with the ABA system before accessing structures.
- Enumerators are "limited multi-threaded": safe for single-threaded use or when no concurrent removals/growth occur; mutation is signaled via error codes (ECANCELLED/EBUSY).

## 3. Core API & Data Structures

### 3.1. `mulle-concurrent-types.h`

Sentinel values and constants:

| Constants                         | Value                 | Meaning
| ----------------------------------|-----------------------|---------
| `MULLE_CONCURRENT_NO_HASH`        | `0`                   | Invalid hash sentinel, never use as actual hash
| `MULLE_CONCURRENT_INVALID_POINTER`| `((void *) INTPTR_MIN)`| Internal REDIRECT marker during migration
| `MULLE_CONCURRENT_NO_POINTER`     | `((void *) 0)`        | Internal sentinel for absent value (NULL)
| `MULLE_CONCURRENT_TOMBSTONE_POINTER`| `((void *) INTPTR_MAX)`| Removed slot marker in pointerset

User code must not use any of these values as payload data.

### 3.2. `mulle-concurrent-hashmap.h`

#### `struct mulle_concurrent_hashmap`

**Purpose:** Wait-free, resizable hash table mapping `intptr_t` hashes to `void *` pointer values.

**Key Fields (opaque):**
- `storage` — current hash table (`union mulle_concurrent_atomichashmapstorage_t`)
- `next_storage` — next storage during resize
- `allocator` — memory allocator (`mulle_atomic_pointer_t`)

**Internal Structures:**
- `struct _mulle_concurrent_hashvaluepair` — hash/value pair with atomic value pointer
- `struct _mulle_concurrent_hashmapstorage` — backing storage with `n_hashs` count and `mask` (capacity-1)

**Lifecycle Functions (single-threaded only):**

```c
int  _mulle_concurrent_hashmap_init( struct mulle_concurrent_hashmap *map, unsigned int size, struct mulle_allocator *allocator);
int  mulle_concurrent_hashmap_init( struct mulle_concurrent_hashmap *map, unsigned int size, struct mulle_allocator *allocator);
```
Returns 0 on success, EINVAL for NULL map, ENOMEM on allocation failure.

```c
void  _mulle_concurrent_hashmap_done( struct mulle_concurrent_hashmap *map);
void  mulle_concurrent_hashmap_done( struct mulle_concurrent_hashmap *map);
```
Frees all internal resources. Safe to call with NULL (checked version).

**Core Operations (multi-threaded safe):**

```c
void  *_mulle_concurrent_hashmap_register( struct mulle_concurrent_hashmap *map, intptr_t hash, void *value);
void  *mulle_concurrent_hashmap_register( struct mulle_concurrent_hashmap *map, intptr_t hash, void *value);
```
Insert or retrieve existing value. Returns `MULLE_CONCURRENT_NO_POINTER` if inserted, `MULLE_CONCURRENT_INVALID_POINTER` on error (check errno), or the existing value. Do not use hash==0 or value==NULL/INTPTR_MIN.

```c
int  _mulle_concurrent_hashmap_insert( struct mulle_concurrent_hashmap *map, intptr_t hash, void *value);
int  mulle_concurrent_hashmap_insert( struct mulle_concurrent_hashmap *map, intptr_t hash, void *value);
```
Insert hash/value pair. Returns 0 on success, EEXIST if duplicate, EINVAL, ENOMEM.

```c
int  _mulle_concurrent_hashmap_patch( struct mulle_concurrent_hashmap *map, intptr_t hash, void *value, void *expect);
int  mulle_concurrent_hashmap_patch( struct mulle_concurrent_hashmap *map, intptr_t hash, void *value, void *expect);
```
**EXPERIMENTAL** — conditionally update existing entry's value atomically. Returns 0 if patched, EEXIST if different value, ENOENT if not found, EINVAL, ENOMEM.

```c
void  *_mulle_concurrent_hashmap_lookup( struct mulle_concurrent_hashmap *map, intptr_t hash);
void  *mulle_concurrent_hashmap_lookup( struct mulle_concurrent_hashmap *map, intptr_t hash);
```
Look up value by hash. Returns value pointer or NULL (not found).

```c
int  _mulle_concurrent_hashmap_remove( struct mulle_concurrent_hashmap *map, intptr_t hash, void *value);
int  mulle_concurrent_hashmap_remove( struct mulle_concurrent_hashmap *map, intptr_t hash, void *value);
```
Remove entry — both hash AND value must match. Returns 0, ENOENT, EINVAL, ENOMEM.

**Inspection Functions:**

```c
unsigned int  _mulle_concurrent_hashmap_get_size( struct mulle_concurrent_hashmap *map);
unsigned int  mulle_concurrent_hashmap_get_size( struct mulle_concurrent_hashmap *map);
```
Returns current storage capacity (not count). Snapshot only.

```c
unsigned int  mulle_concurrent_hashmap_count( struct mulle_concurrent_hashmap *map);
```
Counts entries via enumeration. Expensive. Snapshot only.

```c
void  *mulle_concurrent_hashmap_lookup_any( struct mulle_concurrent_hashmap *map);
```
Returns any pointer from the map, or NULL if empty.

**Enumeration:**

```c
struct mulle_concurrent_hashmapenumerator
{
   struct mulle_concurrent_hashmap   *map;
   unsigned int                      index;
   unsigned int                      mask;
};

struct mulle_concurrent_hashmapenumerator
   mulle_concurrent_hashmap_enumerate( struct mulle_concurrent_hashmap *map);

int  _mulle_concurrent_hashmapenumerator_next( struct mulle_concurrent_hashmapenumerator *rover, intptr_t *hash, void **value);
int  mulle_concurrent_hashmapenumerator_next( struct mulle_concurrent_hashmapenumerator *rover, intptr_t *hash, void **value);
```
Returns 1 for next entry (hash and value filled), 0 when done, ECANCELLED if mutated, ENOMEM, EINVAL.

```c
void  mulle_concurrent_hashmapenumerator_done( struct mulle_concurrent_hashmapenumerator *rover);
```
Cleanup (currently no-op, call for forward compatibility). Enumerator is thread-local, must not be shared.

**Convenience Macros:**

```c
mulle_concurrent_hashmap_for( map, hash, value)       // for-loop style enumeration
mulle_concurrent_hashmap_for_rval( map, hash, value, rval)  // with error return value
```

Unsafe `_prefixed` variants skip NULL validation — use when parameters are guaranteed valid for performance.

### 3.3. `mulle-concurrent-pointerarray.h`

#### `struct mulle_concurrent_pointerarray`

**Purpose:** Wait-free, lock-free, grow-only pointer array. Safe for concurrent reads and appends. Cannot shrink or overwrite elements.

**Key Fields:**
- `storage` — current array storage
- `next_storage` — next storage during growth
- `allocator` — memory allocator (not atomic pointer, unlike hashmap)

**Lifecycle Functions (single-threaded only):**

```c
void  _mulle_concurrent_pointerarray_init( struct mulle_concurrent_pointerarray *array, unsigned int size, struct mulle_allocator *allocator);
int   mulle_concurrent_pointerarray_init( struct mulle_concurrent_pointerarray *array, unsigned int size, struct mulle_allocator *allocator);
```
Returns 0 on success, EINVAL for NULL array.

```c
void  _mulle_concurrent_pointerarray_done( struct mulle_concurrent_pointerarray *array);
void  mulle_concurrent_pointerarray_done( struct mulle_concurrent_pointerarray *array);
```
Frees internal resources.

**Core Operations (multi-threaded safe):**

```c
void  _mulle_concurrent_pointerarray_add( struct mulle_concurrent_pointerarray *array, void *value);
int   mulle_concurrent_pointerarray_add( struct mulle_concurrent_pointerarray *array, void *value);
```
Append value to end of array. Returns 0 on success, EINVAL if NULL/INTPTR_MIN value, ENOMEM on allocation failure.

```c
void  *_mulle_concurrent_pointerarray_get( struct mulle_concurrent_pointerarray *array, unsigned int index);
void  *mulle_concurrent_pointerarray_get( struct mulle_concurrent_pointerarray *array, unsigned int i);
```
Get value at index. Returns pointer or NULL (invalid index).

```c
int  _mulle_concurrent_pointerarray_find( struct mulle_concurrent_pointerarray *array, void *search);
int  mulle_concurrent_pointerarray_find( struct mulle_concurrent_pointerarray *array, void *value);
```
Linear search for value. Returns index on found, EINVAL on error. Note: returns index even when value is stored at index 0 (return != 0 is the important check).

**Inspection Functions:**

```c
unsigned int  _mulle_concurrent_pointerarray_get_size( struct mulle_concurrent_pointerarray *array);
unsigned int  mulle_concurrent_pointerarray_get_size( struct mulle_concurrent_pointerarray *array);
```
Returns current capacity. Snapshot only.

```c
unsigned int  _mulle_concurrent_pointerarray_get_count( struct mulle_concurrent_pointerarray *array);
unsigned int  mulle_concurrent_pointerarray_get_count( struct mulle_concurrent_pointerarray *array);
```
Returns current element count. More reliable than hashmap count (grow-only).

**Mapping:**

```c
int  mulle_concurrent_pointerarray_map( struct mulle_concurrent_pointerarray *list, void (*f)( void *, void *), void *userinfo);
```
Apply function `f` to each element. Returns 0, EINVAL.

**Enumeration:**

```c
struct mulle_concurrent_pointerarrayenumerator { struct mulle_concurrent_pointerarray *array; unsigned int index; };
struct mulle_concurrent_pointerarrayreverseenumerator { struct mulle_concurrent_pointerarray *array; unsigned int index; };

struct mulle_concurrent_pointerarrayenumerator
   mulle_concurrent_pointerarray_enumerate( struct mulle_concurrent_pointerarray *array);

struct mulle_concurrent_pointerarrayreverseenumerator
   mulle_concurrent_pointerarray_reverseenumerate( struct mulle_concurrent_pointerarray *array, unsigned int n);

void  *_mulle_concurrent_pointerarrayenumerator_next( struct mulle_concurrent_pointerarrayenumerator *rover);
void  *mulle_concurrent_pointerarrayenumerator_next( struct mulle_concurrent_pointerarrayenumerator *rover);
```
Returns next pointer or NULL when exhausted. Safe even if array grows during iteration.

```c
void  *_mulle_concurrent_pointerarrayreverseenumerator_next( struct mulle_concurrent_pointerarrayreverseenumerator *rover);
void  *mulle_concurrent_pointerarrayreverseenumerator_next( struct mulle_concurrent_pointerarrayreverseenumerator *rover);
```
Same but in reverse order.

```c
void  mulle_concurrent_pointerarrayenumerator_done( struct mulle_concurrent_pointerarrayenumerator *rover);
void  mulle_concurrent_pointerarrayreverseenumerator_done( struct mulle_concurrent_pointerarrayreverseenumerator *rover);
```
Convenience Macros:

```c
mulle_concurrent_pointerarray_for( array, item)                 // forward enumeration
mulle_concurrent_pointerarray_for_reverse( array, n, item)       // reverse enumeration
```

### 3.4. `mulle-concurrent-pointerset.h`

#### `struct mulle_concurrent_pointerset`

**Purpose:** Wait-free set of `void *` pointers using linear probing with single-word-per-slot layout. The pointer value itself is hashed internally — no separate hash key needed.

**Key Fields (opaque):**
- `storage` — current storage
- `next_storage` — next storage during migration
- `allocator` — memory allocator (`mulle_atomic_pointer_t`)

**Slot States (linear probing):**
- `NULL` (EMPTY) — probe chain stops here
- `INTPTR_MIN` (REDIRECT) — migration in progress, retry
- `INTPTR_MAX` (TOMBSTONE) — removed slot, probe chain continues
- Anything else — live pointer

**Tombstone rationale:** With linear probing, pointers hashing to the same slot form chains. Removing by writing NULL would break the chain; a tombstone preserves correctness by saying "keep probing." Tombstones are NOT reused for new inserts (to avoid races) but are dropped during migration.

**Lifecycle Functions (single-threaded only):**

```c
int  _mulle_concurrent_pointerset_init( struct mulle_concurrent_pointerset *set, unsigned int size, struct mulle_allocator *allocator);
int  mulle_concurrent_pointerset_init( struct mulle_concurrent_pointerset *set, unsigned int size, struct mulle_allocator *allocator);
```
Returns 0 on success, EINVAL for NULL set, ENOMEM on allocation failure.

```c
void  _mulle_concurrent_pointerset_done( struct mulle_concurrent_pointerset *set);
void  mulle_concurrent_pointerset_done( struct mulle_concurrent_pointerset *set);
```
Frees all internal resources.

**Core Operations (multi-threaded safe):**

```c
void  *_mulle_concurrent_pointerset_register( struct mulle_concurrent_pointerset *set, void *ptr);
void  *mulle_concurrent_pointerset_register( struct mulle_concurrent_pointerset *set, void *ptr);
```
Insert or retrieve existing pointer. Returns `MULLE_CONCURRENT_NO_POINTER` if inserted, `MULLE_CONCURRENT_INVALID_POINTER` on error (check errno), or the existing pointer.

```c
int  _mulle_concurrent_pointerset_insert( struct mulle_concurrent_pointerset *set, void *ptr);
int  mulle_concurrent_pointerset_insert( struct mulle_concurrent_pointerset *set, void *ptr);
```
Insert pointer. Returns 0, EEXIST if already present, EINVAL, ENOMEM.

```c
int  _mulle_concurrent_pointerset_member( struct mulle_concurrent_pointerset *set, void *ptr);
int  mulle_concurrent_pointerset_member( struct mulle_concurrent_pointerset *set, void *ptr);
```
Returns 1 if ptr is in the set, 0 if not (including NULL ptr — returns 0 silently without setting errno).

```c
int  _mulle_concurrent_pointerset_remove( struct mulle_concurrent_pointerset *set, void *ptr);
int  mulle_concurrent_pointerset_remove( struct mulle_concurrent_pointerset *set, void *ptr);
```
Remove pointer. Returns 0, ENOENT if not found, EINVAL, ENOMEM.

**Do not pass NULL, `MULLE_CONCURRENT_INVALID_POINTER`, or `MULLE_CONCURRENT_TOMBSTONE_POINTER` as pointer values to insert/register/remove.**

**Inspection Functions:**

```c
unsigned int  _mulle_concurrent_pointerset_get_size( struct mulle_concurrent_pointerset *set);
unsigned int  mulle_concurrent_pointerset_get_size( struct mulle_concurrent_pointerset *set);
```
Returns current storage capacity. Snapshot only.

```c
struct mulle_allocator  *mulle_concurrent_pointerset_get_allocator( struct mulle_concurrent_pointerset *set);
```
Returns the allocator used by the set, or NULL if set is NULL.

```c
unsigned int  mulle_concurrent_pointerset_count( struct mulle_concurrent_pointerset *set);
```
Counts entries via enumeration with automatic retry on ECANCELED. Expensive; snapshot only.

```c
void  *mulle_concurrent_pointerset_lookup_any( struct mulle_concurrent_pointerset *set);
```
Returns any pointer from the set, or NULL if empty.

**Reset:**

```c
void  mulle_concurrent_pointerset_reset( struct mulle_concurrent_pointerset *set);
```
Clears the set by combining done+init with same size and allocator. Efficient reuse.

**Enumeration:**

```c
struct mulle_concurrent_pointerset_enumerator
{
   struct mulle_concurrent_pointerset   *set;
   unsigned int                         index;
   unsigned int                         mask;
};

struct mulle_concurrent_pointerset_enumerator
   mulle_concurrent_pointerset_enumerate( struct mulle_concurrent_pointerset *set);

int  _mulle_concurrent_pointerset_enumerator_next( struct mulle_concurrent_pointerset_enumerator *rover, void **ptr);
int  mulle_concurrent_pointerset_enumerator_next( struct mulle_concurrent_pointerset_enumerator *rover, void **ptr);
```
Returns 1 for next entry (ptr filled), 0 when done, ECANCELLED if mutated, ENOMEM, EINVAL.

```c
void  mulle_concurrent_pointerset_enumerator_done( struct mulle_concurrent_pointerset_enumerator *rover);
```

**Convenience Macro:**

```c
mulle_concurrent_pointerset_for( set, ptr)
```

### 3.5. `mulle-concurrent.h`

Umbrella header including all: types, hashmap, pointerarray, pointerset, and version check.

```c
#define MULLE__CONCURRENT_VERSION  ((3UL << 20) | (2 << 8) | 0)
```
Encoded version: (major << 20) | (minor << 8) | patch.

## 4. Performance Characteristics

### mulle_concurrent_hashmap

- **Lookup:** O(1) average, wait-free with bounded retry on collision
- **Insert:** O(1) average, wait-free with atomic CAS
- **Remove:** O(1) average, wait-free with atomic CAS
- **Resize:** Amortized O(n), non-blocking for concurrent callers
- **Enumeration/Count:** O(capacity), scans entire storage
- **Space:** O(capacity), capacity >= count, doubles on growth
- **Thread-safety:** Wait-free for core operations. Enumeration returns ECANCELLED on mutation.
- **Memory reclamation:** Old storage freed via mulle-aba once all threads quiesce.

### mulle_concurrent_pointerarray

- **Add:** O(1) amortized, wait-free with occasional resize
- **Get:** O(1), direct index with atomic load
- **Find:** O(n), linear search
- **Enumeration:** O(count), safe even during concurrent additions
- **Map:** O(count)
- **Space:** O(capacity), capacity >= count, doubles on growth
- **Thread-safety:** Wait-free for all operations. No removal operations exist.

### mulle_concurrent_pointerset

- **Insert/Register/Member/Remove:** O(1) average, wait-free with linear probing and atomic CAS
- **Migration:** Amortized O(n), triggered when (tombstones + live entries) >= 50% capacity
- **Enumeration/Count:** O(capacity)
- **Space:** O(capacity), capacity >= count + tombstones, doubles on growth. Tombstones consume capacity but are dropped during migration.
- **Thread-safety:** Wait-free for core operations. Enumeration returns ECANCELLED on mutation.
- **Tombstone accumulation:** Tombstones are never reused for inserts; they accumulate until migration. Remove-heavy workloads trigger earlier migration.

### General

- No locks or mutexes — all synchronization via atomic operations (CAS, atomic loads/stores)
- No spinning — wait-free guarantees bounded-time completion
- Hash quality heavily impacts performance; avoid clustering
- Custom allocators can reduce allocation overhead

## 5. AI Usage Recommendations & Patterns

### Best Practices

1. **Always initialize ABA system:** Call `mulle_aba_init(allocator)` once per process. Each thread MUST call `mulle_aba_register()` before accessing structures and `mulle_aba_unregister()` when done.
2. **Single-threaded lifecycle:** `_init` and `_done` must be called in single-threaded contexts.
3. **Use good hash functions:** Ensure well-distributed hashes; use avalanche functions for simple integer keys. Never use hash==0.
4. **Avoid sentinel values:** Never store NULL, INTPTR_MIN, or INTPTR_MAX as payload values.
5. **Handle enumeration mutations:** Check for ECANCELLED and retry from beginning.
6. **Prefer `_prefixed` functions for performance** when parameters are guaranteed valid.
7. **Use convenience macros** (`mulle_concurrent_hashmap_for`, `mulle_concurrent_pointerarray_for`, `mulle_concurrent_pointerset_for`) for correct enumerator lifecycle.
8. **Accept snapshot semantics:** `get_count()` and `get_size()` return potentially-stale snapshots.
9. **Match hash AND value on hashmap remove** — both must match to prevent accidental removal of updated entries.
10. **Pointerset reset for tombstone cleanup:** Use `mulle_concurrent_pointerset_reset()` to clear tombstone accumulation in remove-heavy workloads.
11. **Connect allocator to ABA:** Use `mulle_allocator_set_aba()` for automatic deferred freeing.

### Common Pitfalls

1. **Forgetting ABA registration** causes crashes or memory corruption.
2. **Using zero hash** — hashmap treats 0 as invalid sentinel.
3. **Storing sentinel pointers** (NULL, INTPTR_MIN, INTPTR_MAX) causes undefined behavior.
4. **Concurrent init/done** while other threads access the structure.
5. **Ignoring ECANCELLED** during enumeration leads to incomplete iteration.
6. **Assuming count accuracy** in multi-threaded contexts.
7. **Sharing enumerators** between threads — each thread must create its own.
8. **`mulle_concurrent_hashmap_patch()` is experimental** — prefer remove+insert for production code.
9. **Pointerset member with NULL** returns 0 silently without setting errno.
10. **Pointerset tombstone accumulation** — heavy remove usage triggers early migration.

### Idiomatic Usage

Initialize ABA at startup, register each thread before use:
```c
// Program startup (single thread)
mulle_aba_init( NULL);

// Each thread entry
mulle_aba_register();
// ... use structures ...
mulle_aba_unregister();

// Program shutdown (single thread)
mulle_aba_done();
```

Error handling: Check return values for 0 (success) or errno codes (EINVAL, ENOMEM, ENOENT, EEXIST, ECANCELLED).

## 6. Integration Examples

### Example 1: Basic Hashmap — Insert, Lookup, Remove

```c
#include <mulle-concurrent/mulle-concurrent.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>

int   main( int argc, char *argv[])
{
   struct mulle_concurrent_hashmap   map;
   int                               rval;
   void                              *value;

   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_hashmap_init( &map, 0, NULL);

   rval = mulle_concurrent_hashmap_insert( &map, 0x1848, (void *) 1848);
   assert( rval == 0);
   rval = mulle_concurrent_hashmap_insert( &map, 0x1849, (void *) 1849);
   assert( rval == 0);

   value = mulle_concurrent_hashmap_lookup( &map, 0x1848);
   assert( value == (void *) 1848);

   rval = mulle_concurrent_hashmap_remove( &map, 0x1848, (void *) 9999);
   assert( rval == ENOENT);

   rval = mulle_concurrent_hashmap_remove( &map, 0x1848, (void *) 1848);
   assert( rval == 0);

   value = mulle_concurrent_hashmap_lookup( &map, 0x1848);
   assert( value == NULL);

   mulle_concurrent_hashmap_done( &map);

   mulle_aba_unregister();
   mulle_aba_done();
   return( 0);
}
```

### Example 2: Hashmap Enumeration with Mutation Retry

```c
#include <mulle-concurrent/mulle-concurrent.h>
#include <stdio.h>
#include <errno.h>

int   main( int argc, char *argv[])
{
   struct mulle_concurrent_hashmap             map;
   struct mulle_concurrent_hashmapenumerator   rover;
   intptr_t                                    hash;
   void                                        *value;
   unsigned int                                i;
   int                                         rval;

   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_hashmap_init( &map, 0, NULL);
   for( i = 1; i <= 10; i++)
      _mulle_concurrent_hashmap_insert( &map, i, (void *) (intptr_t) (i * 100));

retry:
   rover = mulle_concurrent_hashmap_enumerate( &map);
   while( (rval = mulle_concurrent_hashmapenumerator_next( &rover, &hash, &value)) == 1)
      printf( "Hash: 0x%lx, Value: %p\n", (unsigned long) hash, value);
   mulle_concurrent_hashmapenumerator_done( &rover);

   if( rval == ECANCELLED)
      goto retry;

   mulle_concurrent_hashmap_done( &map);
   mulle_aba_unregister();
   mulle_aba_done();
   return( 0);
}
```

### Example 3: Hashmap with Enumeration Macro

```c
#include <mulle-concurrent/mulle-concurrent.h>
#include <stdio.h>

int   main( int argc, char *argv[])
{
   struct mulle_concurrent_hashmap   map;
   intptr_t                          hash;
   void                              *value;

   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_hashmap_init( &map, 0, NULL);
   _mulle_concurrent_hashmap_insert( &map, 0xABCD, (void *) 0x1111);
   _mulle_concurrent_hashmap_insert( &map, 0xDEAD, (void *) 0x2222);
   _mulle_concurrent_hashmap_insert( &map, 0xBEEF, (void *) 0x3333);

   mulle_concurrent_hashmap_for( &map, hash, value)
   {
      printf( "hash=0x%lx, value=%p\n", (unsigned long) hash, value);
   }

   mulle_concurrent_hashmap_done( &map);
   mulle_aba_unregister();
   mulle_aba_done();
   return( 0);
}
```

### Example 4: Pointer Array — Add, Get, Find, Enumeration

```c
#include <mulle-concurrent/mulle-concurrent.h>
#include <stdio.h>
#include <assert.h>

int   main( int argc, char *argv[])
{
   struct mulle_concurrent_pointerarray   array;
   void                                   *item;
   unsigned int                           i;
   unsigned int                           count;

   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_pointerarray_init( &array, 0, NULL);

   for( i = 1; i <= 5; i++)
      mulle_concurrent_pointerarray_add( &array, (void *) (uintptr_t) (i * 10));

   item = mulle_concurrent_pointerarray_get( &array, 2);
   assert( item == (void *) 30);

   i = mulle_concurrent_pointerarray_find( &array, (void *) 40);
   assert( i == 3);

   printf( "Forward:\n");
   mulle_concurrent_pointerarray_for( &array, item)
      printf( "  %p\n", item);

   count = mulle_concurrent_pointerarray_get_count( &array);
   printf( "Reverse:\n");
   mulle_concurrent_pointerarray_for_reverse( &array, count, item)
      printf( "  %p\n", item);

   mulle_concurrent_pointerarray_done( &array);
   mulle_aba_unregister();
   mulle_aba_done();
   return( 0);
}
```

### Example 5: Custom Allocator with ABA Integration

```c
#include <mulle-concurrent/mulle-concurrent.h>
#include <mulle-allocator/mulle-allocator.h>
#include <mulle-testallocator/mulle-testallocator.h>
#include <stdio.h>

int   main( int argc, char *argv[])
{
   struct mulle_concurrent_hashmap   map;
   unsigned int                      i;

   mulle_testallocator_reset();
   mulle_aba_init( &mulle_testallocator);

   mulle_allocator_set_aba( &mulle_testallocator,
                            mulle_aba_get_global(),
                            (mulle_allocator_aba_t *) _mulle_aba_free);

   mulle_aba_register();

   mulle_concurrent_hashmap_init( &map, 0, &mulle_testallocator);
   for( i = 1; i <= 100; i++)
      _mulle_concurrent_hashmap_insert( &map, i, (void *) (intptr_t) (i * 10));

   printf( "Inserted %u entries\n", i - 1);

   mulle_concurrent_hashmap_done( &map);
   mulle_aba_unregister();

   mulle_allocator_set_aba( &mulle_testallocator, NULL, NULL);
   mulle_aba_done();
   mulle_testallocator_reset();
   return( 0);
}
```

### Example 6: Pointerset — Insert, Member, Remove, Enumerate

```c
#include <mulle-concurrent/mulle-concurrent.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>

int   main( int argc, char *argv[])
{
   struct mulle_concurrent_pointerset   set;
   void                                 *p1 = (void *) 0x1000;
   void                                 *p2 = (void *) 0x2000;
   void                                 *p3 = (void *) 0x3000;
   void                                 *result;
   int                                  rval;

   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_pointerset_init( &set, 0, NULL);

   rval = mulle_concurrent_pointerset_insert( &set, p1);
   assert( rval == 0);
   rval = mulle_concurrent_pointerset_insert( &set, p2);
   assert( rval == 0);
   rval = mulle_concurrent_pointerset_insert( &set, p1);
   assert( rval == EEXIST);

   assert( mulle_concurrent_pointerset_member( &set, p1) == 1);
   assert( mulle_concurrent_pointerset_member( &set, p3) == 0);

   result = mulle_concurrent_pointerset_register( &set, p3);
   assert( result == MULLE_CONCURRENT_NO_POINTER);
   result = mulle_concurrent_pointerset_register( &set, p3);
   assert( result == p3);

   rval = mulle_concurrent_pointerset_remove( &set, p1);
   assert( rval == 0);
   rval = mulle_concurrent_pointerset_remove( &set, p1);
   assert( rval == ENOENT);

   mulle_concurrent_pointerset_for( &set, result)
   {
      printf( "Found pointer: %p\n", result);
   }

   mulle_concurrent_pointerset_done( &set);
   mulle_aba_unregister();
   mulle_aba_done();
   return( 0);
}
```

### Example 7: Pointerset with Growth and Reset

```c
#include <mulle-concurrent/mulle-concurrent.h>
#include <stdio.h>
#include <assert.h>

int   main( int argc, char *argv[])
{
   struct mulle_concurrent_pointerset   set;
   unsigned int                         i;
   uintptr_t                            base = 0x10000;

   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_pointerset_init( &set, 4, NULL);

   for( i = 1; i <= 200; i++)
      mulle_concurrent_pointerset_insert( &set, (void *) (base + i * 16));

   printf( "After insert: count=%u, size=%u\n",
           mulle_concurrent_pointerset_count( &set),
           mulle_concurrent_pointerset_get_size( &set));

   for( i = 1; i <= 100; i++)
      mulle_concurrent_pointerset_remove( &set, (void *) (base + i * 16));

   printf( "After remove: count=%u, size=%u\n",
           mulle_concurrent_pointerset_count( &set),
           mulle_concurrent_pointerset_get_size( &set));

   mulle_concurrent_pointerset_reset( &set);

   mulle_concurrent_pointerset_done( &set);
   mulle_aba_unregister();
   mulle_aba_done();
   return( 0);
}
```

## 7. Dependencies

- **mulle-aba** — Lock-free ABA problem solution. Provides automatic deferred memory reclamation for concurrent data structures. Essential for safe freeing of old storage during resize/migration.

Transitive dependencies of mulle-aba:
- `mulle-c11` — C11 compatibility and atomic operations
- `mulle-thread` — Cross-platform threading primitives
- `mulle-allocator` — Memory allocation abstraction

All resolved automatically by the mulle-sde build system.
