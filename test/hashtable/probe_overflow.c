//
// Probe-bound overshoot test.
//
// The probe loop terminates because migration starts at 50% claimed capacity,
// so a virgin slot always remains. But the threshold check (n < max) and the
// claim's increment are not atomic: multiple threads can pass the check and
// then all claim slots, overshooting the threshold and filling the table.
//
// This test creates the condition: a tiny table (min size 4, max=2) with many
// threads each inserting a distinct key. If the design is correct, migration
// rescues the situation — every insert eventually succeeds and every lookup
// finds its value. The assert(index != sentinel) catches the failure mode in
// debug builds if the self-healing migration doesn't kick in.
//
// This is a stress test, not a reproducer of a known bug: it exercises the
// boundary condition that makes the wait-free bound thread-count-dependent.
//
#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-testallocator/mulle-testallocator.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define N_THREADS    8
#define INIT_SIZE    4     // minimum table: 4 slots, migration at 2 claimed

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
   return( 50000);
}

static struct mulle_concurrent_hashtable   g_map;
static volatile int                       g_go;
static int                                g_failed;


static void  *inserter( void *arg)
{
   intptr_t   base;
   intptr_t   key;
   int        i;
   int        rval;
   void       *found;

   mulle_aba_register();

   base = (intptr_t)(uintptr_t) arg;

   // wait for all threads to be ready
   while( ! g_go)
      mulle_thread_yield();

   for( i = 0; i < n_iters() && ! g_failed; i++)
   {
      // use a different key each iteration so the table grows under pressure
      key = base + (intptr_t) i * N_THREADS;

      rval = mulle_concurrent_hashtable_insert( &g_map, key, (void *)(uintptr_t) key);
      if( rval != 0 && rval != EEXIST)
      {
         printf( "FAILED: insert returned %d for key %ld at iter %d\n",
                 rval, (long) key, i);
         g_failed = 1;
         break;
      }

      found = mulle_concurrent_hashtable_lookup( &g_map, key);
      if( found != (void *)(uintptr_t) key)
      {
         printf( "FAILED: lookup lost key %ld at iter %d\n", (long) key, i);
         g_failed = 1;
         break;
      }

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


int   main( void)
{
   mulle_thread_t   threads[ N_THREADS];
   int              i;

   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_hashtable_init( &g_map, INIT_SIZE, NULL);

   for( i = 0; i < N_THREADS; i++)
   {
      if( mulle_thread_create( (void *) inserter, (void *)(uintptr_t)(i + 1), &threads[ i]))
      {
         perror( "thread_create" );
         return( 1);
      }
   }

   // release the hounds
   g_go = 1;

   for( i = 0; i < N_THREADS; i++)
      mulle_thread_join( threads[ i]);

   // verify final count
   {
      unsigned int   count;

      count = mulle_concurrent_hashtable_count( &g_map);
      if( count != (unsigned int) (N_THREADS * n_iters()))
      {
         printf( "FAILED: count %u != expected %u\n", count, (unsigned int) (N_THREADS * n_iters()));
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
