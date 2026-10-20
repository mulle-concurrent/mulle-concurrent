//
//  mulle-concurrent-pointerarray.c
//  mulle-concurrent
//
//  Copyright (c) 2018 Nat! - Mulle kybernetiK.
//  Copyright (c) 2016 Codeon GmbH.
//  All rights reserved.
//
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
#pragma clang diagnostic ignored "-Wparentheses"

#include "mulle-concurrent-pointerarray.h"

#include "mulle-concurrent-types.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>


struct _mulle_concurrent_pointerarraystorage
{
   mulle_atomic_pointer_t   n;
   uintptr_t                size;

   mulle_atomic_pointer_t   entries[ 1];
};


static const struct _mulle_concurrent_pointerarraystorage   empty_storage;

//
// the nice thing about the redirect is, that another thread that is
// trying to add at the same time cooperates
// The negative is, that we encure extra write cycles, if there is no
// other thread
//
#define REDIRECT_VALUE   MULLE_CONCURRENT_INVALID_POINTER


//
// Test scaffolding, same idea as MULLE_CONCURRENT_HASHTABLE_RACE_YIELD.
// Defining MULLE_CONCURRENT_POINTERARRAY_RACE_YIELD widens the growth race
// windows so that the stochastic tests can hit them. Compiles to nothing
// otherwise.
//
#ifdef MULLE_CONCURRENT_POINTERARRAY_RACE_YIELD

#define  pointerarray_race_yield( void)   MULLE_THREAD_UNPLEASANT_RACE_YIELD()
#else
# define pointerarray_race_yield()   do {} while( 0)
#endif

//
// Named race points. By default each one is just pointerarray_race_yield().
// A test can #define pointerarray_race_point( site) and #include this file to
// park a thread at an exact point, which turns a probabilistic race into a
// deterministic interleaving (see test/array/migration_stall.c).
//
#define POINTERARRAY_RACE_ADD          1   // add: storage read, slot not claimed
#define POINTERARRAY_RACE_ADD_SLOT     2   // add: index read, slot CAS not done
#define POINTERARRAY_RACE_ADD_CLAIMED  3   // add: slot CAS won, n not published yet
#define POINTERARRAY_RACE_GET          4   // get: storage read, entry not read
#define POINTERARRAY_RACE_COPY_READ    5   // copy: value read, not stored in successor
#define POINTERARRAY_RACE_COPY_CARRIED 6   // copy: stored in successor
#define POINTERARRAY_RACE_MIGRATE      7   // migrate: copied, storage not swapped

#ifndef pointerarray_race_point
# define pointerarray_race_point( site)   pointerarray_race_yield()
#endif


#pragma mark - _mulle_concurrent_pointerarraystorage


// n must be a power of 2
MULLE_C_NONNULL_RETURN
static struct _mulle_concurrent_pointerarraystorage *
   _mulle_concurrent_alloc_pointerarraystorage( unsigned int n,
                                                struct mulle_allocator *allocator)
{
   struct _mulle_concurrent_pointerarraystorage  *p;

   if( n < 8)
      n = 8;

   if( (size_t) n - 1 > (SIZE_MAX - sizeof( struct _mulle_concurrent_pointerarraystorage)) /
                          sizeof( void *))
      _mulle_allocator_abort( allocator, NULL, (size_t) -1);

   p = _mulle_allocator_calloc( allocator, 1, sizeof( void *) * (n - 1) +
                                sizeof( struct _mulle_concurrent_pointerarraystorage));
   p->size = n;

   /*
    * in theory, one should be able to use different values for NO_POINTER and
    * INVALID_POINTER
    */
   if( MULLE_CONCURRENT_NO_POINTER)
   {
      mulle_atomic_pointer_t   *q;
      mulle_atomic_pointer_t   *sentinel;

      q        = p->entries;
      sentinel = &p->entries[ (unsigned int) p->size];
      while( q < sentinel)
      {
         _mulle_atomic_pointer_nonatomic_write( q, MULLE_CONCURRENT_NO_POINTER);
         ++q;
      }
   }

