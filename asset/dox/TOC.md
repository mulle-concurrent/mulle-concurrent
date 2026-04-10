# mulle-concurrent Library Documentation for AI
<!-- Keywords: lockfree, waitfree, hashmap, pointerarray, concurrent, C -->
## 1. Introduction & Purpose

- mulle-concurrent provides wait-free, lock-free concurrent data structures in C: primarily a resizable hashmap and a growable pointer array.
- Solves contention in multithreaded environments where low-latency, non-blocking operations are required.
- Key features: wait-free register/insert/lookup/remove for hashmap, lock-free add/get/find and enumerators for pointerarray, optional custom allocator integration, ABA handling via mulle-aba.
- Relationship: component often used as part of mulle-core; depends on mulle-aba and mulle-allocator conventions.

## 2. Key Concepts & Design Philosophy

- Wait-free designs: operations aim to complete in a finite number of steps regardless of other threads.
- Resizable concurrent map inspired by Preshing's resizable concurrent map but implemented to be wait-free.
- Atomic storage units (mulle_atomic_pointer_t) and atomic unions for versioned storage swaps.
- ABA problem management via mulle-aba; many APIs expect caller to initialize/register ABA.
- Enumerators are "limited multi-threaded": safe for single-threaded use or when no concurrent removals/growth occur; enumerators will signal mutation via error codes.

## 3. Core API & Data Structures

### 3.1. [mulle-concurrent.h]
- Purpose: umbrella header; exposes public headers: types, hashmap, pointerarray.
- Usage: include <mulle-concurrent/mulle-concurrent.h> for main API.

### 3.2. [mulle-concurrent-types.h]
- Defines sentinel values and small constants:
  - MULLE_CONCURRENT_NO_HASH (0)
  - MULLE_CONCURRENT_INVALID_POINTER ((void*) INTPTR_MIN)
  - MULLE_CONCURRENT_NO_POINTER ((void*) 0)
- Use these to interpret return values and special states.

### 3.3. [mulle-concurrent-hashmap.h]

struct mulle_concurrent_hashmap
- Purpose: wait-free, resizable hashmap storing (hash -> value) pairs.
- Key fields (opaque in practice): storage (atomic pointer to storage), next_storage (for resizing), allocator (optional).
- Lifecycle:
  - mulle_concurrent_hashmap_init(map, size, allocator) : initialize; returns 0 or errno codes (EINVAL/ENOMEM).
  - mulle_concurrent_hashmap_done(map) : destroy/cleanup.
- Core operations (multi-threaded):
  - mulle_concurrent_hashmap_register(map, hash, value) -> returns inserted value or existing value / sentinel codes.
  - mulle_concurrent_hashmap_insert(map, hash, value) -> 0 on success, EEXIST/EINVAL/ENOMEM on error.
  - mulle_concurrent_hashmap_patch(map, hash, value, expect) -> conditional update (experimental).
  - mulle_concurrent_hashmap_lookup(map, hash) -> value or NULL if not found.
  - mulle_concurrent_hashmap_remove(map, hash, value) -> 0 on removed, ENOENT/EINVAL/ENOMEM on error.
- Inspection:
  - mulle_concurrent_hashmap_get_size(map) -> current storage size (buckets allocated).
  - mulle_concurrent_hashmap_count(map) -> count of entries.
- Enumerators:
  - struct mulle_concurrent_hashmapenumerator (map, index, mask)
  - mulle_concurrent_hashmap_enumerate(map), mulle_concurrent_hashmapenumerator_next(rover, &hash, &value)
  - Convenience macros: mulle_concurrent_hashmap_for and mulle_concurrent_hashmap_for_rval
- Important constraints:
  - Do not use hash == 0.
  - Do not use value == 0 or MULLE_CONCURRENT_INVALID_POINTER.

### 3.4. [mulle-concurrent-pointerarray.h]

struct mulle_concurrent_pointerarray
- Purpose: wait/lock-free growable array of void* pointers.
- Key fields: storage, next_storage, allocator.
- Lifecycle:
  - mulle_concurrent_pointerarray_init(array, size, allocator) -> returns 0 or errno codes.
  - mulle_concurrent_pointerarray_done(array)
- Core operations (multi-threaded):
  - mulle_concurrent_pointerarray_add(array, value) -> add value (may reallocate/reserve atomically).
  - mulle_concurrent_pointerarray_get(array, index) -> returns value or NULL.
  - mulle_concurrent_pointerarray_find(array, value) -> index or error.
