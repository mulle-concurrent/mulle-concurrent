## `mulle_concurrent_pointerlist`

> Taking the definitions here from [concurrencyfreaks.blogspot.de](http://concurrencyfreaks.blogspot.de/2013/05/lock-free-and-wait-free-definition-and.html)

A growing array of pointers, that is wait-free. 

Here is a simple usage example:
```
#include <mulle_standalone_concurrent/mulle_standalone_concurrent.h>


static void   test( void)
{
   struct mulle_concurrent_pointerarray             map;
   struct mulle_concurrent_pointerarrayenumerator   rover;
   unsigned int                                     i;
   void                                             *value;

   _mulle_concurrent_pointerarray_init( &map, 0, mulle_aba_as_allocator());

   value = (void *) 0x1848;

   _mulle_concurrent_pointerarray_add( &map, value);
   value = _mulle_concurrent_pointerarray_get( &map, 0);
   printf( "%p\n", value);

   rover = mulle_concurrent_pointerarray_enumerate( &map);
   while( _mulle_concurrent_pointerarrayenumerator_next( &rover, &value) == 1)
   {
      printf( "%p\n", value);
   }
   _mulle_concurrent_pointerarrayenumerator_done( &rover);

   _mulle_concurrent_pointerarray_free( &map);
}


int   main( void)
{
   mulle_aba_init( &mulle_default_allocator);
   mulle_aba_register();

   test();

   mulle_aba_unregister();
   mulle_aba_done();

   return( 0);
}
```

## `mulle_concurrent_hashmap`

A mutable map of pointers, indexed by a hash, that is wait-free.

Here is a also a simple usage example:

```
#include <mulle_standalone_concurrent/mulle_standalone_concurrent.h>

static void  test( void)
{
   intptr_t                                     hash;
   struct mulle_concurrent_hashmap             map;
   struct mulle_concurrent_hashmapenumerator   rover;
   unsigned int                                i;
   void                                        *value;

   _mulle_concurrent_hashmap_init( &map, 0, mulle_aba_as_allocator());
   {
      _mulle_concurrent_hashmap_insert( &map, 100000, (void *) 0x1848);
      value =  _mulle_concurrent_hashmap_lookup( &map, 100000);
      printf( "%p\n", value);

      value =  _mulle_concurrent_hashmap_lookup( &map, 123456);
      printf( "%s\n", value == (void *) 0x1848 ? "unexpected" : "expected");


      rover = mulle_concurrent_hashmap_enumerate( &map);
      while( _mulle_concurrent_hashmapenumerator_next( &rover, &hash, &value) == 1)
      {
         printf( "%ld %p\n", hash, value);
      }
      _mulle_concurrent_hashmapenumerator_done( &rover);

      _mulle_concurrent_hashmap_remove( &map, 100000, (void *) 0x1848);

      value =  _mulle_concurrent_hashmap_lookup( &map, 100000);
      printf( "%s\n", value == (void *) 0x1848 ? "unexpected" : "expected");
   }
   _mulle_concurrent_hashmap_free( &map);
}


int   main( void)
{
   mulle_aba_init( &mulle_default_allocator);
   mulle_aba_register();

   test();

   mulle_aba_unregister();
   mulle_aba_done();

   return( 0);
}
```

## ABA

This library assumes that the allocator you give it is smart enough to solve
the ABA problem when freeing memory.

If you use `mulle_aba` you can ask it to act as an allocator (call 
`mulle_aba_as_allocator()`). If you don't want to use `mulle_aba`, create an 
allocator like this for your own scheme:


```
struct my_aba_allocator
{
   struct mulle_allocator  allocator;
   void                    *my_aba;
};


static void  *my_calloc( struct mulle_allocator *allocator,
                         size_t  n,
                         size_t  size)
{
   return( calloc( n, size));
}


static void  *my_realloc( struct mulle_allocator *allocator,
                          void  *block,
                          size_t  size)
{
   return( realloc( block, size));
}


static void  my_free( struct mulle_allocator *allocator,
                      void *pointer)
{
   struct my_aba_allocator   *p;
   
   p = (struct my_aba_allocator *) allocator;
   clever_free( p->my_aba, pointer);
}

struct my_aba_allocator    my_aba_allocator = 
{
   { my_calloc, my_realloc, my_free, 1 }, &clever_struct
};

```

## Dependencies

* mulle_allocator
* mulle_thread
* mulle_aba (for testing)

## How to build on OS X

Get the newest version of `mulle-bootstrap`

```
brew tap mulle-kybernetik/software
brew install mulle-bootstrap
```

Download this repository

```
git clone https://www.mulle-kybernetik.com/repositories/mulle-concurrent

# download and build dependencies
cd mulle-concurrent
mulle-bootstrap
```
