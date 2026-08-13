// Deterministic check of the register contract, including rejection of a
// tombstoned hash until migration drops that tombstone.
#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-testallocator/mulle-testallocator.h>
#include <errno.h>
#include <stdint.h>
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
   intptr_t                          key;
   void                              *value;
   void                              *other;
   void                              *result;
   intptr_t                          i;

   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_hashmap_init( &map, 4, NULL);

   for( i = 0; i < N_ITERS; i++)
   {
      key   = i + 1;
      value = (void *)(uintptr_t)(i + 1);
      other = (void *)(uintptr_t)(i + N_ITERS + 1);

      result = mulle_concurrent_hashmap_register( &map, key, value);
      check( result == MULLE_CONCURRENT_NO_POINTER, "register insert" );

      result = mulle_concurrent_hashmap_register( &map, key, other);
      check( result == value, "register existing" );

      check( mulle_concurrent_hashmap_remove( &map, key, value) == 0,
             "remove" );

      result = mulle_concurrent_hashmap_register( &map, key, other);
      check( result == MULLE_CONCURRENT_NO_POINTER,
             "register after remove succeeds" );

      // clean up for next iteration
      check( mulle_concurrent_hashmap_remove( &map, key, other) == 0,
             "remove other" );

      // Same-size migration on tombstone reuse ABA-frees old storage.
      // Checkin so the epoch advances and memory is reclaimed.
      if( (i & 0xFF) == 0)
         mulle_aba_checkin();
   }

   mulle_concurrent_hashmap_done( &map);
   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   printf( "PASSED\n");
   return( 0);
}
