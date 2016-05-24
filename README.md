## `mulle_concurrent_pointerlist`

> Most of the ideas are taken from [Preshing on Programming](http://preshing.com/20160222/a-resizable-concurrent-map/).
> The definition of concurrent and wait-free are from [concurrencyfreaks.blogspot.de](http://concurrencyfreaks.blogspot.de/2013/05/lock-free-and-wait-free-definition-and.html)

A growing array of pointers, that is **wait-free**.

Here is a simple usage example:
```
#include <mulle_standalone_concurrent/mulle_standalone_concurrent.h>


static void   test( void)
{
   struct mulle_concurrent_pointerarray             map;
   struct mulle_concurrent_pointerarrayenumerator   rover;
   unsigned int                                     i;
   void                                             *value;

   _mulle_concurrent_pointerarray_init( &map, 0, NULL);

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

   _mulle_concurrent_pointerarray_done( &map);
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

A mutable map of pointers, indexed by a hash, that is **wait-free**.

Here is a also a simple usage example:

```
#include <mulle_standalone_concurrent/mulle_standalone_concurrent.h>

static void  test( void)
{
   intptr_t                                    hash;
   struct mulle_concurrent_hashmap             map;
   struct mulle_concurrent_hashmapenumerator   rover;
   unsigned int                                i;
   void                                        *value;

   _mulle_concurrent_hashmap_init( &map, 0, NULL);
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
   _mulle_concurrent_hashmap_done( &map);
}


int   main( void)
{
   mulle_aba_init( NULL);
   mulle_aba_register();

   test();

   mulle_aba_unregister();
   mulle_aba_done();

   return( 0);
}
```

## ABA

This library assumes that the allocator you give it, has a vector installed
for 'abafree', that is smart enough to solve the ABA problem when freeing memory.

> Hint : Use [mulle-aba](https://www.mulle-kybernetik.com/weblog/2015/mulle_aba_release.html) for that.


## Dependencies

* [mulle-thread](//www.mulle-kybernetik.com/repositories/mulle-thread)
* [mulle-allocator](//www.mulle-kybernetik.com/repositories/mulle-allocator)
* [mulle-aba](//www.mulle-kybernetik.com/repositories/mulle-aba) (for testing)
* [mulle-bootstrap](//www.mulle-kybernetik.com/repositories/mulle-bootstrap) (optional)
* xcodebuild for OS X
* cmake 3.0 for other Unixes


## How to build on OS X

Get the newest version of `mulle-bootstrap` (at least 0.19)

```
brew tap mulle-kybernetik/software
brew install mulle-bootstrap
```

Download and build this repository, with dependent libraries:

```
mulle-bootstrap clone https://www.mulle-kybernetik.com/repositories/mulle-concurrent
```
