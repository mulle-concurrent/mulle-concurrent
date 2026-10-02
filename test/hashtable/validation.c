//
// Argument validation, plus the API consequence of moving migration state out
// of the value word: INVALID_POINTER and TOMBSTONE_POINTER are no longer
// reserved and can be stored as ordinary payloads. Only NULL is reserved.
//
#include <mulle-concurrent/mulle-concurrent.h>

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

   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_hashtable_init( &map, 0, NULL);

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
   // is a caller bug and is rejected with EINVAL (NULL for lookup), never
   // silently folded. The usable hash space is [1, INTPTR_MAX]. On LP64
   // user-space pointers never have bit 63 set, so pointer-as-hash is safe.
   // On ILP32 pointers above 0x80000000 are out of range.
   //
   check( mulle_concurrent_hashtable_insert( &map, INTPTR_MIN, (void *) 0x99) == EINVAL,
          "insert hash with FROZEN bit set" );
   check( mulle_concurrent_hashtable_insert( &map, (intptr_t) (INTPTR_MIN | 0x5), (void *) 0x99) == EINVAL,
          "insert hash with FROZEN bit set + low bits" );
   check( mulle_concurrent_hashtable_lookup( &map, INTPTR_MIN) == NULL,
          "lookup hash with FROZEN bit set" );
   check( mulle_concurrent_hashtable_register( &map, INTPTR_MIN, (void *) 0x99, &old) == EINVAL,
          "register hash with FROZEN bit set" );
   check( mulle_concurrent_hashtable_remove( &map, INTPTR_MIN, (void *) 0x99) == EINVAL,
          "remove hash with FROZEN bit set" );

   // the predicate lets a caller tell "refused" from "absent" before looking up
   check( ! mulle_concurrent_hashtable_hash_is_valid( &map, MULLE_CONCURRENT_NO_HASH),
          "hash 0 is invalid" );
   check( ! mulle_concurrent_hashtable_hash_is_valid( &map, INTPTR_MIN),
          "hash with FROZEN bit is invalid" );
   check( mulle_concurrent_hashtable_hash_is_valid( &map, 1),
          "hash 1 is valid" );
   check( mulle_concurrent_hashtable_hash_is_valid( &map, INTPTR_MAX),
          "hash INTPTR_MAX is valid" );

   // edge case: hash == 1 is fine (explicit 1 must work)
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

   //
   // even mode: bit 0 is FROZEN, so odd hashes are out of range and rejected
   //
   {
      struct mulle_concurrent_hashtable   even_map;

      mulle_concurrent_hashtable_init_even( &even_map, 0, NULL);

      check( mulle_concurrent_hashtable_insert( &even_map, 3, (void *) 0xaa) == EINVAL,
             "even mode rejects odd hash" );
      check( mulle_concurrent_hashtable_lookup( &even_map, 3) == NULL,
             "even mode rejects odd lookup" );
      check( mulle_concurrent_hashtable_remove( &even_map, 3, (void *) 0xaa) == EINVAL,
             "even mode rejects odd remove" );
      check( ! mulle_concurrent_hashtable_hash_is_valid( &even_map, 3),
             "even mode: odd hash is invalid" );
      check( mulle_concurrent_hashtable_hash_is_valid( &even_map, 4),
             "even mode: even hash is valid" );

      check( mulle_concurrent_hashtable_insert( &even_map, 4, (void *) 0xaa) == 0,
             "even mode accepts even hash" );
      check( mulle_concurrent_hashtable_lookup( &even_map, 4) == (void *) 0xaa,
             "even mode lookup even hash" );
      check( mulle_concurrent_hashtable_remove( &even_map, 4, (void *) 0xaa) == 0,
             "even mode remove even hash" );

      mulle_concurrent_hashtable_done( &even_map);
   }

   mulle_concurrent_hashtable_done( &map);
   mulle_aba_unregister();
   mulle_aba_done();

   printf( "PASSED\n");
   return( 0);
}
