//
// Deterministic reproducers for BUGREPORT-POINTERSET-REMOVE.md.
//
// The pointerset source is #included with pointerset_race_point() redefined, so
// a thread can be parked at an exact race point and released when the test says
// so. Each interleaving is forced, not hoped for: no iteration counts, no
// dependence on core count, and it fails the same way pinned to one CPU.
//
// Scenarios:
//
//   lost_insert    A inserts PTR into generation p and stalls before the slot
//                  CAS. p -> q completes, but copy skipped PTR's empty slot
//                  without freezing it. The insert then lands in the retired p
//                  and returns 0; member() on q says 0 (bug A).
//
//   lost_register  Same with register (bug A).
//
//   member_redirect A reader captures p and stalls before probing. p -> q
//                  completes and redirects PTR's slot. The reader probes the
//                  retired p, runs past REDIRECT into NULL and reports 0 for a
//                  live pointer (bug B).
//
//   full_cycle_bound Four writers claim the four slots of a size-4 storage
//                  and park before bumping n_used. An absent member and a new
//                  insert must still complete (bounded probe, migrate).
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
   ROLE_WRITER2,
   ROLE_WRITER3,
   ROLE_WRITER4,
   ROLE_READER,
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


#define pointerset_race_point( site)   test_race_point( site)
// by name, not by relative path: the amalgamated copy in mulle-core keeps the
// source flat in src/mulle-concurrent/, the constituent keeps it in
// src/pointerset/. Both directories are on the include path.
#include "mulle-concurrent-pointerset.c"


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


#define BYSTANDER   ((void *) 0x2000)
#define PTR         ((void *) 0x1002)


enum
{
   OP_INSERT,
   OP_REGISTER,
   OP_MEMBER,
   OP_MIGRATE
};

struct job
{
   void   *found;
   void   *value;
   int    op;
   int    role;
   int    rval;
};

static struct mulle_concurrent_pointerset   g_set;


