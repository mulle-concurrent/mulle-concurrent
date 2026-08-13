//
// Punishing test for mulle_concurrent_hashmap_pose ("poseAs").
//
// Every pose here races a migration on purpose: the map starts undersized and
// insert threads flood it, so generations keep turning over while poses are in
// flight. That is exactly the interleaving that silently reverted the old
// patch() — a migrator reads the pre-pose value, carries it in a register,
// and stores it into the next generation after the writer already returned.
//
// NOTE ON SIZES: a successful pose performs a full migration (it builds the
// next generation with its value pre-seeded, which is what makes it safe), so
// every pose doubles the map. Key counts are therefore deliberately small —
// N successful poses means 2^N growth.
//
#define HAVE_MULLE_CONCURRENT_POSEAS_PATCH
#include <mulle-concurrent/mulle-concurrent.h>
#include <mulle-testallocator/mulle-testallocator.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>


static void   check( int condition, char *name)
{
   if( condition)
      return;
   fprintf( stderr, "FAILED: %s\n", name);
   exit( 1);
}


#define N_INSERT_THREADS      4
#define N_INSERT_PER_THREAD   3000

// insert keys are even, posed keys are odd, so they never collide
#define INSERT_KEY( t, i)     ((intptr_t) (((t) * N_INSERT_PER_THREAD + (i) + 1) * 2))

static mulle_atomic_pointer_t   start_gate;


static void   wait_for_gate( void)
{
   while( ! _mulle_atomic_pointer_read_relaxed( &start_gate))
      ;
}


struct insert_info
{
   struct mulle_concurrent_hashmap   *map;
   unsigned int                      thread;
};


static void   *insert_worker( struct insert_info *info)
{
   unsigned int   i;

   mulle_aba_register();
   wait_for_gate();

   for( i = 0; i < N_INSERT_PER_THREAD; i++)
      _mulle_concurrent_hashmap_insert( info->map, INSERT_KEY( info->thread, i),
                                        (void *) (uintptr_t) (i + 1));

   mulle_aba_unregister();
   return( NULL);
}


#pragma mark - test 1: one pose per key, must never revert

#define N_POSE_KEYS        12
#define POSE_KEY( i)       ((intptr_t) ((i) * 2 + 1))
#define POSE_OLD( i)       ((void *) (uintptr_t) ((i) + 0x10000))
#define POSE_NEW( i)       ((void *) (uintptr_t) ((i) + 0x40000000))


static void   *pose_worker( struct mulle_concurrent_hashmap *map)
{
   unsigned int   i;
   int            rval;

   mulle_aba_register();
   wait_for_gate();

   for( i = 0; i < N_POSE_KEYS; i++)
   {
      rval = _mulle_concurrent_hashmap_pose( map, POSE_KEY( i), POSE_NEW( i), POSE_OLD( i));
      check( rval == 0, "pose returned non-zero");
   }

   mulle_aba_unregister();
   return( NULL);
}


static void   one_pose_per_key_test( void)
{
   struct mulle_concurrent_hashmap   map;
   struct insert_info                infos[ N_INSERT_THREADS];
   mulle_thread_t                    inserters[ N_INSERT_THREADS];
   mulle_thread_t                    poser;
   unsigned int                      i;
   unsigned int                      n_stale;
   void                              *found;

   // start small so migrations begin immediately
   mulle_concurrent_hashmap_init( &map, 8, NULL);
   _mulle_atomic_pointer_write( &start_gate, NULL);

   for( i = 0; i < N_POSE_KEYS; i++)
      check( _mulle_concurrent_hashmap_insert( &map, POSE_KEY( i), POSE_OLD( i)) == 0,
             "seed insert");

   for( i = 0; i < N_INSERT_THREADS; i++)
   {
      infos[ i].map    = &map;
      infos[ i].thread = i;
      if( mulle_thread_create( (void *) insert_worker, &infos[ i], &inserters[ i]))
      {
         perror( "mulle_thread_create");
         abort();
      }
   }
   if( mulle_thread_create( (void *) pose_worker, &map, &poser))
   {
      perror( "mulle_thread_create");
      abort();
   }

   _mulle_atomic_pointer_write( &start_gate, (void *) 1);

   mulle_thread_join( poser);
   for( i = 0; i < N_INSERT_THREADS; i++)
      mulle_thread_join( inserters[ i]);

   n_stale = 0;
   for( i = 0; i < N_POSE_KEYS; i++)
   {
      found = _mulle_concurrent_hashmap_lookup( &map, POSE_KEY( i));
      if( found != POSE_NEW( i))
      {
         fprintf( stderr, "key %ld: expected %p, got %p%s\n",
                  (long) POSE_KEY( i), POSE_NEW( i), found,
                  found == POSE_OLD( i) ? " (STALE: pose reverted)" : "");
         ++n_stale;
      }
   }
   if( n_stale)
   {
      fprintf( stderr, "FAILED: one_pose_per_key: %u of %u poses lost\n",
               n_stale, (unsigned int) N_POSE_KEYS);
      exit( 1);
   }

   printf( "one_pose_per_key: OK\n");
   fprintf( stderr, "one_pose_per_key: %u keys, final size %u\n",
            (unsigned int) N_POSE_KEYS,
            mulle_concurrent_hashmap_get_size( &map));

   mulle_concurrent_hashmap_done( &map);
}