- Inspection:
  - mulle_concurrent_pointerarray_get_size(array) -> allocated size
  - mulle_concurrent_pointerarray_get_count(array) -> number of stored elements
- Enumerators:
  - mulle_concurrent_pointerarray_enumerate(array) and reverse enumerate variant
  - mulle_concurrent_pointerarrayenumerator_next() returns items until exhausted.
  - Convenience macros: mulle_concurrent_pointerarray_for, mulle_concurrent_pointerarray_for_reverse
- Threading: add/get/find are safe concurrently; enumerators are intended for single-threaded consumption or with no concurrent destructive modifications.

### 3.5. [include.h]
- Purpose: central include glue: exposes MULLE__CONCURRENT_GLOBAL macro and pulls generated include files.

## 4. Performance Characteristics

- Typical expected complexity (average-case):
  - Hashmap insert/lookup/remove: O(1) amortized (resizing when necessary).
  - Pointerarray add/get: O(1) for add (amortized), O(1) get.
- Resizing/growing is atomic and non-blocking for callers; there is copy/rehash work during growth but designed to avoid blocking other threads.
- Memory vs speed trade-offs: waiting threads may observe internal copies; using custom allocators can reduce allocation overhead.
- Thread-safety:
  - Core operations are implemented to be wait-free or lock-free (library aims for wait-free semantics).
  - Enumerators are limited: concurrent removals or growth may cause enumerator to return ECANCELLED/EBUSY.

## 5. AI Usage Recommendations & Patterns

- Best Practices:
  - Always call mulle_aba_init(...) and mulle_aba_register() in threads that access these structures when ABA is required.
  - Use provided lifecycle functions (init/done). Do not manipulate internal storage directly.
  - Treat returned pointers from lookup as borrowed; do not free them unless you own them.
  - Prefer using enumerator macros for simple loops (mulle_concurrent_hashmap_for, mulle_concurrent_pointerarray_for).
- Common Pitfalls:
  - Never use hash == 0. Never use value == NULL or MULLE_CONCURRENT_INVALID_POINTER as payload.
  - Enumerators are not robust against concurrent mutation (they will signal via specific errno return values).
  - Do not assume deterministic iteration order.
- Idiomatic usage:
  - Provide an allocator if memory management or ABA ownership must be tracked by the embedding project.
  - Use register/insert semantics depending on whether duplicates should be detected or replaced.

## 6. Integration Examples

### Example 1: Creating and using a hashmap (from test/example.c)

```c
#include <mulle-concurrent/mulle-concurrent.h>
#include <errno.h>

int main(void)
{
   struct mulle_concurrent_hashmap  map;
   void                             *v;

   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_hashmap_init( &map, 0, NULL);

   mulle_concurrent_hashmap_insert( &map, 0x1, (void *) 1848);
   mulle_concurrent_hashmap_insert( &map, 0x2, (void *) 1849);

   v = mulle_concurrent_hashmap_lookup( &map, 0x2);
   /* v == (void*)1849 */

   mulle_concurrent_hashmap_remove( &map, 0x2, v);

   mulle_concurrent_hashmap_done( &map);

   mulle_aba_unregister();
   mulle_aba_done();

   return( 0);
}
```

### Example 2: Using pointerarray with multi-thread test patterns (from test/array/example)

```c
#include <mulle-concurrent/mulle-concurrent.h>
#include <mulle-testallocator/mulle-testallocator.h>
#include <assert.h>

void use_pointerarray(void)
{
   struct mulle_concurrent_pointerarray  array;

   mulle_aba_init( &mulle_testallocator);
   mulle_allocator_set_aba( &mulle_testallocator,
                            mulle_aba_get_global(),
                            (mulle_allocator_aba_t *) _mulle_aba_free_owned_pointer);
   mulle_aba_register();

   mulle_concurrent_pointerarray_init( &array, 0, &mulle_testallocator);

   _mulle_concurrent_pointerarray_add( &array, (void *) 10);
   assert( mulle_concurrent_pointerarray_get( &array, 0) == (void *) 10);

   mulle_concurrent_pointerarray_done( &array);

   mulle_aba_unregister();
   mulle_aba_done();
}
```

## 7. Dependencies

