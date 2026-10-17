//
// S6 from dox/HASHTABLE.md: remove racing the freeze.
//
// remove_race.c relies on threshold-driven migrations, which become rare once
// the table is large, so the window almost never coincides with a removal. Here
// a probe thread calls _mulle_concurrent_hashtable_migrate_same_size() in a tight
// loop instead, so generations change constantly at bounded memory cost.
//
// The owner thread owns its key exclusively. Every insert must succeed and every
// remove of a value it just inserted must return 0. If remove reports ENOENT
// because a copier carried and consumed the value first, the removal was lost.
//
#include <mulle-concurrent/mulle-concurrent.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define OWNED_KEY    4242
#define OTHER_KEYS   64

static int   n_iters( void)
{
   char   *s;

   // soak runs: MULLE_TEST_ITERATIONS=100000000 ./remove_migrate_race.exe
   s = getenv( "MULLE_TEST_ITERATIONS");
   if( s)
      return( atoi( s));

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
   return( 100000);
}

static struct mulle_concurrent_hashtable   g_map;
static volatile int                       g_stop;
static int                                g_failed;
static void                               *g_value = (void *) 0xBEEF;


static void  *migrator( void *unused)
{
   intptr_t   i;

   MULLE_C_UNUSED( unused);

   mulle_aba_register();

   for( i = 0; ! g_stop; i++)
   {
      _mulle_concurrent_hashtable_migrate_same_size( &g_map);

      // every migration abafrees a storage, so check in each time, or the
      // backlog grows until the other thread checks in too
      mulle_aba_checkin();

      // Yield periodically so that under cooperative schedulers (valgrind)
      // other threads get a chance to run.
      if( (i & 0x3F) == 0)
         mulle_thread_yield();
   }

   mulle_aba_unregister();
   return( NULL);
}


static void  *owner( void *unused)
{
   int    i, n;
   int    rval;
   void   *found;

   MULLE_C_UNUSED( unused);

   mulle_aba_register();

   for( i = 0, n = n_iters(); i < n && ! g_failed; i++)
   {
      rval = mulle_concurrent_hashtable_insert( &g_map, OWNED_KEY, g_value);
      if( rval != 0)
      {
         printf( "FAILED: insert returned %d at %d\n", rval, i);
         g_failed = 1;
         break;
      }

      // we own this key, so this must find it
      found = mulle_concurrent_hashtable_lookup( &g_map, OWNED_KEY);
      if( found != g_value)
      {
         printf( "FAILED: lookup lost a live value at %d\n", i);
         g_failed = 1;
         break;
      }

      // and this must remove it, not report it missing
      rval = mulle_concurrent_hashtable_remove( &g_map, OWNED_KEY, g_value);
      if( rval != 0)
      {
         printf( "FAILED: remove returned %d at %d (lost removal)\n", rval, i);
         g_failed = 1;
         break;
      }

      found = mulle_concurrent_hashtable_lookup( &g_map, OWNED_KEY);
      if( found != NULL)
      {
         printf( "FAILED: removed value resurrected at %d\n", i);
         g_failed = 1;
         break;
      }

      if( (i & 0xF) == 0)
         mulle_aba_checkin();
   }

   g_stop = 1;

   mulle_aba_unregister();
   return( NULL);
}


int   main( void)
{
   mulle_thread_t   threads[ 2];
   intptr_t         i;

   mulle_aba_init( NULL);
   mulle_aba_register();

   // integer payloads, so positive values (even values are the default)
   mulle_concurrent_hashtable_init_with_mode( &g_map, 256,
                                              MULLE_CONCURRENT_HASHTABLE_POSITIVE_VALUE,
                                              NULL);

   // some unrelated live entries, so copy has real work to do on every pass
   for( i = 0; i < OTHER_KEYS; i++)
      mulle_concurrent_hashtable_insert( &g_map, i + 1000, (void *)(uintptr_t)(i + 1));

   if( mulle_thread_create( (void *) migrator, NULL, &threads[ 0]))
   {
      perror( "create migrator" );
      return( 1);
   }
   if( mulle_thread_create( (void *) owner, NULL, &threads[ 1]))
   {
      perror( "create owner" );
      return( 1);
   }

   //
   // A registered thread that never checks in blocks all reclamation, and
   // every same-size migration abafrees a storage. Waiting registered in
   // join would pile up gigabytes, so main steps out while it waits.
   //
   mulle_aba_unregister();

   mulle_thread_join( threads[ 0]);
   mulle_thread_join( threads[ 1]);

   mulle_aba_register();

   // the unrelated entries must have survived every migration
   for( i = 0; i < OTHER_KEYS; i++)
      if( mulle_concurrent_hashtable_lookup( &g_map, i + 1000) != (void *)(uintptr_t)(i + 1))
      {
         printf( "FAILED: bystander key %ld lost\n", (long) i);
         g_failed = 1;
         break;
      }

   mulle_concurrent_hashtable_done( &g_map);
   mulle_aba_unregister();
   mulle_aba_done();

   printf( g_failed ? "FAILED\n" : "PASSED\n");
   return( g_failed ? 1 : 0);
}