   return( p);
}


static void   *_mulle_concurrent_pointerarraystorage_get( struct _mulle_concurrent_pointerarraystorage *p,
                                                          unsigned int i)
{
   unsigned int   n;

   // The acquire on n publishes the slot writes performed before the count
   // was advanced; the entry itself can then be read relaxed.
   n = (unsigned int) (uintptr_t) _mulle_atomic_pointer_read_acquire( &p->n);
   if( i >= n)
      return( MULLE_CONCURRENT_NO_POINTER);

   return( _mulle_atomic_pointer_read_relaxed( &p->entries[ i]));
}


//static void   *_mulle_concurrent_pointerarraystorage_extract( struct _mulle_concurrent_pointerarraystorage *p,
//                                                    unsigned int i)
//{
//   void   *value;
//
//   do
//   {
//      assert( i < (unsigned int) (uintptr_t) _mulle_atomic_pointer_read_relaxed( &p->n));
//      value = _mulle_atomic_pointer_read_relaxed( &p->entries[ i]);
//      if( value == MULLE_CONCURRENT_NO_POINTER)
//         break;
//   }
//   while( ! _mulle_atomic_pointer_weakcas( &p->entries[ i], MULLE_CONCURRENT_NO_POINTER, value));
//
//   return( value);
//}


//
// Publish the contiguous prefix of filled slots. A slot is filled before n is
// advanced; helping another writer means observing its slot and advancing n
// yourself. The CAS on n is the gate, so no count is bumped twice.
//
// Each iteration either returns or advances n, so the loop is bounded by size.
//
static void   _mulle_concurrent_pointerarraystorage_publish( struct _mulle_concurrent_pointerarraystorage *p)
{
   unsigned int   k;
   void           *value;

   for(;;)
   {
      k = (unsigned int) (uintptr_t) _mulle_atomic_pointer_read_acquire( &p->n);
      if( k >= (unsigned int) p->size)
         return;

      value = _mulle_atomic_pointer_read_acquire( &p->entries[ k]);
      if( value == MULLE_CONCURRENT_NO_POINTER || value == REDIRECT_VALUE)
         return;

      __mulle_atomic_pointer_cas( &p->n,
                                  (void *) (uintptr_t) (k + 1),
                                  (void *) (uintptr_t) k);
   }
}


//
// insert:
//
//  0      : did insert
//  EBUSY  : this storage can't be written to
//  ENOSPC : storage is full
//
static int   _mulle_concurrent_pointerarraystorage_add( struct _mulle_concurrent_pointerarraystorage *p,
                                                        void *value)
{
   void           *found;
   unsigned int   i;

   assert( p);
   assert( value != MULLE_CONCURRENT_NO_POINTER);
   assert( value != MULLE_CONCURRENT_INVALID_POINTER);

   for(;;)
   {
      i = (unsigned int) (uintptr_t) _mulle_atomic_pointer_read_acquire( &p->n);
      if( i >= (unsigned int) p->size)
         return( ENOSPC);

      pointerarray_race_point( POINTERARRAY_RACE_ADD_SLOT);

      found = __mulle_atomic_pointer_cas( &p->entries[ i],
                                          value,
                                          MULLE_CONCURRENT_NO_POINTER);
      if( MULLE_C_UNLIKELY( found == REDIRECT_VALUE))
         return( EBUSY);

      if( found == MULLE_CONCURRENT_NO_POINTER)
         pointerarray_race_point( POINTERARRAY_RACE_ADD_CLAIMED);

      // slot i is filled now, by us or by a peer: publish the prefix
      _mulle_concurrent_pointerarraystorage_publish( p);

      if( found == MULLE_CONCURRENT_NO_POINTER)
         return( 0);
      // lost the slot to a peer, and the publish advanced n; try the next one
   }
}


