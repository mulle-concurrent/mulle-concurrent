//
// Single-threaded _mulle_concurrent_hashmap_remove / reuse semantics.
//
// remove clears the value while keeping the hash claim (so probe chains stay
// intact) and lookup then reports the key as absent. Unlike the old tombstone
// design, the slot may be repopulated immediately (with patch, register or
// insert) without waiting for a migration. This is safe because remove is
// single-threaded only: it never runs concurrently with the value-word
// monotonicity that bounds a migration.
//
#include <mulle-concurrent/mulle-concurrent.h>

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


static void   remove_reuse_test( void)
{
   struct mulle_concurrent_hashmap   map;
   void                              *result;

   mulle_concurrent_hashmap_init( &map, 0, NULL);

   check( mulle_concurrent_hashmap_insert( &map, 1, (void *) 10) == 0,
          "insert");
   check( mulle_concurrent_hashmap_count( &map) == 1, "count after insert");

   check( _mulle_concurrent_hashmap_remove( &map, 1) == 0, "remove");
   check( mulle_concurrent_hashmap_lookup( &map, 1) == MULLE_CONCURRENT_NO_POINTER,
          "lookup after remove");
   check( mulle_concurrent_hashmap_count( &map) == 0, "count after remove");

   // remove is an unconditional clear, so a second remove is a no-op
   check( _mulle_concurrent_hashmap_remove( &map, 1) == 0,
          "double remove is a no-op");
   // ...but a key that was never claimed is ENOENT
   check( _mulle_concurrent_hashmap_remove( &map, 2) == ENOENT,
          "remove absent key");

   // the slot is immediately reusable, no migration required
   check( mulle_concurrent_hashmap_insert( &map, 1, (void *) 20) == 0,
          "reinsert after remove");
   check( mulle_concurrent_hashmap_lookup( &map, 1) == (void *) 20,
          "lookup reinserted");

   // register returns the stored value rather than leaking a foreign one
   result = mulle_concurrent_hashmap_register( &map, 1, (void *) 30);
   check( result == (void *) 20, "register returns stored value");

   // patch replaces in place
   check( _mulle_concurrent_hashmap_patch( &map, 1, (void *) 40) == 0,
          "patch");
   check( mulle_concurrent_hashmap_lookup( &map, 1) == (void *) 40,
          "lookup patched");

   // remove, then register reseeds the slot
   check( _mulle_concurrent_hashmap_remove( &map, 1) == 0, "remove again");
   result = mulle_concurrent_hashmap_register( &map, 1, (void *) 50);
   check( result == MULLE_CONCURRENT_NO_POINTER,
          "register resets removed slot");
   check( mulle_concurrent_hashmap_lookup( &map, 1) == (void *) 50,
          "lookup registered");

   mulle_concurrent_hashmap_done( &map);
}


int   main( void)
{
   mulle_aba_init( NULL);
   mulle_aba_register();

   remove_reuse_test();

   mulle_aba_unregister();
   mulle_aba_done();

   printf( "PASSED\n");
   return( 0);
}