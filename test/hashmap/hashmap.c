//
//  main.m
//  mulle_concurrent_hashmap
//
//  Created by Nat! on 04.03.16.
//  Copyright © 2016 Nat! for Mulle kybernetiK.
//  Copyright © 2016 Codeon GmbH.
//  All rights reserved.
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
#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-testallocator/mulle-testallocator.h>
#include <mulle-allocator/mulle-allocator.h>
#include <assert.h>
#include <errno.h>


#define FOREVER   0
//#define ENTERTAIN 1


// sunos rand only does 16 bit randomnes, which makes this test never
// finish, since we can't generate enough random hashes

/* xorshift64s, variant A_1(12,25,27) with multiplier M_32 from line 3 of table 5 */
static intptr_t xorshift64star( void)
{
    static uint64_t x = 1; /* initial seed must be nonzero, don't use a static variable for the state if multithreaded */
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    return (intptr_t) (x * 0x2545F4914F6CDD1DULL);
}


static int   insert_something( struct mulle_concurrent_hashmap *map)
{
   intptr_t   hash;
   void       *value;
   int        rval;

   do
   {
      hash  = xorshift64star() << 1;  // no uneven ids
      value = (void *) (hash * 10);
   }
   while( hash == MULLE_CONCURRENT_NO_HASH ||
          value == MULLE_CONCURRENT_NO_POINTER ||
          value == MULLE_CONCURRENT_INVALID_POINTER);

   rval  = _mulle_concurrent_hashmap_insert( map, hash, value);
   if( ! rval)
      return( 1);
   if( rval == EEXIST)
      return( 0);

   perror( "mulle_concurrent_hashmap_insert");
   abort();
}


static int  delete_something( struct mulle_concurrent_hashmap *map)
{
   intptr_t   hash;
   void      *value;
   int       rval;

   do
   {
      hash  = xorshift64star() << 1;  // no uneven ids
      value = (void *) (hash * 10);
   }
   while( hash == MULLE_CONCURRENT_NO_HASH ||
          value == MULLE_CONCURRENT_NO_POINTER ||
          value == MULLE_CONCURRENT_INVALID_POINTER);

   rval = _mulle_concurrent_hashmap_remove( map, hash, value);
   if( rval == ENOMEM)
   {
      perror( "mulle_concurrent_hashmap_remove");
      abort();
   }
   return( rval != ENOENT);
}


static void  lookup_something( struct mulle_concurrent_hashmap *map)
{
   intptr_t   hash;
   void      *value;

   do
   {
      hash = xorshift64star();
   }
   while( hash == MULLE_CONCURRENT_NO_HASH);

   value = _mulle_concurrent_hashmap_lookup( map, hash);
   if( ! value)
      return;

   assert( ! (hash & 0x1));
   assert( value == (void *) (hash * 10));
}


static void  enumerate_something( struct mulle_concurrent_hashmap *map)
{
   struct mulle_concurrent_hashmapenumerator   rover;
   intptr_t                                    hash;
   void                                        *value;
   int                                         rval;

retry:
   rover = mulle_concurrent_hashmap_enumerate( map);
   while( (rval = _mulle_concurrent_hashmapenumerator_next( &rover, &hash, &value)) == 1)
   {
      assert( ! (hash & 0x1));
      if( value != (void *) (hash * 10))
      {
         fprintf( stderr, "expected %p, got %p\n", (void *) (hash * 10), value);
         exit( 1);
      }
   }
   mulle_concurrent_hashmapenumerator_done( &rover);

   if( rval == EBUSY)
      goto retry;
}


//
// run until the mask is >= 1 mio entries
//
static void  tester( struct mulle_concurrent_hashmap *map)
{
   int            todo;
   unsigned int   size;
   unsigned int   count;
   unsigned int   inserts;
   unsigned int   deletes;

   mulle_aba_register();
   inserts=0;
   deletes=0;
   while( (size = mulle_concurrent_hashmap_get_size( map)) < 1024 * 1024)
   {
      todo = xorshift64star() % 997; //  prime number
      if( todo == 1)   // 0.1 % chance of enumerate
      {
#if ENTERTAIN
         count = mulle_concurrent_hashmap_count( map);
         fprintf( stderr, "Size %0u of %0u (+%u,-%u) enumerate\n", count, size, inserts, deletes);
#endif
         enumerate_something( map);
         continue;
      }

      if( todo < 200 + 1)  // 20 % chance of insert
      {
         inserts += insert_something( map);
         continue;
      }

      if( todo < 100 + 200 + 1)    // 10% chance of delete
      {
         deletes += delete_something( map);
         continue;
      }


      lookup_something( map);
   }
   mulle_aba_unregister();
}



