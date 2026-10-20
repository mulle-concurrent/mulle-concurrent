//
// Owner-based stress test for BUGREPORT-POINTERSET-REMOVE.md.
//
// Every thread owns a disjoint range of pointers, so it knows exactly which of
// them must be in the set after each of its own operations, no matter what the
// other threads do. That is the difference to stress.c/consistency.c, which
// share a handful of pointers and therefore have no ground truth: a lost or
// resurrected pointer there looks just like a concurrent insert.
//
// The set is insert-only, so the test exercises insert and member only.
// Run with MULLE_POINTERSET_SECONDS=<n> to change the time budget.
//
#include <mulle-concurrent/mulle-concurrent.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
# include <windows.h>
#else
# include <unistd.h>
#endif


#define N_POINTERS   16           // per thread
#define MAX_THREADS  16

static unsigned int               g_n_threads;
static struct mulle_concurrent_pointerset   g_set;
static mulle_atomic_pointer_t     g_stop;
static mulle_atomic_pointer_t     g_failed;
static time_t                     g_deadline;
static unsigned char              g_owned[ MAX_THREADS][ N_POINTERS];


static unsigned int   number_of_cpus( void)
{
#ifdef _WIN32
   SYSTEM_INFO   info;

   GetSystemInfo( &info);
   return( (unsigned int) info.dwNumberOfProcessors);
#else
   long   n;

   n = sysconf( _SC_NPROCESSORS_ONLN);
   if( n < 1)
      return( 1);
   return( (unsigned int) n);
#endif
}


static unsigned int   is_slow( void)
{
   if( getenv( "MULLE_TEST_VALGRIND"))
      return( 1);
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
               return( 1);
            }
         fclose( f);
      }
   }
#endif
   return( 0);
}


static unsigned int   n_threads( void)
{
   unsigned int   cpus;

   if( is_slow())
      return( 2);

   cpus = number_of_cpus();
   if( cpus > MAX_THREADS)
      cpus = MAX_THREADS;
   if( cpus < 2)
      cpus = 2;
   return( cpus);
}


static unsigned int   n_seconds( void)
{
   char   *s;

   s = getenv( "MULLE_POINTERSET_SECONDS");
   if( s && atoi( s) > 0)
      return( (unsigned int) atoi( s));
   return( is_slow() ? 1 : 2);
}


// distinct, even, non-zero, well below the sentinel values
static void   *owned_pointer( unsigned int thread, unsigned int index)
{
   uintptr_t   value;

   value = ((uintptr_t) (thread + 1) << 16) | ((uintptr_t) (index + 1) << 2);
   return( (void *) value);
}


static uint64_t   xorshift64star( uint64_t *x)
{
   *x ^= *x >> 12;
   *x ^= *x << 25;
   *x ^= *x >> 27;
   return( *x * 0x2545F4914F6CDD1DULL);
}


static void   fail( unsigned int thread, unsigned int index,
                    char *what, int rval, int expected)
{
   if( ! _mulle_atomic_pointer_cas( &g_failed, (void *) 1, NULL))
      fprintf( stderr, "thread %u index %u: %s (got %d, want %d)\n",
                       thread, index, what, rval, expected);
}


static int   verify_all( unsigned int thread, unsigned char *owned)
{
   unsigned int   i;
   void           *ptr;
   int            found;

   for( i = 0; i < N_POINTERS; i++)
   {
      ptr   = owned_pointer( thread, i);
      found = mulle_concurrent_pointerset_member( &g_set, ptr);
      if( found != owned[ i])
      {
         fail( thread, i,
               owned[ i] ? "lost a live pointer" : "pointer present but not owned",
               found, owned[ i]);
         return( 0);
      }
   }
   return( 1);
}


static mulle_thread_rval_t   worker( void *arg)
{
   unsigned char   owned[ N_POINTERS];
   uint64_t        rng;
   unsigned int    thread;
   unsigned int    i;
   unsigned int    n;
   void            *ptr;
   int             rval;

   thread = (unsigned int) (uintptr_t) arg;

   memset( owned, 0, sizeof( owned));

   mulle_aba_register();

   rng = (uint64_t) thread * 0x9E3779B97F4A7C15ULL + 0xdeadbeefcafe1234ULL;
   if( ! rng)
      rng = 1;

   for( n = 0; ! _mulle_atomic_pointer_read( &g_stop); n++)
   {
      if( (n & 0x3F) == 0 && time( NULL) >= g_deadline)
      {
         _mulle_atomic_pointer_cas( &g_stop, (void *) 1, NULL);
         break;
      }

      i   = (unsigned int) (xorshift64star( &rng) % N_POINTERS);
      ptr = owned_pointer( thread, i);

      if( ! owned[ i])
      {
         rval = mulle_concurrent_pointerset_insert( &g_set, ptr);
         if( rval)
         {
            fail( thread, i, "insert failed", rval, 0);
            break;
         }
         owned[ i] = 1;
         g_owned[ thread][ i] = 1;
      }

      // the pointer we just inserted must be found, whatever else is going on
      rval = mulle_concurrent_pointerset_member( &g_set, ptr);
      if( rval != owned[ i])
      {
         fail( thread, i,
               owned[ i] ? "lost a live pointer" : "pointer present but not owned",
               rval, owned[ i]);
         break;
      }

      // periodically check everything, to catch a loss that only shows up
      // after a later migration
      if( (n & 0x3F) == 0)
      {
         if( ! verify_all( thread, owned))
            break;
         mulle_aba_checkin();

         // Yield periodically so that under cooperative schedulers (valgrind)
         // other threads get a chance to run.
         mulle_thread_yield();
      }
   }

   mulle_aba_unregister();
   return( (mulle_thread_rval_t) 0);
}


