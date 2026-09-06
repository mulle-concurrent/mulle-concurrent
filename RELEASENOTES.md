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
