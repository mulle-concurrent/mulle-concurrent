//
// Deterministic reproducers for the pointerarray growth protocol.
//
// Same technique as test/hashtable/migration_stall.c: the pointerarray source
// is #included with pointerarray_race_point() redefined, so a thread can be
// parked at an exact point and released when the test says so.
//
// Scenarios:
//
//   grow_add       An add read a full generation p and stalls. p -> q
//                  completes, then the add must land in q, not in retired p.
//
//   slot_race      Two adds read the same last free index and stall before the
//                  slot CAS. Exactly one wins the slot, the other must migrate
//                  and append; neither may be lost.
//
//   partial_migration  Migrating a partially filled array must keep only the
//                  live prefix. The current copy walks to src->size and counts
//                  empty slots, which inflates n and leaves NULL holes.
//
//   early_lost_add A migration while the array still has room, plus an add
//                  parked on the retired generation, must not lose the add.
//
//   stale_copier   A copier read a value and stalls before storing it. A second
//                  copier finishes p -> q. The stale copier must not corrupt q.
//
//   carried_stall  A copier stored the value and stalls. A second copier
//                  finishes p -> q; the value must survive.
//
// Exit code 0 if all pass, 1 otherwise. Diagnostics go to stderr.
//
#include <mulle-concurrent/mulle-concurrent.h>

#include <stdint.h>
#include <stdio.h>


