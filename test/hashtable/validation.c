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
   struct mulle_concurrent_hashtable               map;
   struct mulle_concurrent_hashtableenumerator     rover;
   void                                           *old;

   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   check( mulle_concurrent_hashtable_init( NULL, 0, NULL) == EINVAL,
          "init NULL" );
   check( mulle_concurrent_hashtable_init( &map, 0, NULL) == 0,
          "init" );

   check( mulle_concurrent_hashtable_insert( NULL, 1, (void *) 1) == EINVAL,
          "insert NULL map" );
   check( mulle_concurrent_hashtable_insert( &map, MULLE_CONCURRENT_NO_HASH, (void *) 1) == EINVAL,
          "insert no hash" );
   check( mulle_concurrent_hashtable_insert( &map, 1, NULL) == EINVAL,
          "insert NULL value" );

   check( mulle_concurrent_hashtable_remove( NULL, 1, (void *) 1) == EINVAL,
          "remove NULL map" );
   check( mulle_concurrent_hashtable_remove( &map, MULLE_CONCURRENT_NO_HASH, (void *) 1) == EINVAL,
          "remove no hash" );
   check( mulle_concurrent_hashtable_remove( &map, 1, NULL) == EINVAL,
          "remove NULL value" );

   check( mulle_concurrent_hashtable_register( NULL, 1, (void *) 1, &old) == EINVAL,
          "register NULL map" );
   check( mulle_concurrent_hashtable_register( &map, MULLE_CONCURRENT_NO_HASH, (void *) 1, &old) == EINVAL,
          "register no hash" );
   check( mulle_concurrent_hashtable_register( &map, 1, NULL, &old) == EINVAL,
          "register NULL value" );

   check( mulle_concurrent_hashtable_lookup( NULL, 1) == NULL,
          "lookup NULL map" );
   check( mulle_concurrent_hashtable_get_size( NULL) == 0,
          "get_size NULL" );
   check( mulle_concurrent_hashtable_count( NULL) == 0,
          "count NULL" );
   check( mulle_concurrent_hashtableenumerator_next( NULL, NULL, NULL) == EINVAL,
          "enumerator NULL rover" );

   rover = mulle_concurrent_hashtable_enumerate( NULL);
   check( mulle_concurrent_hashtableenumerator_next( &rover, NULL, NULL) == 0,
          "enumerate NULL map" );
   mulle_concurrent_hashtableenumerator_done( &rover);

   //
   // formerly reserved values are ordinary payloads here
   //
   check( mulle_concurrent_hashtable_insert( &map, 10, MULLE_CONCURRENT_INVALID_POINTER) == 0,
          "insert INVALID_POINTER as payload" );
   check( mulle_concurrent_hashtable_lookup( &map, 10) == MULLE_CONCURRENT_INVALID_POINTER,
          "lookup INVALID_POINTER payload" );

   check( mulle_concurrent_hashtable_insert( &map, 11, MULLE_CONCURRENT_TOMBSTONE_POINTER) == 0,
          "insert TOMBSTONE_POINTER as payload" );
   check( mulle_concurrent_hashtable_lookup( &map, 11) == MULLE_CONCURRENT_TOMBSTONE_POINTER,
          "lookup TOMBSTONE_POINTER payload" );

   check( mulle_concurrent_hashtable_remove( &map, 10, MULLE_CONCURRENT_INVALID_POINTER) == 0,
          "remove INVALID_POINTER payload" );
   check( mulle_concurrent_hashtable_remove( &map, 11, MULLE_CONCURRENT_TOMBSTONE_POINTER) == 0,
          "remove TOMBSTONE_POINTER payload" );

   //
   // The FROZEN bit (topmost) is RESERVED. Passing a hash with that bit set
   // is a caller bug (asserts in debug builds). The usable hash space is
   // [1, INTPTR_MAX]. On LP64 user-space pointers never have bit 63 set, so
   // pointer-as-hash is safe. On ILP32 pointers above 0x80000000 are NOT
   // usable as hashes without masking.
   //
   // We do NOT test INTPTR_MIN|x here — it would fire the assert.
   //

   // edge case: hash == 1 is fine (0 is nudged to 1 internally but the
   // caller should not rely on that; explicit 1 must work)
   check( mulle_concurrent_hashtable_insert( &map, 1, (void *) 0x77) == 0,
          "insert hash=1" );
   check( mulle_concurrent_hashtable_lookup( &map, 1) == (void *) 0x77,
          "lookup hash=1" );
   check( mulle_concurrent_hashtable_remove( &map, 1, (void *) 0x77) == 0,
          "remove hash=1" );

   // INTPTR_MAX is the largest legal hash
   check( mulle_concurrent_hashtable_insert( &map, INTPTR_MAX, (void *) 0x88) == 0,
          "insert hash=INTPTR_MAX" );
   check( mulle_concurrent_hashtable_lookup( &map, INTPTR_MAX) == (void *) 0x88,
          "lookup hash=INTPTR_MAX" );
   check( mulle_concurrent_hashtable_remove( &map, INTPTR_MAX, (void *) 0x88) == 0,
          "remove hash=INTPTR_MAX" );

   mulle_concurrent_hashtable_done( &map);
   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   printf( "PASSED\n");
   return( 0);
}