//
// Install value at index i in dst. If dst is itself retiring (its slot was
// frozen), follow next_storage and retry in the newer generation. The value is
// never written into dead storage.
//
static struct _mulle_concurrent_pointerarraystorage *
   _mulle_concurrent_pointerarraystorage_put( struct mulle_concurrent_pointerarray *array,
                                              struct _mulle_concurrent_pointerarraystorage *dst,
                                              unsigned int i,
                                              void *value)
{
   struct _mulle_concurrent_pointerarraystorage   *next;
   void                                           *found;

   for(;;)
   {
      found = __mulle_atomic_pointer_cas( &dst->entries[ i],
                                          value,
                                          MULLE_CONCURRENT_NO_POINTER);
      if( found != REDIRECT_VALUE)
      {
         _mulle_concurrent_pointerarraystorage_publish( dst);
         return( dst);
      }

      next = _mulle_atomic_pointer_read_acquire( &array->next_storage.pointer);
      assert( next != dst);
      dst = next;
   }
}


static void
   _mulle_concurrent_pointerarraystorage_copy( struct mulle_concurrent_pointerarray *array,
                                               struct _mulle_concurrent_pointerarraystorage *dst,
                                               struct _mulle_concurrent_pointerarraystorage *src)
{
   void           *actual;
   void           *value;
   unsigned int   i;

   // the shared empty storage is static const and must never be written
   if( ! src->size)
      return;

   for( i = 0; i < src->size; i++)
   {
      value = _mulle_atomic_pointer_read_acquire( &src->entries[ i]);
      for(;;)
      {
         if( value == MULLE_CONCURRENT_NO_POINTER)
         {
            // Freeze the slot so a late add cannot claim it in a retired
            // generation. If we lose, the winner's value is carried below.
            actual = __mulle_atomic_pointer_cas( &src->entries[ i],
                                                REDIRECT_VALUE,
                                                MULLE_CONCURRENT_NO_POINTER);
            if( actual == MULLE_CONCURRENT_NO_POINTER ||
                actual == REDIRECT_VALUE)
               break;
            value = actual;
            continue;
         }

         if( MULLE_C_UNLIKELY( value == REDIRECT_VALUE))
            break;

         pointerarray_race_point( POINTERARRAY_RACE_COPY_READ);

         // put first: no one gets left behind
         dst = _mulle_concurrent_pointerarraystorage_put( array, dst, i, value);

         pointerarray_race_point( POINTERARRAY_RACE_COPY_CARRIED);

         // then retire the source slot
         actual = __mulle_atomic_pointer_cas( &src->entries[ i],
                                             REDIRECT_VALUE,
                                             value);
         if( actual == value)
            break;

         value = actual;
      }
   }

   // publish the prefix of the generation we would swap in
   _mulle_concurrent_pointerarraystorage_publish( dst);
}


#pragma mark - _mulle_concurrent_pointerarray

void  _mulle_concurrent_pointerarray_init( struct mulle_concurrent_pointerarray *array,
                                           unsigned int size,
                                           struct mulle_allocator *allocator)
{
   struct _mulle_concurrent_pointerarraystorage   *storage;

   if( ! allocator)
      allocator = &mulle_default_allocator;

   assert( allocator->abafree && (void (*)(void)) allocator->abafree != (void (*)(void)) abort);

   array->allocator = allocator;
   if( size == 0)
      storage = (void *) &empty_storage;
   else
      storage = _mulle_concurrent_alloc_pointerarraystorage( size, allocator);

   _mulle_atomic_pointer_nonatomic_write( &array->storage.pointer, storage);
   _mulle_atomic_pointer_nonatomic_write( &array->next_storage.pointer, storage);
}


//
// this is called when you know, no other threads are accessing it anymore
//
void  _mulle_concurrent_pointerarray_done( struct mulle_concurrent_pointerarray *array)
{
   struct _mulle_concurrent_pointerarraystorage   *storage;
   struct _mulle_concurrent_pointerarraystorage   *next_storage;

   storage      = _mulle_atomic_pointer_nonatomic_read( &array->storage.pointer);
   next_storage = _mulle_atomic_pointer_nonatomic_read( &array->next_storage.pointer);

   if( storage != &empty_storage)
      _mulle_allocator_abafree( array->allocator, storage);
   if( storage != next_storage && storage != &empty_storage)
      _mulle_allocator_abafree( array->allocator, next_storage);
}


