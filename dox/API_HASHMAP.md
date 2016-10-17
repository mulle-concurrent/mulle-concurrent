# `mulle_concurrent_hashmap`

`mulle_concurrent_hashmap` is a mutable map of pointers, indexed by a hash.
Such a hashmap is extremely volatile when shared by multiple threads, that are
inserting and removing entries. As an example: when you get the number of elements
of a such a map it is just a fleeting glimpse of it at a now fairly distant point
in time.

The following operations should be executed in single-threaded fashion:

* `_mulle_concurrent_hashmap_init`
* `_mulle_concurrent_hashmap_done`
* `_mulle_concurrent_hashmap_get_size`

The following operations are fine in multi-threaded environments:

* `_mulle_concurrent_hashmap_insert`
* `_mulle_concurrent_hashmap_remove`
* `_mulle_concurrent_hashmap_lookup`


The following operations work in multi-threaded environments, but should be approached with caution:

* `mulle_concurrent_hashmap_enumerate`
* `_mulle_concurrent_hashmapenumerator_next`
* `_mulle_concurrent_hashmapenumerator_done`
* `mulle_concurrent_hashmap_lookup_any`
* `mulle_concurrent_hashmap_count`

## single-threaded


### `_mulle_concurrent_hashmap_init`

```
void   _mulle_concurrent_hashmap_init( struct mulle_concurrent_hashmap *map,
                                       unsigned int size,
                                       struct mulle_allocator *allocator)
```

Initialize `map`, with a starting `size` of elements. `allocator` will be
used to allocate and free memory during the lifetime of `map`.  You can pass in
for `allocator` to use the default. Call this in single-threaded fashion.


### `void  _mulle_concurrent_hashmap_done( struct mulle_concurrent_hashmap *map)`

```
void  _mulle_concurrent_hashmap_done( struct mulle_concurrent_hashmap *map)
```

This will free all allocated resources `map`. It will not **free** `map` itself
though. `map` must be a valid pointer. Call this in single-threaded fashion.


### `_mulle_concurrent_hashmap_get_size`

```
unsigned int   mulle_concurrent_hashmap_get_count( struct mulle_concurrent_hashmap *map);
```

This gives you the current number of hash/value entries of `map`. The returned
number is close to meaningless, when the map is accessed in multi-threaded
fashion. Call this in single-threaded fashion.


## multi-threaded


### `_mulle_concurrent_hashmap_insert`

```
int  _mulle_concurrent_hashmap_insert( struct mulle_concurrent_hashmap *map,
                                       intptr_t hash,
                                       void *value)
```

Insert a `hash`, `value` pair.
`hash` must not be zero. It should be a unique integer key, suitably treated to
be a good hash value. Here is an example of an avalance function for simple
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

`value` can be any `void *` except `NULL` or `(void *) INTPTR_MAX`.  It will
not get dereferenced by the hashmap.


Return Values:
   0      : OK
   EEXIST : duplicate
   ENOMEM : out of memory


### `_mulle_concurrent_hashmap_remove` - remove a hash/value pair

```
int  _mulle_concurrent_hashmap_remove( struct mulle_concurrent_hashmap *map,
                                       intptr_t hash,
                                       void *value)
```

Remove a `hash`, `value` pair. Read the description of
`_mulle_concurrent_hashmap_insert` for information about restrictions
pertaining to both.

Return Values:
   0      : OK
   ENOENT : not found
   ENOMEM : out of memory


### `_mulle_concurrent_hashmap_lookup` - search for a value by hash

```
void   *_mulle_concurrent_hashmap_lookup( struct mulle_concurrent_hashmap *map,
                                          intptr_t hash)
```

Looks up a value by its hash.

Return Values:
   NULL  : not found
   otherwise the value for this hash

---

# `mulle_concurrent_hashmapenumerator`

```
struct mulle_concurrent_hashmapenumerator  mulle_concurrent_hashmap_enumerate( struct mulle_concurrent_hashmap *map)
```

Enumerate a hashtable. This works reliably if `map` is accessed in
single-threaded fashion, which it probably will NOT be. In multi-threaded
environments, the enumeration may be interrupted by mutations of the hashtable
by other threads. The enumerator itself should not be shared with other threads.

Here is a simple usage example:


```
   struct mulle_concurrent_hashmap             *map;
   struct mulle_concurrent_hashmapenumerator   rover;
   intptr_t                                    hash;
   void                                        *value;

   rover = mulle_concurrent_hashmap_enumerate( map);
   while( _mulle_concurrent_hashmapenumerator_next( &rover, &hash, &value) == 1)
   {
      printf( "%ld %p\n", hash, value);
   }
   _mulle_concurrent_hashmapenumerator_done( &rover);
```

### `_mulle_concurrent_hashmapenumerator_next`

```
int  _mulle_concurrent_hashmapenumerator_next( struct mulle_concurrent_hashmapenumerator *rover,
                                               intptr_t *hash,
                                               void **value)
```

Get the next `hash`, `value` pair from the enumerator.

Return Values:
   1           : OK
   0           : nothing left
   -ECANCELLED : hashtable was mutated (Note: **negative errno value**!)
   -ENOMEM     : out of memory         (Note: **negative errno value**!)


### `_mulle_concurrent_hashmapenumerator_done`

```
void  _mulle_concurrent_hashmapenumerator_done( struct mulle_concurrent_hashmapenumerator *rover)
```

It's a mere conventional function. It may be left out.


### `mulle_concurrent_hashmap_count`

```
unsigned int   mulle_concurrent_hashmap_count( struct mulle_concurrent_hashmap *map);
```

This gives you the current number of hash/value entries of `map`. It is implemented as an iterator loop, that counts the number of values.
The returned number is close to meaningless, when the map is accessed in multi-threaded fashion.


### `mulle_concurrent_hashmap_lookup_any` - get a value from the hashmap

```
void  *mulle_concurrent_hashmap_lookup_any( struct mulle_concurrent_hashmap *map);
```

This will return a value from the map. It is implemented as an iterator loop,
that returns the first value. It returns NULL if `map` contains no entries or
is NULL.


