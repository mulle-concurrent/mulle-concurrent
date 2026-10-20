//
// Owner-based completeness test for the pointerarray.
//
// Every thread adds a disjoint range of unique values. Once the threads have
// joined, the array must be dense and contain exactly the union of those
// values, each once. This is the ground truth that test/array/pointerarray.c
// lacks: it only checks that random gets return non-NULL.
//
#include <mulle-concurrent/mulle-concurrent.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
# include <windows.h>
#else
# include <unistd.h>
#endif


#define MAX_THREADS   8
#define VALUES        4096

static struct mulle_concurrent_pointerarray   g_array;
static unsigned int                           g_failed;


static void   *owned_value( unsigned int thread, unsigned int i)
{
   return( (void *) (uintptr_t) (((thread + 1) << 20) | (i + 1)));
}


static mulle_thread_rval_t   adder( void *arg)
{
   unsigned int   thread;
   unsigned int   i;

   thread = (unsigned int) (uintptr_t) arg;

   mulle_aba_register();

   for( i = 0; i < VALUES; i++)
   {
      mulle_concurrent_pointerarray_add( &g_array, owned_value( thread, i));

      // Yield periodically so that under cooperative schedulers (valgrind)
      // other threads get a chance to run.
      if( (i & 0x3F) == 0)
         mulle_thread_yield();
   }

   mulle_aba_unregister();
   return( (mulle_thread_rval_t) 0);
}


static unsigned int   n_threads( void)
{
   unsigned int   cpus;
#ifdef _WIN32
   SYSTEM_INFO    info;

   GetSystemInfo( &info);
   cpus = (unsigned int) info.dwNumberOfProcessors;
#else
   long   n;

   n = sysconf( _SC_NPROCESSORS_ONLN);
   cpus = n < 1 ? 1 : (unsigned int) n;
#endif
   if( getenv( "MULLE_TEST_VALGRIND"))
      return( 2);
   if( cpus > MAX_THREADS)
      cpus = MAX_THREADS;
   if( cpus < 2)
      cpus = 2;
   return( cpus);
}


int   main( void)
{
   struct mulle_concurrent_pointerarrayenumerator   rover;
   mulle_thread_t                                   threads[ MAX_THREADS];
   unsigned char                                    seen[ MAX_THREADS][ VALUES];
   uintptr_t                                        value;
   unsigned int                                     thread;
   unsigned int                                     index;
   unsigned int                                     i;
   unsigned int                                     count;
   unsigned int                                     n;
   unsigned int                                     actual_threads;
   void                                             *ptr;
   int                                              rval;

   actual_threads = n_threads();

   memset( seen, 0, sizeof( seen));

   mulle_aba_init( &mulle_allocator_default);
   mulle_allocator_set_aba( &mulle_allocator_default,
                            mulle_aba_get_global(),
                            _mulle_aba_free_owned_pointer);
   mulle_aba_register();

   mulle_concurrent_pointerarray_init( &g_array, 0, &mulle_allocator_default);

   for( i = 0; i < actual_threads; i++)
   {
      if( mulle_thread_create( adder, (void *) (uintptr_t) i, &threads[ i]))
      {
         perror( "mulle_thread_create");
         abort();
      }
   }

   for( i = 0; i < actual_threads; i++)
      mulle_thread_join( threads[ i]);

   count = mulle_concurrent_pointerarray_get_count( &g_array);
   if( count != actual_threads * VALUES)
   {
      fprintf( stderr, "count is %u, want %u\n", count,
                       actual_threads * VALUES);
      g_failed = 1;
   }

   // the array must be dense, with no holes below the count
   for( i = 0; i < count; i++)
      if( ! mulle_concurrent_pointerarray_get( &g_array, i))
      {
         fprintf( stderr, "NULL hole at index %u\n", i);
         g_failed = 1;
         break;
      }

   rover = mulle_concurrent_pointerarray_enumerate( &g_array);
   n     = 0;
   while( (ptr = mulle_concurrent_pointerarrayenumerator_next( &rover)))
   {
      ++n;
      value  = (uintptr_t) ptr;
      thread = (unsigned int) ((value >> 20) - 1);
      index  = (unsigned int) ((value & 0xFFFFF) - 1);

      if( thread >= actual_threads || index >= VALUES)
      {
         fprintf( stderr, "unknown value %p\n", ptr);
         g_failed = 1;
         continue;
      }
      if( seen[ thread][ index])
      {
         fprintf( stderr, "duplicate value %p\n", ptr);
         g_failed = 1;
         continue;
      }
      seen[ thread][ index] = 1;
   }
   mulle_concurrent_pointerarrayenumerator_done( &rover);

   if( n != count)
   {
      fprintf( stderr, "enumerated %u values, count says %u\n", n, count);
      g_failed = 1;
   }

   for( thread = 0; thread < actual_threads; thread++)
      for( index = 0; index < VALUES; index++)
         if( ! seen[ thread][ index])
         {
            fprintf( stderr, "missing value (%u, %u)\n", thread, index);
            g_failed = 1;
            break;
         }

   mulle_concurrent_pointerarray_done( &g_array);

   mulle_aba_unregister();
   mulle_allocator_set_aba( &mulle_allocator_default, NULL, NULL);
   mulle_aba_done();

   printf( g_failed ? "FAILED\n" : "OK\n");
   return( g_failed ? 1 : 0);
}
