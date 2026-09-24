# `mulle_concurrent_hashmap`

`mulle_concurrent_hashmap` is a mutable map of pointers, indexed by a hash.
What is it good for ?  It's good for a table, that needs to be accessed quickly
and often by multiple threads. Rule of thumb, if your process is spending a lot
of time locking and unlocking a central hashtable, you
want `mulle_concurrent_hashmap`.

Such a hashmap is extremely volatile when shared with multiple threads, that are
inserting entries. For example, when you get the number of elements
of a such a hashmap it is akin to a fleeting glimpse of a distant past.

The hashmap has **no multi-threaded remove**. Entries can be added but not
taken away while other threads are running. Why this is inherent to the hashmap
and not merely unimplemented is the subject of
[REMOVE-MISERY-HASHMAP.md](REMOVE-MISERY-HASHMAP.md). If you need concurrent
removal, use `mulle_concurrent_hashtable` instead
([HASHTABLE.md](HASHTABLE.md)).

The following operations must be executed in single-threaded fashion:

* `mulle_concurrent_hashmap_init`
* `mulle_concurrent_hashmap_done`
* `_mulle_concurrent_hashmap_patch`
* `_mulle_concurrent_hashmap_remove`

The following operations are fine in multi-threaded environments:

* `mulle_concurrent_hashmap_insert`
* `mulle_concurrent_hashmap_register`
* `mulle_concurrent_hashmap_lookup`

They are **wait-free**: each completes in a bounded number of its own steps,
regardless of concurrent activity. See
[WAIT-FREE-LOCK-FREE.md](WAIT-FREE-LOCK-FREE.md) for the argument.

The following operations work in multi-threaded environments, but should be
approached with caution:

* `mulle_concurrent_hashmap_enumerate`
* `mulle_concurrent_hashmap_lookup_any`
* `mulle_concurrent_hashmap_count`
* `mulle_concurrent_hashmap_get_size`


All allocation is **fail-fast**: the mulle allocator contract is *success or
abort*. There are therefore no `ENOMEM` returns: a failed allocation ends the
process rather than being reported.


## single-threaded


### `mulle_concurrent_hashmap_init`

```
void   mulle_concurrent_hashmap_init( struct mulle_concurrent_hashmap *map,
                                      unsigned int size,
                                      struct mulle_allocator *allocator)
```

Initialize `map`, with a starting `size` of elements. `size` is a hint: it is
rounded up to the next power of two and clamped to a minimum of 4. A `size` of
0 uses a shared read-only empty storage, which is replaced on the first insert.
`mulle_concurrent_hashmap_get_size` reports the size that was actually used.

`allocator` will be used to allocate and free memory during the lifetime of
`map`. Pass `NULL` to use the default allocator. Call this in single-threaded
fashion.

Allocation is fail-fast, so there is no return value.


### `void  mulle_concurrent_hashmap_done`

```
void  mulle_concurrent_hashmap_done( struct mulle_concurrent_hashmap *map)
```

This will free all allocated resources `map`. It will not **free** `map` itself
though. `map` must be a valid pointer. Call this in single-threaded fashion.


### `_mulle_concurrent_hashmap_patch`

```
int  _mulle_concurrent_hashmap_patch( struct mulle_concurrent_hashmap *map,
                                      intptr_t hash,
                                      void *value)
```

**SINGLE-THREADED ONLY.** Unconditionally replace the value of an existing
entry. There is no CAS and no migration concern. Calling this while another
thread accesses `map` is undefined behavior.

A general concurrent "replace this value" operation is not implementable over
this data structure; [POSEAS-PATCH.md](POSEAS-PATCH.md) proves why. Use this
during single-threaded setup or teardown phases only.

Return Values:

*   0      : OK
*   ENOENT : `hash` is not present


### `_mulle_concurrent_hashmap_remove`

```
int  _mulle_concurrent_hashmap_remove( struct mulle_concurrent_hashmap *map,
                                       intptr_t hash)
```

**SINGLE-THREADED ONLY.** An inline wrapper for `_patch( map, hash, NULL)`. It
clears the value but leaves the hash claim in place, so probe chains stay
intact. `lookup` reports the hash as absent afterwards, and the slot can be
repopulated later with `_patch` or by `insert`/`register` for the same hash.

The leftover `{ hash, NULL }` state is safe to carry into a later concurrent
phase: the hash-first claim means a foreign key probing that slot sees the
claim and probes onward, and only the hash owner can fill the value again.

Return Values:

*   0      : OK
*   ENOENT : `hash` is not present


## multi-threaded


### `mulle_concurrent_hashmap_insert`

```
int  mulle_concurrent_hashmap_insert( struct mulle_concurrent_hashmap *map,
                                      intptr_t hash,
                                      void *value)
```

