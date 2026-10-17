//
// Exhaustive schedule exploration of mulle_concurrent_hashmap.
//
// The hashmap is insert-only, so the scenarios mix insert, register and
// lookup with growth: a 4 slot table migrates when it has 2 claimed hashes,
// an 8 slot table at 4. Keys 1, 5, 9, 13 and 17 collide in a 4 slot table.
// test/explore/explore.h runs every scenario under every interleaving of the
// atomic operations (sequentially consistent, up to a preemption bound) and
// checks after each execution that
//
//   * the history of operation results is linearizable against a plain
//     sequential map
//   * at quiescence count() and the enumerator agree with lookup()
//   * nothing crashed, asserted or looped, no block leaked or got freed twice
//
// Run with EXPLORE_PREEMPTIONS=3 (or more) for a deeper, slower search.
// Exit code 0 if all scenarios pass, 1 otherwise. Diagnostics go to stderr.
//
#include <mulle-aba/mulle-aba.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include "explore/explore.h"     // must come last before the .c under test
#include "mulle-concurrent-hashmap.c"


enum
{
   OP_END = 0,
   OP_INSERT,
   OP_REGISTER,
   OP_LOOKUP,
   OP_FINAL_LOOKUP
};


static char   *op_name( int kind)
{
   switch( kind)
   {
   case OP_INSERT       : return( "insert");
   case OP_REGISTER     : return( "register");
   case OP_LOOKUP       : return( "lookup");
   case OP_FINAL_LOOKUP : return( "final");
   }
   return( "?");
}


struct step
{
   int        kind;
   intptr_t   key;
   void       *value;
};


#define MAX_STEPS   4

struct script
{
   char          *name;
   int           n_threads;
   unsigned int  preemptions;
   unsigned int  size;            // 0 starts with the shared const storage
   struct step   pre[ MAX_STEPS];
   struct step   thread[ EXPLORE_MAX_THREADS][ MAX_STEPS];
};


static intptr_t   keys[] = { 1, 4, 5, 9, 13, 17 };
#define N_KEYS    (sizeof( keys) / sizeof( keys[ 0]))

#define A   ((void *) 0x10)
#define B   ((void *) 0x20)
#define C   ((void *) 0x30)
#define D   ((void *) 0x40)
#define E   ((void *) 0x50)


static struct script   scripts[] =
{
   // two writers both push the table over its threshold
   {
      "insert_insert_growth", 3, 2, 4,
      { { OP_INSERT, 1, A } },
      {
         { { OP_INSERT, 5, B }, { OP_LOOKUP, 9 } },
         { { OP_INSERT, 9, C }, { OP_LOOKUP, 5 } },
         { { OP_LOOKUP, 1 }, { OP_LOOKUP, 5 } }
      }
   },
   //
   // the RC1 shape: a writer stalls in p while p->q completes and q->r
   // starts, 4 -> 8 -> 16
   //
   {
      "insert_vs_2_growths", 3, 2, 4,
      { { OP_INSERT, 1, A } },
      {
         { { OP_INSERT, 5, B }, { OP_LOOKUP, 5 } },
         { { OP_INSERT, 9, C }, { OP_INSERT, 13, D }, { OP_INSERT, 17, E } },
         { { OP_LOOKUP, 5 }, { OP_LOOKUP, 1 } }
      }
   },
   {
      "register_same_key_growth", 3, 2, 4,
      { { OP_INSERT, 1, A } },
      {
         { { OP_REGISTER, 5, B } },
         { { OP_REGISTER, 5, C } },
         { { OP_INSERT, 9, D }, { OP_LOOKUP, 5 } }
      }
   },
   {
      "insert_same_key_growth", 3, 2, 4,
      { { OP_INSERT, 1, A } },
      {
         { { OP_INSERT, 5, B } },
         { { OP_INSERT, 5, C }, { OP_LOOKUP, 5 } },
         { { OP_INSERT, 9, D } }
      }
   },
   // the first operations migrate away from the shared const empty storage
   {
      "from_empty_storage", 3, 2, 0,
      { { OP_END } },
      {
         { { OP_INSERT, 1, A }, { OP_LOOKUP, 5 } },
         { { OP_REGISTER, 5, B }, { OP_LOOKUP, 1 } },
         { { OP_REGISTER, 1, C } }
      }
   },
   {
      "growth_vs_growth", 2, 3, 4,
      { { OP_INSERT, 1, A } },
      {
         { { OP_INSERT, 5, B }, { OP_INSERT, 4, D }, { OP_LOOKUP, 9 } },
         { { OP_INSERT, 9, C }, { OP_INSERT, 13, E }, { OP_LOOKUP, 5 } }
      }
   }
};


static struct script                     *g_script;
static struct mulle_concurrent_hashmap   g_map;


struct state
{
   void   *v[ N_KEYS];
};

static struct state   g_initial;


static unsigned int   key_index( intptr_t key)
{
   unsigned int   i;

   for( i = 0; i < N_KEYS; i++)
      if( keys[ i] == key)
         return( i);
   abort();
}


