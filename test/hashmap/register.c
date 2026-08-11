// Deterministic check of the register contract:
//
//   register( hash, value) on an empty slot returns NO_POINTER,
//   register( hash, value) on an existing entry returns the *stored* value
//                          (which may differ from the argument after patch)
//   patch( hash, value, expect) flips the stored value
//   remove( hash, value) needs the exact (hash, value) pair
//
// The cycle register -> patch -> register -> remove is repeated many times
// to shake out stale-state bugs.
#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-testallocator/mulle-testallocator.h>
#include <stdio.h>
#include <stdlib.h>


static void   check( int condition, char *name)
{
   if( condition)
      return;

   printf( "FAILED: %s\n", name);
   exit( 1);
}


#define N_ITERS   100000

int   main( void)
{
   struct mulle_concurrent_hashmap   map;
   intptr_t                          key     = 42;
   void                              *value   = (void *) 421;
   void                              *patched = (void *) 426;
   void                              *result;
   intptr_t                          i;
   int                               rval;

   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_hashmap_init( &map, 4, NULL);

   for( i = 0; i < N_ITERS; i++)
   {
      // insert fresh: register must report the insert
      result = mulle_concurrent_hashmap_register( &map, key, value);
      check( result == MULLE_CONCURRENT_NO_POINTER || result == value,
             "register insert" );

      // patch to the new value
      rval = mulle_concurrent_hashmap_patch( &map, key, patched, value);
      check( rval == 0, "patch" );

      // register again: must return the *stored* (patched) value
      result = mulle_concurrent_hashmap_register( &map, key, value);
      check( result == patched, "register after patch" );

      // remove with the current value
      rval = mulle_concurrent_hashmap_remove( &map, key, patched);
      check( rval == 0, "remove" );

      // the key is now absent again
      check( mulle_concurrent_hashmap_lookup( &map, key) == NULL,
             "lookup after remove" );
   }

   mulle_concurrent_hashmap_done( &map);
   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   printf( "PASSED\n");
   return( 0);
}
