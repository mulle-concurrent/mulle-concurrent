<!-- Keywords: register, insert, lookup, remove, enumerate, allocator -->

# Patterns — coder bundle for mulle-concurrent

## ABA lifecycle (required for every thread)

```c
mulle_aba_init( NULL);          // once per process
mulle_aba_register();           // each thread, before touching any structure

// ... use concurrent structures ...

mulle_aba_unregister();         // each thread, before exit
mulle_aba_done();               // once per process
```

Use `mulle_thread_tss_t` destructor for automatic unregister:

```c
static mulle_thread_tss_t   aba_tss_key;

static void   aba_thread_destructor( void *value)
{
   mulle_aba_unregister();
}

static void   aba_setup( void)
{
   mulle_thread_tss_create( aba_thread_destructor, &aba_tss_key);
}

static void   aba_register_thread( void)
{
   mulle_aba_register();
   mulle_thread_tss_set( aba_tss_key, (void *) 1);
}
```

### Custom allocator with ABA deferred free

```c
mulle_aba_init( &mulle_testallocator);
mulle_allocator_set_aba( &mulle_testallocator,
                         mulle_aba_get_global(),
                         (mulle_allocator_aba_t *) _mulle_aba_free);

mulle_aba_register();
mulle_concurrent_hashmap_init( &map, 0, &mulle_testallocator);

// ... use map (old storage freed via ABA when threads quiesce) ...

mulle_concurrent_hashmap_done( &map);
mulle_aba_unregister();
mulle_allocator_set_aba( &mulle_testallocator, NULL, NULL);
mulle_aba_done();
```

## Hashmap

### Insert, lookup, remove

```c
struct mulle_concurrent_hashmap   map;
mulle_concurrent_hashmap_init( &map, 0, NULL);

mulle_concurrent_hashmap_insert( &map, 0x1234, (void *) 0x1000);
mulle_concurrent_hashmap_insert( &map, 0x5678, (void *) 0x2000);

void   *val = mulle_concurrent_hashmap_lookup( &map, 0x1234);
assert( val == (void *) 0x1000);

// remove: both hash AND value must match
int rval = mulle_concurrent_hashmap_remove( &map, 0x1234, (void *) 0x1000);
assert( rval == 0);

mulle_concurrent_hashmap_done( &map);
```

### Register (insert-or-get)

```c
void   *result = mulle_concurrent_hashmap_register( &map, 0xABCD, (void *) 0x4000);
if( result == MULLE_CONCURRENT_NO_POINTER)
   ; // inserted
else if( result == MULLE_CONCURRENT_INVALID_POINTER)
   ; // error — check errno
else
   ; // already present, result is the existing value
```

### Enumeration with retry

```c
intptr_t   hash;
void       *value;
int        rval;

retry:
rover = mulle_concurrent_hashmap_enumerate( &map);
while( (rval = mulle_concurrent_hashmapenumerator_next( &rover, &hash, &value)) == 1)
   printf( "%lu : %p\n", (unsigned long) hash, value);
mulle_concurrent_hashmapenumerator_done( &rover);
if( rval == ECANCELLED || rval == EBUSY)
   goto retry;
```

### Enumeration via macro (single-threaded / no-mutation)

```c
intptr_t   hash;
void       *value;

mulle_concurrent_hashmap_for( &map, hash, value)
   printf( "hash=0x%lx value=%p\n", (unsigned long) hash, value);
```

You can also use `mulle_concurrent_hashmap_for_rval` to expose the return value of
`_next`:

```c
int   rval;
mulle_concurrent_hashmap_for_rval( &map, hash, value, rval)
   ; // rval is 1 on each valid entry
if( rval == ECANCELLED || rval == EBUSY)
   goto retry;
```

### Use `_prefixed` functions for hot paths

When the map pointer is guaranteed non-NULL and parameters are pre-validated:

```c
_mulle_concurrent_hashmap_insert( &map, hash, value);
_mulle_concurrent_hashmap_lookup( &map, hash);
_mulle_concurrent_hashmap_remove( &map, hash, value);
```

This avoids the NULL-check overhead in the safe wrappers.

## Pointerarray

### Add, get, find

