//
//  explore.h
//  mulle-concurrent
//
//  Copyright (c) 2026 Nat! - Mulle kybernetiK.
//  All rights reserved.
//
//  A small stateless model checker (CHESS style) for the lock-free code in
//  this library. It is a header only, test-only helper.
//
//  How it works
//  ------------
//  The code under test is #included as a .c file, with every atomic operation
//  redefined (see "instrumentation" at the end) to first call
//  explore_point(). Only one thread ever runs at a time: the one holding the
//  baton. At every explore_point() the scheduler decides which thread runs
//  next, so the whole execution is a deterministic function of the sequence
//  of decisions.
//
//  explore_run() executes the scenario over and over. After each execution
//  it backtracks to the deepest decision that still has an untried
//  alternative (depth-first search over the decision tree), and replays the
//  prefix up to it. When no alternative is left, every interleaving at atomic
//  operation granularity has been executed exactly once.
//
//  The search is bounded by the number of preemptions (a switch away from a
//  thread that could have continued), as in CHESS. Switches at thread exit
//  are free. With a bound of P, every schedule with at most P preemptions is
//  covered. Most concurrency bugs need very few (the hashtable migration
//  races need one and two). Set EXPLORE_PREEMPTIONS to search deeper.
//
//  What it covers and what not
//  ---------------------------
//  * Interleavings are explored under sequential consistency. All atomics in
//    hashtable/hashmap are seq_cst, except acquire loads in lookup, which
//    have no preceding store in the same thread, so SC loses nothing there.
//    Weak memory model reorderings are NOT explored.
//  * Only the atomic operations are scheduling points. Plain memory is either
//    immutable after publication (mask, modes) or thread local.
//  * Non-termination (a thread looping without bound) is reported via a step
//    cap, as is a decision tree that is too deep.
//  * Crashes and assert()s are caught and reported with the schedule.
//
//  Redistribution and use in source and binary forms, with or without
//  modification, are permitted provided that the following conditions are met:
//
//  Redistributions of source code must retain the above copyright notice, this
//  list of conditions and the following disclaimer.
//
//  Redistributions in binary form must reproduce the above copyright notice,
//  this list of conditions and the following disclaimer in the documentation
//  and/or other materials provided with the distribution.
//
//  Neither the name of Mulle kybernetiK nor the names of its contributors
//  may be used to endorse or promote products derived from this software
//  without specific prior written permission.
//
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
//  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
//  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
//  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
//  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
//  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
//  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
//  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
//  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
//  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
//  POSSIBILITY OF SUCH DAMAGE.
//
#ifndef explore_h__
#define explore_h__

// needs mulle-thread (mutex, cond, tss) and mulle-allocator, which come in
// via mulle-aba
#include <mulle-aba/mulle-aba.h>

#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define EXPLORE_MAX_THREADS     4
#define EXPLORE_MAIN            EXPLORE_MAX_THREADS
#define EXPLORE_MAX_DECISIONS   8192
#define EXPLORE_MAX_STEPS       200000
#define EXPLORE_MAX_BLOCKS      256
#define EXPLORE_MAX_OPS         32


struct explore_decision
{
   const char       *func;
   int              line;
   unsigned short   preempts;      // preemptions before this decision
   unsigned char    cur;           // thread that reached the point
   unsigned char    cur_enabled;   // could 'cur' have continued ?
   unsigned char    n_cands;
   unsigned char    chosen;        // index into cands
   unsigned char    cands[ EXPLORE_MAX_THREADS];
};


struct explore_scenario
{
   const char   *name;
   int          n_threads;
   void         (*setup)( void);          // main thread, sequential
   void         (*body)( int tid);        // explored
   int          (*check)( void);          // main thread, 0 = OK
   void         (*teardown)( void);       // main thread, sequential
};


static struct
{
   mulle_thread_mutex_t      lock;
   mulle_thread_cond_t       cond[ EXPLORE_MAX_THREADS + 1];
   mulle_thread_t            threads[ EXPLORE_MAX_THREADS];
   mulle_thread_tss_t        tid_key;