- Direct runtime / build dependencies to be aware of:
  - mulle-aba (ABA handling, required by tests and often for safe usage)
  - mulle-allocator / custom allocator interface (optional allocator parameter)
- The project is distributed as a component of mulle-core; integrate via mulle-sde or clib where appropriate.


---

References:
- Public headers: src/mulle-concurrent.h, src/hashmap/mulle-concurrent-hashmap.h, src/pointerarray/mulle-concurrent-pointerarray.h, src/mulle-concurrent-types.h
- Tests: test/hashmap/example.c, test/array/example.c (useful concrete examples)


### Volatile Nature in Multi-threaded Environments

Data structures are extremely volatile when shared among multiple threads performing insertions and removals. Queries like "get count" or "get size" return fleeting snapshots that may be outdated immediately. This design accepts eventual consistency in exchange for high-performance concurrent access.

### Single-threaded vs. Multi-threaded Operations

The API clearly distinguishes between operations that must be performed in single-threaded fashion (initialization and destruction) and operations safe for multi-threaded access (insertions, lookups, removals). Some operations like enumeration work in multi-threaded environments but require careful handling of mutation events.

### Memory and Pointer Restrictions

The library uses special pointer values for internal state management:
- `MULLE_CONCURRENT_NO_POINTER` (NULL)
- `MULLE_CONCURRENT_INVALID_POINTER` ((void *) INTPTR_MIN)

User code must not use these values for data. Hash values must not be zero (`MULLE_CONCURRENT_NO_HASH`).

### Growing-Only Data Structures

The `mulle_concurrent_pointerarray` exemplifies a design decision to simplify concurrency: the array can only grow, never shrink. This limitation enables simpler, faster concurrent access without complex synchronization.

## 3. Core API & Data Structures

### 3.1. `mulle-concurrent-types.h`

#### Constants

- **`MULLE_CONCURRENT_NO_HASH`** (0): Invalid hash value sentinel, must not be used as actual hash
- **`MULLE_CONCURRENT_INVALID_POINTER`** ((void *) INTPTR_MIN): Internal sentinel for invalid pointer
- **`MULLE_CONCURRENT_NO_POINTER`** ((void *) 0): Internal sentinel for absent value (NULL)

### 3.2. `mulle-concurrent-hashmap.h`

#### `struct mulle_concurrent_hashmap`

**Purpose:** A wait-free, lock-free, resizable hash table mapping integer hashes to pointer values. Designed for high-performance concurrent access by multiple threads.

**Key Fields:**
- `storage`: Current hash table storage (atomic pointer to `_mulle_concurrent_hashmapstorage`)
- `next_storage`: Next storage during resize operations (atomic pointer)
- `allocator`: Memory allocator for dynamic allocations (atomic pointer)

**Internal Structures:**
- `struct _mulle_concurrent_hashvaluepair`: Hash/value pair entry with atomic value pointer
- `struct _mulle_concurrent_hashmapstorage`: Storage structure containing count, mask, and entries array

**Lifecycle Functions:**

- **`mulle_concurrent_hashmap_init(map, size, allocator)`**: Initialize hashmap with initial capacity. Must be called single-threaded. Returns 0 on success, EINVAL for invalid arguments, ENOMEM on allocation failure.

- **`mulle_concurrent_hashmap_done(map)`**: Free all resources. Must be called single-threaded. Does not free the map structure itself.

**Core Operations (Multi-threaded Safe):**

- **`mulle_concurrent_hashmap_insert(map, hash, value)`**: Insert hash/value pair. Returns 0 on success, EEXIST if duplicate, ENOMEM on allocation failure. Hash must be non-zero. Value must not be NULL or INTPTR_MIN.

- **`mulle_concurrent_hashmap_register(map, hash, value)`**: Insert or retrieve existing value. Returns `MULLE_CONCURRENT_NO_POINTER` if inserted, `MULLE_CONCURRENT_INVALID_POINTER` on error (check errno), or the existing value.

- **`mulle_concurrent_hashmap_patch(map, hash, value, expect)`**: Update existing entry's value atomically. Returns 0 if patched, EEXIST if entry has different value than expected, ENOENT if not found, EINVAL for invalid arguments, ENOMEM on allocation failure. **EXPERIMENTAL - less tested than other functions.**

- **`mulle_concurrent_hashmap_lookup(map, hash)`**: Look up value by hash. Returns NULL if not found, otherwise the associated value pointer.