enum
{
   ROLE_NONE = 0,
   ROLE_ADDER,
   ROLE_ADDER2,
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


#define pointerarray_race_point( site)   test_race_point( site)
// by name, not by relative path: the amalgamated copy in mulle-core keeps the
// source flat in src/mulle-concurrent/, the constituent keeps it in
// src/pointerarray/. Both directories are on the include path.
#include "mulle-concurrent-pointerarray.c"


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


static void   *nth_value( unsigned int i)
{
   return( (void *) (uintptr_t) (0x10000 + i * 0x10));
}


enum
{
   OP_ADD,
   OP_MIGRATE
};

struct job
{
   void   *value;
   int    op;
   int    role;
};

static struct mulle_concurrent_pointerarray   g_array;


static mulle_thread_rval_t   worker( void *arg)
{
   struct job   *job = arg;

   mulle_aba_register();

   mulle_thread_tss_set( g_role_key, (void *) (intptr_t) job->role);

   switch( job->op)
   {
   case OP_ADD :
      mulle_concurrent_pointerarray_add( &g_array, job->value);
      break;

   case OP_MIGRATE :
      _mulle_concurrent_pointerarray_migrate_storage( &g_array,
            _mulle_atomic_pointer_read_relaxed( &g_array.storage.pointer));
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


static void   setup( unsigned int n_entries)
{
   unsigned int   i;

   mulle_concurrent_pointerarray_init( &g_array, 8, &mulle_allocator_default);
   for( i = 0; i < n_entries; i++)
      mulle_concurrent_pointerarray_add( &g_array, nth_value( i));
}


static void   teardown( void)
{
   mulle_concurrent_pointerarray_done( &g_array);
}


static void   migrate( void)
{
   _mulle_concurrent_pointerarray_migrate_storage( &g_array,
         _mulle_atomic_pointer_read_relaxed( &g_array.storage.pointer));
}


static int   check_dense_prefix( char *name, unsigned int count)
{
   unsigned int   i;
   void           *value;
   int            failed;

   failed = 0;
   if( mulle_concurrent_pointerarray_get_count( &g_array) != count)
   {
      fprintf( stderr, "%s: count is %u, want %u\n", name,
                       mulle_concurrent_pointerarray_get_count( &g_array),
                       count);
      failed = 1;
   }
   for( i = 0; i < count; i++)
   {
      value = mulle_concurrent_pointerarray_get( &g_array, i);
      if( ! value)
      {
         fprintf( stderr, "%s: NULL hole at index %u\n", name, i);
         failed = 1;
      }
   }
   return( failed);
}


static int   scenario_grow_add( void)
{
   struct job   adder = { 0 };
   int          failed;

   setup( 8);

   adder.role  = ROLE_ADDER;
   adder.op    = OP_ADD;
   adder.value = (void *) 0x20000;
   arm( ROLE_ADDER, POINTERARRAY_RACE_ADD);
   start( &adder);
   wait_parked( ROLE_ADDER);

   migrate();

   release( ROLE_ADDER);
   mulle_thread_join( g_threads[ ROLE_ADDER]);

   failed = check_dense_prefix( "grow_add", 9);
   if( mulle_concurrent_pointerarray_get( &g_array, 8) != (void *) 0x20000)
   {
      fprintf( stderr, "grow_add: added value is not at the end\n");
      failed = 1;
   }

   teardown();
   return( failed);
}


static int   scenario_slot_race( void)
{
   struct job   adder = { 0 };
   struct job   adder2 = { 0 };
   void         *v0;
   void         *v1;
   int          failed;

   setup( 7);

   adder.role   = ROLE_ADDER;
   adder.op     = OP_ADD;
   adder.value  = (void *) 0x20000;
   adder2.role  = ROLE_ADDER2;
   adder2.op    = OP_ADD;
   adder2.value = (void *) 0x20010;

   arm( ROLE_ADDER, POINTERARRAY_RACE_ADD_SLOT);
   arm( ROLE_ADDER2, POINTERARRAY_RACE_ADD_SLOT);
   start( &adder);
   start( &adder2);
   wait_parked( ROLE_ADDER);
   wait_parked( ROLE_ADDER2);

   release( ROLE_ADDER);
   release( ROLE_ADDER2);
   mulle_thread_join( g_threads[ ROLE_ADDER]);
   mulle_thread_join( g_threads[ ROLE_ADDER2]);

   failed = check_dense_prefix( "slot_race", 9);
   v0 = mulle_concurrent_pointerarray_get( &g_array, 7);
   v1 = mulle_concurrent_pointerarray_get( &g_array, 8);
   if( ! ((v0 == (void *) 0x20000 && v1 == (void *) 0x20010) ||
          (v0 == (void *) 0x20010 && v1 == (void *) 0x20000)))
   {
      fprintf( stderr, "slot_race: the two adds are not both at the end\n");
      failed = 1;
   }

   teardown();
   return( failed);
}


static int   scenario_partial_migration( void)
{
   unsigned int   i;
   int            failed;

   setup( 4);

   migrate();

   failed = check_dense_prefix( "partial_migration", 4);
   for( i = 0; i < 4; i++)
      if( mulle_concurrent_pointerarray_get( &g_array, i) != nth_value( i))
      {
         fprintf( stderr, "partial_migration: index %u changed\n", i);
         failed = 1;
      }

   teardown();
   return( failed);
}


static int   scenario_early_lost_add( void)
{
   struct job   adder = { 0 };
   int          failed;

   setup( 4);

   adder.role  = ROLE_ADDER;
   adder.op    = OP_ADD;
   adder.value = (void *) 0x20000;
   arm( ROLE_ADDER, POINTERARRAY_RACE_ADD);
   start( &adder);
   wait_parked( ROLE_ADDER);

   migrate();

   release( ROLE_ADDER);
   mulle_thread_join( g_threads[ ROLE_ADDER]);

   failed = check_dense_prefix( "early_lost_add", 5);
   if( mulle_concurrent_pointerarray_get( &g_array, 4) != (void *) 0x20000)
   {
      fprintf( stderr, "early_lost_add: add lost in the retired generation\n");
      failed = 1;
   }

   teardown();
   return( failed);
}


static int   scenario_stalled_publisher( void)
{
   struct job   adder = { 0 };
   struct job   helper = { 0 };
   int          failed;

   setup( 7);

   // A claimed the last slot but parks before publishing n
   adder.role  = ROLE_ADDER;
   adder.op    = OP_ADD;
   adder.value = (void *) 0x20000;
   arm( ROLE_ADDER, POINTERARRAY_RACE_ADD_CLAIMED);
   start( &adder);
   wait_parked( ROLE_ADDER);

   // B must complete without A being released
   helper.role  = ROLE_ADDER2;
   helper.op    = OP_ADD;
   helper.value = (void *) 0x20010;
   arm( ROLE_ADDER2, 0);
   start( &helper);
   mulle_thread_join( g_threads[ ROLE_ADDER2]);

   failed = check_dense_prefix( "stalled_publisher", 9);
   if( mulle_concurrent_pointerarray_get( &g_array, 8) != (void *) 0x20010)
   {
      fprintf( stderr, "stalled_publisher: helper value not at the end\n");
      failed = 1;
   }

   release( ROLE_ADDER);
   mulle_thread_join( g_threads[ ROLE_ADDER]);

   if( check_dense_prefix( "stalled_publisher after release", 9))
      failed = 1;
   if( ! mulle_concurrent_pointerarray_find( &g_array, (void *) 0x20000))
   {
      fprintf( stderr, "stalled_publisher: parked writer value lost\n");
      failed = 1;
   }

   teardown();
   return( failed);
}


static int   scenario_publish_prefix( void)
{
   struct _mulle_concurrent_pointerarraystorage   *p;
   unsigned int                                   i;
   int                                            failed;

   setup( 5);

   // simulate three filled but unpublished slots, as a stalled publisher would
   // leave them
   p = _mulle_atomic_pointer_read_acquire( &g_array.storage.pointer);
   for( i = 5; i < 8; i++)
      _mulle_atomic_pointer_nonatomic_write( &p->entries[ i],
                                             (void *) (uintptr_t) (0x30000 + i * 0x10));

   _mulle_concurrent_pointerarraystorage_publish( p);

   failed = 0;
   if( mulle_concurrent_pointerarray_get_count( &g_array) != 8)
   {
      fprintf( stderr, "publish_prefix: publish did not advance the prefix\n");
      failed = 1;
   }

   // a following add must use the published count and append after the prefix
   mulle_concurrent_pointerarray_add( &g_array, (void *) 0x40000);

   failed |= check_dense_prefix( "publish_prefix", 9);
   if( mulle_concurrent_pointerarray_get( &g_array, 8) != (void *) 0x40000)
   {
      fprintf( stderr, "publish_prefix: add did not append\n");
      failed = 1;
   }
   for( i = 5; i < 8; i++)
      if( mulle_concurrent_pointerarray_get( &g_array, i) !=
          (void *) (uintptr_t) (0x30000 + i * 0x10))
      {
         fprintf( stderr, "publish_prefix: index %u changed\n", i);
         failed = 1;
      }

   teardown();
   return( failed);
}


static int   scenario_destination_retired( void)
{
   struct job   copier = { 0 };
   struct job   helper = { 0 };
   unsigned int i;
   int          failed;

   setup( 8);

   copier.role = ROLE_COPIER;
   copier.op   = OP_MIGRATE;
   arm( ROLE_COPIER, POINTERARRAY_RACE_COPY_READ);
   start( &copier);
   wait_parked( ROLE_COPIER);

   // p -> q
   helper.role = ROLE_COPIER2;
   helper.op   = OP_MIGRATE;
   arm( ROLE_COPIER2, 0);
   start( &helper);
   mulle_thread_join( g_threads[ ROLE_COPIER2]);

   // q -> r -> s while the stale copier is still parked
   migrate();
   migrate();

   release( ROLE_COPIER);
   mulle_thread_join( g_threads[ ROLE_COPIER]);

   failed = check_dense_prefix( "destination_retired", 8);
   for( i = 0; i < 8; i++)
      if( mulle_concurrent_pointerarray_get( &g_array, i) != nth_value( i))
      {
         fprintf( stderr, "destination_retired: index %u changed\n", i);
         failed = 1;
      }

   mulle_concurrent_pointerarray_add( &g_array, (void *) 0x50000);
   failed |= check_dense_prefix( "destination_retired after add", 9);

   teardown();
   return( failed);
}


static int   scenario_stale_copier( int carried_stall)
{
   struct job   copier = { 0 };
   struct job   helper = { 0 };
   unsigned int i;
   int          failed;

   setup( 8);

   copier.role = ROLE_COPIER;
   copier.op   = OP_MIGRATE;
   arm( ROLE_COPIER, carried_stall ? POINTERARRAY_RACE_COPY_CARRIED
                                   : POINTERARRAY_RACE_COPY_READ);
   start( &copier);
   wait_parked( ROLE_COPIER);

   helper.role = ROLE_COPIER2;
   helper.op   = OP_MIGRATE;
   start( &helper);
   mulle_thread_join( g_threads[ ROLE_COPIER2]);

   release( ROLE_COPIER);
   mulle_thread_join( g_threads[ ROLE_COPIER]);

   failed = check_dense_prefix( carried_stall ? "carried_stall" : "stale_copier",
                                8);
   for( i = 0; i < 8; i++)
      if( mulle_concurrent_pointerarray_get( &g_array, i) != nth_value( i))
      {
         fprintf( stderr, "%s: index %u changed\n",
                          carried_stall ? "carried_stall" : "stale_copier", i);
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

   failed  = scenario_grow_add();
   failed |= scenario_slot_race();
   failed |= scenario_partial_migration();
   failed |= scenario_early_lost_add();
   failed |= scenario_stalled_publisher();
   failed |= scenario_publish_prefix();
   failed |= scenario_destination_retired();
   failed |= scenario_stale_copier( 0);
   failed |= scenario_stale_copier( 1);

   mulle_aba_unregister();
   mulle_allocator_set_aba( &mulle_allocator_default, NULL, NULL);
   mulle_aba_done();
   mulle_thread_tss_free( g_role_key);

   return( failed ? 1 : 0);
}
