// Deterministic, single-threaded checks for the hashmap's tombstone
// behaviour: remove() must leave a tombstone (not resurrect a foreign-slot
// leak, see register_race.c), lookup()/enumeration/count() must never expose
// MULLE_CONCURRENT_TOMBSTONE_POINTER, and a removed hash must not be reused
// until strict-growth migration drops its tombstone.
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


static void   remove_then_lookup_test( void)
{
   struct mulle_concurrent_hashmap   map;

   mulle_concurrent_hashmap_init( &map, 0, NULL);

   check( mulle_concurrent_hashmap_insert( &map, 42, (void *) 0x1000) == 0,
          "insert" );
   check( mulle_concurrent_hashmap_lookup( &map, 42) == (void *) 0x1000,
          "lookup before remove" );

   check( mulle_concurrent_hashmap_remove( &map, 42, (void *) 0x1000) == 0,
          "remove" );
   check( mulle_concurrent_hashmap_lookup( &map, 42) == MULLE_CONCURRENT_NO_POINTER,
          "lookup after remove is NO_POINTER, not the tombstone" );

   // removing again must fail, not resurrect the tombstone as a match
   check( mulle_concurrent_hashmap_remove( &map, 42, (void *) 0x1000) == ENOENT,
          "double remove" );

   mulle_concurrent_hashmap_done( &map);
}


static void   tombstone_reuse_is_denied_test( void)
{
   struct mulle_concurrent_hashmap   map;
   void                              *result;

   mulle_concurrent_hashmap_init( &map, 4, NULL);

   check( mulle_concurrent_hashmap_insert( &map, 7, (void *) 0x2000) == 0,
          "insert" );
   check( mulle_concurrent_hashmap_remove( &map, 7, (void *) 0x2000) == 0,
          "remove" );

   errno  = 0;
   result = mulle_concurrent_hashmap_register( &map, 7, (void *) 0x3000);
   check( result == MULLE_CONCURRENT_INVALID_POINTER && errno == EEXIST,
          "register does not refill a tombstone" );
   check( mulle_concurrent_hashmap_insert( &map, 7, (void *) 0x3000) == EEXIST,
          "insert does not refill a tombstone" );
   check( mulle_concurrent_hashmap_lookup( &map, 7) == MULLE_CONCURRENT_NO_POINTER,
          "denied reuse remains removed" );

   // Filling the claimed-slot threshold strictly grows the table. Migration
   // drops the tombstone, after which the hash can be registered again.
   check( mulle_concurrent_hashmap_insert( &map, 8, (void *) 0x4000) == 0,
          "insert before migration" );
   check( mulle_concurrent_hashmap_insert( &map, 9, (void *) 0x5000) == 0,
          "insert triggers migration" );
   check( mulle_concurrent_hashmap_get_size( &map) == 8,
          "migration strictly doubles storage" );

   result = mulle_concurrent_hashmap_register( &map, 7, (void *) 0x3000);
   check( result == MULLE_CONCURRENT_NO_POINTER,
          "register succeeds after migration drops tombstone" );
   check( mulle_concurrent_hashmap_lookup( &map, 7) == (void *) 0x3000,
          "lookup sees value registered after migration" );

   mulle_concurrent_hashmap_done( &map);
}


static void   count_and_enumerate_skip_tombstones_test( void)
{
   struct mulle_concurrent_hashmap             map;
   struct mulle_concurrent_hashmapenumerator   rover;
   intptr_t                                    hash;
   void                                        *value;
   unsigned int                                seen;
   int                                          rval;

   mulle_concurrent_hashmap_init( &map, 0, NULL);

   check( mulle_concurrent_hashmap_insert( &map, 1, (void *) 0x10) == 0, "insert 1" );
   check( mulle_concurrent_hashmap_insert( &map, 2, (void *) 0x20) == 0, "insert 2" );
   check( mulle_concurrent_hashmap_insert( &map, 3, (void *) 0x30) == 0, "insert 3" );

   check( mulle_concurrent_hashmap_remove( &map, 2, (void *) 0x20) == 0, "remove 2" );

   check( mulle_concurrent_hashmap_count( &map) == 2,
          "count excludes tombstoned key" );

   seen  = 0;
   rover = mulle_concurrent_hashmap_enumerate( &map);
   for(;;)
   {
      rval = mulle_concurrent_hashmapenumerator_next( &rover, &hash, &value);
      if( rval != 1)
         break;

      check( value != MULLE_CONCURRENT_TOMBSTONE_POINTER,
             "enumerator never yields the tombstone sentinel" );
      check( hash != 2, "enumerator skips the removed key" );
      ++seen;
   }
   mulle_concurrent_hashmapenumerator_done( &rover);

   check( rval == 0, "enumeration completed without ECANCELED" );
   check( seen == 2, "enumerator visited exactly the live keys" );

   mulle_concurrent_hashmap_done( &map);
}


static void   validation_test( void)
{
   struct mulle_concurrent_hashmap   map;

   mulle_concurrent_hashmap_init( &map, 0, NULL);

   check( mulle_concurrent_hashmap_insert( &map, 1, MULLE_CONCURRENT_TOMBSTONE_POINTER) == EINVAL,
          "insert tombstone value" );
   check( mulle_concurrent_hashmap_register( &map, 1, MULLE_CONCURRENT_TOMBSTONE_POINTER)
             == MULLE_CONCURRENT_INVALID_POINTER,
          "register tombstone value" );
   check( mulle_concurrent_hashmap_remove( &map, 1, MULLE_CONCURRENT_TOMBSTONE_POINTER) == EINVAL,
          "remove tombstone value" );

   mulle_concurrent_hashmap_done( &map);
}


int   main( void)
{
   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   remove_then_lookup_test();
   tombstone_reuse_is_denied_test();
   count_and_enumerate_skip_tombstones_test();
   validation_test();

   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   printf( "PASSED\n");
   return( 0);
}
