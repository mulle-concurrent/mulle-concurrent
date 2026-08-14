//
// hashtable basics, with emphasis on the property the original hashmap lacks:
// a removed hash is immediately reusable, with no migration and no
// intermediate error state.
//
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


static void   insert_lookup_remove_test( void)
{
   struct mulle_concurrent_hashtable   map;

   mulle_concurrent_hashtable_init( &map, 0, NULL);

   check( mulle_concurrent_hashtable_insert( &map, 1, (void *) 0x10) == 0,
          "insert" );
   check( mulle_concurrent_hashtable_lookup( &map, 1) == (void *) 0x10,
          "lookup" );
   check( mulle_concurrent_hashtable_insert( &map, 1, (void *) 0x20) == EEXIST,
          "duplicate insert" );
   check( mulle_concurrent_hashtable_count( &map) == 1,
          "count" );

   check( mulle_concurrent_hashtable_remove( &map, 1, (void *) 0x99) == ENOENT,
          "remove wrong value" );
   check( mulle_concurrent_hashtable_remove( &map, 1, (void *) 0x10) == 0,
          "remove" );
   check( mulle_concurrent_hashtable_lookup( &map, 1) == NULL,
          "lookup after remove" );
   check( mulle_concurrent_hashtable_remove( &map, 1, (void *) 0x10) == ENOENT,
          "double remove" );
   check( mulle_concurrent_hashtable_count( &map) == 0,
          "count after remove" );

   mulle_concurrent_hashtable_done( &map);
}


//
// The headline property. No migration, no EEXIST, no tombstone.
//
static void   immediate_reuse_test( void)
{
   struct mulle_concurrent_hashtable   map;
   unsigned int                       size;
   int                                i;

   mulle_concurrent_hashtable_init( &map, 4, NULL);
   size = mulle_concurrent_hashtable_get_size( &map);

   for( i = 0; i < 1000; i++)
   {
      check( mulle_concurrent_hashtable_insert( &map, 7, (void *)(uintptr_t)(i + 1)) == 0,
             "reuse insert" );
      check( mulle_concurrent_hashtable_lookup( &map, 7) == (void *)(uintptr_t)(i + 1),
             "reuse lookup" );
      check( mulle_concurrent_hashtable_remove( &map, 7, (void *)(uintptr_t)(i + 1)) == 0,
             "reuse remove" );
      check( mulle_concurrent_hashtable_lookup( &map, 7) == NULL,
             "reuse lookup after remove" );
   }

   // 1000 remove/reinsert cycles must not have grown the table at all
   check( mulle_concurrent_hashtable_get_size( &map) == size,
          "reuse does not grow the storage" );

   mulle_concurrent_hashtable_done( &map);
}


static void   register_test( void)
{
   struct mulle_concurrent_hashtable   map;
   void                               *old;

   mulle_concurrent_hashtable_init( &map, 0, NULL);

   check( mulle_concurrent_hashtable_register( &map, 1, (void *) 0x10, &old) == 0 &&
          old == NULL,
          "register fresh" );
   check( mulle_concurrent_hashtable_register( &map, 1, (void *) 0x20, &old) == 0 &&
          old == (void *) 0x10,
          "register existing" );
   check( mulle_concurrent_hashtable_lookup( &map, 1) == (void *) 0x10,
          "register did not overwrite" );

   check( mulle_concurrent_hashtable_remove( &map, 1, (void *) 0x10) == 0,
          "register remove" );
   check( mulle_concurrent_hashtable_register( &map, 1, (void *) 0x30, &old) == 0 &&
          old == NULL,
          "register after remove is a fresh insert" );
   check( mulle_concurrent_hashtable_lookup( &map, 1) == (void *) 0x30,
          "register after remove stored" );

   mulle_concurrent_hashtable_done( &map);
}


static void   grow_test( void)
{
   struct mulle_concurrent_hashtable   map;
   intptr_t                           i;

   mulle_concurrent_hashtable_init( &map, 4, NULL);

   for( i = 1; i <= 1000; i++)
      check( mulle_concurrent_hashtable_insert( &map, i, (void *)(uintptr_t)(i * 4)) == 0,
             "grow insert" );

   check( mulle_concurrent_hashtable_count( &map) == 1000, "grow count" );
   check( mulle_concurrent_hashtable_get_size( &map) >= 2000, "grow size" );

   for( i = 1; i <= 1000; i++)
      check( mulle_concurrent_hashtable_lookup( &map, i) == (void *)(uintptr_t)(i * 4),
             "grow lookup" );

   // remove every other key, then reinsert with a new value
   for( i = 2; i <= 1000; i += 2)
      check( mulle_concurrent_hashtable_remove( &map, i, (void *)(uintptr_t)(i * 4)) == 0,
             "grow remove" );
   check( mulle_concurrent_hashtable_count( &map) == 500, "grow count after remove" );

   for( i = 2; i <= 1000; i += 2)
      check( mulle_concurrent_hashtable_insert( &map, i, (void *)(uintptr_t)(i * 8)) == 0,
             "grow reinsert" );
   check( mulle_concurrent_hashtable_count( &map) == 1000, "grow count after reinsert" );
   check( mulle_concurrent_hashtable_lookup( &map, 2) == (void *)(uintptr_t)(2 * 8),
          "grow reinserted value" );

   mulle_concurrent_hashtable_done( &map);
}


static void   enumerate_test( void)
{
   struct mulle_concurrent_hashtable               map;
   struct mulle_concurrent_hashtableenumerator     rover;
   intptr_t                                       hash;
   unsigned int                                   seen;
   void                                           *value;
   int                                            rval;

   mulle_concurrent_hashtable_init( &map, 0, NULL);

   check( mulle_concurrent_hashtable_insert( &map, 1, (void *) 0x10) == 0, "enum insert 1" );
   check( mulle_concurrent_hashtable_insert( &map, 2, (void *) 0x20) == 0, "enum insert 2" );
   check( mulle_concurrent_hashtable_insert( &map, 3, (void *) 0x30) == 0, "enum insert 3" );
   check( mulle_concurrent_hashtable_remove( &map, 2, (void *) 0x20) == 0, "enum remove 2" );

   seen  = 0;
   rover = mulle_concurrent_hashtable_enumerate( &map);
   while( (rval = mulle_concurrent_hashtableenumerator_next( &rover, &hash, &value)) == 1)
   {
      check( hash != 2, "enumerator skips the removed key" );
      check( value != NULL, "enumerator never yields an empty slot" );
      ++seen;
   }
   mulle_concurrent_hashtableenumerator_done( &rover);

   check( rval == 0, "enumeration completed" );
   check( seen == 2, "enumerator visited the live keys" );

   mulle_concurrent_hashtable_done( &map);
}


int   main( void)
{
   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   insert_lookup_remove_test();
   immediate_reuse_test();
   register_test();
   grow_test();
   enumerate_test();

   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   printf( "PASSED\n");
   return( 0);
}
