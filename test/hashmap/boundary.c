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
   struct mulle_concurrent_hashmap   map;
   unsigned int                      i;
   unsigned int                      size;

   for( i = 0; i <= 8; i += 1)
   {
      if( i == 3 || i == 5 || i == 6 || i == 7)
         continue;  // not a power of two, not a supported init size

      mulle_concurrent_hashmap_init( &map, i, NULL);
      size = mulle_concurrent_hashmap_get_size( &map);
      // size 0 uses the shared empty storage, which has mask 0 -> size 1
      if( i == 0)
         check( size == 1, "init size 0" );
      else
      {
         check( size >= i, "init size capacity" );
         check( is_power_of_two( size), "init size power of two" );
      }
      mulle_concurrent_hashmap_done( &map);
   }

   mulle_concurrent_hashmap_init( &map, 1024, NULL);
   check( mulle_concurrent_hashmap_get_size( &map) == 1024, "init size 1024" );
   mulle_concurrent_hashmap_done( &map);
}


static void  load_threshold_test( void)
{
   struct mulle_concurrent_hashmap   map;
   intptr_t                          i;

   mulle_concurrent_hashmap_init( &map, 8, NULL);
   check( mulle_concurrent_hashmap_get_size( &map) == 8, "load threshold base" );

   // growth is triggered at 50 % load: 4 entries fit into 8 slots
   for( i = 1; i <= 4; i++)
      check( mulle_concurrent_hashmap_insert( &map, i, (void *)(uintptr_t) i) == 0,
             "load threshold insert" );
   check( mulle_concurrent_hashmap_get_size( &map) == 8, "load threshold no grow yet" );
   check( mulle_concurrent_hashmap_count( &map) == 4, "load threshold count" );

   // the fifth insert must trigger a migration to 16 slots
   check( mulle_concurrent_hashmap_insert( &map, 5, (void *)(uintptr_t) 5) == 0,
          "load threshold insert grow" );
   check( mulle_concurrent_hashmap_get_size( &map) == 16, "load threshold grew" );
   check( mulle_concurrent_hashmap_count( &map) == 5, "load threshold count after grow" );

   // nothing was lost during migration
   for( i = 1; i <= 5; i++)
      check( mulle_concurrent_hashmap_lookup( &map, i) == (void *)(uintptr_t) i,
             "load threshold survived migration" );

   mulle_concurrent_hashmap_done( &map);
}



#define N_LARGE    (1 << 20)

static void  large_capacity_test( void)
{
   struct mulle_concurrent_hashmap             map;
   struct mulle_concurrent_hashmapenumerator   rover;
   intptr_t                                    hash;
   void                                        *value;
   unsigned int                                i;
   unsigned int                                enumerated;

   mulle_concurrent_hashmap_init( &map, 1024, NULL);

   for( i = 0; i < N_LARGE; i++)
   {
      hash = (intptr_t)(i * 2 + 1);
      check( mulle_concurrent_hashmap_insert( &map, hash, (void *)(uintptr_t) hash) == 0,
             "large insert" );
   }

   check( mulle_concurrent_hashmap_count( &map) == N_LARGE, "large count" );

   enumerated = 0;
   rover      = mulle_concurrent_hashmap_enumerate( &map);
   while( mulle_concurrent_hashmapenumerator_next( &rover, &hash, &value) == 1)
   {
      check( value == (void *)(uintptr_t) hash, "large enumeration value" );
      enumerated++;
   }
   mulle_concurrent_hashmapenumerator_done( &rover);
   check( enumerated == N_LARGE, "large enumeration count" );

   check( mulle_concurrent_hashmap_lookup( &map, 1) == (void *) 1, "large lookup first" );
   check( mulle_concurrent_hashmap_lookup( &map, (intptr_t)((N_LARGE - 1) * 2 + 1))
             == (void *)(uintptr_t)((N_LARGE - 1) * 2 + 1),
          "large lookup last" );

   mulle_concurrent_hashmap_done( &map);
}


int   main( void)
{
   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   init_size_test();
   load_threshold_test();
   large_capacity_test();

   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   printf( "PASSED\n" );
   return( 0);
}