   struct explore_scenario   *scenario;
   int                       current;
   int                       done[ EXPLORE_MAX_THREADS];
   int                       quit;

   unsigned int              max_preemptions;
   unsigned int              preempts;
   unsigned int              n_decisions;
   unsigned int              n_prefix;
   unsigned long             steps;
   unsigned long             executions;
   unsigned long             total_steps;
   unsigned int              max_depth;

   struct explore_decision   d[ EXPLORE_MAX_DECISIONS];
} explore;


static void   explore_print_trace( FILE *fp);


static void   explore_fatal( char *msg)
{
   fprintf( stderr, "\n%s: FAIL in execution #%lu: %s\n",
                    explore.scenario ? explore.scenario->name : "?",
                    explore.executions + 1,
                    msg);
   explore_print_trace( stderr);
   fflush( stderr);
   _Exit( 1);
}


static void   explore_signal_handler( int sig)
{
   explore_fatal( sig == SIGABRT ? "abort (assert ?)" : "crash (signal)");
}


//
// called with the lock held. Returns the thread to run next, or EXPLORE_MAIN
// if every thread is done.
//
static int   explore_decide( const char *func, int line, int cur)
{
   struct explore_decision   *d;
   unsigned char             cands[ EXPLORE_MAX_THREADS];
   int                       cur_enabled;
   int                       n;
   int                       t;
   int                       tid;

   cur_enabled = cur >= 0 && cur < EXPLORE_MAX_THREADS && ! explore.done[ cur];

   // the current thread comes first: continuing it is the default and free
   n = 0;
   if( cur_enabled)
      cands[ n++] = (unsigned char) cur;
   for( t = 0; t < explore.scenario->n_threads; t++)
      if( t != cur && ! explore.done[ t])
         cands[ n++] = (unsigned char) t;

   if( n == 0)
      return( EXPLORE_MAIN);
   if( n == 1)
      return( cands[ 0]);      // no choice, don't record

   if( explore.n_decisions >= EXPLORE_MAX_DECISIONS)
      explore_fatal( "decision cap exceeded (livelock, or raise EXPLORE_MAX_DECISIONS)");

   d = &explore.d[ explore.n_decisions];
   if( explore.n_decisions < explore.n_prefix)
   {
      // replay: the program must reach the same choice
      if( d->n_cands != n || memcmp( d->cands, cands, n) || d->cur != (unsigned char) cur)
         explore_fatal( "nondeterministic replay (code under test depends on something but the schedule)");
   }
   else
   {
      d->n_cands     = (unsigned char) n;
      d->chosen      = 0;
      d->cur         = (unsigned char) cur;
      d->cur_enabled = (unsigned char) cur_enabled;
      memcpy( d->cands, cands, n);
   }
   d->func     = func;
   d->line     = line;
   d->preempts = (unsigned short) explore.preempts;

   tid = d->cands[ d->chosen];
   if( cur_enabled && tid != cur)
      explore.preempts++;

   explore.n_decisions++;
   return( tid);
}


// let 'next' run and wait until it's our turn again (lock held)
static void   explore_handoff( int me, int next)
{
   if( next == me)
      return;

   explore.current = next;
   mulle_thread_cond_signal( &explore.cond[ next]);
   while( explore.current != me)
      mulle_thread_cond_wait( &explore.cond[ me], &explore.lock);
}


static void   explore_point( const char *func, int line)
{
   int   tid;
   int   next;

   tid = (int) (intptr_t) mulle_thread_tss_get( explore.tid_key) - 1;
   if( tid < 0)
      return;           // main thread, setup/check phase: not explored

   mulle_thread_mutex_lock( &explore.lock);

   if( ++explore.steps > EXPLORE_MAX_STEPS)
      explore_fatal( "step cap exceeded (a thread does not terminate)");

   next = explore_decide( func, line, tid);
   explore_handoff( tid, next);

   mulle_thread_mutex_unlock( &explore.lock);
}


