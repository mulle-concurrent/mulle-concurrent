//
// The table size has to be a power of two.
//
// The probe index is `index & mask` with `mask = size - 1`, so the walk visits
// every slot only when the size is a power of two. The storage allocator
// asserts this once, to itself, and neither header says so:
//
//    if( n < 4)
//       n = 4;
//    assert( (~(n - 1) & n) == n);      // the only enforcement there is
//    ...
//    p->mask = n - 1;                   // and this is why it matters
//
// A size like `keys * 2 + 16` = 2064 is a plausible caller computation. Its
// mask 2063 = 0x80f keeps bits 0-3 and bit 11, so the probe reaches 32 of the
// 2064 slots. Fill those and the walk never meets a virgin slot again.
//
// What happens next depends on the call and on the build:
//
//   Debug    the allocator's assert aborts during init, in library code, which
//            reads as an internal invariant rather than a caller mistake.
//   Release  the asserts are gone (NDEBUG). mulle_concurrent_hashtable_insert
//            returns EBUSY when its probe runs out, the caller migrates and the
//            table doubles without ever repairing its shape, so its *inserts*
//            survive. mulle_concurrent_hashtable_lookup and every hashmap probe
//            loop (lookup, register, insert, put) have only an assert and spin.
//
// This test states the property a caller needs: a non-power-of-two size must
// produce a working table, not an abort and not a hang. The workload runs
// under a wall-clock alarm, so the Release hang fails the test instead of
// hanging the test run.
//
#include <mulle-concurrent/mulle-concurrent.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef _WIN32
#include <unistd.h>
#endif


#define N_ENTRIES   64
#define TIMEOUT     10


static void   *value_for( intptr_t hash)
{
   return( (void *) (intptr_t) ((uintptr_t) hash * 16 + 1));
}


static int   is_pow2( size_t x)
{
   return( x && (x & (x - 1)) == 0);
}


static void   check( int condition, char *name)
{
   if( condition)
      return;

   printf( "FAILED: %s\n", name);
   exit( 1);
}


//
// Insert N distinct hashes, then look each one up. An absent hash is looked up
// last: that is the call with no escape once the probe runs out.
//
static void   probe_hashtable( size_t size)
{
   struct mulle_concurrent_hashtable   map;
   size_t                              actual;
   intptr_t                            hash;
   intptr_t                            i;
   void                                *found;
   int                                 rval;

   mulle_concurrent_hashtable_init( &map, size, NULL);

   actual = mulle_concurrent_hashtable_get_size( &map);
   check( is_pow2( actual), "hashtable size is a power of two");
   check( actual >= size, "hashtable size >= requested");

   for( i = 1; i <= N_ENTRIES; i++)
   {
      hash = (intptr_t) i;
      rval = mulle_concurrent_hashtable_insert( &map, hash, value_for( hash));
      check( rval == 0, "hashtable insert");
   }

   for( i = 1; i <= N_ENTRIES; i++)
   {
      hash  = (intptr_t) i;
      found = mulle_concurrent_hashtable_lookup( &map, hash);
      check( found == value_for( hash), "hashtable lookup");
   }

   check( mulle_concurrent_hashtable_lookup( &map, 0x100000) == NULL,
          "hashtable lookup absent");

   mulle_concurrent_hashtable_done( &map);
}


static void   probe_hashmap( unsigned int size)
{
   struct mulle_concurrent_hashmap   map;
   unsigned int                      actual;
   intptr_t                          hash;
   intptr_t                          i;
   void                              *found;
   int                               rval;

   mulle_concurrent_hashmap_init( &map, size, NULL);

   actual = mulle_concurrent_hashmap_get_size( &map);
   check( is_pow2( actual), "hashmap size is a power of two");
   check( (size_t) actual >= (size_t) size, "hashmap size >= requested");

   for( i = 1; i <= N_ENTRIES; i++)
   {
      hash = (intptr_t) i;
      rval = mulle_concurrent_hashmap_insert( &map, hash, value_for( hash));
      check( rval == 0, "hashmap insert");
   }

   for( i = 1; i <= N_ENTRIES; i++)
   {
      hash  = (intptr_t) i;
      found = mulle_concurrent_hashmap_lookup( &map, hash);
      check( found == value_for( hash), "hashmap lookup");
   }

   mulle_concurrent_hashmap_done( &map);
}


int   main( void)
{
   mulle_aba_init( NULL);
   mulle_aba_register();

#ifndef _WIN32
   // a size that is not a power of two must not turn a probe into a spin
   alarm( TIMEOUT);
#endif

   // power of two: the documented, well formed case
   probe_hashtable( 2048);
   probe_hashmap( 2048);

   // below the minimum, clamped rather than rejected or rounded to nothing
   probe_hashtable( 3);
   probe_hashmap( 3);
   probe_hashtable( 5);
   probe_hashmap( 5);

   // 2064 = keys * 2 + 16 is the plausible call the mask turns into a trap
   probe_hashtable( 2064);
   probe_hashmap( 2064);

#ifndef _WIN32
   alarm( 0);
#endif

   mulle_aba_unregister();
   mulle_aba_done();

   printf( "PASSED\n");
   return( 0);
}
