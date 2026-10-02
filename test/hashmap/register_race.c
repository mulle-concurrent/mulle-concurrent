// Short reproducer for a register/insert race condition (cross-key leak).
//
// N_THREADS threads each own a private stream of distinct (hash, value) pairs.
// Every hash is congruent to 2 modulo 4, so all of them collide on slot 2 of
// any fresh 4-slot table and then linear-probe. The threads hammer
// register/insert concurrently, forcing repeated migration.
//
// The hash-first claim guarantees that a thread never observes another key's
// value under its own key: a slot is claimed by CASing its hash word from
// NO_HASH, and only the winner may write the value. Before the fix the code
// read entry->hash and then CASed entry->value as two separate steps, which
// let one thread's value land in a slot a different key had since claimed.
//
// After the threads join, the final state is verified: every key must map to
// its own value, and no enumerated slot may hold a foreign pairing.

#include <mulle-concurrent/mulle-concurrent.h>

#include <errno.h>
#include <stdio.h>
#include <stdint.h>


#define N_THREADS   4
#define N_ITERS     4000

static struct mulle_concurrent_hashmap   g_map;
static volatile int                      g_fail;


static intptr_t   key_for( unsigned int thread, unsigned int i)
{
   return( (intptr_t) (100000 + (intptr_t) thread * 1000000 + (intptr_t) i * 4 + 2));
}


static void   *value_for( intptr_t key)
{
   return( (void *) (uintptr_t) ((uintptr_t) key * 8 + 3));
}


static void  *writer( void *context)
{
   unsigned int   self = (unsigned int) (uintptr_t) context;
   unsigned int   i;
   intptr_t       key;
   void           *value;
   void           *result;
   int            rval;

   mulle_aba_register();

   for( i = 0; i < N_ITERS; i++)
   {
      key   = key_for( self, i);
      value = value_for( key);

      if( i & 1)
      {
         result = mulle_concurrent_hashmap_register( &g_map, key, value);
         if( result != MULLE_CONCURRENT_NO_POINTER && result != value)
         {
            printf( "RACE: register(hash %td) returned foreign value %p\n", key, result);
            g_fail = 1;
            break;
         }
      }
      else
      {
         rval = mulle_concurrent_hashmap_insert( &g_map, key, value);
         if( rval != 0)
         {
            printf( "RACE: insert(hash %td) returned %d\n", key, rval);
            g_fail = 1;
            break;
         }
      }
   }

   mulle_aba_unregister();
   return( g_fail ? (void *) 1 : NULL);
}


static void  verify_final_state( void)
{
   struct mulle_concurrent_hashmapenumerator   rover;
   intptr_t                                    hash;
   void                                        *value;
   unsigned int                                thread;
   unsigned int                                i;
   intptr_t                                    key;
   int                                         rval;
   unsigned int                                seen;

   for( thread = 0; thread < N_THREADS; thread++)
   {
      for( i = 0; i < N_ITERS; i++)
      {
         key = key_for( thread, i);
         if( mulle_concurrent_hashmap_lookup( &g_map, key) != value_for( key))
         {
            printf( "RACE: lookup(hash %td) lost its value\n", key);
            g_fail = 1;
            return;
         }
      }
   }

   seen  = 0;
   rover = mulle_concurrent_hashmap_enumerate( &g_map);
   while( (rval = mulle_concurrent_hashmapenumerator_next( &rover, &hash, &value)) == 1)
   {
      ++seen;
      if( value != value_for( hash))
      {
         printf( "RACE: enumerated foreign pairing hash %td value %p\n", hash, value);
         g_fail = 1;
         break;
      }
   }
   mulle_concurrent_hashmapenumerator_done( &rover);

   if( rval != 0 && rval != ECANCELED)
   {
      printf( "RACE: enumeration ended with %d\n", rval);
      g_fail = 1;
   }
   if( seen != N_THREADS * N_ITERS)
   {
      printf( "RACE: saw %u of %u expected entries\n", seen, N_THREADS * N_ITERS);
      g_fail = 1;
   }
}


int   main( void)
{
   mulle_thread_t   threads[ N_THREADS];
   unsigned int     i;

   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_hashmap_init( &g_map, 4, NULL);

   for( i = 0; i < N_THREADS; i++)
   {
      if( mulle_thread_create( (void *) writer, (void *) (uintptr_t) i, &threads[ i]))
      {
         perror( "mulle_thread_create" );
         return( 1);
      }
   }

   for( i = 0; i < N_THREADS; i++)
      mulle_thread_join( threads[ i]);

   if( ! g_fail)
      verify_final_state();

   mulle_concurrent_hashmap_done( &g_map);
   mulle_aba_unregister();
   mulle_aba_done();

   if( g_fail)
      return( 1);
   printf( "PASSED\n" );
   return( 0);
}