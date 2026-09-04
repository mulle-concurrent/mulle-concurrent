//
// Scenario A from dox/REMOVE-MISERY-HASHMAP.md, as a test.
//
// One thread owns a single key and repeatedly inserts and removes it, while
// migrator threads hammer unrelated keys to force constant migration. After a
// remove returns 0, nobody else touches the key, so a lookup MUST report it
// absent. If a lagging copier can install a stale value into the new
// generation, the lookup finds the removed value again.
//
// The original hashmap fails this by design: it installs the carried value
// before validating that it is still live. hashtable freezes first, so the
// carried value is provably final.
//
#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-testallocator/mulle-testallocator.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define N_MIGRATORS   3
#define OWNED_KEY     777777

static int   n_iters( void)
{
   // Under valgrind all threads are serialized (cooperative scheduling).
   // Reduce iterations so the test completes in reasonable time.
   // Auto-detects valgrind on Linux via /proc/self/maps.
   if( getenv( "MULLE_TEST_VALGRIND"))
      return( 50);
#ifdef __linux__
   {
      FILE   *f;
      char   line[ 256];

      f = fopen( "/proc/self/maps", "r");
      if( f)
      {
         while( fgets( line, sizeof( line), f))
            if( strstr( line, "vgpreload"))
            {
               fclose( f);
               return( 50);
            }
         fclose( f);
      }
   }
#endif
   return( 200000);
}

static struct mulle_concurrent_hashtable   g_map;
static volatile int                       g_stop;
static int                                g_failed;
static void                               *g_value = (void *) 0x1234;


static void  *migrator( void *unused)
{
   intptr_t   hash;
   intptr_t   i;

   MULLE_C_UNUSED( unused);

   mulle_aba_register();

   for( i = 0; ! g_stop; i++)
   {
      hash = (intptr_t)((i * 2654435761u) % 100000) + 1000000;
      mulle_concurrent_hashtable_insert( &g_map, hash, (void *)(uintptr_t)(hash * 2 + 1));
      mulle_concurrent_hashtable_remove( &g_map, hash, (void *)(uintptr_t)(hash * 2 + 1));

      // Yield periodically so that under cooperative schedulers (valgrind)
      // other threads get a chance to run.
      if( (i & 0x3F) == 0)
      {
         mulle_aba_checkin();
         mulle_thread_yield();
      }
   }

   mulle_aba_unregister();
   return( NULL);
}


static void  *owner( void *unused)
{
   int    i;
   int    rval;
   void   *found;

   MULLE_C_UNUSED( unused);

   mulle_aba_register();

   for( i = 0; i < n_iters() && ! g_failed; i++)
   {
      rval = mulle_concurrent_hashtable_insert( &g_map, OWNED_KEY, g_value);
      if( rval != 0)
      {
         printf( "FAILED: insert returned %d at %d\n", rval, i);
         g_failed = 1;
         break;
      }

      found = mulle_concurrent_hashtable_lookup( &g_map, OWNED_KEY);
      if( found != g_value)
      {
         printf( "FAILED: lookup lost the value at %d\n", i);
         g_failed = 1;
         break;
      }

      rval = mulle_concurrent_hashtable_remove( &g_map, OWNED_KEY, g_value);
      if( rval != 0)
      {
         printf( "FAILED: remove returned %d at %d\n", rval, i);
         g_failed = 1;
         break;
      }

      // nobody else touches OWNED_KEY, so this must be absent
      found = mulle_concurrent_hashtable_lookup( &g_map, OWNED_KEY);
      if( found != NULL)
      {
         printf( "FAILED: removed value resurrected at %d\n", i);
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
   unsigned int     i;

   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_hashtable_init( &g_map, 4, NULL);

   for( i = 0; i < N_MIGRATORS; i++)
   {
      if( mulle_thread_create( (void *) migrator, NULL, &threads[ i]))
      {
         perror( "create migrator" );
         return( 1);
      }
   }
   if( mulle_thread_create( (void *) owner, NULL, &threads[ N_MIGRATORS]))
   {
      perror( "create owner" );
      return( 1);
   }

   for( i = 0; i < N_MIGRATORS + 1; i++)
      mulle_thread_join( threads[ i]);

   mulle_concurrent_hashtable_done( &g_map);
   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   printf( g_failed ? "FAILED\n" : "PASSED\n");
   return( g_failed ? 1 : 0);
}