static mulle_thread_rval_t   explore_worker( void *arg)
{
   int   tid;
   int   next;

   tid = (int) (intptr_t) arg;
   mulle_thread_tss_set( explore.tid_key, (void *) (intptr_t) (tid + 1));

   mulle_thread_mutex_lock( &explore.lock);
   for(;;)
   {
      while( explore.current != tid)
         mulle_thread_cond_wait( &explore.cond[ tid], &explore.lock);
      if( explore.quit)
         break;

      mulle_thread_mutex_unlock( &explore.lock);
      (*explore.scenario->body)( tid);
      mulle_thread_mutex_lock( &explore.lock);

      explore.done[ tid] = 1;
      next = explore_decide( "<exit>", 0, tid);
      explore.current = next;
      mulle_thread_cond_signal( &explore.cond[ next]);
   }
   mulle_thread_mutex_unlock( &explore.lock);
   return( (mulle_thread_rval_t) 0);
}


//
// depth first: find the deepest decision with an untried alternative that
// fits the preemption budget. Returns 0 when the search is exhausted.
//
static int   explore_backtrack( void)
{
   struct explore_decision   *d;
   unsigned int              cost;
   unsigned int              i;
   unsigned int              r;

   for( i = explore.n_decisions; i-- > 0;)
   {
      d = &explore.d[ i];
      for( r = d->chosen + 1u; r < d->n_cands; r++)
      {
         cost = d->preempts + (d->cur_enabled && d->cands[ r] != d->cur);
         if( cost <= explore.max_preemptions)
         {
            d->chosen        = (unsigned char) r;
            explore.n_prefix = i + 1;
            return( 1);
         }
      }
   }
   return( 0);
}


static void   explore_print_trace( FILE *fp)
{
   struct explore_decision   *d;
   unsigned int              i;
   int                       tid;

   fprintf( fp, "schedule (%u decisions, %u preemptions), switches only:\n",
                explore.n_decisions, explore.preempts);
   for( i = 0; i < explore.n_decisions; i++)
   {
      d   = &explore.d[ i];
      tid = d->cands[ d->chosen];
      if( d->cur_enabled && tid == d->cur)
         continue;
      if( d->cur >= EXPLORE_MAX_THREADS)
         fprintf( fp, "   #%-4u start                           -> T%d\n", i, tid);
      else
         if( d->cur_enabled)
            fprintf( fp, "   #%-4u T%d preempted before %s:%d -> T%d\n",
                         i, d->cur, d->func, d->line, tid);
         else
            fprintf( fp, "   #%-4u T%d finished                     -> T%d\n",
                         i, d->cur, tid);
   }
}


static void   explore_run_once( void)
{
   int   first;

   mulle_thread_mutex_lock( &explore.lock);

   memset( explore.done, 0, sizeof( explore.done));
   explore.n_decisions = 0;
   explore.preempts    = 0;
   explore.steps       = 0;

   first = explore_decide( "<start>", 0, -1);
   explore_handoff( EXPLORE_MAIN, first);

   mulle_thread_mutex_unlock( &explore.lock);
}


static unsigned int   explore_getenv_uint( char *name, unsigned int dflt)
{
   char   *s;

   s = getenv( name);
   return( s && *s ? (unsigned int) strtoul( s, NULL, 0) : dflt);
}


