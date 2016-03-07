//
//  main.m
//  mulle_concurrent_hashmap
//
//  Created by Nat! on 04.03.16.
//  Copyright © 2016 Mulle kybernetiK. All rights reserved.
//

#include <mulle_standalone_concurrent/mulle_standalone_concurrent.h>

#include <mulle_test_allocator/mulle_test_allocator.h>
#include <mulle_aba/mulle_aba.h>
#include <errno.h>


#define FOREVER  0


static void  insert_something( struct mulle_concurrent_hashmap *map)
{
   intptr_t   hash;
   void       *value;
   int        rval;

   hash = rand() << 1;  // no uneven ids
   value    = (void *) (hash * 10);

   rval = _mulle_concurrent_hashmap_insert( map, hash, value);
   if( rval == 0)
      return;

   switch( errno)
   {
   default :
      perror( "mulle_concurrent_hashmap_insert");
      abort();

   case EEXIST :
      return;
   }
}


static void  delete_something( struct mulle_concurrent_hashmap *map)
{
   intptr_t   hash;
   void      *value;
   int       rval;

   hash = rand() << 1;  // no uneven ids
   value    = (void *) (hash * 10);

   rval = _mulle_concurrent_hashmap_remove( map, hash, value);
   if( rval)
   {
      perror( "mulle_concurrent_hashmap_remove");
      abort();
   }
}


static void  lookup_something( struct mulle_concurrent_hashmap *map)
{
   intptr_t   hash;
   void      *value;

   hash = rand();
   value    = _mulle_concurrent_hashmap_lookup( map, hash);
   if( ! value)
      return;
   assert( ! (hash & 0x1));
   assert( value == (void *) (hash * 10));
}


static void  enumerate_something( struct mulle_concurrent_hashmap *map)
{
   struct mulle_concurrent_hashmapenumerator   rover;
   intptr_t                                     hash;
   void                                        *value;
   int                                         rval;

retry:
   rover = mulle_concurrent_hashmap_enumerate( map);
   for(;;)
   {
      rval = _mulle_concurrent_hashmapenumerator_next( &rover, &hash, &value);
      if( ! rval)
         break;
      if( rval == -1)
      {
         _mulle_concurrent_hashmapenumerator_done( &rover);
         goto retry;
      }
   }
   _mulle_concurrent_hashmapenumerator_done( &rover);
}


//
// run until the mask is >= 1 mio entries
//
static void  tester( struct mulle_concurrent_hashmap *map)
{
   int   todo;

   mulle_aba_register();

   while( _mulle_concurrent_hashmap_get_size( map) < 1024 * 1024)
   {
      todo = rand() % 1000;
      if( todo == 1)   // 1 % chance of enumerate
      {
         enumerate_something( map);
         continue;
      }

      if( todo < 100)    // 10 % chance of delete
      {
         delete_something( map);
         continue;
      }

      if( todo < 200)    // 20 % chance of insert
      {
         insert_something( map);
         continue;
      }

      lookup_something( map);
   }
   mulle_aba_unregister();
}



static void  multi_threaded_test( unsigned int n_threads)
{
   struct mulle_concurrent_hashmap   map;
   mulle_thread_t                             threads[ 32];
   unsigned int                               i;


   assert( n_threads <= 32);

   mulle_aba_init( &mulle_default_allocator);

   _mulle_concurrent_hashmap_init( &map, 0, &mulle_test_allocator, mulle_aba_get_global());

   {
      for( i = 0; i < n_threads; i++)
      {
         if( mulle_thread_create( (void *) tester, &map, &threads[ i]))
         {
            perror( "mulle_thread_create");
            abort();
         }
      }

      for( i = 0; i < n_threads; i++)
         mulle_thread_join( threads[ i]);
   }

   mulle_aba_register();
   _mulle_concurrent_hashmap_free( &map);
   mulle_aba_unregister();

   mulle_aba_done();
}


static void  single_threaded_test( void)
{
   intptr_t                                     hash;
   struct mulle_concurrent_hashmap             map;
   struct mulle_concurrent_hashmapenumerator   rover;
   unsigned int                                i;
   void                                        *value;

   mulle_aba_init( &mulle_default_allocator);
   mulle_aba_register();

   _mulle_concurrent_hashmap_init( &map, 0, &mulle_test_allocator, mulle_aba_get_global());
   {
      for( i = 1; i <= 100; i++)
      {
         hash = i;
         value    = (void *) (i * 10);
         if( _mulle_concurrent_hashmap_insert( &map, hash, value))
         {
            perror( "mulle_concurrent_hashmap_insert");
            abort();
         }
         assert( _mulle_concurrent_hashmap_lookup( &map, i) == (void *) (i * 10));
      }

      for( i = 1; i <= 100; i++)
         assert( _mulle_concurrent_hashmap_lookup( &map, i) == (void *) (i * 10));

      assert( ! _mulle_concurrent_hashmap_lookup( &map, 101));

      i = 0;
      rover = mulle_concurrent_hashmap_enumerate( &map);
      while( _mulle_concurrent_hashmapenumerator_next( &rover, &hash, &value) == 1)
      {
         assert( value == (void *) (hash * 10));
         ++i;
         assert( i <= 100);
      }
      _mulle_concurrent_hashmapenumerator_done( &rover);
      assert( i == 100);

      _mulle_concurrent_hashmap_remove( &map, 50, (void *) (50 * 10));
      assert( _mulle_concurrent_hashmap_lookup( &map, 50) == NULL);

      i = 0;
      rover = mulle_concurrent_hashmap_enumerate( &map);
      while( _mulle_concurrent_hashmapenumerator_next( &rover, &hash, &value) == 1)
      {
         assert( value == (void *) (hash * 10));
         ++i;
         assert( i <= 99);
      }
      _mulle_concurrent_hashmapenumerator_done( &rover);
      assert( i == 99);
   }
   _mulle_concurrent_hashmap_free( &map);

   mulle_aba_unregister();
   mulle_aba_done();
}


int   main(int argc, const char * argv[])
{
   mulle_test_allocator_reset();

   do
   {
      single_threaded_test();
      mulle_test_allocator_reset();

      multi_threaded_test( 1);
      mulle_test_allocator_reset();

      multi_threaded_test( 2);
      mulle_test_allocator_reset();

      multi_threaded_test( 32);
      mulle_test_allocator_reset();
   }
   while( FOREVER);

   return( 0);
}
