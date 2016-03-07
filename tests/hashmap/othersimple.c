#include <mulle_standalone_concurrent/mulle_standalone_concurrent.h>


static void   test( void)
{
   intptr_t                                    hash;
   struct mulle_concurrent_hashmap             map;
   struct mulle_concurrent_hashmapenumerator   rover;
   unsigned int                                i;
   void                                        *value;

   _mulle_concurrent_hashmap_init( &map, 0, NULL);
   {
      _mulle_concurrent_hashmap_insert( &map, 100000, (void *) 0x1848);
      value =  _mulle_concurrent_hashmap_lookup( &map, 100000);
      printf( "%p\n", value);

      value =  _mulle_concurrent_hashmap_lookup( &map, 123456);
      printf( "%s\n", value == (void *) 0x1848 ? "unexpected" : "expected");


      rover = mulle_concurrent_hashmap_enumerate( &map);
      while( _mulle_concurrent_hashmapenumerator_next( &rover, &hash, &value) == 1)
      {
         printf( "%ld %p\n", hash, value);
      }
      _mulle_concurrent_hashmapenumerator_done( &rover);

      _mulle_concurrent_hashmap_remove( &map, 100000, (void *) 0x1848);

      value = _mulle_concurrent_hashmap_lookup( &map, 100000);
      printf( "%s\n", value == (void *) 0x1848 ? "unexpected" : "expected");
   }
   _mulle_concurrent_hashmap_done( &map);
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

