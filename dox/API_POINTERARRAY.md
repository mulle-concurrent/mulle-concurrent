# `mulle_concurrent_pointerlist`

`mulle_concurrent_pointerlist` is a mutable array of pointers that can only
grow. Such an array can be shared with multiple threads, that can access the
array without locking. It's limitations are it's strength, as it makes its
handling very simple.


The following operations should be executed in single-threaded fashion:

* `_mulle_concurrent_pointerarray_init`
* `_mulle_concurrent_pointerarray_done`
* `_mulle_concurrent_pointerarray_get_size`

The following operations are fine in multi-threaded environments:

* `_mulle_concurrent_pointerarray_add`
* `_mulle_concurrent_pointerarray_get`

The following operations work in multi-threaded environments,
but should be approached with caution:

* `mulle_concurrent_pointerarray_enumerate`
* `_mulle_concurrent_pointerarrayenumerator_next`
* `_mulle_concurrent_pointerarrayenumerator_done`
* `_mulle_concurrent_pointerarray_reverseenumerate`
* `_mulle_concurrent_pointerarrayreverseenumerator_next`
* `_mulle_concurrent_pointerarrayreverseenumerator_done`
* `mulle_concurrent_pointerarray_map`
* `_mulle_concurrent_pointerarray_find`
* `mulle_concurrent_pointerarray_get_count`


### `_mulle_concurrent_pointerarray_init` - initialize pointerarray

```
void   _mulle_concurrent_pointerarray_init( struct mulle_concurrent_pointerarray *array,
                                       unsigned int size,
                                       struct mulle_allocator *allocator)
```

Initialize `array`, with a starting `size` of elements. `allocator` will be
used to allocate and free memory during the lifetime of `array`.  You can pass in
for `allocator` to use the default. Call this in single-threaded fashion.


### `_mulle_concurrent_pointerarray_done` - free pointerarray resources

```
void  _mulle_concurrent_pointerarray_done( struct mulle_concurrent_pointerarray *array)
```

This will free all allocated resources `array`. It will not **free** `array`
itself though. `array` must be a valid pointer. Call this in single-threaded
fashion.


### `_mulle_concurrent_pointerarray_insert` - insert a hash/value pair

```
int  _mulle_concurrent_pointerarray_insert( struct mulle_concurrent_pointerarray *array,
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
not get dereferenced by the pointerarray.


Return Values:
   0      : OK
   EEXIST : duplicate
   ENOMEM : out of memory


### `_mulle_concurrent_pointerarray_remove` - remove a hash/value pair

```
int  _mulle_concurrent_pointerarray_remove( struct mulle_concurrent_pointerarray *array,
                                       intptr_t hash,
                                       void *value)
```

Remove a `hash`, `value` pair. Read the description of
`_mulle_concurrent_pointerarray_insert` for information about restrictions
pertaining to both.

Return Values:
   0      : OK
   ENOENT : not found
   ENOMEM : out of memory


### `_mulle_concurrent_pointerarray_lookup` - search for a value by hash

```
void   *_mulle_concurrent_pointerarray_lookup( struct mulle_concurrent_pointerarray *array,
                                          intptr_t hash)
```

Looks up a value by its hash.

Return Values:
   NULL  : not found
   otherwise the value for this hash


### `_mulle_concurrent_pointerarray_get_size` - get size of pointerarray

```
unsigned int  _mulle_concurrent_pointerarray_get_size( struct mulle_concurrent_pointerarray *array)
```

This gives you the capacity of `array`. This value is close to
meaningless, when the array is accessed in multi-threaded fashion.


### `_mulle_concurrent_pointerarray_get_size` - get number of entries of pointerarray

```
unsigned int   mulle_concurrent_pointerarray_get_count( struct mulle_concurrent_pointerarray *array);
```

This gives you the current number of hash/value entries of `array`. The returned
number is close to meaningless, when the array is accessed in multi-threaded
fashion.


## `mulle_concurrent_pointerarrayenumerator` - enumerator interface

```
struct mulle_concurrent_pointerarrayenumerator  mulle_concurrent_pointerarray_enumerate( struct mulle_concurrent_pointerarray *array)
```

Enumerate a hashtable. This works reliably if `array` is accessed in
single-threaded fashion, which it probably will NOT be. In multi-threaded
environments, the enumeration may be interrupted by mutations of the hashtable
by other threads. The enumerator itself should not be shared accessed by other threads.

Here is a simple usage example:


```
   struct mulle_concurrent_pointerarray             *array;
   struct mulle_concurrent_pointerarrayenumerator   rover;
   intptr_t                                    hash;
   void                                        *value;

   rover = mulle_concurrent_pointerarray_enumerate( array);
   while( _mulle_concurrent_pointerarrayenumerator_next( &rover, &hash, &value) == 1)
   {
      printf( "%ld %p\n", hash, value);
   }
   _mulle_concurrent_pointerarrayenumerator_done( &rover);
```

### `_mulle_concurrent_pointerarrayenumerator_next` - get next hash/value pair

```
int  _mulle_concurrent_pointerarrayenumerator_next( struct mulle_concurrent_pointerarrayenumerator *rover,
                                               intptr_t *hash,
                                               void **value)
```

Get the next `hash`, `value` pair from the enumerator.

Return Values:
   1           : OK
   0           : nothing left
   -ECANCELLED : hashtable was mutated (Note: **negative errno value**!)
   -ENOMEM     : out of memory         (Note: **negative errno value**!)


### `_mulle_concurrent_pointerarrayenumerator_done` - mark the end of the enumerator

```
void  _mulle_concurrent_pointerarrayenumerator_done( struct mulle_concurrent_pointerarrayenumerator *rover)
```

It's a mere conventional function. It may be left out.



