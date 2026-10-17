//
// Exhaustive schedule exploration of mulle_concurrent_hashtable.
//
// Every scenario runs a few operations on two or three threads against a
// 4 slot table. test/explore/explore.h executes the scenario under every
// interleaving of the atomic operations (sequentially consistent, up to a
// preemption bound) and checks after each execution that
//
//   * the history of operation results is linearizable against a plain
//     sequential map (insert/register/remove/lookup, migrate is a no-op)
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
#include "mulle-concurrent-hashtable.c"


enum
{
   OP_END = 0,
   OP_INSERT,
   OP_REGISTER,
   OP_REMOVE,
   OP_LOOKUP,
   OP_MIGRATE,
   OP_FINAL_LOOKUP
};


static char   *op_name( int kind)
{
   switch( kind)
   {
   case OP_INSERT       : return( "insert");
   case OP_REGISTER     : return( "register");
   case OP_REMOVE       : return( "remove");
   case OP_LOOKUP       : return( "lookup");
   case OP_MIGRATE      : return( "migrate");
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
   struct step   pre[ MAX_STEPS];
   struct step   thread[ EXPLORE_MAX_THREADS][ MAX_STEPS];
};


// keys 1, 5 and 9 all start probing at slot 1 of a 4 slot table, key 4 at 0
static intptr_t   keys[] = { 1, 4, 5, 9 };
#define N_KEYS    (sizeof( keys) / sizeof( keys[ 0]))

// even values, as required by the default value mode
#define A   ((void *) 0x10)
#define B   ((void *) 0x20)
#define C   ((void *) 0x30)
#define D   ((void *) 0x40)


static struct script   scripts[] =
{
   //
   // RC1: a writer stalled in p after its claim, p->q completes and q->r
   // starts. The writer must land in q (where lookup looks), not in r.
   //
   {
      "insert_vs_2_migrations", 3, 2,
      { { OP_INSERT, 4, B } },
      {
         { { OP_INSERT, 1, A }, { OP_LOOKUP, 1 } },
         { { OP_MIGRATE } },
         { { OP_MIGRATE } }
      }
   },
   {
      "register_vs_2_migrations", 3, 2,
      { { OP_INSERT, 1, A } },
      {
         { { OP_REGISTER, 1, B }, { OP_LOOKUP, 1 } },
         { { OP_MIGRATE } },
         { { OP_MIGRATE } }
      }
   },
   //
   // RC2: a copier read A and stalls before carrying it. The owner removes A
   // (helping the migration finish). The copier must not resurrect A.
   //
   {
      "remove_vs_copier", 2, 3,
      { { OP_INSERT, 1, A } },
      {
         { { OP_MIGRATE } },
         { { OP_REMOVE, 1, A }, { OP_LOOKUP, 1 }, { OP_INSERT, 1, B } }
      }
   },
   {
      "remove_reinsert_churn", 3, 2,
      { { OP_INSERT, 1, A } },
      {
         { { OP_REMOVE, 1, A }, { OP_INSERT, 1, B } },
         { { OP_MIGRATE } },
         { { OP_LOOKUP, 1 }, { OP_REGISTER, 1, C } }
      }
   },
   {
      "insert_insert_migrate", 3, 2,
      { { OP_END } },
      {
         { { OP_INSERT, 1, A } },
         { { OP_INSERT, 1, B } },
         { { OP_MIGRATE } }
      }
   },
   {
      "register_register_remove", 3, 2,
      { { OP_END } },
      {
         { { OP_REGISTER, 1, A } },
         { { OP_REGISTER, 1, B } },
         { { OP_REMOVE, 1, A }, { OP_LOOKUP, 1 } }
      }
   },
   {
      "remove_remove_migrate", 3, 2,
      { { OP_INSERT, 1, A } },
      {
         { { OP_REMOVE, 1, A } },
         { { OP_REMOVE, 1, A } },
         { { OP_MIGRATE } }
      }
   },
   //
   // a 4 slot table migrates at 2 claimed hashes, so inserting 9 grows it
   // while others remove and re-insert colliding keys
   //
   {
      "growth_vs_remove", 3, 2,
      { { OP_INSERT, 1, A }, { OP_INSERT, 5, B } },
      {
         { { OP_INSERT, 9, C }, { OP_LOOKUP, 5 } },
         { { OP_REMOVE, 5, B }, { OP_INSERT, 5, D } },
         { { OP_LOOKUP, 1 }, { OP_LOOKUP, 9 } }
      }
   },
   {
      "growth_vs_growth", 2, 3,
      { { OP_INSERT, 1, A } },
      {
         { { OP_INSERT, 5, B }, { OP_INSERT, 4, D } },
         { { OP_INSERT, 9, C }, { OP_REMOVE, 1, A } }
      }
   }
};


static struct script                       *g_script;
static struct mulle_concurrent_hashtable   g_map;


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

   if( op->kind == OP_MIGRATE)
      return( 0);

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
         return( ! (op->rval == 0 && op->result == *v));
      *v = op->value;
      return( ! (op->rval == 0 && op->result == NULL));

   case OP_REMOVE :
      if( *v == op->value)
      {
         *v = NULL;
         return( op->rval != 0);
      }
      return( op->rval != ENOENT);

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
      op->rval = mulle_concurrent_hashtable_insert( &g_map, op->key, op->value);
      break;

   case OP_REGISTER :
      op->result = (void *) 0xDEAD;
      op->rval   = mulle_concurrent_hashtable_register( &g_map, op->key, op->value, &op->result);
      break;

   case OP_REMOVE :
      op->rval = mulle_concurrent_hashtable_remove( &g_map, op->key, op->value);
      break;

   case OP_LOOKUP       :
   case OP_FINAL_LOOKUP :
      op->result = mulle_concurrent_hashtable_lookup( &g_map, op->key);
      break;

   case OP_MIGRATE :
      _mulle_concurrent_hashtable_migrate_same_size( &g_map);
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

   mulle_concurrent_hashtable_init( &g_map, 4, &explore_allocator);

   for( step = g_script->pre; step < &g_script->pre[ MAX_STEPS] && step->kind; step++)
   {
      assert( step->kind == OP_INSERT);
      rval = mulle_concurrent_hashtable_insert( &g_map, step->key, step->value);
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
   struct mulle_concurrent_hashtableenumerator   rover;
   struct explore_op                             *op;
   intptr_t                                      hash;
   unsigned int                                  i;
   unsigned int                                  seen[ N_KEYS];
   void                                          *final[ N_KEYS];
   void                                          *value;
   size_t                                        live;
   size_t                                        count;
   int                                           rval;

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

   count = mulle_concurrent_hashtable_count( &g_map);
   if( count != live)
   {
      fprintf( stderr, "count %zu, but %zu keys are live\n", count, live);
      return( 1);
   }

   memset( seen, 0, sizeof( seen));
   rover = mulle_concurrent_hashtable_enumerate( &g_map);
   while( (rval = mulle_concurrent_hashtableenumerator_next( &rover, &hash, &value)) == 1)
   {
      i = key_index( hash);
      if( seen[ i]++ || value != final[ i])
      {
         fprintf( stderr, "enumerator: key %ld value %p (lookup says %p, seen %u)\n",
                          (long) hash, value, final[ i], seen[ i]);
         return( 1);
      }
   }
   mulle_concurrent_hashtableenumerator_done( &rover);
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

   mulle_concurrent_hashtable_done( &g_map);
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