static void  multi_threaded_test( unsigned int n_threads)
{
   struct mulle_concurrent_hashmap   map;
   mulle_thread_t                    threads[ 32];
   unsigned int                      i;


   assert( n_threads <= 32);

   mulle_aba_init( &mulle_testallocator);
   mulle_allocator_set_aba( &mulle_testallocator,
                            mulle_aba_get_global(),
                            (mulle_allocator_aba_t *) _mulle_aba_free_owned_pointer);
   mulle_aba_register();

   mulle_concurrent_hashmap_init( &map, 0, &mulle_testallocator);

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

   mulle_concurrent_hashmap_done( &map);

   mulle_aba_unregister();
   mulle_allocator_set_aba( &mulle_testallocator, NULL, NULL);
   mulle_aba_done();
}


static void  single_threaded_test( void)
{
   intptr_t                                    hash;
   struct mulle_concurrent_hashmap             map;
   struct mulle_concurrent_hashmapenumerator   rover;
   unsigned int                                i;
   void                                        *value;
   int                                         rval;

   mulle_aba_init( &mulle_testallocator);
   mulle_allocator_set_aba( &mulle_testallocator,
                            mulle_aba_get_global(),
                            (mulle_allocator_aba_t *) _mulle_aba_free);
   mulle_aba_register();

   mulle_concurrent_hashmap_init( &map, 0, &mulle_testallocator);
   {
      for( i = 1; i <= 100; i++)
      {
         hash  = i;
         value = (void *) (uintptr_t) (i * 10);
         if( mulle_concurrent_hashmap_insert( &map, hash, value))
         {
            perror( "mulle_concurrent_hashmap_insert");
            abort();
         }
         assert( mulle_concurrent_hashmap_lookup( &map, i) == (void *) (intptr_t) (i * 10));
      }

      for( i = 1; i <= 100; i++)
         assert( mulle_concurrent_hashmap_lookup( &map, i) == (void *) (intptr_t) (i * 10));

      assert( ! mulle_concurrent_hashmap_lookup( &map, 101));

      i = 0;
      rover = mulle_concurrent_hashmap_enumerate( &map);
      while( mulle_concurrent_hashmapenumerator_next( &rover, &hash, &value) == 1)
      {
         assert( value == (void *) (hash * 10));
         ++i;
         assert( i <= 100);
      }
      mulle_concurrent_hashmapenumerator_done( &rover);
      assert( i == 100);

      // same as above just with a for loop
      i = 0;
      mulle_concurrent_hashmap_for( &map, hash, value, rval)
      {
         assert( value == (void *) (hash * 10));
         ++i;
         assert( i <= 100);
      }
      assert( i == 100);

      mulle_concurrent_hashmap_remove( &map, 50, (void *) (50 * 10));
      assert( mulle_concurrent_hashmap_lookup( &map, 50) == NULL);

      i = 0;
      rover = mulle_concurrent_hashmap_enumerate( &map);
      while( mulle_concurrent_hashmapenumerator_next( &rover, &hash, &value) == 1)
      {
         assert( value == (void *) (hash * 10));
         ++i;
         assert( i <= 99);
      }
      mulle_concurrent_hashmapenumerator_done( &rover);
      assert( i == 99);
   }
   mulle_concurrent_hashmap_done( &map);

   mulle_aba_unregister();
   mulle_allocator_set_aba( &mulle_testallocator, NULL, NULL);
   mulle_aba_done();
}


int   main(int argc, const char * argv[])
{
   mulle_testallocator_reset();

   do
   {
      single_threaded_test();
      mulle_testallocator_reset();

      multi_threaded_test( 1);
      mulle_testallocator_reset();

      multi_threaded_test( 2);
      mulle_testallocator_reset();

      multi_threaded_test( 32);
      mulle_testallocator_reset();
   }
   while( FOREVER);

   return( 0);
}