```c
struct mulle_concurrent_pointerarray   array;
mulle_concurrent_pointerarray_init( &array, 0, NULL);

mulle_concurrent_pointerarray_add( &array, (void *) 0x10);
mulle_concurrent_pointerarray_add( &array, (void *) 0x20);
mulle_concurrent_pointerarray_add( &array, (void *) 0x30);

void *val = mulle_concurrent_pointerarray_get( &array, 1);
assert( val == (void *) 0x20);

int index = mulle_concurrent_pointerarray_find( &array, (void *) 0x30);
assert( index == 2);

mulle_concurrent_pointerarray_done( &array);
```

### Forward and reverse enumeration

```c
unsigned int   count = mulle_concurrent_pointerarray_get_count( &array);
void           *item;

mulle_concurrent_pointerarray_for( &array, item)
   printf( "%p\n", item);

mulle_concurrent_pointerarray_for_reverse( &array, count, item)
   printf( "%p\n", item);
```

### Manual enumeration

```c
struct mulle_concurrent_pointerarrayenumerator   rover;
void                                             *value;

rover = mulle_concurrent_pointerarray_enumerate( &array);
while( (value = mulle_concurrent_pointerarrayenumerator_next( &rover)))
   printf( "%p\n", value);
mulle_concurrent_pointerarrayenumerator_done( &rover);
```

Pointer array enumeration is safe even during concurrent adds (no removal
operations exist).

## Pointerset

### Insert, register, member, remove

```c
struct mulle_concurrent_pointerset   set;
mulle_concurrent_pointerset_init( &set, 0, NULL);

mulle_concurrent_pointerset_insert( &set, (void *) 0xAAAA);
mulle_concurrent_pointerset_insert( &set, (void *) 0xBBBB);

assert( mulle_concurrent_pointerset_member( &set, (void *) 0xAAAA) == 1);
assert( mulle_concurrent_pointerset_member( &set, (void *) 0xCCCC) == 0);

void *result = mulle_concurrent_pointerset_register( &set, (void *) 0x9999);
// MULLE_CONCURRENT_NO_POINTER = inserted,
// MULLE_CONCURRENT_INVALID_POINTER = error,
// pointer = already present

mulle_concurrent_pointerset_remove( &set, (void *) 0xAAAA);

mulle_concurrent_pointerset_done( &set);
```

### Enumeration

```c
void *ptr;
mulle_concurrent_pointerset_for( &set, ptr)
   printf( "Found: %p\n", ptr);
```

### Reset for tombstone cleanup

```c
// after many removes in a tight loop, call reset to drop tombstones
mulle_concurrent_pointerset_reset( &set);
```

### Member with NULL returns 0 silently

```c
// does NOT set errno — safe to call without error handling
if( mulle_concurrent_pointerset_member( &set, NULL))
   ; // never true
```

## Multi-threaded stress test skeleton

```c
static mulle_thread_rval_t   worker( void *arg)
{
   struct mulle_concurrent_hashmap   *map = arg;
   mulle_aba_register();

   while( mulle_concurrent_hashmap_get_size( map) < 1024 * 1024)
   {
      // 20% insert, 10% remove, 70% lookup, 0.1% enumerate
   }

   mulle_aba_unregister();
   mulle_thread_return();
}

static void   run( unsigned int n_threads)
{
   struct mulle_concurrent_hashmap   map;
   mulle_thread_t                    threads[ 32];
   unsigned int                      i;

   mulle_aba_init( &mulle_testallocator);
   mulle_allocator_set_aba( &mulle_testallocator,
                            mulle_aba_get_global(),
                            (mulle_allocator_aba_t *) _mulle_aba_free_owned_pointer);
   mulle_aba_register();

   mulle_concurrent_hashmap_init( &map, 0, &mulle_testallocator);

   for( i = 0; i < n_threads; i++)
      mulle_thread_create( worker, &map, &threads[ i]);
   for( i = 0; i < n_threads; i++)
      mulle_thread_join( threads[ i]);

   mulle_concurrent_hashmap_done( &map);
   mulle_aba_unregister();
   mulle_allocator_set_aba( &mulle_testallocator, NULL, NULL);
   mulle_aba_done();
}
```

See `test/hashmap/hashmap.c` and `test/array/pointerarray.c` for working
multi-threaded stress tests.
