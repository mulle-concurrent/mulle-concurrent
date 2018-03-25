#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-test-allocator/mulle-test-allocator.h>
#include <assert.h>
#include <stdlib.h>
#include <errno.h>


static void   test( void)
{
   struct mulle_concurrent_pointerarray             map;
   struct mulle_concurrent_pointerarrayenumerator   rover;
   unsigned int                                     i;
   void                                             *value;

   mulle_concurrent_pointerarray_init( &map, 0, NULL);
   {
      value = (void *) 0x1848;

      mulle_concurrent_pointerarray_add( &map, value);
      value = mulle_concurrent_pointerarray_get( &map, 0);
      printf( "%p\n", value);

      rover = mulle_concurrent_pointerarray_enumerate( &map);
      while( value = mulle_concurrent_pointerarrayenumerator_next( &rover))
         printf( "%p\n", value);
      mulle_concurrent_pointerarrayenumerator_done( &rover);
   }
   mulle_concurrent_pointerarray_done( &map);
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