- **`mulle_concurrent_hashmap_remove(map, hash, value)`**: Remove hash/value pair (both must match). Returns 0 on success, ENOENT if not found, EINVAL for invalid arguments, ENOMEM on allocation failure.

**Inspection Functions:**

- **`mulle_concurrent_hashmap_get_size(map)`**: Get current capacity (not count). Value is snapshot that may be immediately outdated in multi-threaded use.

- **`mulle_concurrent_hashmap_count(map)`**: Count actual entries via iteration. Expensive operation; result is snapshot only.

- **`mulle_concurrent_hashmap_lookup_any(map)`**: Return any value from map, or NULL if empty. Uses iteration internally.

**Enumeration:**

- **`mulle_concurrent_hashmap_enumerate(map)`**: Create enumerator for iterating entries. Returns `struct mulle_concurrent_hashmapenumerator`. Enumerator should not be shared between threads.

**Unsafe Variants (No NULL Checks):**

All core functions have `_prefixed` variants (e.g., `_mulle_concurrent_hashmap_init`) that skip NULL pointer validation for performance. Use only when parameters are guaranteed valid.

**Convenience Macros:**

- **`mulle_concurrent_hashmap_for(map, hash, value)`**: Enumerate all entries in a for-loop style. Automatically handles enumerator lifecycle.

- **`mulle_concurrent_hashmap_for_rval(map, hash, value, rval)`**: Like above but exposes return value for error handling.

#### `struct mulle_concurrent_hashmapenumerator`

**Purpose:** Iterator for traversing hashmap entries. Thread-local, must not be shared.

**Key Fields:**
- `map`: Pointer to the hashmap being enumerated
- `index`: Current iteration index
- `mask`: Storage mask captured at enumeration start (for mutation detection)

**Operations:**

- **`mulle_concurrent_hashmapenumerator_next(rover, hash, value)`**: Get next hash/value pair. Returns 1 for success, 0 for completion, ECANCELLED if hashmap mutated during iteration (restart required), ENOMEM on allocation failure, EINVAL for invalid arguments.

- **`mulle_concurrent_hashmapenumerator_done(rover)`**: Conventional cleanup function (currently no-op, but should be called for future compatibility).

### 3.3. `mulle-concurrent-pointerarray.h`

#### `struct mulle_concurrent_pointerarray`

**Purpose:** A wait-free, lock-free, grow-only pointer array. Safe for concurrent reads and appends. Cannot shrink or overwrite elements.

**Key Fields:**
- `storage`: Current array storage (atomic pointer to `_mulle_concurrent_pointerarraystorage`)
- `next_storage`: Next storage during growth (atomic pointer)
- `allocator`: Memory allocator for dynamic allocations

**Lifecycle Functions:**

- **`mulle_concurrent_pointerarray_init(array, size, allocator)`**: Initialize array with initial capacity. Must be called single-threaded. Returns 0 on success, EINVAL for invalid arguments, ENOMEM on allocation failure.

- **`mulle_concurrent_pointerarray_done(array)`**: Free all resources. Must be called single-threaded. Does not free the array structure itself.

**Core Operations (Multi-threaded Safe):**

- **`mulle_concurrent_pointerarray_add(array, value)`**: Append value to end of array. Returns 0 on success, EINVAL for invalid arguments, ENOMEM on allocation failure. Value must not be NULL or INTPTR_MIN.

- **`mulle_concurrent_pointerarray_get(array, index)`**: Get value at index. Returns NULL if index invalid, otherwise the pointer value. Safe for concurrent access.

- **`mulle_concurrent_pointerarray_find(array, value)`**: Search for value in array. Returns index if found, or -1 if not found.

**Inspection Functions:**

- **`mulle_concurrent_pointerarray_get_size(array)`**: Get current capacity. Snapshot value, may be outdated.

- **`mulle_concurrent_pointerarray_get_count(array)`**: Get current number of elements. As array only grows, this is more reliable than hashmap count, but still a snapshot.

**Mapping:**

- **`mulle_concurrent_pointerarray_map(array, function, userinfo)`**: Apply function to each element. Function signature: `void (*f)(void *value, void *userinfo)`. Returns 0 on success, EINVAL for invalid arguments.

**Enumeration:**

- **`mulle_concurrent_pointerarray_enumerate(array)`**: Create forward enumerator. Returns `struct mulle_concurrent_pointerarrayenumerator`.

