//
// Argument validation, plus the API consequence of moving migration state out
// of the value word: INVALID_POINTER and TOMBSTONE_POINTER are no longer
// reserved and can be stored as ordinary payloads. Only NULL is reserved.
//
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
   struct mulle_concurrent_hashmap2               map;
   struct mulle_concurrent_hashmap2enumerator     rover;
   void                                           *old;

   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   check( mulle_concurrent_hashmap2_init( NULL, 0, NULL) == EINVAL,
          "init NULL" );
   check( mulle_concurrent_hashmap2_init( &map, 0, NULL) == 0,
          "init" );

   check( mulle_concurrent_hashmap2_insert( NULL, 1, (void *) 1) == EINVAL,
          "insert NULL map" );
   check( mulle_concurrent_hashmap2_insert( &map, MULLE_CONCURRENT_NO_HASH, (void *) 1) == EINVAL,
          "insert no hash" );
   check( mulle_concurrent_hashmap2_insert( &map, 1, NULL) == EINVAL,
          "insert NULL value" );

   check( mulle_concurrent_hashmap2_remove( NULL, 1, (void *) 1) == EINVAL,
          "remove NULL map" );
   check( mulle_concurrent_hashmap2_remove( &map, MULLE_CONCURRENT_NO_HASH, (void *) 1) == EINVAL,
          "remove no hash" );
   check( mulle_concurrent_hashmap2_remove( &map, 1, NULL) == EINVAL,
          "remove NULL value" );

   check( mulle_concurrent_hashmap2_register( NULL, 1, (void *) 1, &old) == EINVAL,
          "register NULL map" );
   check( mulle_concurrent_hashmap2_register( &map, MULLE_CONCURRENT_NO_HASH, (void *) 1, &old) == EINVAL,
          "register no hash" );
   check( mulle_concurrent_hashmap2_register( &map, 1, NULL, &old) == EINVAL,
          "register NULL value" );

   check( mulle_concurrent_hashmap2_lookup( NULL, 1) == NULL,
          "lookup NULL map" );
   check( mulle_concurrent_hashmap2_get_size( NULL) == 0,
          "get_size NULL" );
   check( mulle_concurrent_hashmap2_count( NULL) == 0,
          "count NULL" );
   check( mulle_concurrent_hashmap2enumerator_next( NULL, NULL, NULL) == EINVAL,
          "enumerator NULL rover" );

   rover = mulle_concurrent_hashmap2_enumerate( NULL);
   check( mulle_concurrent_hashmap2enumerator_next( &rover, NULL, NULL) == 0,
          "enumerate NULL map" );
   mulle_concurrent_hashmap2enumerator_done( &rover);

   //
   // formerly reserved values are ordinary payloads here
   //
   check( mulle_concurrent_hashmap2_insert( &map, 10, MULLE_CONCURRENT_INVALID_POINTER) == 0,
          "insert INVALID_POINTER as payload" );
   check( mulle_concurrent_hashmap2_lookup( &map, 10) == MULLE_CONCURRENT_INVALID_POINTER,
          "lookup INVALID_POINTER payload" );

   check( mulle_concurrent_hashmap2_insert( &map, 11, MULLE_CONCURRENT_TOMBSTONE_POINTER) == 0,
          "insert TOMBSTONE_POINTER as payload" );
   check( mulle_concurrent_hashmap2_lookup( &map, 11) == MULLE_CONCURRENT_TOMBSTONE_POINTER,
          "lookup TOMBSTONE_POINTER payload" );

   check( mulle_concurrent_hashmap2_remove( &map, 10, MULLE_CONCURRENT_INVALID_POINTER) == 0,
          "remove INVALID_POINTER payload" );
   check( mulle_concurrent_hashmap2_remove( &map, 11, MULLE_CONCURRENT_TOMBSTONE_POINTER) == 0,
          "remove TOMBSTONE_POINTER payload" );

   //
   // the FROZEN bit is folded away, so a hash with the top bit set is usable
   // and lands in the same slot as its folded twin
   //
   check( mulle_concurrent_hashmap2_insert( &map, INTPTR_MIN | 12345, (void *) 0x99) == 0,
          "insert hash with top bit set" );
   check( mulle_concurrent_hashmap2_lookup( &map, INTPTR_MIN | 12345) == (void *) 0x99,
          "lookup hash with top bit set" );
   check( mulle_concurrent_hashmap2_lookup( &map, 12345) == (void *) 0x99,
          "top bit is folded away" );

   mulle_concurrent_hashmap2_done( &map);
   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   printf( "PASSED\n");
   return( 0);
}