static mulle_thread_rval_t   worker( void *arg)
{
   struct job   *job = arg;

   mulle_aba_register();

   mulle_thread_tss_set( g_role_key, (void *) (intptr_t) job->role);

   switch( job->op)
   {
   case OP_INSERT :
      job->rval = mulle_concurrent_pointerset_insert( &g_set, job->value);
      break;

   case OP_REGISTER :
      job->found = mulle_concurrent_pointerset_register( &g_set, job->value);
      job->rval  = errno;
      break;

   case OP_MEMBER :
      job->rval = mulle_concurrent_pointerset_member( &g_set, job->value);
      break;


   case OP_MIGRATE :
      _mulle_concurrent_pointerset_migrate_storage( &g_set,
            _mulle_atomic_pointer_read_relaxed( &g_set.storage.pointer));
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


static void   setup( void)
{
   mulle_concurrent_pointerset_init( &g_set, 4, &mulle_allocator_default);
}


static void   teardown( void)
{
   mulle_concurrent_pointerset_done( &g_set);
}


static int   scenario_lost_insert( int use_register)
{
   struct job   writer = { 0 };
   int          failed;

   setup();
   mulle_concurrent_pointerset_insert( &g_set, BYSTANDER);

   // writer has read generation p and parks before its slot CAS
   writer.role  = ROLE_WRITER;
   writer.op    = use_register ? OP_REGISTER : OP_INSERT;
   writer.value = PTR;
   arm( ROLE_WRITER, use_register ? POINTERSET_RACE_REGISTER : POINTERSET_RACE_INSERT);
   start( &writer);
   wait_parked( ROLE_WRITER);

   // p -> q runs to completion, while PTR's slot is still empty
   _mulle_concurrent_pointerset_migrate_storage( &g_set,
         _mulle_atomic_pointer_read_relaxed( &g_set.storage.pointer));

   release( ROLE_WRITER);
   mulle_thread_join( g_threads[ ROLE_WRITER]);

   failed = 0;
   if( use_register)
   {
      if( writer.found != MULLE_CONCURRENT_NO_POINTER)
      {
         fprintf( stderr, "lost_register: register returned %p (want inserted)\n",
                          writer.found);
         failed = 1;
      }
   }
   else
   {
      if( writer.rval != 0)
      {
         fprintf( stderr, "lost_insert: insert returned %d (want 0)\n",
                          writer.rval);
         failed = 1;
      }
   }

   // A successful insert must be visible in the current generation
   if( ! mulle_concurrent_pointerset_member( &g_set, PTR))
   {
      fprintf( stderr, "%s: insert returned success, but member() lost the pointer\n",
                       use_register ? "lost_register" : "lost_insert");
      failed = 1;
   }

   if( ! mulle_concurrent_pointerset_member( &g_set, BYSTANDER))
   {
      fprintf( stderr, "%s: bystander lost during migration\n",
                       use_register ? "lost_register" : "lost_insert");
      failed = 1;
   }

   teardown();
   return( failed);
}


static int   scenario_member_redirect( void)
{
   struct job   reader = { 0 };
   int          failed;

   setup();
   mulle_concurrent_pointerset_insert( &g_set, PTR);

   // reader has captured p, but has not started probing
   reader.role  = ROLE_READER;
   reader.op    = OP_MEMBER;
   reader.value = PTR;
   arm( ROLE_READER, POINTERSET_RACE_MEMBER);
   start( &reader);
   wait_parked( ROLE_READER);

   // p -> q redirects PTR's slot, then retires p
   _mulle_concurrent_pointerset_migrate_storage( &g_set,
         _mulle_atomic_pointer_read_relaxed( &g_set.storage.pointer));

   release( ROLE_READER);
   mulle_thread_join( g_threads[ ROLE_READER]);

   failed = 0;
   if( reader.rval != 1)
   {
      fprintf( stderr, "member_redirect: member returned %d for a live pointer\n",
                       reader.rval);
      failed = 1;
   }

   // and it must still be there afterwards
   if( ! mulle_concurrent_pointerset_member( &g_set, PTR))
   {
      fprintf( stderr, "member_redirect: pointer lost after the stale probe\n");
      failed = 1;
   }

   teardown();
   return( failed);
}


static int   scenario_full_cycle_bound( void)
{
   struct job   writer[ 4] = { 0 };
   void         *ptr[ 4];
   void         *absent;
   unsigned int i;
   int          failed;

   setup();
   absent = (void *) 0x10004;

   // four pointers map to the four slots of a size-4 storage
   for( i = 0; i < 4; i++)
   {
      ptr[ i] = (void *) (uintptr_t) (0x10000 + i);
      writer[ i].role  = ROLE_WRITER + i;
      writer[ i].op    = OP_INSERT;
      writer[ i].value = ptr[ i];
      arm( writer[ i].role, POINTERSET_RACE_CLAIMED);
   }

   // fill the storage while none of the writers published its n_used bump
   for( i = 0; i < 4; i++)
      start( &writer[ i]);
   for( i = 0; i < 4; i++)
      wait_parked( writer[ i].role);

   failed = 0;

   // a full probe cycle must terminate, even though n_used still says empty
   if( mulle_concurrent_pointerset_member( &g_set, absent))
   {
      fprintf( stderr, "full_cycle_bound: absent member reported present\n");
      failed = 1;
   }

   // and an insert must migrate instead of spinning on the full storage
   if( mulle_concurrent_pointerset_insert( &g_set, absent) != 0)
   {
      fprintf( stderr, "full_cycle_bound: insert into a full storage failed\n");
      failed = 1;
   }

   for( i = 0; i < 4; i++)
   {
      release( writer[ i].role);
      mulle_thread_join( g_threads[ writer[ i].role]);
   }

   for( i = 0; i < 4; i++)
      if( ! mulle_concurrent_pointerset_member( &g_set, ptr[ i]))
      {
         fprintf( stderr, "full_cycle_bound: writer value %u lost\n", i);
         failed = 1;
      }
   if( ! mulle_concurrent_pointerset_member( &g_set, absent))
   {
      fprintf( stderr, "full_cycle_bound: inserted value lost\n");
      failed = 1;
   }

   teardown();
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

   mulle_aba_init( &mulle_allocator_default);
   mulle_allocator_set_aba( &mulle_allocator_default,
                            mulle_aba_get_global(),
                            _mulle_aba_free_owned_pointer);
   mulle_aba_register();

   failed  = scenario_lost_insert( 0);
   failed |= scenario_lost_insert( 1);
   failed |= scenario_member_redirect();
   failed |= scenario_full_cycle_bound();

   mulle_aba_unregister();
   mulle_allocator_set_aba( &mulle_allocator_default, NULL, NULL);
   mulle_aba_done();
   mulle_thread_tss_free( g_role_key);

   return( failed ? 1 : 0);
}
