//
// Consistency test: run N threads briefly, stop them, check for duplicates,
// restart. Repeated N_ROUNDS times on the same set.
//
// "Consistent" means: enumerating the set yields each live pointer at most once.
//
#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-testallocator/mulle-testallocator.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define N_POINTERS   10
#define N_THREADS    8

static int   g_is_slow;

static unsigned int   n_rounds( void)
{
   // Under valgrind all threads are serialized (cooperative scheduling).
   // Reduce rounds so the test completes in reasonable time.
   // Auto-detects valgrind on Linux via /proc/self/maps.
   if( g_is_slow)
      return( 2);
   return( 100);
}

static unsigned int   n_threads( void)
{
   // fewer threads under valgrind: thread creation/join is expensive
   if( g_is_slow)
      return( 2);
   return( N_THREADS);
}

static void   detect_slow_environment( void)
{
   if( getenv( "MULLE_TEST_VALGRIND"))
   {
      g_is_slow = 1;
      return;
   }
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
               g_is_slow = 1;
               return;
            }
         fclose( f);
      }
   }
#endif
}

static void  *ptrs[ N_POINTERS];

static mulle_atomic_pointer_t   stop_flag;


static uint64_t   xorshift64star( uint64_t *x)
{
   *x ^= *x >> 12;
   *x ^= *x << 25;
   *x ^= *x >> 27;
   return *x * 0x2545F4914F6CDD1DULL;
}


static void  worker( struct mulle_concurrent_pointerset *set)
{
   uint64_t   rng;
   void       *ptr;

   mulle_aba_register();

   rng = (uint64_t)(uintptr_t) &rng ^ 0xdeadbeefcafe1234ULL;
   if( ! rng) rng = 1;

   while( ! _mulle_atomic_pointer_read( &stop_flag))
   {
      ptr = ptrs[ xorshift64star( &rng) % N_POINTERS ];
      switch( xorshift64star( &rng) % 2)
      {
         case 0: mulle_concurrent_pointerset_insert( set, ptr); break;
         case 1: mulle_concurrent_pointerset_remove( set, ptr); break;
      }

      // Yield periodically so that under cooperative schedulers (valgrind)
      // other threads get a chance to run.
      mulle_thread_yield();
   }

   mulle_aba_unregister();
}


static void  check_consistency( struct mulle_concurrent_pointerset *set)
{
   // enumerate and verify no pointer appears more than once
   int                                            seen[ N_POINTERS];
   struct mulle_concurrent_pointerset_enumerator  rover;
   void                                           *ptr;
   unsigned int                                   i;
   int                                            rval;

   memset( seen, 0, sizeof( seen));

retry:
   memset( seen, 0, sizeof( seen));
   rover = mulle_concurrent_pointerset_enumerate( set);
   while( (rval = mulle_concurrent_pointerset_enumerator_next( &rover, &ptr)) == 1)
   {
      for( i = 0; i < N_POINTERS; i++)
      {
         if( ptrs[ i] != ptr)
            continue;
         if( seen[ i])
         {
            fprintf( stderr, "DUPLICATE pointer %p found in set!\n", ptr);
            exit( 1);
         }
         seen[ i] = 1;
         break;
      }
      if( i == N_POINTERS)
      {
         fprintf( stderr, "UNKNOWN pointer %p found in set!\n", ptr);
         exit( 1);
      }
   }
   mulle_concurrent_pointerset_enumerator_done( &rover);

   if( rval == ECANCELED)
      goto retry;
}


int   main( void)
{
   struct mulle_concurrent_pointerset   set;
   mulle_thread_t                       threads[ N_THREADS];
   unsigned int                         i;
   unsigned int                         round;
   unsigned int                         actual_threads;

   detect_slow_environment();

   for( i = 0; i < N_POINTERS; i++)
      ptrs[ i] = (void *)(uintptr_t)(i + 1);

   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;

   mulle_aba_init( &mulle_testallocator);
   mulle_allocator_set_aba( &mulle_testallocator,
                            mulle_aba_get_global(),
                            (mulle_allocator_aba_t *) _mulle_aba_free_owned_pointer);
   mulle_aba_register();

   mulle_concurrent_pointerset_init( &set, 4, &mulle_testallocator);

   actual_threads = n_threads();

   for( round = 0; round < n_rounds(); round++)
   {
      _mulle_atomic_pointer_nonatomic_write( &stop_flag, NULL);

      for( i = 0; i < actual_threads; i++)
      {
         if( mulle_thread_create( (void *) worker, &set, &threads[ i]))
         {
            perror( "mulle_thread_create");
            abort();
         }
      }

      // let threads run briefly
      {
         unsigned int burst = g_is_slow ? 50 : 5000;
         mulle_thread_yield();
         for( i = 0; i < burst; i++)
            mulle_thread_yield();
      }

      // stop threads
      __mulle_atomic_pointer_cas( &stop_flag, (void *) 1, NULL);

      for( i = 0; i < actual_threads; i++)
         mulle_thread_join( threads[ i]);

      // now single-threaded: check consistency
      check_consistency( &set);
   }

   mulle_concurrent_pointerset_done( &set);

   mulle_aba_unregister();
   mulle_allocator_set_aba( &mulle_testallocator, NULL, NULL);
   mulle_aba_done();

   mulle_testallocator_reset();

   printf( "OK\n");
   return( 0);
}
