// Deterministic check of the register contract.
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


#define N_ITERS   10000

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

      // first register inserts
      result = mulle_concurrent_hashmap_register( &map, key, value);
      check( result == MULLE_CONCURRENT_NO_POINTER, "register insert" );

      // second register finds existing value
      result = mulle_concurrent_hashmap_register( &map, key, other);
      check( result == value, "register existing" );

      // lookup confirms original value survives
      check( mulle_concurrent_hashmap_lookup( &map, key) == value, "lookup after register" );
   }

   check( mulle_concurrent_hashmap_count( &map) == N_ITERS, "final count" );

   mulle_concurrent_hashmap_done( &map);
   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   printf( "PASSED\n");
   return( 0);
}
