//
// Deterministic reproducers for the hashmap migration protocol.
//
// Same technique as test/hashtable/migration_stall.c: the hashmap source is
// #included with hashmap_race_point() redefined, so a thread can be parked at
// an exact point and released when the test says so. Each interleaving is
// forced, not hoped for.
//
// Scenarios:
//
//   lost_insert     An insert read generation p and stalls before claiming a
//                   slot. p -> q completes (freezing the virgin slots). The
//                   insert must not land in the retired p.
//
//   claimed_insert  An insert claimed its hash in p and stalls before filling
//                   the value. p -> q freeze it, so the insert must retry in q.
//
//   claimed_register Same with register.
//
//   lookup_redirect A reader captures p and stalls before probing. p -> q
//                   freezes the live value; the stale lookup must migrate and
//                   retry instead of returning NULL.
//
//   stale_copier    A copier read a value and stalls before put. A second
//                   copier finishes p -> q. The stale put must not lose the
//                   value or overwrite the successor.
//
//   carried_stall   A copier put the value and stalls before freezing the
//                   source. A second copier finishes p -> q; the value must
//                   survive.
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
   ROLE_READER,
   ROLE_COPIER,
   ROLE_COPIER2,
   ROLE_N
};

static mulle_thread_tss_t       g_role_key;
static mulle_atomic_pointer_t   g_site[ ROLE_N];
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


#define hashmap_race_point( site)   test_race_point( site)
// by name, not by relative path: the amalgamated copy in mulle-core keeps the
// source flat in src/mulle-concurrent/, the constituent keeps it in
// src/hashmap/. Both directories are on the include path.
#include "mulle-concurrent-hashmap.c"


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


#define BYSTANDER_HASH   0x10
#define KEY              2
#define VALUE            ((void *) 0xAAA3)


