# mulle-concurrent Library Documentation for AI
<!-- Keywords: concurrent, wait-free, lock-free, hashmap, pointerarray, C, allocator -->

## 1. Introduction & Purpose

- mulle-concurrent provides wait-free, lock-free concurrent data structures in C: a hashmap and a pointer array.
- Solves contention in hot multi-threaded access paths by offering atomic, non-blocking insert/lookup/remove and enumerators.
- Key features: wait-free hashmap with register/insert/patch/lookup/remove, and a growable pointer array with concurrent add/get/find and enumerators.
- Relationship: component library often used from mulle-core; depends on a mulle allocator and mulle-aba (for ABA handling).

## 2. Key Concepts & Design Philosophy

- Wait-free and lock-free design: relies on atomic pointer updates and CAS-style algorithms (inspired by Preshing's resizable concurrent map) to avoid locks.
- Storage indirection and versioned storages: map/array hold atomic pointers to storage blocks and may switch to new storages when resized.
- Enumerators are "limited multi-threaded": safe for the calling thread but will detect concurrent mutations and return an error (ECANCELLED).
- Minimal API surface: inline-safe wrappers for single-threaded checks and global symbols for multi-threaded operations.

## 3. Core API & Data Structures

### 3.1. [mulle-concurrent-types.h]

- Defines basic sentinel values and constants:
  - MULLE_CONCURRENT_NO_HASH (0)
  - MULLE_CONCURRENT_INVALID_POINTER (INTPTR_MIN)
  - MULLE_CONCURRENT_NO_POINTER (0)

### 3.2. [mulle-concurrent-hashmap.h]

struct mulle_concurrent_hashmap
- Purpose: wait-free, resizable concurrent hashmap of (hash -> pointer) entries.
- Key fields (internal): storage (atomic pointer to current storage), next_storage (atomic pointer used during resize), allocator (for memory ops).

Lifecycle Functions:
- int mulle_concurrent_hashmap_init(struct mulle_concurrent_hashmap *map, unsigned int size, struct mulle_allocator *allocator)
  - Returns 0 on success, EINVAL or ENOMEM on error.
- void mulle_concurrent_hashmap_done(struct mulle_concurrent_hashmap *map)
  - Cleanup; safe to call with NULL.
- unsigned int mulle_concurrent_hashmap_get_size(struct mulle_concurrent_hashmap *map)
  - Returns current size (0 if map NULL).

Core Operations (multi-threaded):
- void *mulle_concurrent_hashmap_register(struct mulle_concurrent_hashmap *map, intptr_t hash, void *value)
  - Registers value for hash. Returns MULLE_CONCURRENT_NO_POINTER (means inserted), MULLE_CONCURRENT_INVALID_POINTER on error, or existing registered value.
- int mulle_concurrent_hashmap_insert(struct mulle_concurrent_hashmap *map, intptr_t hash, void *value)
  - 0 on success, EEXIST if duplicate, EINVAL/ENOMEM on error.
- int mulle_concurrent_hashmap_patch(struct mulle_concurrent_hashmap *map, intptr_t hash, void *value, void *expect)
  - Experimental: change value only if expect differs; returns 0/EEXIST/ENOENT/EINVAL/ENOMEM.
- void *mulle_concurrent_hashmap_lookup(struct mulle_concurrent_hashmap *map, intptr_t hash)
  - Returns value or NULL if not found.
- int mulle_concurrent_hashmap_remove(struct mulle_concurrent_hashmap *map, intptr_t hash, void *value)
  - Returns 0 if removed, ENOENT if not found, EINVAL/ENOMEM on error.

Enumerators & Convenience:
- struct mulle_concurrent_hashmapenumerator { map, index, mask }
- mulle_concurrent_hashmap_enumerate(map) -> enumerator
- int mulle_concurrent_hashmapenumerator_next(rover, intptr_t *hash, void **value)
  - Returns 1 (OK), 0 (nothing left), ECANCELLED (mutation alert), or ENOMEM/EINVAL.
- mulle_concurrent_hashmapenumerator_done(rover) (no-op macro)
- Utilities: mulle_concurrent_hashmap_lookup_any(map), mulle_concurrent_hashmap_count(map)
- Foreach macros: mulle_concurrent_hashmap_for and mulle_concurrent_hashmap_for_rval for concise iteration (they use the enumerator API internally).

Notes & constraints:
- Do not use hash == 0, value == 0, or value == INTPTR_MIN; those are reserved sentinel values.
- Inline wrappers validate NULL pointers and forward to underscore-prefixed implementations for unchecked variants.

### 3.3. [mulle-concurrent-pointerarray.h]

struct mulle_concurrent_pointerarray
- Purpose: wait-free, growable array of pointers with concurrent add/get/find and enumerators.
- Key fields: storage (atomic pointer to storage), next_storage, allocator.

Lifecycle:
- int mulle_concurrent_pointerarray_init(struct mulle_concurrent_pointerarray *array, unsigned int size, struct mulle_allocator *allocator)
  - Returns 0/EINVAL/ENOMEM.
- void mulle_concurrent_pointerarray_done(struct mulle_concurrent_pointerarray *array)
- unsigned int mulle_concurrent_pointerarray_get_size/get_count(array)

Core Operations (multi-threaded):
- int mulle_concurrent_pointerarray_add(struct mulle_concurrent_pointerarray *array, void *value)
  - Returns 0 on success; EINVAL/ENOMEM on error.
- void *mulle_concurrent_pointerarray_get(struct mulle_concurrent_pointerarray *array, unsigned int i)
  - Returns pointer or NULL if invalid.
- int mulle_concurrent_pointerarray_find(struct mulle_concurrent_pointerarray *array, void *value)
  - Returns index or negative/error.
- Convenience: map function mulle_concurrent_pointerarray_map(list, f, userinfo)

Enumerators:
- struct mulle_concurrent_pointerarrayenumerator / reverse enumerator
- mulle_concurrent_pointerarray_enumerate(array) / reverseenumerate(array,n)
- mulle_concurrent_pointerarrayenumerator_next(rover) -> next item or NULL
- Foreach macros: mulle_concurrent_pointerarray_for, mulle_concurrent_pointerarray_for_reverse

Notes:
- Enumerator returned is only safe for the calling thread; concurrent mutations can cancel enumeration.

## 4. Performance Characteristics

- Hashmap: expected O(1) average for lookup/insert/remove under low contention; resizing involves copying to new storage but designed to be wait-free for callers (resizing coordination is internal). Count/lookup_any are O(n) in worst case when scanning storage.
- Pointer array: add is amortized O(1); get is O(1). find is O(n).
- Memory vs speed: uses atomic pointer indirections and multiple storage blocks to avoid locks at the cost of additional memory during resizes (old and new storages may coexist).
- Thread-safety: primitives are designed for multi-threaded usage (wait-free/lock-free). Enumerators are limited — they detect mutations and return ECANCELLED.

## 5. AI Usage Recommendations & Patterns

- Best Practices:
  - Always call _init before use and _done after use using the inline wrappers.
  - Treat returned pointers from lookup as borrowed; do not free them unless ownership is documented.
  - Do not use sentinel values (hash==0 or value==0/INTPTR_MIN).
  - Use provided foreach macros for concise iteration; handle ECANCELLED from enumerator_next when concurrent mutations occur.
- Common Pitfalls:
  - Assuming enumerators survive concurrent mutation — they explicitly detect and cancel.
  - Using value 0 or INTPTR_MIN; these map to special meanings.
  - Not providing a compatible allocator when required by project style.
- Idiomatic patterns:
  - Use register/insert for atomic registration semantics; use lookup for reads. Use patch only when an atomic conditional update is intended (experimental).

## 6. Integration Examples

### Example 1: Creating and using a hashmap

```c
#include "mulle-concurrent.h"

struct mulle_allocator  *allocator;
struct mulle_concurrent_hashmap  map;

int
main()
{
   int  rval;

   rval = mulle_concurrent_hashmap_init( &map, 1024, allocator);
   if( rval != 0)
      return( 1);

   mulle_concurrent_hashmap_insert( &map, 12345, (void *) 0xdeadbeef);
   void *v = mulle_concurrent_hashmap_lookup( &map, 12345);

   mulle_concurrent_hashmap_done( &map);
   return( v == (void *) 0xdeadbeef ? 0 : 2);
}
```

### Example 2: Enumerating a pointer array

```c
#include "mulle-concurrent.h"

struct mulle_concurrent_pointerarray   array;

int
print_all( struct mulle_concurrent_pointerarray *a)
{
   void *item;

   mulle_concurrent_pointerarray_init( &array, 16, NULL);
   // add items omitted

   mulle_concurrent_pointerarray_for( &array, item)
   {
      /* process item */
   }

   mulle_concurrent_pointerarray_done( &array);
   return( 0);
}
```

(Tests in test/hashmap show real usage patterns and error outputs — use them as canonical examples.)

## 7. Dependencies

- mulle-allocator (allocator interface expected)
- mulle-aba (ABA problem helper) — mentioned in README
- mulle-sde used for building and integrating

## 8. Shortcut

- This TOC was generated from public headers (src/*.h, src/*/*.h) and README.md plus test examples in test/hashmap and test/array. If an existing TOC exists, compare against last commit to see changes in API (e.g., new functions or macros such as _mulle_concurrent_hashmap_* underscored variants).



<!-- EOF -->