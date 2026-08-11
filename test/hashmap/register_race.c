// Short reproducer for a register() race condition (cross-key value leak).
//
// N_THREADS threads each own a distinct (hash, value) pair; all hashes probe
// the same slot of a fresh 4-slot table. The race is triggered when a thread
// fills a slot that another thread has just vacated by remove().
//
// The (fixed) claim protocol is: a slot is claimed by CASing its hash from
// NO_HASH to the caller's hash; only the thread that holds the hash claim
// may CAS the value. Before the fix, the code instead read entry->hash and
// then CASed entry->value as two separate steps:
//
//    if( entry->hash == NO_HASH || entry->hash == hash)   // read
//       found = CAS( &entry->value, value, NO_POINTER);   // act, later
//
// remove() clears the value back to NO_POINTER while keeping the hash, so a
// slot could read hash == NO_HASH (stale) while a stalled reader's later CAS
// still landed on a slot a different key had since claimed and vacated. That
// let one thread's value get stored under a foreign key's hash: register()
// would then return a foreign value, or (worse) silently corrupt the table
// so the other key's entry held this key's value.
//
// The window opens whenever a fresh (all-empty) storage is filled
// concurrently and again after each migration, so each round re-inits a
// fresh table and runs the threads for a few remove/register cycles. With
// N_ROUNDS fresh tables the leak was hit within a handful of runs pre-fix.
#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-testallocator/mulle-testallocator.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>


#define N_THREADS   4
#define N_ROUNDS    200
#define N_ITERS     8

static struct mulle_concurrent_hashmap   g_map;
static volatile int                      g_fail;

// all four hashes collide on slot 2 of a 4-slot table (hash & 3 == 2)
static intptr_t   g_hash[ N_THREADS]  = { 400234, 200042, 600042, 800234 };
static void       *g_value[ N_THREADS] = { (void *) 0x3D1225,
                                           (void *) 0x1E87A5,
                                           (void *) 0x5E87A5,
                                           (void *) 0x1D1225 };


static void  *writer( void *context)
{
   unsigned int   self = (unsigned int)(uintptr_t) context;
   int            i;
   void           *result;

   mulle_aba_register();

   for( i = 0; i < N_ITERS; i++)
   {
      // remove first so the slot goes empty, then register
      mulle_concurrent_hashmap_remove( &g_map, g_hash[ self], g_value[ self]);
      errno  = 0;
      result = mulle_concurrent_hashmap_register( &g_map, g_hash[ self], g_value[ self]);
      // A tombstone can deny reuse until migration. Otherwise we inserted or
      // our value is present; any other value is the cross-key leak.
      if( result == MULLE_CONCURRENT_INVALID_POINTER && errno == EEXIST)
         continue;
      if( result != MULLE_CONCURRENT_NO_POINTER && result != g_value[ self])
      {
         printf( "RACE: register(hash %td) returned foreign value %p\n",
                 g_hash[ self], result);
         g_fail = 1;
         mulle_aba_unregister();
         return( (void *) 1);
      }
   }

   mulle_aba_unregister();
   return( NULL);
}


int   main( void)
{
   mulle_thread_t   threads[ N_THREADS];
   unsigned int     round;
   unsigned int     i;

   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   for( round = 0; round < N_ROUNDS && ! g_fail; round++)
   {
      mulle_concurrent_hashmap_init( &g_map, 4, NULL);

      for( i = 0; i < N_THREADS; i++)
      {
         if( mulle_thread_create( (void *) writer, (void *)(uintptr_t) i, &threads[ i]))
         {
            perror( "mulle_thread_create" );
            return( 1);
         }
      }

      for( i = 0; i < N_THREADS; i++)
         mulle_thread_join( threads[ i]);

      mulle_concurrent_hashmap_done( &g_map);
   }

   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   if( g_fail)
      return( 1);
   printf( "PASSED\n");
   return( 0);
}
