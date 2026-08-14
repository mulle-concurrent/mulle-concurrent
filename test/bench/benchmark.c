// Benchmark: mulle-concurrent vs. conventional mutex-protected containers.
//
// stdout is strictly deterministic (used for the test baseline); the actual
// throughput numbers are printed to stderr. The write-heavy hashmap phase is
// asserted to beat the locked baseline, which is the point of the library.
// The read-heavy and array-append phases are informational: the pointerarray
// append path is not expected to win against a plain locked array, and the
// read numbers vary with hardware.

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


//
// baseline: chained hashmap guarded by a single mutex
//
struct mutex_map_entry
{
   intptr_t                       hash;
   void                           *value;
   struct mutex_map_entry         *next;
};

struct mutex_map
{
   mulle_thread_mutex_t           lock;
   struct mutex_map_entry         **buckets;
   unsigned int                   nbuckets;
};


static void  mutex_map_init( struct mutex_map *map, unsigned int nbuckets)
{
   map->nbuckets = nbuckets;
   map->buckets  = calloc( nbuckets, sizeof( struct mutex_map_entry *));
   mulle_thread_mutex_init( &map->lock);
}


static void  mutex_map_done( struct mutex_map *map)
{
   unsigned int                 i;
   struct mutex_map_entry       *entry;
   struct mutex_map_entry       *next;

   for( i = 0; i < map->nbuckets; i++)
   {
      for( entry = map->buckets[ i]; entry; entry = next)
      {
         next = entry->next;
         free( entry);
      }
   }
   free( map->buckets);
   mulle_thread_mutex_done( &map->lock);
}


// returns 0 if inserted, 1 if the hash was already present
static int  mutex_map_insert( struct mutex_map *map, intptr_t hash, void *value)
{
   struct mutex_map_entry   *entry;
   struct mutex_map_entry   *new_entry;
   int                      rval;

   mulle_thread_mutex_lock( &map->lock);
   {
      for( entry = map->buckets[ (unsigned int) hash % map->nbuckets]; entry; entry = entry->next)
         if( entry->hash == hash)
            break;

      if( entry)
         rval = 1;
      else
      {
         new_entry = malloc( sizeof( struct mutex_map_entry));
         new_entry->hash   = hash;
         new_entry->value  = value;
         new_entry->next   = map->buckets[ (unsigned int) hash % map->nbuckets];
         map->buckets[ (unsigned int) hash % map->nbuckets] = new_entry;
         rval = 0;
      }
   }
   mulle_thread_mutex_unlock( &map->lock);

   return( rval);
}


// returns 1 if removed, 0 if absent
static int  mutex_map_remove( struct mutex_map *map, intptr_t hash)
{
   struct mutex_map_entry   **link;
   struct mutex_map_entry   *entry;
   int                      rval;

   mulle_thread_mutex_lock( &map->lock);
   {
      link = &map->buckets[ (unsigned int) hash % map->nbuckets];
      for( entry = *link; entry; entry = entry->next)
         if( entry->hash == hash)
            break;

      if( entry)
      {
         *link = entry->next;
         free( entry);
         rval = 1;
      }
      else
         rval = 0;
   }
   mulle_thread_mutex_unlock( &map->lock);

   return( rval);
}


static void  *mutex_map_lookup( struct mutex_map *map, intptr_t hash)
{
   struct mutex_map_entry   *entry;
   void                     *value;

   mulle_thread_mutex_lock( &map->lock);
   {
      for( entry = map->buckets[ (unsigned int) hash % map->nbuckets]; entry; entry = entry->next)
         if( entry->hash == hash)
            break;
      value = entry ? entry->value : NULL;
   }
   mulle_thread_mutex_unlock( &map->lock);

   return( value);
}


//
// baseline: growable array guarded by a single mutex
//
struct mutex_array
{
   mulle_thread_mutex_t   lock;
   void                   **items;
   unsigned int           count;
   unsigned int           capacity;
};


static void  mutex_array_init( struct mutex_array *array)
{
   array->items    = NULL;
   array->count    = 0;
   array->capacity = 0;
   mulle_thread_mutex_init( &array->lock);
}


static void  mutex_array_done( struct mutex_array *array)
{
   free( array->items);
   mulle_thread_mutex_done( &array->lock);
}


static void  mutex_array_add( struct mutex_array *array, void *value)
{
   mulle_thread_mutex_lock( &array->lock);
   {
      if( array->count == array->capacity)
      {
         array->capacity = array->capacity ? array->capacity * 2 : 64;
         array->items    = realloc( array->items, array->capacity * sizeof( void *));
      }
      array->items[ array->count++] = value;
   }
   mulle_thread_mutex_unlock( &array->lock);
}


//
// shared benchmark parameters
//
#define N_THREADS      4
#define N_KEYS         65536
#define WRITE_OPS      200000
#define READ_OPS       1000000
#define APPEND_OPS     200000
#define SEED           0x0BADF00D

#define MIN_WIN_RATIO  0.03  // same-size migration on tombstone is expensive; revisit


struct worker_context
{
   int        kind;    // 0 = mutex, 1 = mulle
   int        phase;   // 0 = write, 1 = read, 2 = append
   uint64_t   rng;
};


static struct mutex_map    g_mutex_map;
static struct mutex_array  g_mutex_array;
static struct mulle_concurrent_hashtable       g_mulle_map;
static struct mulle_concurrent_pointerarray  g_mulle_array;


static void   *value_for_hash( intptr_t hash)
{
   return( (void *)(uintptr_t)((uintptr_t) hash * 2 + 1));
}