#pragma mark - test 2: contended pose, exactly one winner

//
// Several threads race to pose the *same* key, each with its own distinct
// final value. Exactly one must win; the others must get EEXIST. And the
// winner's value must be what the map reports at the end: a winner that got
// reverted, or a loser's value showing up, are both failures.
//
#define N_CONTENDERS       4
#define N_CONTESTS         10
#define CONTEST_KEY( i)    ((intptr_t) ((i) * 2 + 1))
#define CONTEST_OLD( i)    ((void *) (uintptr_t) ((i) + 0x20000))
#define CONTEST_NEW( i, t) ((void *) (uintptr_t) (((i) << 4) + (t) + 0x50000000))

struct contest_info
{
   struct mulle_concurrent_hashmap   *map;
   unsigned int                      thread;
   int                               rvals[ N_CONTESTS];
};


static void   *contest_worker( struct contest_info *info)
{
   unsigned int   i;
   int            rval;

   mulle_aba_register();
   wait_for_gate();

   for( i = 0; i < N_CONTESTS; i++)
   {
      rval = _mulle_concurrent_hashmap_pose( info->map,
                                             CONTEST_KEY( i),
                                             CONTEST_NEW( i, info->thread),
                                             CONTEST_OLD( i));
      check( rval == 0 || rval == EEXIST, "contested pose bad return");
      info->rvals[ i] = rval;
   }

   mulle_aba_unregister();
   return( NULL);
}


static void   contested_pose_test( void)
{
   struct mulle_concurrent_hashmap   map;
   struct contest_info               infos[ N_CONTENDERS];
   mulle_thread_t                    threads[ N_CONTENDERS];
   struct insert_info                ins_infos[ N_INSERT_THREADS];
   mulle_thread_t                    inserters[ N_INSERT_THREADS];
   unsigned int                      i;
   unsigned int                      t;
   unsigned int                      n_winners;
   unsigned int                      winner;
   void                              *found;

   mulle_concurrent_hashmap_init( &map, 8, NULL);
   _mulle_atomic_pointer_write( &start_gate, NULL);

   for( i = 0; i < N_CONTESTS; i++)
      check( _mulle_concurrent_hashmap_insert( &map, CONTEST_KEY( i), CONTEST_OLD( i)) == 0,
             "contest seed insert");

   for( i = 0; i < N_CONTENDERS; i++)
   {
      infos[ i].map    = &map;
      infos[ i].thread = i;
      if( mulle_thread_create( (void *) contest_worker, &infos[ i], &threads[ i]))
      {
         perror( "mulle_thread_create");
         abort();
      }
   }
   for( i = 0; i < N_INSERT_THREADS; i++)
   {
      ins_infos[ i].map    = &map;
      ins_infos[ i].thread = i;
      if( mulle_thread_create( (void *) insert_worker, &ins_infos[ i], &inserters[ i]))
      {
         perror( "mulle_thread_create");
         abort();
      }
   }

   _mulle_atomic_pointer_write( &start_gate, (void *) 1);

   for( i = 0; i < N_CONTENDERS; i++)
      mulle_thread_join( threads[ i]);
   for( i = 0; i < N_INSERT_THREADS; i++)
      mulle_thread_join( inserters[ i]);

   for( i = 0; i < N_CONTESTS; i++)
   {
      n_winners = 0;
      winner    = 0;
      for( t = 0; t < N_CONTENDERS; t++)
         if( infos[ t].rvals[ i] == 0)
         {
            ++n_winners;
            winner = t;
         }

      if( n_winners != 1)
      {
         fprintf( stderr, "FAILED: contested_pose: key %ld had %u winners\n",
                  (long) CONTEST_KEY( i), n_winners);
         exit( 1);
      }

      found = _mulle_concurrent_hashmap_lookup( &map, CONTEST_KEY( i));
      if( found != CONTEST_NEW( i, winner))
      {
         fprintf( stderr, "FAILED: contested_pose: key %ld winner %u expected %p, got %p%s\n",
                  (long) CONTEST_KEY( i), winner, CONTEST_NEW( i, winner), found,
                  found == CONTEST_OLD( i) ? " (STALE: pose reverted)" : "");
         exit( 1);
      }
   }

   printf( "contested_pose: OK\n");
   fprintf( stderr, "contested_pose: %u keys, %u contenders, final size %u\n",
            (unsigned int) N_CONTESTS, (unsigned int) N_CONTENDERS,
            mulle_concurrent_hashmap_get_size( &map));

   mulle_concurrent_hashmap_done( &map);
}


int   main( void)
{
   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   one_pose_per_key_test();
   contested_pose_test();

   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   printf( "PASSED\n");
   return( 0);
}
