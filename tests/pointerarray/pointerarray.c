//
//  main.c
//  mulle_concurrent_pointerlist
//
//  Created by Nat! on 06.03.16.
//  Copyright © 2016 Mulle kybernetiK. All rights reserved.
//
//  Redistribution and use in source and binary forms, with or without
//  modification, are permitted provided that the following conditions are met:
//
//  Redistributions of source code must retain the above copyright notice, this
//  list of conditions and the following disclaimer.
//
//  Redistributions in binary form must reproduce the above copyright notice,
//  this list of conditions and the following disclaimer in the documentation
//  and/or other materials provided with the distribution.
//
//  Neither the name of Mulle kybernetiK nor the names of its contributors
//  may be used to endorse or promote products derived from this software
//  without specific prior written permission.
//
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
//  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
//  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
//  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
//  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
//  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
//  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
//  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
//  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
//  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
//  POSSIBILITY OF SUCH DAMAGE.
//
#include <mulle_standalone_concurrent/mulle_standalone_concurrent.h>

#include <mulle_test_allocator/mulle_test_allocator.h>
#include <mulle_aba/mulle_aba.h>
#include <assert.h>
#include <errno.h>


#define FOREVER  0


static void  insert_something( struct mulle_concurrent_pointerarray *map)
{
   void   *value;
   int    rval;

   do
      value = (void *) (rand() << 1); // no uneven pointer values
   while( ! value);

   rval = _mulle_concurrent_pointerarray_add( map, value);

   switch( rval)
   {
   default :
      perror( "_mulle_concurrentpointerarray_add");
      abort();

   case 0 :
   case EEXIST :
      return;
   }
}


static void  lookup_something( struct mulle_concurrent_pointerarray *map)
{
   void           *value;
   unsigned int   i;
   unsigned int   n;

   n = mulle_concurrent_pointerarray_get_count( map);
   if( ! n)
      return;

   i     = rand() % n;
   value = _mulle_concurrent_pointerarray_get( map, i);
   assert( value);
   assert( ! ((uintptr_t) value & 0x1));
}


static void  enumerate_something( struct mulle_concurrent_pointerarray *map)
{
   struct mulle_concurrent_pointerarrayenumerator   rover;
   void                                             *value;
   int                                              rval;

retry:
   rover = mulle_concurrent_pointerarray_enumerate( map);
   for(;;)
   {
      rval = _mulle_concurrent_pointerarrayenumerator_next( &rover, &value);
      if( ! rval)
         break;
      if( rval == -1)
      {
         _mulle_concurrent_pointerarrayenumerator_done( &rover);
         goto retry;
      }
   }
   _mulle_concurrent_pointerarrayenumerator_done( &rover);
}


//
// run until the mask is >= 1 mio entries
//
static void  tester( struct mulle_concurrent_pointerarray *map)
{
   int   todo;

   mulle_aba_register();

   while( _mulle_concurrent_pointerarray_get_size( map) < 1024 * 1024)
   {
      todo = rand() % 1000;
      if( todo == 1)   // 1 % chance of enumerate
      {
         enumerate_something( map);
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
   struct mulle_concurrent_pointerarray   map;
   mulle_thread_t                         threads[ 32];
   unsigned int                           i;

   assert( n_threads <= 32);

   mulle_aba_init( &mulle_test_allocator);
   mulle_allocator_set_aba( &mulle_test_allocator, mulle_aba_get_global(), (void *) _mulle_aba_free);
   
   _mulle_concurrent_pointerarray_init( &map, 0, &mulle_test_allocator);

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
   _mulle_concurrent_pointerarray_done( &map);
   mulle_aba_unregister();

   mulle_allocator_set_aba( &mulle_test_allocator, NULL, NULL);

   mulle_aba_done();
}


static void  single_threaded_test( void)
{
   struct mulle_concurrent_pointerarray             map;
   struct mulle_concurrent_pointerarrayenumerator   rover;
   unsigned int                                     i;
   void                                             *value;

   mulle_aba_init( &mulle_test_allocator);
   
   mulle_allocator_set_aba( &mulle_test_allocator,  mulle_aba_get_global(), (void *) _mulle_aba_free);
   
   mulle_aba_register();

   _mulle_concurrent_pointerarray_init( &map, 0, &mulle_test_allocator);
   {
      for( i = 1; i <= 100; i++)
      {
         value = (void *) (i * 10);
         if( _mulle_concurrent_pointerarray_add( &map, value))
         {
            perror( "_mulle_concurrent_pointerarray_add");
            abort();
         }
         assert( _mulle_concurrent_pointerarray_get( &map, i - 1) == (void *) (i * 10));
      }

      for( i = 1; i <= 100; i++)
         assert( _mulle_concurrent_pointerarray_get( &map, i - 1) == (void *) (i * 10));

      i = 1;
      rover = mulle_concurrent_pointerarray_enumerate( &map);
      while( _mulle_concurrent_pointerarrayenumerator_next( &rover, &value) == 1)
      {
         assert( value == (void *) (i * 10));
         ++i;
         assert( i <= 101);
      }
      _mulle_concurrent_pointerarrayenumerator_done( &rover);
      assert( i == 101);
   }
   _mulle_concurrent_pointerarray_done( &map);

   mulle_aba_unregister();
   
   mulle_allocator_set_aba( &mulle_test_allocator, NULL, NULL);

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
