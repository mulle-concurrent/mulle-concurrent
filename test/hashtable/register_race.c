//
// S11 from dox/HASHTABLE.md: register's *p_old must report the carried value.
//
// Invariant: the owner inserts V1 once and never removes it. The registrant
// calls register(KEY, V2) in a loop. Since V1 is always live, register must
// always report *p_old == V1 (or V2 if it sees its own previous attempt).
// It must NEVER report *p_old == NULL ("I freshly inserted") because that
// would mean it believes the key was empty when it wasn't.
//
// A migrator forces same-size migrations so that the carry-and-consume of V1
// into a new generation creates the window where the registrant's CAS lands
// on a consumed slot. Without the post-check fix, the registrant reports
// *p_old == NULL — a linearizability violation.
//
// Compile with -DMULLE_CONCURRENT_HASHTABLE_RACE_YIELD to widen the window.
//
#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-testallocator/mulle-testallocator.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define KEY         7777

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
static volatile int                       g_ready;   // owner has inserted
static volatile int                       g_stop;
static int                                g_failed;

static void   *V1 = (void *) 0xAAAA;
static void   *V2 = (void *) 0xBBBB;


static void  *migrator( void *unused)
{
   intptr_t   i;

   MULLE_C_UNUSED( unused);

   mulle_aba_register();

   while( ! g_ready)
      mulle_thread_yield();

   for( i = 0; ! g_stop; i++)
   {
      _mulle_concurrent_hashtable_migrate_same_size( &g_map);

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


static void  *registrant( void *unused)
{
   int    i;
   int    rval;
   void   *old;

   MULLE_C_UNUSED( unused);

   mulle_aba_register();

   while( ! g_ready)
      mulle_thread_yield();

   for( i = 0; i < n_iters() && ! g_failed; i++)
   {
      old  = (void *) 0xDEAD;
      rval = mulle_concurrent_hashtable_register( &g_map, KEY, V2, &old);
      if( rval != 0)
      {
         printf( "FAILED: register returned %d at %d\n", rval, i);
         g_failed = 1;
         break;
      }

      if( old == NULL)
      {
         //
         // BUG: V1 was never removed, so the key was never empty.
         // register has no right to claim it freshly inserted.
         //
         printf( "FAILED: register said inserted (p_old==NULL) but V1 is"
                 " permanently live, at iteration %d\n", i);
         g_failed = 1;
         break;
      }
      else if( old == V1)
      {
         // correct: V1 is live, we lost
      }
      else if( old == V2)
      {
         // our own value from a previous iteration is still there —
         // remove it so we can retry cleanly
         mulle_concurrent_hashtable_remove( &g_map, KEY, V2);
      }
      else
      {
         printf( "FAILED: register *p_old is unexpected at %d\n", i);
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
   mulle_thread_t   threads[ 2];
   int              rval;

   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_hashtable_init( &g_map, 64, NULL);

   // insert V1 before anyone else starts — it will never be removed
   rval = mulle_concurrent_hashtable_insert( &g_map, KEY, V1);
   if( rval != 0)
   {
      printf( "FAILED: initial insert returned %d\n", rval);
      return( 1);
   }

   if( mulle_thread_create( (void *) migrator, NULL, &threads[ 0])
    || mulle_thread_create( (void *) registrant, NULL, &threads[ 1]))
   {
      perror( "thread_create" );
      return( 1);
   }

   // signal both threads to go
   g_ready = 1;

   mulle_thread_join( threads[ 0]);
   mulle_thread_join( threads[ 1]);

   // V1 must still be retrievable (or V2 if registrant won the last round)
   {
      void *found = mulle_concurrent_hashtable_lookup( &g_map, KEY);
      if( found != V1 && found != V2)
      {
         printf( "FAILED: final lookup lost both values\n");
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
