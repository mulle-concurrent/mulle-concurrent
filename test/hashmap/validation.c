#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-testallocator/mulle-testallocator.h>
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


int   main( void)
{
   struct mulle_concurrent_hashmap             map;
   struct mulle_concurrent_hashmapenumerator   rover;

   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_hashmap_init( &map, 0, NULL);

   check( mulle_concurrent_hashmap_insert( NULL, 1, (void *) 1) == EINVAL,
          "hashmap insert NULL");
   check( mulle_concurrent_hashmap_insert( &map, MULLE_CONCURRENT_NO_HASH, (void *) 1) == EINVAL,
          "hashmap insert no hash");
   check( mulle_concurrent_hashmap_insert( &map, 1, MULLE_CONCURRENT_NO_POINTER) == EINVAL,
          "hashmap insert NULL value");
   check( mulle_concurrent_hashmap_insert( &map, 1, MULLE_CONCURRENT_INVALID_POINTER) == EINVAL,
          "hashmap insert invalid value");

   errno = 0;
   check( mulle_concurrent_hashmap_register( NULL, 1, (void *) 1) == MULLE_CONCURRENT_INVALID_POINTER &&
          errno == EINVAL,
          "hashmap register NULL");
   check( mulle_concurrent_hashmap_register( &map, MULLE_CONCURRENT_NO_HASH, (void *) 1) == MULLE_CONCURRENT_INVALID_POINTER,
          "hashmap register no hash");
   check( mulle_concurrent_hashmap_register( &map, 1, MULLE_CONCURRENT_NO_POINTER) == MULLE_CONCURRENT_INVALID_POINTER,
          "hashmap register NULL value");
   check( mulle_concurrent_hashmap_register( &map, 1, MULLE_CONCURRENT_INVALID_POINTER) == MULLE_CONCURRENT_INVALID_POINTER,
          "hashmap register invalid value");

   rover = mulle_concurrent_hashmap_enumerate( NULL);
   check( mulle_concurrent_hashmapenumerator_next( &rover, NULL, NULL) == 0,
          "hashmap enumerate NULL");
   mulle_concurrent_hashmapenumerator_done( &rover);

   check( mulle_concurrent_hashmap_insert( &map, 1, (void *) 10) == 0,
          "hashmap insert valid");

   // duplicate / register / convenience semantics
   check( mulle_concurrent_hashmap_insert( &map, 1, (void *) 10) == EEXIST,
          "hashmap duplicate insert");
   check( mulle_concurrent_hashmap_register( &map, 1, (void *) 20) == (void *) 10,
          "hashmap register existing");
   check( mulle_concurrent_hashmap_register( &map, 2, (void *) 30) == MULLE_CONCURRENT_NO_POINTER,
          "hashmap register fresh");
   check( mulle_concurrent_hashmap_lookup( NULL, 1) == NULL,
          "hashmap lookup NULL map");
   check( mulle_concurrent_hashmap_get_size( NULL) == 0,
          "hashmap get_size NULL");
   check( mulle_concurrent_hashmap_count( NULL) == 0,
          "hashmap count NULL");
   check( mulle_concurrent_hashmap_lookup_any( NULL) == NULL,
          "hashmap lookup_any NULL");
   check( mulle_concurrent_hashmapenumerator_next( NULL, NULL, NULL) == EINVAL,
          "hashmap enumerator NULL rover");
   check( mulle_concurrent_hashmap_count( &map) == 2,
          "hashmap count");
   check( mulle_concurrent_hashmap_lookup_any( &map) != NULL,
          "hashmap lookup_any");

   mulle_concurrent_hashmap_done( &map);
   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   printf( "PASSED\n");
   return( 0);
}
