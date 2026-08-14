//
// A value that is present and never removed must never be reported absent,
// no matter how many migrations run underneath the lookup.
//
// This catches the hazard that the "consume after carry" rule introduces: a
// reader can observe the hash word as unfrozen, then a copier freezes the
// slot, carries the value into the newer generation and empties the source, so
// a naive value read afterwards sees an empty slot and reports the live entry
// as absent. The same shape hits remove, which would wrongly report ENOENT.
//
#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-testallocator/mulle-testallocator.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>


#define N_MIGRATORS   3
#define N_ITERS       200000
#define N_STABLE      16

static struct mulle_concurrent_hashtable   g_map;
static volatile int                       g_stop;
static int                                g_failed;


static void   *stable_value( intptr_t hash)
{
   return( (void *)(uintptr_t)((uintptr_t) hash * 16 + 3));
}


static void  *migrator( void *unused)
{
   intptr_t   hash;
   intptr_t   i;

   MULLE_C_UNUSED( unused);

   mulle_aba_register();

   for( i = 0; ! g_stop; i++)
   {
      hash = (intptr_t)((i * 2654435761u) % 50000) + 1000000;
      mulle_concurrent_hashtable_insert( &g_map, hash, (void *)(uintptr_t)(hash * 2 + 1));
      mulle_concurrent_hashtable_remove( &g_map, hash, (void *)(uintptr_t)(hash * 2 + 1));

      if( (i & 0xFF) == 0)
         mulle_aba_checkin();
   }

   mulle_aba_unregister();
   return( NULL);
}


static void  *reader( void *unused)
{
   intptr_t   hash;
   int        i;
   int        k;
   void       *found;

   MULLE_C_UNUSED( unused);

   mulle_aba_register();

   for( i = 0; i < N_ITERS && ! g_failed; i++)
   {
      k    = i % N_STABLE;
      hash = k + 1;

      // these were inserted before the threads started and are never removed
      found = mulle_concurrent_hashtable_lookup( &g_map, hash);
      if( found != stable_value( hash))
      {
         printf( "FAILED: stable key reported absent or wrong at %d\n", i);
         g_failed = 1;
         break;
      }

      if( (i & 0xFF) == 0)
         mulle_aba_checkin();
   }

   g_stop = 1;

   mulle_aba_unregister();
   return( NULL);
}


int   main( void)
{
   mulle_thread_t   threads[ N_MIGRATORS + 1];
   intptr_t         hash;
   unsigned int     i;

   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_hashtable_init( &g_map, 4, NULL);

   for( i = 0; i < N_STABLE; i++)
   {
      hash = (intptr_t) i + 1;
      if( mulle_concurrent_hashtable_insert( &g_map, hash, stable_value( hash)))
      {
         printf( "FAILED: seeding\n");
         return( 1);
      }
   }

   for( i = 0; i < N_MIGRATORS; i++)
      if( mulle_thread_create( (void *) migrator, NULL, &threads[ i]))
      {
         perror( "create migrator" );
         return( 1);
      }

   if( mulle_thread_create( (void *) reader, NULL, &threads[ N_MIGRATORS]))
   {
      perror( "create reader" );
      return( 1);
   }

   for( i = 0; i < N_MIGRATORS + 1; i++)
      mulle_thread_join( threads[ i]);

   // and they must all still be there afterwards
   for( i = 0; i < N_STABLE; i++)
   {
      hash = (intptr_t) i + 1;
      if( mulle_concurrent_hashtable_lookup( &g_map, hash) != stable_value( hash))
      {
         printf( "FAILED: stable key lost by the end\n");
         g_failed = 1;
      }
   }

   mulle_concurrent_hashtable_done( &g_map);
   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   printf( g_failed ? "FAILED\n" : "PASSED\n");
   return( g_failed ? 1 : 0);
}