- **`mulle_concurrent_pointerarray_reverseenumerate(array, n)`**: Create reverse enumerator starting from index n-1. Returns `struct mulle_concurrent_pointerarrayreverseenumerator`.

**Unsafe Variants:**

All core functions have `_prefixed` variants for performance when parameters are known valid.

**Convenience Macros:**

- **`mulle_concurrent_pointerarray_for(array, item)`**: Enumerate all elements forward in for-loop style.

- **`mulle_concurrent_pointerarray_for_reverse(array, n, item)`**: Enumerate all elements backward in for-loop style.

#### `struct mulle_concurrent_pointerarrayenumerator`

**Purpose:** Forward iterator for pointer array. Thread-local.

**Key Fields:**
- `array`: Pointer to array being enumerated
- `index`: Current iteration index

**Operations:**

- **`mulle_concurrent_pointerarrayenumerator_next(rover)`**: Get next value. Returns NULL when exhausted, otherwise the pointer value. Safe even if array grows during iteration.

- **`mulle_concurrent_pointerarrayenumerator_done(rover)`**: Conventional cleanup (currently no-op).

#### `struct mulle_concurrent_pointerarrayreverseenumerator`

**Purpose:** Reverse iterator for pointer array. Thread-local.

**Key Fields:**
- `array`: Pointer to array being enumerated
- `index`: Current iteration index (decrements)

**Operations:**

- **`mulle_concurrent_pointerarrayreverseenumerator_next(rover)`**: Get next value (moving backward). Returns NULL when exhausted, otherwise the pointer value.

- **`mulle_concurrent_pointerarrayreverseenumerator_done(rover)`**: Conventional cleanup (currently no-op).

### 3.4. `mulle-concurrent.h`

Main header that includes all subcomponents:
- `mulle-concurrent-types.h`
- `mulle-concurrent-hashmap.h`
- `mulle-concurrent-pointerarray.h`
- Version check headers

**Version Constant:**
- **`MULLE__CONCURRENT_VERSION`**: Encoded version number ((major << 20) | (minor << 8) | patch)

## 4. Performance Characteristics

### mulle_concurrent_hashmap

- **Lookup:** O(1) average case, wait-free with bounded retry on collision
- **Insert:** O(1) average case, wait-free with atomic compare-and-swap operations
- **Remove:** O(1) average case, wait-free with atomic compare-and-swap operations
- **Resize:** Amortized O(n), performed incrementally without blocking all operations
- **Enumerate:** O(capacity), must scan entire storage array including empty slots
- **Count:** O(capacity), implemented via full enumeration
- **Space:** O(capacity) where capacity >= count, grows by doubling

**Thread-safety:** Wait-free for lookup, insert, remove, and register. Enumeration may be interrupted by mutations (ECANCELLED error, requires retry). Resize operations are non-blocking to other threads.

**Memory reclamation:** Depends on `mulle-aba` for safe deferred freeing. Old storage is freed only when all threads have quiesced.

### mulle_concurrent_pointerarray

- **Add:** O(1) amortized, wait-free append with occasional resize
- **Get:** O(1), direct array indexing with atomic load
- **Find:** O(n), linear search through array
- **Enumerate:** O(count), sequential scan
- **Map:** O(count), sequential application of function
- **Space:** O(capacity) where capacity >= count, grows by doubling

**Thread-safety:** Wait-free for all operations. Enumeration is safe even during concurrent additions (may not see new elements added during iteration). No removal operations exist, eliminating entire class of concurrency issues.

**Memory reclamation:** Old storage freed via `mulle-aba` during growth operations.

### General Characteristics

- **No locks or mutexes:** All synchronization via atomic operations (CAS, atomic loads/stores)
- **No spinning:** Wait-free guarantees mean operations complete in bounded time
- **Cache considerations:** Hash collisions and array growth can cause cache line contention
- **Hash quality:** Performance heavily depends on hash function quality (avoid clustering)
- **Allocator overhead:** Custom allocators can be provided to optimize allocation patterns

## 5. AI Usage Recommendations & Patterns

### Best Practices

1. **Always initialize ABA system:** Call `mulle_aba_init(allocator)` before using any concurrent structures. Each thread must call `mulle_aba_register()` before accessing structures and `mulle_aba_unregister()` when done.

2. **Single-threaded lifecycle:** Always call `_init` and `_done` functions in single-threaded contexts. Do not allow concurrent access during initialization or destruction.

