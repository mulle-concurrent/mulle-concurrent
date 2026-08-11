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


static void  init_size_test( void)
{
   struct mulle_concurrent_pointerarray   array;

   // zero capacity uses the shared empty storage
   mulle_concurrent_pointerarray_init( &array, 0, NULL);
   check( mulle_concurrent_pointerarray_get_size( &array) == 0, "init size 0" );
   check( mulle_concurrent_pointerarray_get_count( &array) == 0, "count 0" );
   check( mulle_concurrent_pointerarray_get( &array, 0) == NULL, "get empty" );
   mulle_concurrent_pointerarray_done( &array);

   // any initial size is honored, but at least 8
   mulle_concurrent_pointerarray_init( &array, 1, NULL);
   check( mulle_concurrent_pointerarray_get_size( &array) == 8, "init size 1 rounded" );
   mulle_concurrent_pointerarray_done( &array);

   mulle_concurrent_pointerarray_init( &array, 3, NULL);
   check( mulle_concurrent_pointerarray_get_size( &array) == 8, "init size 3 rounded" );
   mulle_concurrent_pointerarray_done( &array);

   mulle_concurrent_pointerarray_init( &array, 8, NULL);
   check( mulle_concurrent_pointerarray_get_size( &array) == 8, "init size 8" );
   mulle_concurrent_pointerarray_done( &array);

   mulle_concurrent_pointerarray_init( &array, 1024, NULL);
   check( mulle_concurrent_pointerarray_get_size( &array) == 1024, "init size 1024" );
   mulle_concurrent_pointerarray_done( &array);
}


static void  grow_test( void)
{
   struct mulle_concurrent_pointerarray   array;
   unsigned int                           i;

   mulle_concurrent_pointerarray_init( &array, 8, NULL);

   for( i = 0; i < 8; i++)
      check( mulle_concurrent_pointerarray_add( &array, (void *)(uintptr_t)(i + 1)) == 0,
             "grow add" );
   check( mulle_concurrent_pointerarray_get_size( &array) == 8, "grow no grow yet" );
   check( mulle_concurrent_pointerarray_get_count( &array) == 8, "grow count" );

   // the ninth add must migrate to a 16 slot storage
   check( mulle_concurrent_pointerarray_add( &array, (void *) 0x4242) == 0, "grow add triggers" );
   check( mulle_concurrent_pointerarray_get_size( &array) == 16, "grow size" );
   check( mulle_concurrent_pointerarray_get_count( &array) == 9, "grow count after" );

   // nothing was lost during migration
   for( i = 0; i < 8; i++)
      check( mulle_concurrent_pointerarray_get( &array, i) == (void *)(uintptr_t)(i + 1),
             "grow survived migration" );
   check( mulle_concurrent_pointerarray_get( &array, 8) == (void *) 0x4242,
          "grow appended value" );

   mulle_concurrent_pointerarray_done( &array);
}


#define N_LARGE    (1 << 20)

static void  large_capacity_test( void)
{
   struct mulle_concurrent_pointerarray   array;
   unsigned int                           i;

   mulle_concurrent_pointerarray_init( &array, 0, NULL);

   for( i = 0; i < N_LARGE; i++)
      check( mulle_concurrent_pointerarray_add( &array, (void *)(uintptr_t)(i * 2 + 1)) == 0,
             "large add" );

   check( mulle_concurrent_pointerarray_get_count( &array) == N_LARGE, "large count" );
   check( mulle_concurrent_pointerarray_get_size( &array) >= N_LARGE, "large size" );
   check( mulle_concurrent_pointerarray_get( &array, 0) == (void *) 1, "large get first" );
   check( mulle_concurrent_pointerarray_get( &array, N_LARGE - 1)
             == (void *)(uintptr_t)((N_LARGE - 1) * 2 + 1),
          "large get last" );
   check( mulle_concurrent_pointerarray_get( &array, N_LARGE) == NULL, "large get past end" );

   mulle_concurrent_pointerarray_done( &array);
}


int   main( void)
{
   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   init_size_test();
   grow_test();
   large_capacity_test();

   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   printf( "PASSED\n" );
   return( 0);
}
