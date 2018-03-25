#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-test-allocator/mulle-test-allocator.h>
#include <assert.h>
#include <stdlib.h>
#include <errno.h>


static void   test( void)
{
   intptr_t                                    hash;
   struct mulle_concurrent_hashmap             map;
   struct mulle_concurrent_hashmapenumerator   rover;
   unsigned int                                i;
   void                                        *value;

   mulle_concurrent_hashmap_init( &map, 0, NULL);
   {
      mulle_concurrent_hashmap_insert( &map, 100000, (void *) 0x1848);
      value =  mulle_concurrent_hashmap_lookup( &map, 100000);
      printf( "%p\n", value);

      value =  mulle_concurrent_hashmap_lookup( &map, 123456);
      printf( "%s\n", value == (void *) 0x1848 ? "unexpected" : "expected");


      rover = mulle_concurrent_hashmap_enumerate( &map);
      while( mulle_concurrent_hashmapenumerator_next( &rover, &hash, &value) == 1)
      {
         printf( "%ld %p\n", hash, value);
      }
      mulle_concurrent_hashmapenumerator_done( &rover);

      mulle_concurrent_hashmap_remove( &map, 100000, (void *) 0x1848);

      value = _mulle_concurrent_hashmap_lookup( &map, 100000);
      printf( "%s\n", value == (void *) 0x1848 ? "unexpected" : "expected");
   }
   mulle_concurrent_hashmap_done( &map);
}


int   main( void)
{
   mulle_test_allocator_initialize();
   mulle_default_allocator = mulle_test_allocator;

   mulle_aba_init( NULL);
   mulle_aba_register();

   test();

   mulle_aba_unregister();
   mulle_aba_done();

   mulle_test_allocator_reset();

   return( 0);
}