Insert a `hash`, `value` pair.
`hash` must not be zero. It should be a unique integer key, suitably treated to
be a good hash value. Here is an example ([murmur3](https://en.wikipedia.org/wiki/MurmurHash) of an avalance function for simple
integer keys (1-...)

```
static inline uint64_t   mulle_hash_avalanche64(uint64_t h)
{
   h ^= h >> 33;
   h *= 0xff51afd7ed558ccd;
   h ^= h >> 33;
   h *= 0xc4ceb9fe1a85ec53;
   h ^= h >> 33;
   return h;
}
```

`value` can be any `void *` except `NULL` (`MULLE_CONCURRENT_NO_POINTER`, the
empty-slot sentinel) or `(void *) INTPTR_MIN`
(`MULLE_CONCURRENT_INVALID_POINTER`, the `REDIRECT` freeze marker). It will not
get dereferenced by the hashmap.

`(void *) INTPTR_MAX` (`MULLE_CONCURRENT_TOMBSTONE_POINTER`) used to be reserved
as well. The hashmap has no tombstones anymore, so it is accepted and stored
like any other pointer. Avoid it anyway if the same payloads also go into a
`mulle_concurrent_pointerset`, which does still reserve it.

Return Values:

*   0      : OK
*   EEXIST : duplicate
*   EINVAL : invalid argument (includes a reserved `hash` or `value`)


### `mulle_concurrent_hashmap_register`

```
void   *mulle_concurrent_hashmap_register( struct mulle_concurrent_hashmap *map,
                                           intptr_t hash,
                                           void *value)
```

Insert a `hash`, `value` pair if `hash` is not present yet, and report what
happened in one step. The restrictions on `hash` and `value` are the same as for
`mulle_concurrent_hashmap_insert`.

Return Values:

*   `MULLE_CONCURRENT_NO_POINTER` (`NULL`)             : inserted `value`
*   `MULLE_CONCURRENT_INVALID_POINTER` (`INTPTR_MIN`)  : invalid argument
*   otherwise                                          : the value that is
    already registered for `hash`, which is left untouched

`register` never returns a foreign key's value. A slot is claimed by CASing its
hash word, and only operations carrying that hash may write the slot's value, so
two different keys can never collide on one slot's payload.


### `mulle_concurrent_hashmap_lookup`

```
void   *mulle_concurrent_hashmap_lookup( struct mulle_concurrent_hashmap *map,
                                          intptr_t hash)
```

Looks up a value by its hash.

Return Values:

*   NULL  : not found
*   otherwise the value for this hash


### `mulle_concurrent_hashmap_enumerate`

```
struct mulle_concurrent_hashmapenumerator  mulle_concurrent_hashmap_enumerate( struct mulle_concurrent_hashmap *map)
```

Enumerate a hashtable. Passing `NULL` produces an empty enumerator. See
`mulle_concurrent_hashmapenumerator_next` for more details. It returns a
`mulle_concurrent_hashmapenumerator`. This enumerator should not be jointly
accessed by multiple threads.

---


### `mulle_concurrent_hashmap_get_size`

```
unsigned int   mulle_concurrent_hashmap_get_size( struct mulle_concurrent_hashmap *map);
```

This gives you the current capacity of hash/value entries of `map`. The returned
number maybe not as meaningful as one might think, if the map is accessed in multi-threaded fashion.


# `mulle_concurrent_hashmapenumerator`

The following two functions operate on an enumerator over the possibly multi-threaded hashmap, the enumerator itself should not be accessed by multiple threads.

* `mulle_concurrent_hashmapenumerator_next`
* `mulle_concurrent_hashmapenumerator_done`


### `mulle_concurrent_hashmapenumerator_next`

```
int  mulle_concurrent_hashmapenumerator_next( struct mulle_concurrent_hashmapenumerator *rover,
                                               intptr_t *hash,
                                               void **value)
```

Get the next `hash`, `value` pair from the enumerator. This works reliably if
`map` is accessed in single-threaded fashion, which it probably will NOT be.
In multi-threaded environments, the enumeration may be interrupted by mutations
of the hashtable by other threads. The enumerator itself should not be shared
with other threads.

Here is a simple usage example:


```
   struct mulle_concurrent_hashmap             *map;
   struct mulle_concurrent_hashmapenumerator   rover;
   intptr_t                                    hash;
   void                                        *value;
   int                                         rval;

retry:
   rover = mulle_concurrent_hashmap_enumerate( map);
   while( (rval = mulle_concurrent_hashmapenumerator_next( &rover, &hash, &value) == 1))
   {
      printf( "%ld %p\n", hash, value);
   }
   mulle_concurrent_hashmapenumerator_done( &rover);

   if( rval == ECANCELED) // interrupted!
      goto retry;         // restart from the beginning will duplicate some
```

Return Values:

*   1         : OK
*   0         : nothing left
*   ECANCELED : hashmap was mutated, restart the enumeration


### `mulle_concurrent_hashmapenumerator_done`

```
void  mulle_concurrent_hashmapenumerator_done( struct mulle_concurrent_hashmapenumerator *rover)
```

It's a mere conventional function. It may be left out.


### `mulle_concurrent_hashmap_count`

```
unsigned int   mulle_concurrent_hashmap_count( struct mulle_concurrent_hashmap *map);
```

This gives you the current number of hash/value entries of `map`. It is
implemented as an iterator loop, that counts the number of values.
The returned number may be close to meaningless, when the map is accessed in
multi-threaded fashion.


### `mulle_concurrent_hashmap_lookup_any` - get a value from the hashmap

```
void  *mulle_concurrent_hashmap_lookup_any( struct mulle_concurrent_hashmap *map);
```

This will return a value from the map. It is implemented as an iterator loop,
that returns the first value. It returns NULL if `map` contains no entries or
is NULL.