static int   run_phase( void)
{
   mulle_thread_t                 threads[ MAX_THREADS];
   unsigned char                  seen[ MAX_THREADS][ N_POINTERS];
   struct mulle_concurrent_pointerset_enumerator   rover;
   uintptr_t                      value;
   unsigned int                   i, j;
   unsigned int                   expected_count;
   unsigned int                   seen_count;
   unsigned int                   thread;
   unsigned int                   index;
   void                           *ptr;
   int                            rval;
   int                            failed;

   memset( g_owned, 0, sizeof( g_owned));
   memset( seen, 0, sizeof( seen));

   mulle_concurrent_pointerset_init( &g_set, 4, &mulle_allocator_default);

   _mulle_atomic_pointer_nonatomic_write( &g_stop, NULL);
   _mulle_atomic_pointer_nonatomic_write( &g_failed, NULL);
   g_deadline = time( NULL) + n_seconds();

   for( i = 0; i < g_n_threads; i++)
   {
      if( mulle_thread_create( worker, (void *) (uintptr_t) i, &threads[ i]))
      {
         perror( "mulle_thread_create");
         abort();
      }
   }

   for( i = 0; i < g_n_threads; i++)
      mulle_thread_join( threads[ i]);

   failed = (_mulle_atomic_pointer_read( &g_failed) != NULL);

   //
   // single-threaded now: the set must contain exactly the union of what the
   // (now stopped) threads own, each pointer exactly once
   //
   expected_count = 0;
   for( i = 0; i < g_n_threads; i++)
      for( j = 0; j < N_POINTERS; j++)
         expected_count += g_owned[ i][ j];

   seen_count = 0;
retry:
   rover = mulle_concurrent_pointerset_enumerate( &g_set);
   while( (rval = mulle_concurrent_pointerset_enumerator_next( &rover, &ptr)) == 1)
   {
      value  = (uintptr_t) ptr;
      thread = (unsigned int) ((value >> 16) - 1);
      index  = (unsigned int) (((value >> 2) & 0x3FFF) - 1);

      if( thread >= g_n_threads || index >= N_POINTERS)
      {
         fprintf( stderr, "unknown pointer %p in set\n", ptr);
         failed = 1;
         continue;
      }
      if( seen[ thread][ index])
      {
         fprintf( stderr, "duplicate pointer %p in set\n", ptr);
         failed = 1;
         continue;
      }
      seen[ thread][ index] = 1;
      ++seen_count;
   }
   mulle_concurrent_pointerset_enumerator_done( &rover);
   if( rval == ECANCELED)
   {
      memset( seen, 0, sizeof( seen));
      seen_count = 0;
      goto retry;
   }

   if( seen_count != expected_count)
   {
      fprintf( stderr, "insert-only: enumerated %u pointers, owners own %u\n",
                       seen_count, expected_count);
      failed = 1;
   }

   for( i = 0; i < g_n_threads; i++)
      for( j = 0; j < N_POINTERS; j++)
      {
         rval = mulle_concurrent_pointerset_member( &g_set,
                                                    owned_pointer( i, j));
         if( rval != g_owned[ i][ j])
         {
            fprintf( stderr, "insert-only: final member(%u,%u)=%d, owner says %d\n",
                             i, j, rval, g_owned[ i][ j]);
            failed = 1;
         }
      }

   mulle_concurrent_pointerset_done( &g_set);

   printf( "insert-only: %s\n", failed ? "FAILED" : "ok");
   return( failed);
}


int   main( void)
{
   int   failed;

   g_n_threads = n_threads();

   mulle_aba_init( &mulle_allocator_default);
   mulle_allocator_set_aba( &mulle_allocator_default,
                            mulle_aba_get_global(),
                            _mulle_aba_free_owned_pointer);
   mulle_aba_register();

   failed = run_phase();

   mulle_aba_unregister();
   mulle_allocator_set_aba( &mulle_allocator_default, NULL, NULL);
   mulle_aba_done();

   printf( failed ? "FAILED\n" : "OK\n");
   return( failed ? 1 : 0);
}
