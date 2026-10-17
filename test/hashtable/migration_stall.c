//
// Deterministic reproducers for BUGREPORT-hashtable-migration-races.md.
//
// The hashtable source is #included with hashtable_race_point() redefined, so
// a thread can be parked at an exact race point and released when the test
// says so. Each interleaving is forced, not hoped for: no iteration counts, no
// dependence on core count, and it fails the same way pinned to one CPU.
//
// Scenarios:
//
//   skip_insert    A writer claims a slot in generation p and stalls there
//                  while p->q completes and q->r starts. Its post_check then
//                  carries into map->next_storage (r) and skips q, which is
//                  where readers still look. insert returns 0, the lookup
//                  right after returns NULL.
//
//   skip_register  Same stall with register while V1 is permanently live.
//                  register installs V2 into r ahead of q's copier and
//                  reports *p_old == NULL (S11). V1 is then lost.
//
//   stale_copier   A copier reads V from a frozen slot and stalls before
//                  carry. A helper drains the slot, the owner removes V in q,
//                  then the stale copier carries V into q again (S6).
//
// Exit code 0 if all pass, 1 otherwise. Diagnostics go to stderr.
//
#include <mulle-concurrent/mulle-concurrent.h>

#include <stdint.h>
#include <stdio.h>


enum
{
   ROLE_NONE = 0,
   ROLE_WRITER,
   ROLE_COPIER,
   ROLE_N
};

static mulle_thread_tss_t       g_role_key;              // role of the calling thread
static mulle_atomic_pointer_t   g_site[ ROLE_N];         // armed race point, 0 if none
static mulle_atomic_pointer_t   g_parked[ ROLE_N];
static mulle_atomic_pointer_t   g_release[ ROLE_N];

static mulle_thread_t           g_threads[ ROLE_N];


static void   test_race_point( int site)
{
   int   role;

   role = (int) (intptr_t) mulle_thread_tss_get( g_role_key);
   if( role == ROLE_NONE)
      return;
   if( (intptr_t) _mulle_atomic_pointer_read( &g_site[ role]) != site)
      return;

   _mulle_atomic_pointer_write( &g_site[ role], (void *) 0);   // one shot
   _mulle_atomic_pointer_write( &g_parked[ role], (void *) 1);
   while( ! _mulle_atomic_pointer_read( &g_release[ role]))
      mulle_thread_yield();
}


#define hashtable_race_point( site)   test_race_point( site)
// by name, not by relative path: the amalgamated copy in mulle-core keeps the
// source flat in src/mulle-concurrent/, the constituent keeps it in
// src/hashtable/. Both directories are on the include path.
#include "mulle-concurrent-hashtable.c"


static void   arm( int role, int site)
{
   _mulle_atomic_pointer_write( &g_parked[ role], (void *) 0);
   _mulle_atomic_pointer_write( &g_release[ role], (void *) 0);
   _mulle_atomic_pointer_write( &g_site[ role], (void *) (intptr_t) site);
}


static void   wait_parked( int role)
{
   while( ! _mulle_atomic_pointer_read( &g_parked[ role]))
      mulle_thread_yield();
}


static void   release( int role)
{
   _mulle_atomic_pointer_write( &g_release[ role], (void *) 1);
}


#define BYSTANDER   1      // probes before KEY, so copy reaches it first
#define KEY         10
#define V1          ((void *) 0xAAA0)
#define V2          ((void *) 0xBBB0)


enum
{
   OP_INSERT,
   OP_REGISTER,
   OP_MIGRATE
};

struct job
{
   void   *old;
   void   *value;
   int    op;
   int    role;
   int    rval;
};

static struct mulle_concurrent_hashtable   g_map;


static mulle_thread_rval_t   worker( void *arg)
{
   struct job   *job = arg;

   mulle_aba_register();

   mulle_thread_tss_set( g_role_key, (void *) (intptr_t) job->role);

   switch( job->op)
   {
   case OP_INSERT :
      job->rval = mulle_concurrent_hashtable_insert( &g_map, KEY, job->value);
      break;

   case OP_REGISTER :
      job->rval = mulle_concurrent_hashtable_register( &g_map, KEY, job->value, &job->old);
      break;

   case OP_MIGRATE :
      _mulle_concurrent_hashtable_migrate_same_size( &g_map);
      break;
   }

   mulle_thread_tss_set( g_role_key, (void *) (intptr_t) ROLE_NONE);
   mulle_aba_unregister();
   return( (mulle_thread_rval_t) 0);
}


static void   start( struct job *job)
{
   if( mulle_thread_create( worker, job, &g_threads[ job->role]))
   {
      perror( "mulle_thread_create");
      abort();
   }
}


