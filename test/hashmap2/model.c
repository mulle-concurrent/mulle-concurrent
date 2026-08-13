//
// Strict model check. Each thread owns a disjoint key range and runs a seeded
// operation sequence while the shared storage migrates underneath. Because the
// ranges are disjoint, the per-thread sequential specification is exact, so
// every result is checkable with no leniency whatsoever.
//
#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-testallocator/mulle-testallocator.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define N_THREADS         4
#define KEYS_PER_THREAD   256
#define OPS_PER_THREAD    20000
#define N_ROUNDS          3
#define SEED              0x1234ABCD

struct worker_context
{
   struct mulle_concurrent_hashmap2   *map;
   unsigned int                       thread;
};

static void   *model[ N_THREADS][ KEYS_PER_THREAD];
static unsigned int   present[ N_THREADS];


static void   check( int condition, char *name)
{
   if( condition)
      return;

   printf( "FAILED: %s\n", name);
   exit( 1);
}


static uint64_t   xorshift64star( uint64_t *x)
{
   *x ^= *x >> 12;
   *x ^= *x << 25;
   *x ^= *x >> 27;
   return( *x * 0x2545F4914F6CDD1DULL);
}


static intptr_t   global_key( unsigned int thread, unsigned int k)
{
   return( (intptr_t)((thread + 1) * 100000 + k + 1));
}


static void   *value_for_key( intptr_t key)
{
   return( (void *)(uintptr_t)((uintptr_t) key * 10 + 1));
}


static void   worker( struct worker_context *context)
{
   struct mulle_concurrent_hashmap2   *map = context->map;
   unsigned int                       thread = context->thread;
   intptr_t                           key;
   uint64_t                           rng;
   unsigned int                       k;
   unsigned int                       op;
   void                               *old;
   void                               *result;
   void                               *value;
   int                                rval;

   mulle_aba_register();

   rng = SEED ^ ((uint64_t) thread * 0x9E3779B97F4A7C15ULL);
   if( ! rng)
      rng = 1;

   for( op = 0; op < OPS_PER_THREAD; op++)
   {
      k     = (unsigned int)( xorshift64star( &rng) % KEYS_PER_THREAD);
      key   = global_key( thread, k);
      value = value_for_key( key);

      switch( xorshift64star( &rng) % 8)
      {
      case 0:
      case 1:
      case 2:
         result = mulle_concurrent_hashmap2_lookup( map, key);
         check( result == model[ thread][ k], "model lookup" );
         break;

      case 3:
      case 4:
         rval = mulle_concurrent_hashmap2_insert( map, key, value);
         if( model[ thread][ k] == NULL)
         {
            check( rval == 0, "model insert fresh" );
            model[ thread][ k] = value;
         }
         else
            check( rval == EEXIST, "model insert duplicate" );
         break;

      case 5:
      case 6:
         check( mulle_concurrent_hashmap2_register( map, key, value, &old) == 0,
                "model register rval" );
         check( old == model[ thread][ k], "model register old value" );
         model[ thread][ k] = value;
         break;

      case 7:
         rval = mulle_concurrent_hashmap2_remove( map, key, value);
         if( model[ thread][ k] == value)
         {
            check( rval == 0, "model remove present" );
            model[ thread][ k] = NULL;
         }
         else
            check( rval == ENOENT, "model remove absent" );
         break;

      default:
         break;
      }

      if( (op & 0xFF) == 0)
         mulle_aba_checkin();
   }

   present[ thread] = 0;
   for( k = 0; k < KEYS_PER_THREAD; k++)
      if( model[ thread][ k])
         present[ thread]++;

   mulle_aba_unregister();
}


static void   verify_final_state( struct mulle_concurrent_hashmap2 *map)
{
   struct mulle_concurrent_hashmap2enumerator   rover;
   intptr_t                                     hash;
   intptr_t                                     key;
   unsigned int                                 expected;
   unsigned int                                 k;
   unsigned int                                 seen;
   unsigned int                                 thread;
   void                                         *value;
   int                                          rval;

   expected = 0;
   for( thread = 0; thread < N_THREADS; thread++)
      expected += present[ thread];

   check( mulle_concurrent_hashmap2_count( map) == expected, "model final count" );

   for( thread = 0; thread < N_THREADS; thread++)
      for( k = 0; k < KEYS_PER_THREAD; k++)
      {
         key = global_key( thread, k);
         check( mulle_concurrent_hashmap2_lookup( map, key) == model[ thread][ k],
                "model final lookup" );
      }

   seen  = 0;
   rover = mulle_concurrent_hashmap2_enumerate( map);
   while( (rval = mulle_concurrent_hashmap2enumerator_next( &rover, &hash, &value)) == 1)
   {
      check( value == value_for_key( hash), "model final enumeration value" );
      ++seen;
   }
   mulle_concurrent_hashmap2enumerator_done( &rover);

   check( rval == 0, "model enumeration completed" );
   check( seen == expected, "model enumeration count" );
}


static void   run_round( void)
{
   struct mulle_concurrent_hashmap2   map;
   struct worker_context              context[ N_THREADS];
   mulle_thread_t                     threads[ N_THREADS];
   unsigned int                       i;

   memset( model, 0, sizeof( model));

   mulle_concurrent_hashmap2_init( &map, 4, NULL);

   for( i = 0; i < N_THREADS; i++)
   {
      context[ i].map    = &map;
      context[ i].thread = i;
      if( mulle_thread_create( (void *) worker, &context[ i], &threads[ i]))
      {
         perror( "mulle_thread_create" );
         abort();
      }
   }

   for( i = 0; i < N_THREADS; i++)
      mulle_thread_join( threads[ i]);

   verify_final_state( &map);

   mulle_concurrent_hashmap2_done( &map);
}


int   main( void)
{
   unsigned int   round;

   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   for( round = 0; round < N_ROUNDS; round++)
      run_round();

   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   printf( "PASSED\n");
   return( 0);
}