//
// Returns 0 if every explored execution passed check(), 1 otherwise (after
// printing the failing schedule). 'preemptions' is the default bound,
// EXPLORE_PREEMPTIONS overrides it.
//
static int   explore_run( struct explore_scenario *scenario,
                          unsigned int preemptions)
{
   int   t;

   assert( scenario->n_threads >= 1 && scenario->n_threads <= EXPLORE_MAX_THREADS);

   memset( &explore, 0, sizeof( explore));
   explore.scenario        = scenario;
   explore.max_preemptions = explore_getenv_uint( "EXPLORE_PREEMPTIONS", preemptions);
   explore.current         = EXPLORE_MAIN;

   mulle_thread_mutex_init( &explore.lock);
   for( t = 0; t <= EXPLORE_MAX_THREADS; t++)
      mulle_thread_cond_init( &explore.cond[ t]);
   if( mulle_thread_tss_create( NULL, &explore.tid_key))
   {
      perror( "mulle_thread_tss_create");
      exit( 1);
   }

   signal( SIGABRT, explore_signal_handler);
   signal( SIGSEGV, explore_signal_handler);

   for( t = 0; t < scenario->n_threads; t++)
      if( mulle_thread_create( explore_worker, (void *) (intptr_t) t, &explore.threads[ t]))
      {
         perror( "mulle_thread_create");
         exit( 1);
      }

   for(;;)
   {
      if( scenario->setup)
         (*scenario->setup)();

      explore_run_once();

      if( (*scenario->check)())
         explore_fatal( "check failed (see above)");

      if( scenario->teardown)
         (*scenario->teardown)();

      explore.executions++;
      explore.total_steps += explore.steps;
      if( explore.n_decisions > explore.max_depth)
         explore.max_depth = explore.n_decisions;

      if( ! explore_backtrack())
         break;
   }

   mulle_thread_mutex_lock( &explore.lock);
   explore.quit = 1;
   for( t = 0; t < scenario->n_threads; t++)
   {
      explore.current = t;
      mulle_thread_cond_signal( &explore.cond[ t]);
      mulle_thread_mutex_unlock( &explore.lock);
      mulle_thread_join( explore.threads[ t]);
      mulle_thread_mutex_lock( &explore.lock);
   }
   mulle_thread_mutex_unlock( &explore.lock);

   signal( SIGABRT, SIG_DFL);
   signal( SIGSEGV, SIG_DFL);

   for( t = 0; t <= EXPLORE_MAX_THREADS; t++)
      mulle_thread_cond_done( &explore.cond[ t]);
   mulle_thread_mutex_done( &explore.lock);
   mulle_thread_tss_free( explore.tid_key);

   fprintf( stderr, "%-24s %2d threads, <= %u preemptions: %8lu executions,"
                    " max depth %4u, avg %4lu steps: OK\n",
                    scenario->name,
                    scenario->n_threads,
                    explore.max_preemptions,
                    explore.executions,
                    explore.max_depth,
                    explore.total_steps / (explore.executions ? explore.executions : 1));
   return( 0);
}


#pragma mark - allocator

//
// An allocator for the structure under test. It doesn't need mulle-aba:
// abafree only retires the block, and the retired blocks are freed after the
// execution, when all threads are done. It also checks double frees and
// leaks.
//
enum
{
   EXPLORE_BLOCK_LIVE = 1,
   EXPLORE_BLOCK_FREED,
   EXPLORE_BLOCK_RETIRED
};

static struct
{
   void   *block[ EXPLORE_MAX_BLOCKS];
   int    state[ EXPLORE_MAX_BLOCKS];
   int    n;
   int    error;
} explore_blocks;


static int   explore_block_index( void *block)
{
   int   i;

   for( i = 0; i < explore_blocks.n; i++)
      if( explore_blocks.block[ i] == block && explore_blocks.state[ i] == EXPLORE_BLOCK_LIVE)
         return( i);
   return( -1);
}


static void   *explore_calloc( size_t n, size_t size, struct mulle_allocator *allocator)
{
   void   *block;

   MULLE_C_UNUSED( allocator);

   if( explore_blocks.n >= EXPLORE_MAX_BLOCKS)
      explore_fatal( "too many allocations");
   block = calloc( n, size);
   if( ! block)
      explore_fatal( "out of memory");

   explore_blocks.block[ explore_blocks.n] = block;
   explore_blocks.state[ explore_blocks.n] = EXPLORE_BLOCK_LIVE;
   explore_blocks.n++;
   return( block);
}


static void   *explore_reallocarray( void *block, size_t n, size_t size, struct mulle_allocator *allocator)
{
   MULLE_C_UNUSED( block);
   MULLE_C_UNUSED( n);
   MULLE_C_UNUSED( size);
   MULLE_C_UNUSED( allocator);

   explore_fatal( "realloc not supported");
   return( NULL);
}