3. **Use good hash functions:** For hashmap, ensure hash values are well-distributed. Use avalanche functions (like murmur3) for simple integer keys. Never use zero as a hash value.

4. **Avoid sentinel values:** Never store NULL or INTPTR_MIN as values. These are reserved for internal use.

5. **Handle enumeration mutations:** When enumerating in multi-threaded environments, always check for ECANCELLED and retry from beginning if hashmap was mutated.

6. **Prefer unsafe variants when safe:** Use `_prefixed` functions (e.g., `_mulle_concurrent_hashmap_insert`) when parameters are guaranteed non-NULL for better performance.

7. **Use convenience macros:** The `mulle_concurrent_hashmap_for` and `mulle_concurrent_pointerarray_for` macros handle enumerator lifecycle correctly.

8. **Accept snapshot semantics:** Functions like `get_count()` and `get_size()` return snapshots. Do not rely on precise values in multi-threaded scenarios.

9. **Test allocator integration:** Use `mulle_testallocator` for testing to detect memory leaks and allocation issues. Connect allocator to ABA system via `mulle_allocator_set_aba()`.

10. **Match hash/value on remove:** The hashmap `remove` function requires both hash AND value to match. This prevents accidental removal of updated entries.

### Common Pitfalls

1. **Forgetting ABA registration:** Accessing concurrent structures without calling `mulle_aba_register()` first can cause crashes or memory corruption.

2. **Using zero hash:** The hashmap treats zero as an invalid hash sentinel. Always ensure hashes are non-zero.

3. **Storing NULL pointers:** NULL and INTPTR_MIN are reserved values and will cause undefined behavior if stored.

4. **Concurrent init/done:** Calling lifecycle functions while other threads access the structure causes crashes.

5. **Not retrying enumeration:** Ignoring ECANCELLED during hashmap enumeration leads to incomplete iteration.

6. **Assuming count accuracy:** In multi-threaded environments, counts are snapshots. The value may be stale by the time you use it.

7. **Sharing enumerators:** Enumerators are not thread-safe. Each thread must create its own enumerator.

8. **Modifying during enumeration:** While supported, adding/removing entries during enumeration may cause ECANCELLED, requiring restart.

9. **Forgetting patch is experimental:** The `mulle_concurrent_hashmap_patch()` function is less tested. Prefer remove+insert for production code unless you need atomic update semantics.

10. **Using wrong value in remove:** If a hash/value pair has been updated, removal with the old value will fail with ENOENT.

### Idiomatic Usage

**Initialize once, use everywhere:**
```c
// At program startup, single thread
mulle_aba_init( NULL);

// In each thread before using structures
mulle_aba_register();
```

**Cleanup pattern:**
```c
// In each thread after done with structures
mulle_aba_unregister();

// At program shutdown, single thread
mulle_aba_done();
```

**Error handling:**
Check return values for 0 (success) or errno values (EINVAL, ENOMEM, ENOENT, EEXIST, ECANCELLED).

**Enumeration with mutation handling:**
Use labels and goto for clean retry logic when ECANCELLED occurs.

## 6. Integration Examples

### Example 1: Basic Hashmap Usage - Insert, Lookup, Remove

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
   
   // Initialize ABA system (once per process)
   mulle_aba_init( NULL);
   mulle_aba_register();
   
   // Initialize hashmap (single-threaded)
   rval = mulle_concurrent_hashmap_init( &map, 0, NULL);
   if( rval)
   {
      fprintf( stderr, "init failed: %d\n", rval);
      return( 1);
   }
   
   // Insert values (multi-thread safe)
   rval = mulle_concurrent_hashmap_insert( &map, 0x1848, (void *) 1848);
   if( rval)
   {
      fprintf( stderr, "insert failed: %d\n", rval);
      return( 1);
   }
   
   rval = mulle_concurrent_hashmap_insert( &map, 0x1849, (void *) 1849);
   if( rval)
   {
      fprintf( stderr, "insert failed: %d\n", rval);
      return( 1);
   }
   
   // Lookup value
   value = mulle_concurrent_hashmap_lookup( &map, 0x1848);
   assert( value == (void *) 1848);
   printf( "Found value: %p\n", value);
   
   // Remove with wrong value (should fail)
   rval = mulle_concurrent_hashmap_remove( &map, 0x1848, (void *) 9999);
   assert( rval == ENOENT);
   
   // Remove with correct value
   rval = mulle_concurrent_hashmap_remove( &map, 0x1848, (void *) 1848);
   if( rval)
   {
      fprintf( stderr, "remove failed: %d\n", rval);
      return( 1);
   }
   
   // Verify removal
   value = mulle_concurrent_hashmap_lookup( &map, 0x1848);
   assert( value == NULL);
   
   // Cleanup (single-threaded)
   mulle_concurrent_hashmap_done( &map);
   
   mulle_aba_unregister();
   mulle_aba_done();
   
   return( 0);
}
```

### Example 2: Hashmap Enumeration with Mutation Handling

```c
#include <mulle-concurrent/mulle-concurrent.h>
#include <stdio.h>
#include <errno.h>

