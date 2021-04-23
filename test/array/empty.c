#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-testallocator/mulle-testallocator.h>
#include <assert.h>
#include <stdlib.h>
#include <errno.h>


static void   test( void)
{
   struct mulle_concurrent_pointerarray map;

   mulle_concurrent_pointerarray_init( &map, 0, NULL);
   printf( "%ld\n", (long) _mulle_concurrent_pointerarray_get_size( &map));
   printf( "%ld\n", (long) _mulle_concurrent_pointerarray_get_count( &map));
   printf( "%sfound\n",
      _mulle_concurrent_pointerarray_find( &map, (void *) 0x1)
           ? "" : "not ");
   mulle_concurrent_pointerarray_done( &map);
}


int   main( void)
{
   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;

   mulle_aba_init( NULL);
   mulle_aba_register();

   test();

   mulle_aba_unregister();
   mulle_aba_done();

   mulle_testallocator_reset();

   return( 0);
}
