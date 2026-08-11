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

   printf( "FAILED: %s\n", name);
   exit( 1);
}


static int   is_power_of_two( unsigned int n)
{
   return( n && ! (n & (n - 1)));
}


static void  init_size_test( void)
{
   struct mulle_concurrent_pointerset   set;
   unsigned int                         i;
   unsigned int                         size;

   for( i = 0; i <= 8; i += 1)
   {
      if( i == 3 || i == 5 || i == 6 || i == 7)
         continue;  // not a power of two, not a supported init size

      mulle_concurrent_pointerset_init( &set, i, NULL);
      size = mulle_concurrent_pointerset_get_size( &set);
      // size 0 uses the shared empty storage, which has mask 0 -> size 1
      if( i == 0)
         check( size == 1, "init size 0" );
      else
      {
         check( size >= i, "init size capacity" );
         check( is_power_of_two( size), "init size power of two" );
      }
      mulle_concurrent_pointerset_done( &set);
   }
}


static void  load_threshold_test( void)
{
   struct mulle_concurrent_pointerset   set;
   unsigned int                         i;

   mulle_concurrent_pointerset_init( &set, 8, NULL);
   check( mulle_concurrent_pointerset_get_size( &set) == 8, "load threshold base" );

   // growth is triggered at 50 % load: 4 live entries fit into 8 slots
   for( i = 1; i <= 4; i++)
      check( mulle_concurrent_pointerset_insert( &set, (void *)(uintptr_t) i) == 0,
             "load threshold insert" );
   check( mulle_concurrent_pointerset_get_size( &set) == 8, "load threshold no grow yet" );
   check( mulle_concurrent_pointerset_count( &set) == 4, "load threshold count" );

   check( mulle_concurrent_pointerset_insert( &set, (void *) 5) == 0, "load threshold insert grow" );
   check( mulle_concurrent_pointerset_get_size( &set) == 16, "load threshold grew" );
   check( mulle_concurrent_pointerset_count( &set) == 5, "load threshold count after grow" );

   for( i = 1; i <= 5; i++)
      check( mulle_concurrent_pointerset_member( &set, (void *)(uintptr_t) i) == 1,
             "load threshold survived migration" );

   mulle_concurrent_pointerset_done( &set);
}


static void  tombstone_growth_test( void)
{
   struct mulle_concurrent_pointerset   set;
   unsigned int                         i;

   mulle_concurrent_pointerset_init( &set, 8, NULL);

   for( i = 1; i <= 4; i++)
      check( mulle_concurrent_pointerset_insert( &set, (void *)(uintptr_t) i) == 0,
             "tombstone insert" );

   // removing leaves tombstones; the slots stay consumed, so the next
   // insert has to grow the set even though the live count is small
   for( i = 1; i <= 3; i++)
      check( mulle_concurrent_pointerset_remove( &set, (void *)(uintptr_t) i) == 0,
             "tombstone remove" );
   check( mulle_concurrent_pointerset_get_size( &set) == 8, "tombstone no grow yet" );
   check( mulle_concurrent_pointerset_count( &set) == 1, "tombstone live count" );

   check( mulle_concurrent_pointerset_insert( &set, (void *) 0x9999) == 0,
          "tombstone insert grows" );
   check( mulle_concurrent_pointerset_get_size( &set) == 16, "tombstone grew" );
   check( mulle_concurrent_pointerset_count( &set) == 2, "tombstone count after grow" );
   check( mulle_concurrent_pointerset_member( &set, (void *) 4) == 1, "tombstone survivor" );

   mulle_concurrent_pointerset_done( &set);
}


static void  reset_test( void)
{
   struct mulle_concurrent_pointerset   set;
   unsigned int                         i;
   unsigned int                         size;

   mulle_concurrent_pointerset_init( &set, 4, NULL);

   for( i = 1; i <= 100; i++)
      check( mulle_concurrent_pointerset_insert( &set, (void *)(uintptr_t)(i * 16 + 1)) == 0,
             "reset insert" );
   check( mulle_concurrent_pointerset_get_size( &set) >= 128, "reset grew" );

   for( i = 1; i <= 100; i += 2)
      check( mulle_concurrent_pointerset_remove( &set, (void *)(uintptr_t)(i * 16 + 1)) == 0,
             "reset remove" );
   check( mulle_concurrent_pointerset_count( &set) == 50, "reset count" );

   size = mulle_concurrent_pointerset_get_size( &set);
   mulle_concurrent_pointerset_reset( &set);
   check( mulle_concurrent_pointerset_count( &set) == 0, "reset empty" );
   check( mulle_concurrent_pointerset_get_size( &set) == size, "reset size preserved" );

   check( mulle_concurrent_pointerset_insert( &set, (void *) 0x100) == 0, "reset reuse" );
   check( mulle_concurrent_pointerset_member( &set, (void *) 0x100) == 1, "reset member" );

   mulle_concurrent_pointerset_done( &set);
}


#define N_LARGE    (1 << 20)

static void  large_capacity_test( void)
{
   struct mulle_concurrent_pointerset   set;
   unsigned int                         i;

   mulle_concurrent_pointerset_init( &set, 1024, NULL);

   for( i = 0; i < N_LARGE; i++)
      check( mulle_concurrent_pointerset_insert( &set, (void *)(uintptr_t)(i * 2 + 1)) == 0,
             "large insert" );

   check( mulle_concurrent_pointerset_count( &set) == N_LARGE, "large count" );
   check( mulle_concurrent_pointerset_get_size( &set) >= N_LARGE, "large size" );
   check( mulle_concurrent_pointerset_member( &set, (void *) 1) == 1, "large member first" );
   check( mulle_concurrent_pointerset_member( &set, (void *)(uintptr_t)((N_LARGE - 1) * 2 + 1)) == 1,
          "large member last" );
   check( mulle_concurrent_pointerset_member( &set, (void *) 0xAAAAAAAA) == 0,
          "large member absent" );

   mulle_concurrent_pointerset_done( &set);
}


int   main( void)
{
   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   init_size_test();
   load_threshold_test();
   tombstone_growth_test();
   reset_test();
   large_capacity_test();

   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   printf( "PASSED\n" );
   return( 0);
}
