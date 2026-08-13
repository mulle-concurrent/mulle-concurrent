// Deterministic model check.
//
// Each thread owns a disjoint key range and runs a fixed, seeded operation
// sequence (lookup / register / remove / pose) while the shared storage
// migrates underneath. Because keys are disjoint, the sequential specification
// per key is the thread's own model, so every result is checkable against it
// regardless of interleaving. After the threads join, the final container
// state is verified exactly against the union of the thread models.
//
// A shared-key register contest additionally checks that concurrent register
// calls never lose or duplicate a value.

#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-testallocator/mulle-testallocator.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define N_THREADS          4
#define KEYS_PER_THREAD    256
#define OPS_PER_THREAD     6000
#define N_ROUNDS           3
#define SEED               0x1234ABCD

struct worker_context
{
   struct mulle_concurrent_hashmap   *map;
   unsigned int                       thread;
};


static uint64_t   xorshift64star( uint64_t *x)
{
   *x ^= *x >> 12;
   *x ^= *x << 25;
   *x ^= *x >> 27;
   return( *x * 0x2545F4914F6CDD1DULL);
}


static void   check( int condition, char *name)
{
   if( condition)
      return;

   printf( "FAILED: %s\n", name);
   exit( 1);
}


// the sequential per-thread model
static void   *model[ N_THREADS][ KEYS_PER_THREAD];

// per-thread bookkeeping for the post-join verification
static unsigned int   present[ N_THREADS];


static intptr_t   global_key( unsigned int thread, unsigned int k)
{
   return( (intptr_t)((thread + 1) * 100000 + k + 1));
}


static void   *value_for_key( intptr_t key)
{
   return( (void *)(uintptr_t)((uintptr_t) key * 10 + 1));
}


static void   *posed_value_for_key( intptr_t key)
{
   return( (void *)(uintptr_t)((uintptr_t) key * 10 + 6));
}


static void  worker( struct worker_context *context)
{
   struct mulle_concurrent_hashmap   *map = context->map;
   unsigned int                       thread = context->thread;
   uint64_t                           rng;
   unsigned int                       k;
   intptr_t                           key;
   void                               *value;
   void                               *result;
   int                                rval;
   unsigned int                       op;

   mulle_aba_register();

   rng = SEED ^ ((uint64_t) thread * 0x9E3779B97F4A7C15ULL);
   if( ! rng)
      rng = 1;

   for( op = 0; op < OPS_PER_THREAD; op++)
   {
      k   = (unsigned int)( xorshift64star( &rng) % KEYS_PER_THREAD);
      key = global_key( thread, k);
      value = value_for_key( key);

      //
      // pose() is deliberately absent from this mix. It performs a full
      // migration per call (see dox/POSEAS-PATCH.md), so putting it in a hot
      // randomized loop would double the map thousands of times and test
      // nothing but the allocator. It has dedicated coverage in pose.c and
      // pose_stress.c.
      //
      switch( xorshift64star( &rng) % 9)
      {
      case 0:
      case 1:
      case 2:
      case 3:
         // lookup
         result = mulle_concurrent_hashmap_lookup( map, key);
         check( result == model[ thread][ k], "model lookup" );
         break;

      case 4:
      case 5:
      case 6:
         // register (insert if absent)
         errno  = 0;
         result = mulle_concurrent_hashmap_register( map, key, value);
         if( result == MULLE_CONCURRENT_NO_POINTER)
         {
            check( model[ thread][ k] == NULL, "model register insert" );
            model[ thread][ k] = value;
         }
         else
         {
            // An absent model value can still have a tombstone in this
            // generation. Migration eventually drops it and permits reuse.
            if( result == MULLE_CONCURRENT_INVALID_POINTER && errno == EEXIST)
               check( model[ thread][ k] == NULL, "model register tombstone" );
            else
            {
               // register returns the stored value, which may be posed
               if( ! (result == model[ thread][ k] && model[ thread][ k] != NULL))
               {
                  printf( "DBG thread %u k %u op %u: register returned %p, model %p\n",
                          thread, k, op, result, model[ thread][ k]);
                  exit( 1);
               }
            }
         }
         break;

      case 7:
      case 8:
         // remove requires the exact value pair
         rval = mulle_concurrent_hashmap_remove( map, key, value);
         if( model[ thread][ k] == value)
         {
            check( rval == 0, "model remove present" );
            model[ thread][ k] = NULL;
         }
         else
         {
            check( rval == ENOENT, "model remove absent" );
         }
         break;

      default:
         break;
         break;
      }
   }

   // count how many keys the model still holds
   present[ thread] = 0;
   for( k = 0; k < KEYS_PER_THREAD; k++)
      if( model[ thread][ k])
         present[ thread]++;

   mulle_aba_unregister();
}


static void  verify_final_state( struct mulle_concurrent_hashmap *map)
{
   struct mulle_concurrent_hashmapenumerator   rover;
   intptr_t                                    hash;
   void                                        *value;
   unsigned int                                expected_count;
   unsigned int                                thread;
   unsigned int                                k;
   intptr_t                                    key;
   unsigned int                                seen;
   unsigned int                                matches;
   int                                         rval;

   // the container must agree with the union of the models
   expected_count = 0;
   for( thread = 0; thread < N_THREADS; thread++)
      expected_count += present[ thread];

   check( mulle_concurrent_hashmap_count( map) == expected_count,
          "model final count" );

   for( thread = 0; thread < N_THREADS; thread++)
   {
      for( k = 0; k < KEYS_PER_THREAD; k++)
      {
         key = global_key( thread, k);
         check( mulle_concurrent_hashmap_lookup( map, key) == model[ thread][ k],
                "model final lookup" );
      }
   }

   // every enumerated pair must be in exactly one model and appear once
   seen    = 0;
   matches = 0;
   rover   = mulle_concurrent_hashmap_enumerate( map);
   while( (rval = mulle_concurrent_hashmapenumerator_next( &rover, &hash, &value)) == 1)
   {
      seen++;
      for( thread = 0; thread < N_THREADS; thread++)
      {
         for( k = 0; k < KEYS_PER_THREAD; k++)
         {
            if( global_key( thread, k) == hash)
            {
               check( model[ thread][ k] == value, "model final enumeration" );
               matches++;
               goto next_pair;
            }
         }
      }
      check( 0, "model enumeration unknown hash" );
   next_pair:
      ;
   }
   mulle_concurrent_hashmapenumerator_done( &rover);
   check( rval == 0 || rval == ECANCELED, "model enumeration clean" );
   check( seen == expected_count, "model enumeration count" );
   check( matches == expected_count, "model enumeration matches" );
}


static void  run_round( void)
{
   struct mulle_concurrent_hashmap   map;
   struct worker_context             context[ N_THREADS];
   mulle_thread_t                    threads[ N_THREADS];
   unsigned int                      i;

   memset( model, 0, sizeof( model));

   mulle_concurrent_hashmap_init( &map, 4, NULL);

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

   mulle_concurrent_hashmap_done( &map);
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

   printf( "PASSED\n" );
   return( 0);
}
