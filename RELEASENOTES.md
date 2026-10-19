### 4.1.1




* fix a race in hashtable
* improve test framework to catch those races

















feature: add hashtable hash validity check and math helpers, reject out-of-range hashes

* Add ``mulle_concurrent_hashtable_hash_is_valid`` (and its inline declaration) to test whether a hash is a legal key for a hashtable.
* Hashtable keys carrying the mode's FROZEN bit are no longer silently aliased: mutating calls return `EINVAL` and `lookup` returns NULL, preserving the "hash is the key" invariant.
* Add the `mulle-concurrent-math.h` header with ``_mulle_concurrent_is_pow2`` and ``_mulle_concurrent_pow2roundup`/`_pow2roundup_32`/`_pow2roundup_64`` bit helpers.
* The `size` passed to hashmap/hashtable init is now a hint, rounded up to the next power of two and clamped to a minimum of 4 `(`get_size`` reports the effective size).
* ``mulle_concurrent_hashmap_register`` no longer reports `EEXIST`, and enumeration no longer returns `ENOMEM`.

feature: add hashtable hash validity check and math helpers, reject out-of-range hashes

* Add ``mulle_concurrent_hashtable_hash_is_valid`` (and its inline declaration) to test whether a hash is a legal key for a hashtable.
* Hashtable keys carrying the mode's FROZEN bit are no longer silently aliased: mutating calls return `EINVAL` and `lookup` returns NULL, preserving the "hash is the key" invariant.
* Add the `mulle-concurrent-math.h` header with ``_mulle_concurrent_is_pow2`` and ``_mulle_concurrent_pow2roundup`/`_pow2roundup_32`/`_pow2roundup_64`` bit helpers.
* The `size` passed to hashmap/hashtable init is now a hint, rounded up to the next power of two and clamped to a minimum of 4 `(`get_size`` reports the effective size).
* ``mulle_concurrent_hashmap_register`` no longer reports `EEXIST`, and enumeration no longer returns `ENOMEM`.



* test suite now allocates through the default mulle allocator instead of the testallocator
* minimum required mulle-aba version raised to 3.1.7


* restore hashmap to former glory, wait-freeness

## 4.1.0


feature: add hashtable hash validity check and math helpers, reject out-of-range hashes

* Add ``mulle_concurrent_hashtable_hash_is_valid`` (and its inline declaration) to test whether a hash is a legal key for a hashtable.
* Hashtable keys carrying the mode's FROZEN bit are no longer silently aliased: mutating calls return `EINVAL` and `lookup` returns NULL, preserving the "hash is the key" invariant.
* Add the `mulle-concurrent-math.h` header with ``_mulle_concurrent_is_pow2`` and ``_mulle_concurrent_pow2roundup`/`_pow2roundup_32`/`_pow2roundup_64`` bit helpers.
* The `size` passed to hashmap/hashtable init is now a hint, rounded up to the next power of two and clamped to a minimum of 4 `(`get_size`` reports the effective size).
* ``mulle_concurrent_hashmap_register`` no longer reports `EEXIST`, and enumeration no longer returns `ENOMEM`.



* test suite now allocates through the default mulle allocator instead of the testallocator
* minimum required mulle-aba version raised to 3.1.7


* restore hashmap to former glory, wait-freeness


# 4.0.0




* concurrency tests now auto-detect slow environments (valgrind) and reduce target sizes
* heavy 32-thread runs are skipped under valgrind to keep test time reasonable
* added periodic thread yields so cooperative schedulers progress under valgrind
* benchmarks and hashtable/hashmap/pointerarray tests affected


feature: add for-each iteration macros for hashtable enumeration

* new ``mulle_concurrent_hashtable_for`` macro iterates over all (hash, value) pairs in a hashtable
* new ``mulle_concurrent_hashtable_for_rval`` macro exposes the enumerator return value (1, 0 or ECANCELED)
* both macros assert that the hash and value types match the container's storage sizes










* ``mulle_concurrent_pointerset_get_allocator`` now uses ``_mulle_atomic_pointer_read`` to safely access the allocator field in concurrent contexts


## 3.2.0





* experimental pointerset added
