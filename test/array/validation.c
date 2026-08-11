#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-testallocator/mulle-testallocator.h>
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


int   main( void)
{
   struct mulle_concurrent_pointerarray             array;
   struct mulle_concurrent_pointerarrayenumerator   rover;
   struct mulle_concurrent_pointerarrayreverseenumerator   reverse_rover;

   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   check( mulle_concurrent_pointerarray_init( NULL, 0, NULL) == EINVAL,
          "pointerarray init NULL");
   check( mulle_concurrent_pointerarray_init( &array, 0, NULL) == 0,
          "pointerarray init");
   check( mulle_concurrent_pointerarray_add( NULL, (void *) 1) == EINVAL,
          "pointerarray add NULL");
   check( mulle_concurrent_pointerarray_add( &array, MULLE_CONCURRENT_NO_POINTER) == EINVAL,
          "pointerarray add NULL value");
   check( mulle_concurrent_pointerarray_add( &array, MULLE_CONCURRENT_INVALID_POINTER) == EINVAL,
          "pointerarray add invalid value");
   check( mulle_concurrent_pointerarray_find( NULL, (void *) 1) == EINVAL,
          "pointerarray find NULL");
   check( mulle_concurrent_pointerarray_find( &array, MULLE_CONCURRENT_NO_POINTER) == EINVAL,
          "pointerarray find NULL value");
   check( mulle_concurrent_pointerarray_find( &array, MULLE_CONCURRENT_INVALID_POINTER) == EINVAL,
          "pointerarray find invalid value");
   check( mulle_concurrent_pointerarray_get( NULL, 0) == NULL,
          "pointerarray get NULL");
   check( mulle_concurrent_pointerarray_get( &array, 0) == NULL,
          "pointerarray get empty");

   rover = mulle_concurrent_pointerarray_enumerate( NULL);
   check( mulle_concurrent_pointerarrayenumerator_next( &rover) == NULL,
          "pointerarray enumerate NULL");
   mulle_concurrent_pointerarrayenumerator_done( &rover);

   reverse_rover = mulle_concurrent_pointerarray_reverseenumerate( NULL, 0);
   check( mulle_concurrent_pointerarrayreverseenumerator_next( &reverse_rover) == NULL,
          "pointerarray reverse enumerate NULL");
   mulle_concurrent_pointerarrayreverseenumerator_done( &reverse_rover);

   check( mulle_concurrent_pointerarray_add( &array, (void *) 10) == 0,
          "pointerarray add valid");
   check( mulle_concurrent_pointerarray_get( &array, 0) == (void *) 10,
          "pointerarray get valid");
   check( mulle_concurrent_pointerarray_get( &array, 1) == NULL,
          "pointerarray get out of range");

   // find / duplicate / convenience semantics
   check( mulle_concurrent_pointerarray_find( &array, (void *) 10) == 1,
          "pointerarray find present");
   check( mulle_concurrent_pointerarray_find( &array, (void *) 0x7777) == 0,
          "pointerarray find missing");
   check( mulle_concurrent_pointerarray_add( &array, (void *) 10) == 0,
          "pointerarray duplicate add");
   check( mulle_concurrent_pointerarray_add( &array, (void *) 20) == 0,
          "pointerarray add third");
   check( mulle_concurrent_pointerarray_get_count( &array) == 3,
          "pointerarray count");
   check( mulle_concurrent_pointerarray_get_size( NULL) == 0,
          "pointerarray get_size NULL");
   check( mulle_concurrent_pointerarray_get_count( NULL) == 0,
          "pointerarray get_count NULL");
   check( mulle_concurrent_pointerarray_map( NULL, NULL, NULL) == 0,
          "pointerarray map NULL");
   check( mulle_concurrent_pointerarrayenumerator_next( NULL) == NULL,
          "pointerarray enumerator NULL rover");
   check( mulle_concurrent_pointerarrayreverseenumerator_next( NULL) == NULL,
          "pointerarray reverse enumerator NULL rover");

   reverse_rover = mulle_concurrent_pointerarray_reverseenumerate( &array, 3);
   check( mulle_concurrent_pointerarrayreverseenumerator_next( &reverse_rover) == (void *) 20,
          "pointerarray reverse order");
   mulle_concurrent_pointerarrayreverseenumerator_done( &reverse_rover);

   mulle_concurrent_pointerarray_done( &array);
   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   printf( "PASSED\n");
   return( 0);
}
