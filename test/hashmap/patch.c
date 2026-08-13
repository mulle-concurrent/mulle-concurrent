//
// _mulle_concurrent_hashmap_patch: single-threaded unconditional value
// replacement.  No validation wrapper — asserts only.
//
#define HAVE_MULLE_CONCURRENT_POSEAS_PATCH
#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-testallocator/mulle-testallocator.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>


static void   check( int condition, char *name)
{
   if( condition)
      return;
   printf( "FAILED: %s\n", name);
   exit( 1);
}


static void   sequential_test( void)
{
   struct mulle_concurrent_hashmap   map;

   mulle_concurrent_hashmap_init( &map, 0, NULL);

   // patch missing key
   check( _mulle_concurrent_hashmap_patch( &map, 1, (void *) 20) == ENOENT,
          "patch absent key");

   // insert then patch
   check( mulle_concurrent_hashmap_insert( &map, 1, (void *) 10) == 0,
          "insert for patch");
   check( _mulle_concurrent_hashmap_patch( &map, 1, (void *) 20) == 0,
          "patch existing");
   check( mulle_concurrent_hashmap_lookup( &map, 1) == (void *) 20,
          "patch applied");

   // patch again (repeatable)
   check( _mulle_concurrent_hashmap_patch( &map, 1, (void *) 30) == 0,
          "patch again");
   check( mulle_concurrent_hashmap_lookup( &map, 1) == (void *) 30,
          "patch again applied");

   // patch yet again
   check( _mulle_concurrent_hashmap_patch( &map, 1, (void *) 40) == 0,
          "patch third time");
   check( mulle_concurrent_hashmap_lookup( &map, 1) == (void *) 40,
          "patch third time applied");

   // patch a tombstoned entry fails
   check( mulle_concurrent_hashmap_remove( &map, 1, (void *) 40) == 0,
          "remove for tombstone test");
   check( _mulle_concurrent_hashmap_patch( &map, 1, (void *) 50) == ENOENT,
          "patch tombstoned");

   // multiple keys
   check( mulle_concurrent_hashmap_insert( &map, 2, (void *) 100) == 0,
          "insert key 2");
   check( mulle_concurrent_hashmap_insert( &map, 3, (void *) 200) == 0,
          "insert key 3");
   check( _mulle_concurrent_hashmap_patch( &map, 2, (void *) 111) == 0,
          "patch key 2");
   check( _mulle_concurrent_hashmap_patch( &map, 3, (void *) 222) == 0,
          "patch key 3");
   check( mulle_concurrent_hashmap_lookup( &map, 2) == (void *) 111,
          "key 2 patched");
   check( mulle_concurrent_hashmap_lookup( &map, 3) == (void *) 222,
          "key 3 patched");

   mulle_concurrent_hashmap_done( &map);
}


int   main( void)
{
   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   sequential_test();

   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   printf( "PASSED\n");
   return( 0);
}
