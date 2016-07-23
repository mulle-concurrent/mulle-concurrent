#include <mulle_concurrent/mulle_concurrent.h>


static void   test( void)
{
   struct mulle_concurrent_pointerarray             map;
   struct mulle_concurrent_pointerarrayenumerator   rover;
   unsigned int                                     i;
   void                                             *value;

   _mulle_concurrent_pointerarray_init( &map, 0, NULL);
   {
      value = (void *) 0x1848;

      _mulle_concurrent_pointerarray_add( &map, value);
      value = _mulle_concurrent_pointerarray_get( &map, 0);
      printf( "%p\n", value);

      rover = mulle_concurrent_pointerarray_enumerate( &map);
      while( _mulle_concurrent_pointerarrayenumerator_next( &rover, &value) == 1)
      {
         printf( "%p\n", value);
      }
      _mulle_concurrent_pointerarrayenumerator_done( &rover);
   }
   _mulle_concurrent_pointerarray_done( &map);
}


int   main( void)
{
   mulle_aba_init( NULL);
   mulle_aba_register();

   test();

   mulle_aba_unregister();
   mulle_aba_done();

   return( 0);
}