// the sequential specification. returns 0 if op's results are possible
static int   spec( void *p, struct explore_op *op)
{
   struct state   *state = p;
   void           **v;

   v = &state->v[ key_index( op->key)];
   switch( op->kind)
   {
   case OP_INSERT :
      if( *v)
         return( op->rval != EEXIST);
      *v = op->value;
      return( op->rval != 0);

   case OP_REGISTER :
      if( *v)
         return( op->result != *v);
      *v = op->value;
      return( op->result != NULL);

   case OP_LOOKUP       :
   case OP_FINAL_LOOKUP :
      return( op->result != *v);
   }
   abort();
}


static void   perform( struct explore_op *op)
{
   switch( op->kind)
   {
   case OP_INSERT :
      op->rval = mulle_concurrent_hashmap_insert( &g_map, op->key, op->value);
      break;

   case OP_REGISTER :
      op->result = mulle_concurrent_hashmap_register( &g_map, op->key, op->value);
      break;

   case OP_LOOKUP       :
   case OP_FINAL_LOOKUP :
      op->result = mulle_concurrent_hashmap_lookup( &g_map, op->key);
      break;
   }
}


static void   setup( void)
{
   struct step   *step;
   int           rval;

   explore_blocks_reset();
   explore_history_reset();
   memset( &g_initial, 0, sizeof( g_initial));

   mulle_concurrent_hashmap_init( &g_map, g_script->size, &explore_allocator);

   for( step = g_script->pre; step < &g_script->pre[ MAX_STEPS] && step->kind; step++)
   {
      assert( step->kind == OP_INSERT);
      rval = mulle_concurrent_hashmap_insert( &g_map, step->key, step->value);
      if( rval)
         explore_fatal( "setup insert failed");
      g_initial.v[ key_index( step->key)] = step->value;
   }
}


static void   body( int tid)
{
   struct explore_op   *op;
   struct step         *step;

   for( step = g_script->thread[ tid]; step < &g_script->thread[ tid][ MAX_STEPS] && step->kind; step++)
   {
      op = explore_op_begin( step->kind, tid, step->key, step->value);
      perform( op);
      explore_op_end( op);
   }
}


//
// at quiescence: count and enumeration must agree with the final lookups,
// and no key may be enumerated twice
//
static int   check_quiescent( void)
{
   struct mulle_concurrent_hashmapenumerator   rover;
   struct explore_op                           *op;
   intptr_t                                    hash;
   unsigned int                                count;
   unsigned int                                i;
   unsigned int                                live;
   unsigned int                                seen[ N_KEYS];
   void                                        *final[ N_KEYS];
   void                                        *value;
   int                                         rval;

   live = 0;
   for( i = 0; i < N_KEYS; i++)
   {
      op = explore_op_begin( OP_FINAL_LOOKUP, -1, keys[ i], NULL);
      perform( op);
      explore_op_end( op);
      final[ i] = op->result;
      if( final[ i])
         ++live;
   }

   count = mulle_concurrent_hashmap_count( &g_map);
   if( count != live)
   {
      fprintf( stderr, "count %u, but %u keys are live\n", count, live);
      return( 1);
   }

   memset( seen, 0, sizeof( seen));
   rover = mulle_concurrent_hashmap_enumerate( &g_map);
   while( (rval = mulle_concurrent_hashmapenumerator_next( &rover, &hash, &value)) == 1)
   {
      i = key_index( hash);
      if( seen[ i]++ || value != final[ i])
      {
         fprintf( stderr, "enumerator: key %ld value %p (lookup says %p, seen %u)\n",
                          (long) hash, value, final[ i], seen[ i]);
         return( 1);
      }
   }
   mulle_concurrent_hashmapenumerator_done( &rover);
   if( rval != 0)
   {
      fprintf( stderr, "enumerator returned %d at quiescence\n", rval);
      return( 1);
   }
   for( i = 0; i < N_KEYS; i++)
      if( final[ i] && ! seen[ i])
      {
         fprintf( stderr, "enumerator missed key %ld\n", (long) keys[ i]);
         return( 1);
      }
   return( 0);
}


static int   check( void)
{
   int   failed;

   failed = check_quiescent();
   if( ! failed)
      failed = explore_history_check( spec, &g_initial, sizeof( g_initial), op_name);
   else
      explore_history_print( stderr, op_name);

   mulle_concurrent_hashmap_done( &g_map);
   failed |= explore_blocks_check_and_free();
   return( failed);
}


int   main( void)
{
   struct explore_scenario   scenario;
   unsigned int              i;
   char                      *only;
   int                       failed;

   only   = getenv( "EXPLORE_SCENARIO");
   failed = 0;
   for( i = 0; i < sizeof( scripts) / sizeof( scripts[ 0]); i++)
   {
      g_script = &scripts[ i];
      if( only && *only && strcmp( only, g_script->name))
         continue;

      memset( &scenario, 0, sizeof( scenario));
      scenario.name      = g_script->name;
      scenario.n_threads = g_script->n_threads;
      scenario.setup     = setup;
      scenario.body      = body;
      scenario.check     = check;

      failed |= explore_run( &scenario, g_script->preemptions);
   }
   return( failed ? 1 : 0);
}