static int   scenario_skip( int use_register)
{
   struct job   copier = { 0 };
   struct job   writer = { 0 };
   void         *found;
   int          failed;

   mulle_concurrent_hashtable_init( &g_map, 64, NULL);
   mulle_concurrent_hashtable_insert( &g_map, BYSTANDER, (void *) 0x10);
   if( use_register)
      mulle_concurrent_hashtable_insert( &g_map, KEY, V1);   // never removed

   // writer parks in generation p, after the claim, before the value CAS
   writer.role  = ROLE_WRITER;
   writer.op    = use_register ? OP_REGISTER : OP_INSERT;
   writer.value = use_register ? V2 : V1;
   arm( ROLE_WRITER, use_register ? HASHTABLE_RACE_REGISTER : HASHTABLE_RACE_INSERT);
   start( &writer);
   wait_parked( ROLE_WRITER);

   // p -> q runs to completion
   _mulle_concurrent_hashtable_migrate_same_size( &g_map);

   // q -> r starts and parks after freezing BYSTANDER, before KEY's slot
   copier.role = ROLE_COPIER;
   copier.op   = OP_MIGRATE;
   arm( ROLE_COPIER, HASHTABLE_RACE_COPY_FROZEN);
   start( &copier);
   wait_parked( ROLE_COPIER);

   release( ROLE_WRITER);
   mulle_thread_join( g_threads[ ROLE_WRITER]);

   failed = 0;
   if( use_register)
   {
      if( writer.rval != 0 || writer.old != V1)
      {
         fprintf( stderr, "skip_register: register rval=%d *p_old=%p, but V1 (%p)"
                          " is permanently live\n",
                          writer.rval, writer.old, V1);
         failed = 1;
      }
   }
   else
   {
      found = mulle_concurrent_hashtable_lookup( &g_map, KEY);
      if( writer.rval != 0 || found != V1)
      {
         fprintf( stderr, "skip_insert: insert rval=%d, lookup right after=%p"
                          " (want %p)\n",
                          writer.rval, found, V1);
         failed = 1;
      }
   }

   release( ROLE_COPIER);
   mulle_thread_join( g_threads[ ROLE_COPIER]);

   found = mulle_concurrent_hashtable_lookup( &g_map, KEY);
   if( found != V1)
   {
      fprintf( stderr, "%s: after all migrations lookup=%p (want %p)\n",
                       use_register ? "skip_register" : "skip_insert",
                       found, V1);
      failed = 1;
   }

   mulle_concurrent_hashtable_done( &g_map);
   return( failed);
}


static int   scenario_stale_copier( void)
{
   struct job   copier = { 0 };
   void         *found;
   int          failed;
   int          rval;

   mulle_concurrent_hashtable_init( &g_map, 64, NULL);
   mulle_concurrent_hashtable_insert( &g_map, KEY, V1);

   // copier freezes KEY's slot, reads V1 and parks before carrying it
   copier.role = ROLE_COPIER;
   copier.op   = OP_MIGRATE;
   arm( ROLE_COPIER, HASHTABLE_RACE_COPY_READ);
   start( &copier);
   wait_parked( ROLE_COPIER);

   // remove sees the frozen slot, helps finish p -> q, removes V1 from q
   failed = 0;
   rval   = mulle_concurrent_hashtable_remove( &g_map, KEY, V1);
   if( rval != 0)
   {
      fprintf( stderr, "stale_copier: remove returned %d (want 0)\n", rval);
      failed = 1;
   }

   release( ROLE_COPIER);
   mulle_thread_join( g_threads[ ROLE_COPIER]);

   found = mulle_concurrent_hashtable_lookup( &g_map, KEY);
   if( found != NULL)
   {
      fprintf( stderr, "stale_copier: removed value resurrected, lookup=%p\n",
                       found);
      failed = 1;
   }

   rval = mulle_concurrent_hashtable_insert( &g_map, KEY, V1);
   if( rval != 0)
   {
      fprintf( stderr, "stale_copier: insert after remove returned %d (want 0)\n",
                       rval);
      failed = 1;
   }

   mulle_concurrent_hashtable_done( &g_map);
   return( failed);
}


int   main( void)
{
   int   failed;

   if( mulle_thread_tss_create( NULL, &g_role_key))
   {
      perror( "mulle_thread_tss_create");
      return( 1);
   }

   mulle_aba_init( NULL);
   mulle_aba_register();

   failed  = scenario_skip( 0);
   failed |= scenario_skip( 1);
   failed |= scenario_stale_copier();

   mulle_aba_unregister();
   mulle_aba_done();
   mulle_thread_tss_free( g_role_key);

   return( failed ? 1 : 0);
}