enum
{
   OP_INSERT,
   OP_REGISTER,
   OP_LOOKUP,
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

static struct mulle_concurrent_hashmap   g_map;


static mulle_thread_rval_t   worker( void *arg)
{
   struct job   *job = arg;

   mulle_aba_register();

   mulle_thread_tss_set( g_role_key, (void *) (intptr_t) job->role);

   switch( job->op)
   {
   case OP_INSERT :
      job->rval = mulle_concurrent_hashmap_insert( &g_map, KEY, job->value);
      break;

   case OP_REGISTER :
      job->found = mulle_concurrent_hashmap_register( &g_map, KEY, job->value);
      break;

   case OP_LOOKUP :
      job->found = mulle_concurrent_hashmap_lookup( &g_map, KEY);
      break;

   case OP_MIGRATE :
      _mulle_concurrent_hashmap_migrate_storage( &g_map,
            _mulle_atomic_pointer_read( &g_map.storage.pointer));
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
   mulle_concurrent_hashmap_init( &g_map, 4, &mulle_allocator_default);
   mulle_concurrent_hashmap_insert( &g_map, BYSTANDER_HASH, (void *) 0x10);
}


static void   teardown( void)
{
   mulle_concurrent_hashmap_done( &g_map);
}


static void   migrate( void)
{
   _mulle_concurrent_hashmap_migrate_storage( &g_map,
         _mulle_atomic_pointer_read( &g_map.storage.pointer));
}


static int   check_live( char *name)
{
   int   failed;

   failed = 0;
   if( mulle_concurrent_hashmap_lookup( &g_map, KEY) != VALUE)
   {
      fprintf( stderr, "%s: KEY lost after the migration\n", name);
      failed = 1;
   }
   if( mulle_concurrent_hashmap_lookup( &g_map, BYSTANDER_HASH) != (void *) 0x10)
   {
      fprintf( stderr, "%s: bystander lost after the migration\n", name);
      failed = 1;
   }
   return( failed);
}


static int   scenario_lost_insert( int use_register)
{
   struct job   writer = { 0 };
   int          failed;

   setup();

   // writer has read generation p and parks before claiming its slot
   writer.role  = ROLE_WRITER;
   writer.op    = use_register ? OP_REGISTER : OP_INSERT;
   writer.value = VALUE;
   arm( ROLE_WRITER, use_register ? HASHMAP_RACE_REGISTER : HASHMAP_RACE_INSERT);
   start( &writer);
   wait_parked( ROLE_WRITER);

   migrate();

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
   failed |= check_live( use_register ? "lost_register" : "lost_insert");

   teardown();
   return( failed);
}


static int   scenario_claimed_insert( int use_register)
{
   struct job   writer = { 0 };
   int          failed;

   setup();

   // writer claimed its slot in p and parks before filling the value
   writer.role  = ROLE_WRITER;
   writer.op    = use_register ? OP_REGISTER : OP_INSERT;
   writer.value = VALUE;
   arm( ROLE_WRITER, HASHMAP_RACE_FILL);
   start( &writer);
   wait_parked( ROLE_WRITER);

   migrate();

   release( ROLE_WRITER);
   mulle_thread_join( g_threads[ ROLE_WRITER]);

   failed = 0;
   if( use_register)
   {
      if( writer.found != MULLE_CONCURRENT_NO_POINTER)
      {
         fprintf( stderr, "claimed_register: register returned %p (want inserted)\n",
                          writer.found);
         failed = 1;
      }
   }
   else
   {
      if( writer.rval != 0)
      {
         fprintf( stderr, "claimed_insert: insert returned %d (want 0)\n",
                          writer.rval);
         failed = 1;
      }
   }
   failed |= check_live( use_register ? "claimed_register" : "claimed_insert");

   teardown();
   return( failed);
}


static int   scenario_lookup_redirect( void)
{
   struct job   reader = { 0 };
   int          failed;

   setup();
   mulle_concurrent_hashmap_insert( &g_map, KEY, VALUE);

   // reader captured p and parks before probing
   reader.role = ROLE_READER;
   reader.op   = OP_LOOKUP;
   arm( ROLE_READER, HASHMAP_RACE_LOOKUP);
   start( &reader);
   wait_parked( ROLE_READER);

   migrate();

   release( ROLE_READER);
   mulle_thread_join( g_threads[ ROLE_READER]);

   failed = 0;
   if( reader.found != VALUE)
   {
      fprintf( stderr, "lookup_redirect: lookup returned %p for a live value\n",
                       reader.found);
      failed = 1;
   }
   failed |= check_live( "lookup_redirect");

   teardown();
   return( failed);
}


static int   scenario_stale_copier( int carried_stall)
{
   struct job   copier = { 0 };
   struct job   helper = { 0 };
   int          failed;

   setup();
   mulle_concurrent_hashmap_insert( &g_map, KEY, VALUE);

   copier.role = ROLE_COPIER;
   copier.op   = OP_MIGRATE;
   arm( ROLE_COPIER, carried_stall ? HASHMAP_RACE_COPY_CARRIED
                                   : HASHMAP_RACE_COPY_READ);
   start( &copier);
   wait_parked( ROLE_COPIER);

   // a second copier finishes p -> q and retires p
   helper.role = ROLE_COPIER2;
   helper.op   = OP_MIGRATE;
   start( &helper);
   mulle_thread_join( g_threads[ ROLE_COPIER2]);

   release( ROLE_COPIER);
   mulle_thread_join( g_threads[ ROLE_COPIER]);

   failed = check_live( carried_stall ? "carried_stall" : "stale_copier");

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
   failed |= scenario_claimed_insert( 0);
   failed |= scenario_claimed_insert( 1);
   failed |= scenario_lookup_redirect();
   failed |= scenario_stale_copier( 0);
   failed |= scenario_stale_copier( 1);

   mulle_aba_unregister();
   mulle_allocator_set_aba( &mulle_allocator_default, NULL, NULL);
   mulle_aba_done();
   mulle_thread_tss_free( g_role_key);

   return( failed ? 1 : 0);
}
