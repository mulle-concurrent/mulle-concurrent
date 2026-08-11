# `mulle_concurrent_pointerset`

`mulle_concurrent_pointerset` is a set of pointers. It uses the same storage
strategy as the hashmap, but keeps only a single pointer per slot. The pointer
itself is hashed internally, so there is no separate hash key to supply.

The set is resizable and uses linear probing with tombstones: a removed
pointer leaves a tombstone in its slot so that probe chains are not broken.
Tombstones are never reused for new inserts, but are dropped when the set
migrates to a larger storage. A remove-heavy workload therefore triggers
migration earlier, and `mulle_concurrent_pointerset_reset` can be used to
clear accumulated tombstones.

Such a set is extremely volatile when shared with multiple threads that are
inserting and removing pointers. For example, when you get the number of
elements of such a set it is akin to a fleeting glimpse of a distant past.

The following operations should be executed in single-threaded fashion:

* `mulle_concurrent_pointerset_init`
* `mulle_concurrent_pointerset_done`
* `mulle_concurrent_pointerset_reset`

The following operations are fine in multi-threaded environments:

* `mulle_concurrent_pointerset_insert`
* `mulle_concurrent_pointerset_register`
* `mulle_concurrent_pointerset_member`
* `mulle_concurrent_pointerset_remove`

The following operations work in multi-threaded environments, but should be
approached with caution:

* `mulle_concurrent_pointerset_enumerate`
* `mulle_concurrent_pointerset_lookup_any`
* `mulle_concurrent_pointerset_count`
* `mulle_concurrent_pointerset_get_size`


## single-threaded


### `mulle_concurrent_pointerset_init`

```
int   mulle_concurrent_pointerset_init( struct mulle_concurrent_pointerset *set,
                                        unsigned int size,
                                        struct mulle_allocator *allocator)
```

Initialize `set`, with a starting `size` of elements. `allocator` will be
used to allocate and free memory during the lifetime of `set`. You can pass in
`NULL` for `allocator` to use the default. Call this in single-threaded
fashion. Allocation is fail-fast: see *Memory allocation*.

Return Values:

*   0      : OK
*   EINVAL : invalid argument


### `mulle_concurrent_pointerset_done`

```
void  mulle_concurrent_pointerset_done( struct mulle_concurrent_pointerset *set)
```

This will free all allocated resources of `set`. It will not **free** `set`
itself though. Call this in single-threaded fashion, and only after no other
thread accesses `set` anymore.


### `mulle_concurrent_pointerset_reset`

```
void  mulle_concurrent_pointerset_reset( struct mulle_concurrent_pointerset *set)
```

Empty `set` and reinitialize it with the same size and allocator. Useful to
clear accumulated tombstones after a remove-heavy workload. Call this in
single-threaded fashion.


### `mulle_concurrent_pointerset_get_size`

```
unsigned int   mulle_concurrent_pointerset_get_size( struct mulle_concurrent_pointerset *set)
```

This gives you the current capacity of `set`. The returned number may not be
as meaningful as one might think, if the set is accessed in multi-threaded
fashion.


### `mulle_concurrent_pointerset_get_allocator`

```
struct mulle_allocator   *mulle_concurrent_pointerset_get_allocator( struct mulle_concurrent_pointerset *set)
```

Returns the allocator `set` was initialized with, or `NULL` if `set` is
`NULL`.


## multi-threaded


### `mulle_concurrent_pointerset_register`

```
void   *mulle_concurrent_pointerset_register( struct mulle_concurrent_pointerset *set,
                                              void *ptr)
```

Insert `ptr` into the set, or return the already-registered pointer if it is
already present. This is the atomic "insert if absent" primitive.

Return Values:

*   `MULLE_CONCURRENT_NO_POINTER`      : inserted (was not previously present)
*   `MULLE_CONCURRENT_INVALID_POINTER` : error (check `errno`)
*   `ptr`                              : already present

Do not pass `NULL`, `(void *) INTPTR_MIN` or `(void *) INTPTR_MAX` as `ptr`
(see *Reserved pointer values*).


### `mulle_concurrent_pointerset_insert`

```
int   mulle_concurrent_pointerset_insert( struct mulle_concurrent_pointerset *set,
                                          void *ptr)
```