static void   explore_free( void *block, struct mulle_allocator *allocator)
{
   int   i;

   MULLE_C_UNUSED( allocator);

   if( ! block)
      return;
   i = explore_block_index( block);
   if( i < 0)
      explore_fatal( "free of a block that is not live (double free ?)");
   explore_blocks.state[ i] = EXPLORE_BLOCK_FREED;
   free( block);
}


static int   explore_abafree( void *aba,
                              mulle_allocator_deferred_free_t f,
                              void *block,
                              void *owner)
{
   int   i;

   MULLE_C_UNUSED( aba);
   MULLE_C_UNUSED( f);
   MULLE_C_UNUSED( owner);

   if( ! block)
      return( 0);
   i = explore_block_index( block);
   if( i < 0)
      explore_fatal( "abafree of a block that is not live (double free ?)");
   explore_blocks.state[ i] = EXPLORE_BLOCK_RETIRED;
   return( 0);
}


MULLE_C_NO_RETURN
static void   explore_fail( struct mulle_allocator *allocator, void *block, size_t size)
{
   MULLE_C_UNUSED( allocator);
   MULLE_C_UNUSED( block);
   MULLE_C_UNUSED( size);

   explore_fatal( "allocator fail");
   abort();
}


static struct mulle_allocator   explore_allocator =
{
   explore_calloc,
   explore_reallocarray,
   explore_free,
   explore_fail,
   explore_abafree,
   NULL
};


static void   explore_blocks_reset( void)
{
   memset( &explore_blocks, 0, sizeof( explore_blocks));
}


// after the structure's done(): nothing may be left live. Frees retired.
static int   explore_blocks_check_and_free( void)
{
   int   i;
   int   leaks;

   leaks = 0;
   for( i = 0; i < explore_blocks.n; i++)
      switch( explore_blocks.state[ i])
      {
      case EXPLORE_BLOCK_LIVE    :
         ++leaks;
         free( explore_blocks.block[ i]);
         break;
      case EXPLORE_BLOCK_RETIRED :
         free( explore_blocks.block[ i]);
         break;
      }
   explore_blocks_reset();

   if( leaks)
      fprintf( stderr, "%d block(s) leaked\n", leaks);
   return( leaks);
}


#pragma mark - history and linearizability

//
// Every operation of the explored threads is recorded with a logical
// invocation and response time. check() then searches for a sequential order
// that respects real time (if A returned before B was called, A comes first)
// and in which every result matches a sequential specification.
//
struct explore_op
{
   int        kind;
   int        tid;
   intptr_t   key;
   void       *value;
   int        rval;       // returned code
   void       *result;    // returned pointer (lookup, register old)
   unsigned   inv;
   unsigned   resp;
};


// sequential spec: apply 'op' to 'state', return 0 if the op's results match
typedef int   explore_spec_t( void *state, struct explore_op *op);


static struct
{
   struct explore_op   ops[ EXPLORE_MAX_OPS];
   int                 n;
   unsigned            clock;
} explore_history;


static void   explore_history_reset( void)
{
   memset( &explore_history, 0, sizeof( explore_history));
}


// only called by the thread holding the baton (or main), so no race
static struct explore_op   *explore_op_begin( int kind, int tid, intptr_t key, void *value)
{
   struct explore_op   *op;

   if( explore_history.n >= EXPLORE_MAX_OPS)
      explore_fatal( "too many ops");
   op        = &explore_history.ops[ explore_history.n++];
   op->kind  = kind;
   op->tid   = tid;
   op->key   = key;
   op->value = value;
   op->inv   = ++explore_history.clock;
   return( op);
}


static void   explore_op_end( struct explore_op *op)
{
   op->resp = ++explore_history.clock;
}