static void   enumerate_with_retry( struct mulle_concurrent_hashmap *map)
{
   struct mulle_concurrent_hashmapenumerator   rover;
   intptr_t                                    hash;
   void                                        *value;
   int                                         rval;
   
retry:
   rover = mulle_concurrent_hashmap_enumerate( map);
   while( (rval = mulle_concurrent_hashmapenumerator_next( &rover, &hash, &value)) == 1)
   {
      printf( "Hash: 0x%lx, Value: %p\n", (unsigned long) hash, value);
   }
   mulle_concurrent_hashmapenumerator_done( &rover);
   
   // If hashmap was mutated during enumeration, retry
   if( rval == ECANCELLED)
   {
      printf( "Enumeration interrupted, retrying...\n");
      goto retry;
   }
   
   if( rval < 0)
      fprintf( stderr, "Enumeration error: %d\n", rval);
}

int   main( int argc, char *argv[])
{
   struct mulle_concurrent_hashmap   map;
   unsigned int                      i;
   
   mulle_aba_init( NULL);
   mulle_aba_register();
   
   mulle_concurrent_hashmap_init( &map, 0, NULL);
   
   // Insert some values
   for( i = 1; i <= 10; i++)
      _mulle_concurrent_hashmap_insert( &map, i, (void *) (intptr_t) (i * 100));
   
   enumerate_with_retry( &map);
   
   mulle_concurrent_hashmap_done( &map);
   
   mulle_aba_unregister();
   mulle_aba_done();
   
   return( 0);
}
```

### Example 3: Using Enumeration Macro

```c
#include <mulle-concurrent/mulle-concurrent.h>
#include <stdio.h>

int   main( int argc, char *argv[])
{
   struct mulle_concurrent_hashmap   map;
   intptr_t                          hash;
   void                              *value;
   unsigned int                      count;
   
   mulle_aba_init( NULL);
   mulle_aba_register();
   
   mulle_concurrent_hashmap_init( &map, 0, NULL);
   
   // Insert values
   _mulle_concurrent_hashmap_insert( &map, 0xABCD, (void *) 0x1111);
   _mulle_concurrent_hashmap_insert( &map, 0xDEAD, (void *) 0x2222);
   _mulle_concurrent_hashmap_insert( &map, 0xBEEF, (void *) 0x3333);
   
   // Use convenience macro for enumeration
   count = 0;
   mulle_concurrent_hashmap_for( &map, hash, value)
   {
      printf( "Entry %u: hash=0x%lx, value=%p\n", 
              count, (unsigned long) hash, value);
      count++;
   }
   
   printf( "Total entries: %u\n", count);
   
   mulle_concurrent_hashmap_done( &map);
   
   mulle_aba_unregister();
   mulle_aba_done();
   
   return( 0);
}
```

### Example 4: Pointer Array - Adding and Accessing Elements

```c
#include <mulle-concurrent/mulle-concurrent.h>
#include <stdio.h>
#include <assert.h>

