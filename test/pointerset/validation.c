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
   struct mulle_concurrent_pointerset             set;
   struct mulle_concurrent_pointerset_enumerator  rover;
   void                                           *result;

   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   check( mulle_concurrent_pointerset_init( NULL, 0, NULL) == EINVAL,
          "pointerset init NULL");
   check( mulle_concurrent_pointerset_init( &set, 0, NULL) == 0,
          "pointerset init");
   check( mulle_concurrent_pointerset_insert( NULL, (void *) 1) == EINVAL,
          "pointerset insert NULL set");
   check( mulle_concurrent_pointerset_insert( &set, NULL) == EINVAL,
          "pointerset insert NULL value");
   check( mulle_concurrent_pointerset_insert( &set, MULLE_CONCURRENT_INVALID_POINTER) == EINVAL,
          "pointerset insert invalid value");
   check( mulle_concurrent_pointerset_insert( &set, MULLE_CONCURRENT_TOMBSTONE_POINTER) == EINVAL,
          "pointerset insert tombstone value");
   check( mulle_concurrent_pointerset_remove( NULL, (void *) 1) == EINVAL,
          "pointerset remove NULL set");
   check( mulle_concurrent_pointerset_remove( &set, NULL) == EINVAL,
          "pointerset remove NULL value");
   check( mulle_concurrent_pointerset_remove( &set, MULLE_CONCURRENT_INVALID_POINTER) == EINVAL,
          "pointerset remove invalid value");
   check( mulle_concurrent_pointerset_remove( &set, MULLE_CONCURRENT_TOMBSTONE_POINTER) == EINVAL,
          "pointerset remove tombstone value");
   check( mulle_concurrent_pointerset_member( NULL, (void *) 1) == 0,
          "pointerset member NULL set");
   check( mulle_concurrent_pointerset_member( &set, NULL) == 0,
          "pointerset member NULL value");

   errno = 0;
   result = mulle_concurrent_pointerset_register( NULL, (void *) 1);
   check( result == MULLE_CONCURRENT_INVALID_POINTER && errno == EINVAL,
          "pointerset register NULL set");
   result = mulle_concurrent_pointerset_register( &set, NULL);
   check( result == MULLE_CONCURRENT_INVALID_POINTER,
          "pointerset register NULL value");
   result = mulle_concurrent_pointerset_register( &set, MULLE_CONCURRENT_INVALID_POINTER);
   check( result == MULLE_CONCURRENT_INVALID_POINTER,
          "pointerset register invalid value");
   result = mulle_concurrent_pointerset_register( &set, MULLE_CONCURRENT_TOMBSTONE_POINTER);
   check( result == MULLE_CONCURRENT_INVALID_POINTER,
          "pointerset register tombstone value");

   rover = mulle_concurrent_pointerset_enumerate( NULL);
   check( mulle_concurrent_pointerset_enumerator_next( &rover, NULL) == 0,
          "pointerset enumerate NULL");
   mulle_concurrent_pointerset_enumerator_done( &rover);

   check( mulle_concurrent_pointerset_insert( &set, (void *) 10) == 0,
          "pointerset insert valid");
   check( mulle_concurrent_pointerset_member( &set, (void *) 10) == 1,
          "pointerset member valid");

   // duplicate / register / convenience semantics
   check( mulle_concurrent_pointerset_insert( &set, (void *) 10) == EEXIST,
          "pointerset duplicate insert");
   check( mulle_concurrent_pointerset_remove( &set, (void *) 0xDEAD) == ENOENT,
          "pointerset remove missing");
   check( mulle_concurrent_pointerset_register( &set, (void *) 20) == MULLE_CONCURRENT_NO_POINTER,
          "pointerset register fresh");
   check( mulle_concurrent_pointerset_register( &set, (void *) 20) == (void *) 20,
          "pointerset register existing");
   check( mulle_concurrent_pointerset_count( NULL) == 0,
          "pointerset count NULL");
   check( mulle_concurrent_pointerset_lookup_any( NULL) == NULL,
          "pointerset lookup_any NULL");
   check( mulle_concurrent_pointerset_get_size( NULL) == 0,
          "pointerset get_size NULL");
   check( mulle_concurrent_pointerset_get_allocator( NULL) == NULL,
          "pointerset get_allocator NULL");
   check( mulle_concurrent_pointerset_enumerator_next( NULL, NULL) == EINVAL,
          "pointerset enumerator NULL rover");
   check( mulle_concurrent_pointerset_count( &set) == 2,
          "pointerset count");
   check( mulle_concurrent_pointerset_lookup_any( &set) != NULL,
          "pointerset lookup_any");

   mulle_concurrent_pointerset_done( &set);
   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   printf( "PASSED\n");
   return( 0);
}