static int   explore_linearize( explore_spec_t *spec,
                                void *state,
                                size_t state_size,
                                unsigned int used)
{
   char                buf[ 256];
   struct explore_op   *op;
   unsigned int        min_resp;
   int                 i;

   assert( state_size <= sizeof( buf));

   if( used == (1u << explore_history.n) - 1)
      return( 1);

   // an op can go next only if it was invoked before every pending response
   min_resp = ~0u;
   for( i = 0; i < explore_history.n; i++)
      if( ! (used & (1u << i)) && explore_history.ops[ i].resp < min_resp)
         min_resp = explore_history.ops[ i].resp;

   for( i = 0; i < explore_history.n; i++)
   {
      if( used & (1u << i))
         continue;
      op = &explore_history.ops[ i];
      if( op->inv > min_resp)
         continue;

      memcpy( buf, state, state_size);
      if( (*spec)( buf, op))
         continue;
      if( explore_linearize( spec, buf, state_size, used | (1u << i)))
         return( 1);
   }
   return( 0);
}


static void   explore_history_print( FILE *fp,
                                     char *(*name)( int kind))
{
   struct explore_op   *op;
   int                 i;

   fprintf( fp, "history:\n");
   for( i = 0; i < explore_history.n; i++)
   {
      op = &explore_history.ops[ i];
      fprintf( fp, "   [%3u,%3u] T%d %-10s key=%ld value=%p -> rval=%d result=%p\n",
                   op->inv, op->resp,
                   op->tid, (*name)( op->kind),
                   (long) op->key, op->value, op->rval, op->result);
   }
}


// 0 if linearizable
static int   explore_history_check( explore_spec_t *spec,
                                    void *initial,
                                    size_t state_size,
                                    char *(*name)( int kind))
{
   assert( explore_history.n < 32);

   if( explore_linearize( spec, initial, state_size, 0))
      return( 0);

   fprintf( stderr, "history is not linearizable\n");
   explore_history_print( stderr, name);
   return( 1);
}


#pragma mark - instrumentation

//
// Include explore.h after all other headers and right before #include-ing
// the .c under test.
// Each macro refers to the real inline function of the same name, which a
// function-like macro does not expand recursively.
//
#define _mulle_atomic_pointer_read( a)                                                                     \
   (explore_point( __func__, __LINE__), _mulle_atomic_pointer_read( a))
#define _mulle_atomic_pointer_read_acquire( a)                                                             \
   (explore_point( __func__, __LINE__), _mulle_atomic_pointer_read_acquire( a))
#define _mulle_atomic_pointer_read_relaxed( a)                                                             \
   (explore_point( __func__, __LINE__), _mulle_atomic_pointer_read_relaxed( a))
#define _mulle_atomic_pointer_write( a, v)                                                                 \
   (explore_point( __func__, __LINE__), _mulle_atomic_pointer_write( a, v))
#define _mulle_atomic_pointer_write_relaxed( a, v)                                                         \
   (explore_point( __func__, __LINE__), _mulle_atomic_pointer_write_relaxed( a, v))
#define __mulle_atomic_pointer_cas( a, v, e)                                                               \
   (explore_point( __func__, __LINE__), __mulle_atomic_pointer_cas( a, v, e))
#define _mulle_atomic_pointer_cas( a, v, e)                                                                \
   (explore_point( __func__, __LINE__), _mulle_atomic_pointer_cas( a, v, e))
#define __mulle_atomic_pointer_weakcas( a, v, e)                                                           \
   (explore_point( __func__, __LINE__), __mulle_atomic_pointer_weakcas( a, v, e))
#define _mulle_atomic_pointer_weakcas( a, v, e)                                                            \
   (explore_point( __func__, __LINE__), _mulle_atomic_pointer_weakcas( a, v, e))
#define _mulle_atomic_pointer_increment( a)                                                                \
   (explore_point( __func__, __LINE__), _mulle_atomic_pointer_increment( a))
#define _mulle_atomic_pointer_decrement( a)                                                                \
   (explore_point( __func__, __LINE__), _mulle_atomic_pointer_decrement( a))

#endif