unsigned int  _mulle_concurrent_pointerarray_get_size( struct mulle_concurrent_pointerarray *array)
{
   struct _mulle_concurrent_pointerarraystorage   *p;

   p = _mulle_atomic_pointer_read_acquire( &array->storage.pointer);
   return( (unsigned int) p->size);
}


//
// obviously just a snapshot at some recent point in time
//
unsigned int   _mulle_concurrent_pointerarray_get_count( struct mulle_concurrent_pointerarray *array)
{
   struct _mulle_concurrent_pointerarraystorage   *p;

   p = _mulle_atomic_pointer_read_acquire( &array->storage.pointer);
   return( (unsigned int) (uintptr_t) _mulle_atomic_pointer_read_acquire( &p->n));
}


# pragma mark - multi-threaded

static void  _mulle_concurrent_pointerarray_migrate_storage( struct mulle_concurrent_pointerarray *array,
                                                             struct _mulle_concurrent_pointerarraystorage *p)
{

   struct _mulle_concurrent_pointerarraystorage   *q;
   struct _mulle_concurrent_pointerarraystorage   *alloced;
   struct _mulle_concurrent_pointerarraystorage   *previous;
   unsigned int                                   new_size;
   unsigned int                                   old_size;

   assert( p);

   // A stale source must not copy. Every retry after EBUSY/ENOSPC then observes
   // a strictly newer generation, which is what bounds the retry chain.
   if( _mulle_atomic_pointer_read_acquire( &array->storage.pointer) != p)
      return;

   // acquire new storage
   alloced = NULL;
   q       = _mulle_atomic_pointer_read_acquire( &array->next_storage.pointer);

   assert( q);

   if( q == p)
   {
      old_size = (unsigned int) p->size;
      if( old_size > ((unsigned int) -1) / 2)
         _mulle_allocator_abort( array->allocator, NULL, (size_t) -1);

      new_size = old_size * 2;
      // migration must at least double; the wait-free retry bound depends on it
      assert( new_size == old_size * 2);

      alloced = _mulle_concurrent_alloc_pointerarraystorage( new_size,
                                                             array->allocator);

      // make this the next world, assume that's still set to 'p' (SIC)
      q = __mulle_atomic_pointer_cas( &array->next_storage.pointer, alloced, p);
      if( q != p)
      {
         // someone else produced a next world, use that and get rid of 'alloced'
         _mulle_allocator_abafree( array->allocator, alloced);
         alloced = NULL;
      }
      else
         q = alloced;
   }

   // this thread can partake in copying
   _mulle_concurrent_pointerarraystorage_copy( array, q, p);

   pointerarray_race_point( POINTERARRAY_RACE_MIGRATE);

   // now update world, giving it the same value as 'next_world'
   previous = __mulle_atomic_pointer_cas( &array->storage.pointer, q, p);

   // if this assert hits, it means that mulle-concurrent has been linked
   // twice (happens sometimes)
   assert( previous->size || previous == &empty_storage);

   // ok, if we succeed free old, if we fail alloced is
   // already gone
   if( previous == p && p->size)
      _mulle_allocator_abafree( array->allocator, previous);
}


void  *_mulle_concurrent_pointerarray_get( struct mulle_concurrent_pointerarray *array,
                                           unsigned int index)
{
   struct _mulle_concurrent_pointerarraystorage   *p;
   void                                           *value;

retry:
   p     = _mulle_atomic_pointer_read_acquire( &array->storage.pointer);
   pointerarray_race_point( POINTERARRAY_RACE_GET);
   value = _mulle_concurrent_pointerarraystorage_get( p, index);
   if( value == REDIRECT_VALUE)
   {
      _mulle_concurrent_pointerarray_migrate_storage( array, p);
      goto retry;
   }
   return( value);
}


