#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-testallocator/mulle-testallocator.h>
#include <assert.h>
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


// per-thread deterministic rng (xorshift64*)
static uint64_t   xorshift64star( uint64_t *x)
{
   *x ^= *x >> 12;
   *x ^= *x << 25;
   *x ^= *x >> 27;
   return( *x * 0x2545F4914F6CDD1DULL);
}


static void  sequential_test( void)
{
   struct mulle_concurrent_hashmap   map;

   mulle_concurrent_hashmap_init( &map, 0, NULL);

   // patch of a missing entry
   check( mulle_concurrent_hashmap_patch( &map, 1, (void *) 20, (void *) 10) == ENOENT,
          "patch missing key" );

   // insert then patch
   check( mulle_concurrent_hashmap_insert( &map, 1, (void *) 10) == 0,
          "insert for patch" );
   check( mulle_concurrent_hashmap_patch( &map, 1, (void *) 20, (void *) 10) == 0,
          "patch matching expect" );
   check( mulle_concurrent_hashmap_lookup( &map, 1) == (void *) 20,
          "patch applied" );

   // wrong expect leaves value untouched
   check( mulle_concurrent_hashmap_patch( &map, 1, (void *) 30, (void *) 10) == EEXIST,
          "patch wrong expect" );
   check( mulle_concurrent_hashmap_lookup( &map, 1) == (void *) 20,
          "patch wrong expect unchanged" );

   // register sees the patched value
   check( mulle_concurrent_hashmap_register( &map, 1, (void *) 20) == (void *) 20,
          "register sees patched value" );

   // patch with the same value as expect is a documented no-no, but with a
   // distinct value it must be atomic and keep working after repeated patches
   {
      intptr_t   i;

      for( i = 0; i < 100; i++)
      {
         check( mulle_concurrent_hashmap_patch( &map, 1, (void *)(i + 21), (void *)(i + 20)) == 0,
                "patch sequence" );
      }
      check( mulle_concurrent_hashmap_lookup( &map, 1) == (void *) 120,
             "patch sequence final" );
   }

   // reserved arguments are rejected (BUG-01 regression)
   check( mulle_concurrent_hashmap_patch( NULL, 1, (void *) 2, (void *) 1) == EINVAL,
          "patch NULL map" );
   check( mulle_concurrent_hashmap_patch( &map, MULLE_CONCURRENT_NO_HASH, (void *) 2, (void *) 1) == EINVAL,
          "patch no hash" );
   check( mulle_concurrent_hashmap_patch( &map, 1, MULLE_CONCURRENT_NO_POINTER, (void *) 1) == EINVAL,
          "patch NULL value" );
   check( mulle_concurrent_hashmap_patch( &map, 1, MULLE_CONCURRENT_INVALID_POINTER, (void *) 1) == EINVAL,
          "patch invalid value" );
   check( mulle_concurrent_hashmap_patch( &map, 1, (void *) 2, MULLE_CONCURRENT_NO_POINTER) == EINVAL,
          "patch NULL expect" );
   check( mulle_concurrent_hashmap_patch( &map, 1, (void *) 2, MULLE_CONCURRENT_INVALID_POINTER) == EINVAL,
          "patch invalid expect" );

   mulle_concurrent_hashmap_done( &map);
}


#define N_INCR_THREADS   8
#define N_INCR_ITERS     2000
#define INCR_KEY         0x1848
// the seed must not be a reserved value (NULL / (void *) -1 are rejected)
#define INCR_START       ((void *) 1)

static mulle_atomic_pointer_t   increment_stop;  // unused, keeps pattern

static void  *increment_worker( struct mulle_concurrent_hashmap *map)
{
   void     *value;
   int      rval;
   int      i;

   mulle_aba_register();

   for( i = 0; i < N_INCR_ITERS; i++)
   {
      for(;;)
      {
         value = mulle_concurrent_hashmap_lookup( map, INCR_KEY);
         rval  = mulle_concurrent_hashmap_patch( map, INCR_KEY, (void *)((uintptr_t) value + 1), value);
         if( rval == 0)
            break;
         // EEXIST: someone else patched in between, retry with fresh value
         assert( rval == EEXIST);
      }
   }

   mulle_aba_unregister();
   return( NULL);
}


static void  concurrent_increment_test( void)
{
   struct mulle_concurrent_hashmap   map;
   mulle_thread_t                    threads[ N_INCR_THREADS];
   unsigned int                      i;

   mulle_concurrent_hashmap_init( &map, 0, NULL);
   check( mulle_concurrent_hashmap_insert( &map, INCR_KEY, INCR_START) == 0,
          "increment seed insert" );

   for( i = 0; i < N_INCR_THREADS; i++)
   {
      if( mulle_thread_create( (void *) increment_worker, &map, &threads[ i]))
      {
         perror( "mulle_thread_create" );
         abort();
      }
   }

   for( i = 0; i < N_INCR_THREADS; i++)
      mulle_thread_join( threads[ i]);

   check( mulle_concurrent_hashmap_lookup( &map, INCR_KEY)
             == (void *)(uintptr_t)((uintptr_t) INCR_START + N_INCR_THREADS * N_INCR_ITERS),
          "concurrent increments" );
   check( mulle_concurrent_hashmap_count( &map) == 1,
          "concurrent increments count" );

   mulle_concurrent_hashmap_done( &map);
}


#define N_REG_THREADS   8
#define REG_KEY         0xBEEF

static void  *register_worker( struct mulle_concurrent_hashmap *map)
{
   uint64_t   rng;
   void       *result;
   int        i;

   mulle_aba_register();

   rng = (uint64_t)(uintptr_t) &rng ^ 0x123456789abcdef0ULL;
   if( ! rng)
      rng = 1;

   for( i = 0; i < 1000; i++)
   {
      result = mulle_concurrent_hashmap_register( map, REG_KEY, (void *) 0x1111);
      check( result == MULLE_CONCURRENT_NO_POINTER || result == (void *) 0x1111,
             "register contest result" );
      xorshift64star( &rng);
   }

   mulle_aba_unregister();
   return( NULL);
}


static void  concurrent_register_test( void)
{
   struct mulle_concurrent_hashmap   map;
   mulle_thread_t                    threads[ N_REG_THREADS];
   unsigned int                      i;

   mulle_concurrent_hashmap_init( &map, 0, NULL);

   for( i = 0; i < N_REG_THREADS; i++)
   {
      if( mulle_thread_create( (void *) register_worker, &map, &threads[ i]))
      {
         perror( "mulle_thread_create" );
         abort();
      }
   }

   for( i = 0; i < N_REG_THREADS; i++)
      mulle_thread_join( threads[ i]);

   check( mulle_concurrent_hashmap_lookup( &map, REG_KEY) == (void *) 0x1111,
          "register contest final value" );
   check( mulle_concurrent_hashmap_count( &map) == 1,
          "register contest count" );

   mulle_concurrent_hashmap_done( &map);
}


int   main( void)
{
   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   sequential_test();

   concurrent_increment_test();
   concurrent_register_test();

   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   printf( "PASSED\n" );
   return( 0);
}