Insert `ptr` into the set.

Return Values:

*   0      : inserted
*   EEXIST : already present
*   EINVAL : invalid argument


### `mulle_concurrent_pointerset_member`

```
int   mulle_concurrent_pointerset_member( struct mulle_concurrent_pointerset *set,
                                          void *ptr)
```

Return 1 if `ptr` is in the set, 0 if not. Passing a `NULL` set or a `NULL`
pointer returns 0 (silently, without setting `errno`).


### `mulle_concurrent_pointerset_remove`

```
int   mulle_concurrent_pointerset_remove( struct mulle_concurrent_pointerset *set,
                                          void *ptr)
```

Remove `ptr` from the set. The slot becomes a tombstone; see the intro for
what that means for growth.

Return Values:

*   0      : removed
*   ENOENT : not found
*   EINVAL : invalid argument


### `mulle_concurrent_pointerset_lookup_any` - get a pointer from the set

```
void  *mulle_concurrent_pointerset_lookup_any( struct mulle_concurrent_pointerset *set)
```

This will return any pointer from the set. It is implemented as an iterator
loop that returns the first value found. It returns NULL if `set` contains no
entries or is NULL.


### `mulle_concurrent_pointerset_count`

```
unsigned int   mulle_concurrent_pointerset_count( struct mulle_concurrent_pointerset *set)
```

This gives you the current number of entries in `set`. It is implemented as an
enumerator loop that counts the values, and it automatically retries if a
concurrent mutation interrupts the enumeration. The returned number may be
close to meaningless, when the set is accessed in multi-threaded fashion.


## limited multi-threaded (enumerator)


### `mulle_concurrent_pointerset_enumerate`

```
struct mulle_concurrent_pointerset_enumerator   mulle_concurrent_pointerset_enumerate( struct mulle_concurrent_pointerset *set)
```

Enumerate the set. Passing `NULL` produces an empty enumerator. The returned
enumerator should only be used by the calling thread.

Here is a simple usage example:

```
   struct mulle_concurrent_pointerset              *set;
   struct mulle_concurrent_pointerset_enumerator   rover;
   void                                            *ptr;
   int                                             rval;

retry:
   rover = mulle_concurrent_pointerset_enumerate( set);
   while( (rval = mulle_concurrent_pointerset_enumerator_next( &rover, &ptr)) == 1)
   {
      printf( "%p\n", ptr);
   }
   mulle_concurrent_pointerset_enumerator_done( &rover);

   if( rval == ECANCELED) // interrupted!
      goto retry;         // restart from the beginning will duplicate some
```


### `mulle_concurrent_pointerset_enumerator_next`

```
int  mulle_concurrent_pointerset_enumerator_next( struct mulle_concurrent_pointerset_enumerator *rover,
                                                  void **ptr)
```

Get the next pointer from the enumerator. In multi-threaded environments the
enumeration may be interrupted by mutations of the set by other threads. The
enumerator itself should not be shared with other threads.

Return Values:

*   1          : OK, `*ptr` filled
*   0          : nothing left
*   ECANCELED  : set was mutated, restart the enumeration
*   EINVAL     : invalid argument


### `mulle_concurrent_pointerset_enumerator_done`

```
void  mulle_concurrent_pointerset_enumerator_done( struct mulle_concurrent_pointerset_enumerator *rover)
```

It's a mere conventional function. It may be left out.


### `mulle_concurrent_pointerset_for` - convenience macro

```
mulle_concurrent_pointerset_for( set, ptr)
```

For-loop style enumeration of the set, invoking the body for each live
pointer. The macro manages the enumerator lifecycle for you.


## Reserved pointer values

The following values are used internally and must not be stored in the set:

| Value                           | Meaning
|---------------------------------|----------------------
| `NULL`                          | empty slot
| `(void *) INTPTR_MIN`           | REDIRECT marker during migration
| `(void *) INTPTR_MAX`           | tombstone of a removed slot

See the README section *Reserved pointer values* for the full table.


## Memory allocation

Allocation is **fail-fast**: the mulle allocator contract is *success or
abort*. When the default allocator cannot satisfy an allocation it prints an
error and aborts the process. The functions above therefore never return an
allocation error.