//// hackish: replaces contents with NULL, returns previous value
////          which could be NULL again
////          not too sure about tj
//void  *_mulle_concurrent_pointerarray_extract( struct mulle_concurrent_pointerarray *array,
//                                               unsigned int index);
//
//void  *_mulle_concurrent_pointerarray_extract( struct mulle_concurrent_pointerarray *array,
//                                               unsigned int index)
//{
//   struct _mulle_concurrent_pointerarraystorage   *p;
//   void                                           *value;
//
//retry:
//   p     = _mulle_atomic_pointer_read_relaxed( &array->storage.pointer);
//   value = _mulle_concurrent_pointerarraystorage_extract( p, index);
//   if( value == REDIRECT_VALUE)
//   {
//      _mulle_concurrent_pointerarray_migrate_storage( array, p);
//      goto retry;
//   }
//   return( value);
//}


void  _mulle_concurrent_pointerarray_add( struct mulle_concurrent_pointerarray *array,
                                         void *value)
{
   struct _mulle_concurrent_pointerarraystorage   *p;
   int                                            rval;

   assert( value != MULLE_CONCURRENT_NO_POINTER);
   assert( value != REDIRECT_VALUE);

retry:
   p    = _mulle_atomic_pointer_read_acquire( &array->storage.pointer);
   pointerarray_race_point( POINTERARRAY_RACE_ADD);
   rval = _mulle_concurrent_pointerarraystorage_add( p, value);
   if( MULLE_C_UNLIKELY( rval == EBUSY || rval == ENOSPC))
   {
      _mulle_concurrent_pointerarray_migrate_storage( array, p);
      goto retry;
   }
}



#pragma mark - not so concurrent enumerator

void  *_mulle_concurrent_pointerarrayenumerator_next( struct mulle_concurrent_pointerarrayenumerator *rover)
{
   void           *value;
   unsigned int   n;

   if( ! rover || ! rover->array)
      return( MULLE_CONCURRENT_NO_POINTER);

   n = mulle_concurrent_pointerarray_get_count( rover->array);
   if( MULLE_C_UNLIKELY( rover->index >= n))
      return( MULLE_CONCURRENT_NO_POINTER);

   value = _mulle_concurrent_pointerarray_get( rover->array, rover->index);
   assert( value != MULLE_CONCURRENT_NO_POINTER);

   ++rover->index;
   return( value);
}


void   *_mulle_concurrent_pointerarrayreverseenumerator_next( struct mulle_concurrent_pointerarrayreverseenumerator *rover)
{
   void   *value;

   if( ! rover || ! rover->array)
      return( MULLE_CONCURRENT_NO_POINTER);

   if( MULLE_C_UNLIKELY( ! rover->index))
      return( MULLE_CONCURRENT_NO_POINTER);

   value = _mulle_concurrent_pointerarray_get( rover->array, --rover->index);
   assert( value != MULLE_CONCURRENT_NO_POINTER);

   return( value);
}


int   _mulle_concurrent_pointerarray_find( struct mulle_concurrent_pointerarray *array,
                                           void *search)
{
   struct mulle_concurrent_pointerarrayenumerator   rover;
   int                                              found;
   void                                             *value;

   found = 0;
   rover = mulle_concurrent_pointerarray_enumerate( array);
   while( (value = _mulle_concurrent_pointerarrayenumerator_next( &rover)))
      if( value == search)
      {
         found = 1;
         break;
      }
   mulle_concurrent_pointerarrayenumerator_done( &rover);

   return( found);
}


int   mulle_concurrent_pointerarray_map( struct mulle_concurrent_pointerarray *list,
                                         void (*f)( void *, void *),
                                         void *userinfo)
{
   struct mulle_concurrent_pointerarrayenumerator  rover;
   void                                            *value;

   rover = mulle_concurrent_pointerarray_enumerate( list);
   while( (value = _mulle_concurrent_pointerarrayenumerator_next( &rover)))
      (*f)( value, userinfo);
   mulle_concurrent_pointerarrayenumerator_done( &rover);

   return( 0);
}
