//
// hashmap (original) vs hashmap2 (hash-word freeze, no tombstones).
//
// stdout is strictly deterministic (it is the test baseline); the throughput
// numbers go to stderr. Nothing is asserted about speed, this is a measuring
// stick, not a gate. What is asserted is that both containers agree on the
// resulting element count, so a fast wrong answer cannot look good.
//
// Three phases isolate the interesting axes:
//
//   mixed      insert 1000 / lookup 10000 / remove 100, repeatedly, with keys
//              recycled across rounds. This is the remove-then-reinsert path,
//              where the original pays a migration per tombstone reuse.
//   read-heavy fill once, then lookups only. Isolates probe cost.
//   grow       insert only from a small initial size. Isolates migration cost.
//
#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-testallocator/mulle-testallocator.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
# include <windows.h>
#else
# include <time.h>
#endif


#define N_THREADS        4
#define KEYS_PER_THREAD  2048

#define INSERTS          1000
#define LOOKUPS          10000
#define REMOVES          100
#define N_ROUNDS         10

#define READ_OPS         200000
#define GROW_KEYS        20000


static struct mulle_concurrent_hashtable    g_map1;
static struct mulle_concurrent_hashtable   g_map2;


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


static double   now_seconds( void)
{
#ifdef _WIN32
   LARGE_INTEGER   frequency;
   LARGE_INTEGER   counter;

   QueryPerformanceFrequency( &frequency);
   QueryPerformanceCounter( &counter);
   return( (double) counter.QuadPart / (double) frequency.QuadPart);
#else
   struct timespec   ts;

   clock_gettime( CLOCK_MONOTONIC, &ts);
   return( (double) ts.tv_sec + (double) ts.tv_nsec / 1e9);
#endif
}


static intptr_t   key_for( unsigned int thread, unsigned int k)
{
   return( (intptr_t)((thread + 1) * 1000000 + k + 1));
}


static void   *value_for( intptr_t hash)
{
   return( (void *)(uintptr_t)((uintptr_t) hash * 8 + 1));
}


struct worker_context
{
   unsigned int   thread;
   unsigned int   kind;      // 0 == hashmap, 1 == hashmap2
   unsigned int   phase;     // 0 == mixed, 1 == read-heavy, 2 == grow
};


static void   insert_key( unsigned int kind, intptr_t hash)
{
   if( kind == 0)
      mulle_concurrent_hashtable_insert( &g_map1, hash, value_for( hash));
   else
      mulle_concurrent_hashtable_insert( &g_map2, hash, value_for( hash));
}


static void   remove_key( unsigned int kind, intptr_t hash)
{
   if( kind == 0)
      mulle_concurrent_hashtable_remove( &g_map1, hash, value_for( hash));
   else
      mulle_concurrent_hashtable_remove( &g_map2, hash, value_for( hash));
}


static void   *lookup_key( unsigned int kind, intptr_t hash)
{
   if( kind == 0)
      return( mulle_concurrent_hashtable_lookup( &g_map1, hash));
   return( mulle_concurrent_hashtable_lookup( &g_map2, hash));
}


static void   mixed_worker( struct worker_context *context)
{
   intptr_t       hash;
   uint64_t       rng;
   unsigned int   i;
   unsigned int   k;
   unsigned int   round;

   rng = 0x9E3779B9 ^ ((uint64_t) context->thread * 0x2545F4914F6CDD1DULL);
   if( ! rng)
      rng = 1;

   for( round = 0; round < N_ROUNDS; round++)
   {
      // insert a window of keys, recycled every round so that removed keys
      // get reinserted over and over
      for( i = 0; i < INSERTS; i++)
      {
         k = (round * INSERTS + i) % KEYS_PER_THREAD;
         insert_key( context->kind, key_for( context->thread, k));
      }

      for( i = 0; i < LOOKUPS; i++)
      {
         k = (unsigned int)( xorshift64star( &rng) % KEYS_PER_THREAD);
         lookup_key( context->kind, key_for( context->thread, k));
      }

      for( i = 0; i < REMOVES; i++)
      {
         k = (unsigned int)( xorshift64star( &rng) % KEYS_PER_THREAD);
         remove_key( context->kind, key_for( context->thread, k));
      }

      mulle_aba_checkin();
   }
}


static void   read_worker( struct worker_context *context)
{
   intptr_t       hash;
   uint64_t       rng;
   unsigned int   i;
   unsigned int   k;

   rng = 0xABCDEF ^ ((uint64_t) context->thread * 0x9E3779B97F4A7C15ULL);
   if( ! rng)
      rng = 1;

   for( i = 0; i < READ_OPS; i++)
   {
      k    = (unsigned int)( xorshift64star( &rng) % KEYS_PER_THREAD);
      hash = key_for( context->thread, k);
      if( lookup_key( context->kind, hash) != value_for( hash))
      {
         printf( "FAILED: read-heavy lookup mismatch\n");
         exit( 1);
      }

      if( (i & 0xFFF) == 0)
         mulle_aba_checkin();
   }
}


