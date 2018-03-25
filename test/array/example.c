#include <mulle-concurrent/mulle-concurrent.h>
#include <mulle-test-allocator/mulle-test-allocator.h>
#include <assert.h>
#include <stdlib.h>
#include <errno.h>


static void   fail( char *s)
{
   perror( s);
   exit( 1);
}


int   main( int argc, const char * argv[])
{
   struct mulle_concurrent_pointerarrayenumerator   rover;
   struct mulle_concurrent_pointerarray             array;
   unsigned int                                     i, n;
   void                                             *value;
   int                                              rval;

   mulle_aba_init( NULL);
   mulle_aba_register();
   mulle_concurrent_pointerarray_init( &array, 0, NULL);

   {
      rval = mulle_concurrent_pointerarray_add( &array, (void *) 0x1);
      if( rval)
         fail( "mulle_concurrent_pointerarray_add");
      rval = mulle_concurrent_pointerarray_add( &array, (void *) 0x2);
      if( rval)
         fail( "mulle_concurrent_pointerarray_add");

      n = mulle_concurrent_pointerarray_get_count( &array);
      for( i = 0; i < n; i++)
      {
         value = mulle_concurrent_pointerarray_get( &array, i);
         printf( "%p\n", value);
      }
   }

   mulle_concurrent_pointerarray_done( &array);

   mulle_aba_unregister();
   mulle_aba_done();

   return( 0);
}