int   main( int argc, char *argv[])
{
   struct mulle_concurrent_pointerarray   array;
   unsigned int                           i;
   void                                   *value;
   int                                    rval;
   
   mulle_aba_init( NULL);
   mulle_aba_register();
   
   // Initialize array (single-threaded)
   rval = mulle_concurrent_pointerarray_init( &array, 0, NULL);
   if( rval)
   {
      fprintf( stderr, "init failed: %d\n", rval);
      return( 1);
   }
   
   // Add elements (multi-thread safe)
   for( i = 1; i <= 5; i++)
   {
      value = (void *) (uintptr_t) (i * 10);
      rval  = mulle_concurrent_pointerarray_add( &array, value);
      if( rval)
      {
         fprintf( stderr, "add failed: %d\n", rval);
         return( 1);
      }
   }
   
   // Get element by index
   value = mulle_concurrent_pointerarray_get( &array, 2);
   assert( value == (void *) 30);
   printf( "Element at index 2: %p\n", value);
   
   // Get array info
   printf( "Count: %u\n", mulle_concurrent_pointerarray_get_count( &array));
   printf( "Size: %u\n", mulle_concurrent_pointerarray_get_size( &array));
   
   // Find element
   i = mulle_concurrent_pointerarray_find( &array, (void *) 40);
   printf( "Found value 40 at index: %u\n", i);
   
   mulle_concurrent_pointerarray_done( &array);
   
   mulle_aba_unregister();
   mulle_aba_done();
   
   return( 0);
}
```

### Example 5: Pointer Array Enumeration

```c
#include <mulle-concurrent/mulle-concurrent.h>
#include <stdio.h>

int   main( int argc, char *argv[])
{
   struct mulle_concurrent_pointerarray             array;
   struct mulle_concurrent_pointerarrayenumerator   rover;
   void                                             *item;
   unsigned int                                     i;
   unsigned int                                     count;
   
   mulle_aba_init( NULL);
   mulle_aba_register();
   
   mulle_concurrent_pointerarray_init( &array, 0, NULL);
   
   // Add some elements
   for( i = 1; i <= 8; i++)
      _mulle_concurrent_pointerarray_add( &array, (void *) (intptr_t) (i * 100));
   
   // Forward enumeration using macro
   printf( "Forward enumeration:\n");
   mulle_concurrent_pointerarray_for( &array, item)
   {
      printf( "  %p\n", item);
   }
   
   // Reverse enumeration using macro
   count = mulle_concurrent_pointerarray_get_count( &array);
   printf( "\nReverse enumeration:\n");
   mulle_concurrent_pointerarray_for_reverse( &array, count, item)
   {
      printf( "  %p\n", item);
   }
   
   mulle_concurrent_pointerarray_done( &array);
   
   mulle_aba_unregister();
   mulle_aba_done();
   
   return( 0);
}
```

### Example 6: Using Custom Allocator with ABA Integration

```c
#include <mulle-concurrent/mulle-concurrent.h>
#include <mulle-allocator/mulle-allocator.h>
#include <mulle-testallocator/mulle-testallocator.h>
#include <stdio.h>

int   main( int argc, char *argv[])
{
   struct mulle_concurrent_hashmap   map;
   unsigned int                      i;
   
   // Initialize test allocator (for leak detection)
   mulle_testallocator_reset();
   
   // Initialize ABA with custom allocator
   mulle_aba_init( &mulle_testallocator);
   
   // Connect allocator to ABA for automatic deferred freeing
   mulle_allocator_set_aba( &mulle_testallocator,
                            mulle_aba_get_global(),
                            (mulle_allocator_aba_t *) _mulle_aba_free);
   
   mulle_aba_register();
   
   // Create hashmap with custom allocator
   mulle_concurrent_hashmap_init( &map, 0, &mulle_testallocator);
   
   // Use the hashmap
   for( i = 1; i <= 100; i++)
      _mulle_concurrent_hashmap_insert( &map, i, (void *) (intptr_t) (i * 10));
   
   printf( "Inserted %u entries\n", i - 1);
   printf( "Map count: %u\n", mulle_concurrent_hashmap_count( &map));
   
   // Cleanup
   mulle_concurrent_hashmap_done( &map);
   
   mulle_aba_unregister();
   
   // Disconnect allocator from ABA before shutting down
   mulle_allocator_set_aba( &mulle_testallocator, NULL, NULL);
   mulle_aba_done();
   
   // Check for leaks (test allocator will report any)
   mulle_testallocator_reset();
   
   return( 0);
}
```

## 7. Dependencies

- **mulle-aba**: Lock-free, cross-platform solution to the ABA problem. Provides automatic memory reclamation for concurrent data structures. Essential for safe deferred freeing of old storage during resize operations.

**Note:** `mulle-aba` itself depends on:
- `mulle-c11`: C11 compatibility layer and atomic operations
- `mulle-thread`: Cross-platform threading primitives
- `mulle-allocator`: Memory allocation abstraction

These transitive dependencies are typically resolved automatically by the `mulle-sde` build system.