static void   grow_worker( struct worker_context *context)
{
   unsigned int   i;

   for( i = 0; i < GROW_KEYS; i++)
   {
      insert_key( context->kind, key_for( context->thread, i % KEYS_PER_THREAD));
      if( (i & 0xFFF) == 0)
         mulle_aba_checkin();
   }
}


static void   worker( struct worker_context *context)
{
   mulle_aba_register();

   switch( context->phase)
   {
   case 0:
      mixed_worker( context);
      break;

   case 1:
      read_worker( context);
      break;

   default:
      grow_worker( context);
      break;
   }

   mulle_aba_unregister();
}


static double   run_phase( unsigned int kind, unsigned int phase)
{
   struct worker_context   context[ N_THREADS];
   mulle_thread_t          threads[ N_THREADS];
   double                  start;
   unsigned int            i;

   for( i = 0; i < N_THREADS; i++)
   {
      context[ i].thread = i;
      context[ i].kind   = kind;
      context[ i].phase  = phase;
   }

   start = now_seconds();

   for( i = 0; i < N_THREADS; i++)
      if( mulle_thread_create( (void *) worker, &context[ i], &threads[ i]))
      {
         perror( "mulle_thread_create" );
         abort();
      }

   for( i = 0; i < N_THREADS; i++)
      mulle_thread_join( threads[ i]);

   return( now_seconds() - start);
}


static void   report( char *name,
                      double elapsed1,
                      double elapsed2,
                      double ops)
{
   double   rate1;
   double   rate2;

   rate1 = ops / elapsed1;
   rate2 = ops / elapsed2;
   fprintf( stderr, "%-11s hashmap %9.0f ops/s, hashmap2 %9.0f ops/s (hashmap2 is %.2fx)\n",
            name, rate1, rate2, rate2 / rate1);
}


int   main( void)
{
   double         elapsed1;
   double         elapsed2;
   double         ops;
   intptr_t       hash;
   unsigned int   i;
   unsigned int   t;

   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   //
   // mixed: insert / lookup / remove with recycled keys
   //
   mulle_concurrent_hashtable_init( &g_map1, 1024, NULL);
   mulle_concurrent_hashtable_init( &g_map2, 1024, NULL);

   elapsed1 = run_phase( 0, 0);
   elapsed2 = run_phase( 1, 0);

   check( mulle_concurrent_hashtable_count( &g_map1) ==
          mulle_concurrent_hashtable_count( &g_map2),
          "mixed phase counts agree" );

   ops = (double) N_THREADS * N_ROUNDS * (INSERTS + LOOKUPS + REMOVES);
   report( "mixed", elapsed1, elapsed2, ops);
   printf( "benchmark: mixed insert/lookup/remove\n");

   mulle_concurrent_hashtable_done( &g_map1);
   mulle_concurrent_hashtable_done( &g_map2);

   //
   // read-heavy: fill once, then lookups only
   //
   mulle_concurrent_hashtable_init( &g_map1, 1024, NULL);
   mulle_concurrent_hashtable_init( &g_map2, 1024, NULL);

   for( t = 0; t < N_THREADS; t++)
      for( i = 0; i < KEYS_PER_THREAD; i++)
      {
         hash = key_for( t, i);
         insert_key( 0, hash);
         insert_key( 1, hash);
      }

   elapsed1 = run_phase( 0, 1);
   elapsed2 = run_phase( 1, 1);

   ops = (double) N_THREADS * READ_OPS;
   report( "read-heavy", elapsed1, elapsed2, ops);
   printf( "benchmark: read-heavy lookup\n");

   mulle_concurrent_hashtable_done( &g_map1);
   mulle_concurrent_hashtable_done( &g_map2);

   //
   // grow: insert only, from a deliberately small initial size
   //
   mulle_concurrent_hashtable_init( &g_map1, 4, NULL);
   mulle_concurrent_hashtable_init( &g_map2, 4, NULL);

   elapsed1 = run_phase( 0, 2);
   elapsed2 = run_phase( 1, 2);

   check( mulle_concurrent_hashtable_count( &g_map1) ==
          mulle_concurrent_hashtable_count( &g_map2),
          "grow phase counts agree" );

   ops = (double) N_THREADS * GROW_KEYS;
   report( "grow", elapsed1, elapsed2, ops);
   fprintf( stderr, "grow sizes: hashmap %u, hashmap2 %u\n",
            mulle_concurrent_hashtable_get_size( &g_map1),
            mulle_concurrent_hashtable_get_size( &g_map2));
   printf( "benchmark: grow insert\n");

   mulle_concurrent_hashtable_done( &g_map1);
   mulle_concurrent_hashtable_done( &g_map2);

   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   printf( "PASSED\n");
   return( 0);
}
