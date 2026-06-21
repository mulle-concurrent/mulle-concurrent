#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-testallocator/mulle-testallocator.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>


static void   test_basic( void)
{
   struct mulle_concurrent_pointerset             set;
   struct mulle_concurrent_pointerset_enumerator  rover;
   void                                           *ptr;
   void                                           *p1 = (void *) 0x1000;
   void                                           *p2 = (void *) 0x2000;
   void                                           *p3 = (void *) 0x3000;
   int                                            rval;
   unsigned int                                   count;

   mulle_concurrent_pointerset_init( &set, 0, NULL);
   {
      // insert
      rval = mulle_concurrent_pointerset_insert( &set, p1);
      assert( rval == 0);

      rval = mulle_concurrent_pointerset_insert( &set, p2);
      assert( rval == 0);

      // duplicate
      rval = mulle_concurrent_pointerset_insert( &set, p1);
      assert( rval == EEXIST);

      // member
      assert( mulle_concurrent_pointerset_member( &set, p1) == 1);
      assert( mulle_concurrent_pointerset_member( &set, p2) == 1);
      assert( mulle_concurrent_pointerset_member( &set, p3) == 0);

      // count
      count = mulle_concurrent_pointerset_count( &set);
      assert( count == 2);

      // remove
      rval = mulle_concurrent_pointerset_remove( &set, p1);
      assert( rval == 0);

      assert( mulle_concurrent_pointerset_member( &set, p1) == 0);
      assert( mulle_concurrent_pointerset_member( &set, p2) == 1);

      // remove non-existent
      rval = mulle_concurrent_pointerset_remove( &set, p3);
      assert( rval == ENOENT);

      // re-insert after remove (goes into new slot, tombstone stays)
      rval = mulle_concurrent_pointerset_insert( &set, p1);
      assert( rval == 0);
      assert( mulle_concurrent_pointerset_member( &set, p1) == 1);

      // enumerate
      count = 0;
      rover = mulle_concurrent_pointerset_enumerate( &set);
      while( mulle_concurrent_pointerset_enumerator_next( &rover, &ptr) == 1)
      {
         assert( ptr == p1 || ptr == p2);
         count++;
      }
      mulle_concurrent_pointerset_enumerator_done( &rover);
      assert( count == 2);

      // for macro
      count = 0;
      mulle_concurrent_pointerset_for( &set, ptr)
      {
         assert( ptr == p1 || ptr == p2);
         count++;
      }
      assert( count == 2);

      // lookup_any returns something
      ptr = mulle_concurrent_pointerset_lookup_any( &set);
      assert( ptr == p1 || ptr == p2);
   }
   mulle_concurrent_pointerset_done( &set);
}


// stress: insert enough to trigger a migration
static void   test_grow( void)
{
   struct mulle_concurrent_pointerset   set;
   unsigned int                         i;
   unsigned int                         count;
   uintptr_t                            base = 0x10000;

   mulle_concurrent_pointerset_init( &set, 4, NULL);
   {
      for( i = 1; i <= 200; i++)
         mulle_concurrent_pointerset_insert( &set, (void *) (base + i * 16));

      count = mulle_concurrent_pointerset_count( &set);
      assert( count == 200);

      for( i = 1; i <= 200; i++)
         assert( mulle_concurrent_pointerset_member( &set, (void *) (base + i * 16)) == 1);

      // remove half
      for( i = 1; i <= 100; i++)
         mulle_concurrent_pointerset_remove( &set, (void *) (base + i * 16));

      count = mulle_concurrent_pointerset_count( &set);
      assert( count == 100);
   }
   mulle_concurrent_pointerset_done( &set);
}


// register semantics
static void   test_register( void)
{
   struct mulle_concurrent_pointerset   set;
   void                                 *p = (void *) 0xDEAD0;
   void                                 *result;

   mulle_concurrent_pointerset_init( &set, 0, NULL);
   {
      // first register: inserted → NO_POINTER
      result = mulle_concurrent_pointerset_register( &set, p);
      assert( result == MULLE_CONCURRENT_NO_POINTER);

      // second register: already there → returns p
      result = mulle_concurrent_pointerset_register( &set, p);
      assert( result == p);
   }
   mulle_concurrent_pointerset_done( &set);
}


int   main( void)
{
   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;

   mulle_aba_init( NULL);
   mulle_aba_register();

   test_basic();
   test_grow();
   test_register();

   printf( "OK\n");

   mulle_aba_unregister();
   mulle_aba_done();

   mulle_testallocator_reset();

   return( 0);
}