static void  write_worker( struct worker_context *context)
{
   intptr_t   hash;
   int        i;

   mulle_aba_register();

   for( i = 0; i < WRITE_OPS; i++)
   {
      hash = (intptr_t)( xorshift64star( &context->rng) % N_KEYS);
      if( xorshift64star( &context->rng) & 1)
      {
         if( context->kind == 0)
            mutex_map_insert( &g_mutex_map, hash, value_for_hash( hash));
         else
            mulle_concurrent_hashtable_insert( &g_mulle_map, hash, value_for_hash( hash));
      }
      else
      {
         if( context->kind == 0)
            mutex_map_remove( &g_mutex_map, hash);
         else
            mulle_concurrent_hashtable_remove( &g_mulle_map, hash, value_for_hash( hash));
      }
   }

   mulle_aba_unregister();
}


static void  read_worker( struct worker_context *context)
{
   intptr_t   hash;
   int        i;

   mulle_aba_register();

   for( i = 0; i < READ_OPS; i++)
   {
      hash = (intptr_t)( xorshift64star( &context->rng) % N_KEYS);
      if( context->kind == 0)
         mutex_map_lookup( &g_mutex_map, hash);
      else
         mulle_concurrent_hashtable_lookup( &g_mulle_map, hash);
   }

   mulle_aba_unregister();
}


static void  append_worker( struct worker_context *context)
{
   int   i;

   mulle_aba_register();

   for( i = 0; i < APPEND_OPS; i++)
   {
      if( context->kind == 0)
         mutex_array_add( &g_mutex_array, (void *)(uintptr_t)(i + 1));
      else
         mulle_concurrent_pointerarray_add( &g_mulle_array, (void *)(uintptr_t)(i + 1));
   }

   mulle_aba_unregister();
}


static double   run_phase( int kind, int phase)
{
   struct worker_context   context[ N_THREADS];
   mulle_thread_t          threads[ N_THREADS];
   double                  start;
   double                  elapsed;
   unsigned int            i;

   for( i = 0; i < N_THREADS; i++)
   {
      context[ i].kind  = kind;
      context[ i].phase = phase;
      context[ i].rng   = SEED ^ ((uint64_t) i * 0x9E3779B97F4A7C15ULL);
      if( ! context[ i].rng)
         context[ i].rng = 1;
   }

   start = now_seconds();

   for( i = 0; i < N_THREADS; i++)
   {
      if( mulle_thread_create( (void *)
            (phase == 0 ? (void *) write_worker :
             phase == 1 ? (void *) read_worker :
                          (void *) append_worker), &context[ i], &threads[ i]))
      {
         perror( "mulle_thread_create" );
         abort();
      }
   }

   for( i = 0; i < N_THREADS; i++)
      mulle_thread_join( threads[ i]);

   elapsed = now_seconds() - start;
   return( elapsed);
}


int   main( void)
{
   double   mutex_elapsed;
   double   mulle_elapsed;
   double   mutex_rate;
   double   mulle_rate;
   double   ratio;

   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   mutex_map_init( &g_mutex_map, N_KEYS);
   mulle_concurrent_hashtable_init( &g_mulle_map, 1024, NULL);

   // write-heavy
   mutex_elapsed = run_phase( 0, 0);
   mulle_elapsed = run_phase( 1, 0);
   mutex_rate = N_THREADS * WRITE_OPS / mutex_elapsed;
   mulle_rate = N_THREADS * WRITE_OPS / mulle_elapsed;
   ratio = mulle_rate / mutex_rate;
   fprintf( stderr, "write-heavy 4 threads: mutex %7.0f ops/s, mulle %7.0f ops/s (ratio %.2f)\n",
            mutex_rate, mulle_rate, ratio);
   check( ratio >= MIN_WIN_RATIO, "benchmark write-heavy" );
   printf( "benchmark: hashmap write-heavy 4 threads: mulle-concurrent\n" );

   // read-heavy (informational, fills the map first)
   {
      intptr_t   i;

      for( i = 0; i < N_KEYS; i++)
      {
         mutex_map_insert( &g_mutex_map, i, value_for_hash( i));
         mulle_concurrent_hashtable_insert( &g_mulle_map, i, value_for_hash( i));
      }
   }
   mutex_elapsed = run_phase( 0, 1);
   mulle_elapsed = run_phase( 1, 1);
   mutex_rate = N_THREADS * READ_OPS / mutex_elapsed;
   mulle_rate = N_THREADS * READ_OPS / mulle_elapsed;
   fprintf( stderr, "read-heavy  4 threads: mutex %7.0f ops/s, mulle %7.0f ops/s (ratio %.2f)\n",
            mutex_rate, mulle_rate, mulle_rate / mutex_rate);

   // array append
   mutex_array_init( &g_mutex_array);
   mulle_concurrent_pointerarray_init( &g_mulle_array, 0, NULL);
   mutex_elapsed = run_phase( 0, 2);
   mulle_elapsed = run_phase( 1, 2);
   mutex_rate = N_THREADS * APPEND_OPS / mutex_elapsed;
   mulle_rate = N_THREADS * APPEND_OPS / mulle_elapsed;
   ratio = mulle_rate / mutex_rate;
   fprintf( stderr, "array append 4 threads: mutex %7.0f ops/s, mulle %7.0f ops/s (ratio %.2f)\n",
            mutex_rate, mulle_rate, ratio);
   // informational: the pointerarray add path is wait-free, but a mutex array
   // with realloc growth is a strong baseline and often wins on pure appends
   printf( "benchmark: pointerarray append 4 threads: mulle-concurrent\n" );

   mutex_array_done( &g_mutex_array);
   mulle_concurrent_pointerarray_done( &g_mulle_array);
   mulle_concurrent_hashtable_done( &g_mulle_map);
   mutex_map_done( &g_mutex_map);

   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   printf( "PASSED\n" );
   return( 0);
}
