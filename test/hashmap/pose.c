//
// _mulle_concurrent_hashmap_pose ("poseAs"): replace an existing value once,
// with a value that is unique to the caller and final.
// No validation wrapper — asserts only.
//
#define HAVE_MULLE_CONCURRENT_POSEAS_PATCH
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

   // pose of a missing entry
   check( _mulle_concurrent_hashmap_pose( &map, 1, (void *) 20, (void *) 10) == ENOENT,
          "pose missing key" );

   // insert then pose
   check( mulle_concurrent_hashmap_insert( &map, 1, (void *) 10) == 0,
          "insert for pose" );
   check( _mulle_concurrent_hashmap_pose( &map, 1, (void *) 20, (void *) 10) == 0,
          "pose matching expect" );
   check( mulle_concurrent_hashmap_lookup( &map, 1) == (void *) 20,
          "pose applied" );

   // wrong expect leaves value untouched
   check( _mulle_concurrent_hashmap_pose( &map, 1, (void *) 30, (void *) 10) == EEXIST,
          "pose wrong expect" );
   check( mulle_concurrent_hashmap_lookup( &map, 1) == (void *) 20,
          "pose wrong expect unchanged" );

   // posing again to the value already in place is idempotent
   check( _mulle_concurrent_hashmap_pose( &map, 1, (void *) 20, (void *) 10) == 0,
          "pose idempotent" );
   check( mulle_concurrent_hashmap_lookup( &map, 1) == (void *) 20,
          "pose idempotent unchanged" );

   // register sees the posed value
   check( mulle_concurrent_hashmap_register( &map, 1, (void *) 20) == (void *) 20,
          "register sees posed value" );

   // a removed entry can not be posed
   check( mulle_concurrent_hashmap_remove( &map, 1, (void *) 20) == 0,
          "remove posed" );
   check( _mulle_concurrent_hashmap_pose( &map, 1, (void *) 40, (void *) 20) == ENOENT,
          "pose removed key" );

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
   concurrent_register_test();

   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   printf( "PASSED\n" );
   return( 0);
}
