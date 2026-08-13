// insert_after_remove.c
//
// A key that has been removed must be insertable/registerable again.
// The tombstone must not block reuse from the caller's perspective.
//
// If the underlying storage generation has a tombstone, the hashmap layer
// should trigger a migration and retry, so that the caller never sees
// a spurious EEXIST for a dead key.
//
#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-testallocator/mulle-testallocator.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>


static int   failed;

static void   check( int condition, char *name)
{
   if( condition)
      return;

   printf( "FAILED: %s\n", name);
   failed = 1;
}


static void   insert_remove_insert_test( void)
{
   struct mulle_concurrent_hashmap   map;
   int                               rval;
   void                              *result;

   mulle_concurrent_hashmap_init( &map, 0, NULL);

   // insert key=0x1, value=0x2
   rval = mulle_concurrent_hashmap_insert( &map, 0x1, (void *) 0x2);
   check( rval == 0, "initial insert succeeds" );
   result = mulle_concurrent_hashmap_lookup( &map, 0x1);
   check( result == (void *) 0x2, "lookup after insert" );
   check( result != MULLE_CONCURRENT_TOMBSTONE_POINTER,
          "lookup never returns TOMBSTONE" );

   // remove key=0x1
   rval = mulle_concurrent_hashmap_remove( &map, 0x1, (void *) 0x2);
   check( rval == 0, "remove succeeds" );
   result = mulle_concurrent_hashmap_lookup( &map, 0x1);
   check( result == NULL, "lookup after remove returns NULL" );
   check( result != MULLE_CONCURRENT_TOMBSTONE_POINTER,
          "lookup after remove never returns TOMBSTONE" );

   // insert key=0x1 again — this MUST succeed
   rval = mulle_concurrent_hashmap_insert( &map, 0x1, (void *) 0x4);
   check( rval == 0, "insert after remove succeeds" );
   result = mulle_concurrent_hashmap_lookup( &map, 0x1);
   check( result == (void *) 0x4, "lookup returns new value" );
   check( result != MULLE_CONCURRENT_TOMBSTONE_POINTER,
          "lookup after reinsert never returns TOMBSTONE" );

   mulle_concurrent_hashmap_done( &map);
}


static void   register_remove_register_test( void)
{
   struct mulle_concurrent_hashmap   map;
   void                              *result;

   mulle_concurrent_hashmap_init( &map, 0, NULL);

   // register key=0x1
   result = mulle_concurrent_hashmap_register( &map, 0x1, (void *) 0x2);
   check( result == MULLE_CONCURRENT_NO_POINTER, "initial register succeeds" );
   check( result != MULLE_CONCURRENT_TOMBSTONE_POINTER,
          "register never returns TOMBSTONE" );

   // remove key=0x1
   check( mulle_concurrent_hashmap_remove( &map, 0x1, (void *) 0x2) == 0,
          "remove succeeds" );
   result = mulle_concurrent_hashmap_lookup( &map, 0x1);
   check( result == NULL, "lookup after remove returns NULL" );
   check( result != MULLE_CONCURRENT_TOMBSTONE_POINTER,
          "lookup after remove never returns TOMBSTONE" );

   // register key=0x1 again — this MUST succeed (insert fresh)
   result = mulle_concurrent_hashmap_register( &map, 0x1, (void *) 0x6);
   check( result == MULLE_CONCURRENT_NO_POINTER,
          "register after remove succeeds (fresh insert)" );
   check( result != MULLE_CONCURRENT_TOMBSTONE_POINTER,
          "register after remove never returns TOMBSTONE" );
   check( mulle_concurrent_hashmap_lookup( &map, 0x1) == (void *) 0x6,
          "lookup returns new value" );

   mulle_concurrent_hashmap_done( &map);
}


static void   insert_remove_insert_multiple_test( void)
{
   struct mulle_concurrent_hashmap   map;
   int                               rval;
   int                               i;

   mulle_concurrent_hashmap_init( &map, 0, NULL);

   // repeatedly insert, remove, re-insert the same key
   for( i = 0; i < 100; i++)
   {
      rval = mulle_concurrent_hashmap_insert( &map, 0x1, (void *)(uintptr_t)((i + 1) * 2));
      check( rval == 0, "insert in cycle" );

      check( mulle_concurrent_hashmap_lookup( &map, 0x1) == (void *)(uintptr_t)((i + 1) * 2),
             "lookup in cycle" );

      rval = mulle_concurrent_hashmap_remove( &map, 0x1, (void *)(uintptr_t)((i + 1) * 2));
      check( rval == 0, "remove in cycle" );

      check( mulle_concurrent_hashmap_lookup( &map, 0x1) == NULL,
             "lookup after remove in cycle" );

      // Reinsertion of a removed hash triggers a same-size migration, which
      // ABA-frees the old storage. Checkin periodically so the ABA epoch
      // advances and old storage is actually reclaimed, preventing unbounded
      // memory growth in tight remove/reinsert loops.
      if( (i & 0xF) == 0)
         mulle_aba_checkin();
   }

   mulle_concurrent_hashmap_done( &map);
}


int   main( void)
{
   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   insert_remove_insert_test();
   register_remove_register_test();
   insert_remove_insert_multiple_test();

   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   if( failed)
   {
      printf( "FAILED\n");
      return( 1);
   }
   printf( "PASSED\n");
   return( 0);
}
