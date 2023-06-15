//
//  mulle_concurrent_pointerarray.c
//  mulle-concurrent
//
//  Created by Nat! on 06.03.16.
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


#pragma mark - _mulle_concurrent_pointerarraystorage


// n must be a power of 2
static struct _mulle_concurrent_pointerarraystorage *
   _mulle_concurrent_alloc_pointerarraystorage( unsigned int n,
                                                struct mulle_allocator *allocator)
{
   struct _mulle_concurrent_pointerarraystorage  *p;

   if( n < 8)
      n = 8;

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
   assert( i < (unsigned int) (uintptr_t) _mulle_atomic_pointer_read( &p->n));
   return( _mulle_atomic_pointer_read( &p->entries[ i]));
}


//static void   *_mulle_concurrent_pointerarraystorage_extract( struct _mulle_concurrent_pointerarraystorage *p,
//                                                    unsigned int i)
//{
//   void   *value;
//
//   do
//   {
//      assert( i < (unsigned int) (uintptr_t) _mulle_atomic_pointer_read( &p->n));
//      value = _mulle_atomic_pointer_read( &p->entries[ i]);
//      if( value == MULLE_CONCURRENT_NO_POINTER)
//         break;
//   }
//   while( ! _mulle_atomic_pointer_weakcas( &p->entries[ i], MULLE_CONCURRENT_NO_POINTER, value));
//
//   return( value);
//}


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
      i = (unsigned int) (uintptr_t) _mulle_atomic_pointer_read( &p->n);
      if( i >= (unsigned int) p->size)
         return( ENOSPC);

      found = __mulle_atomic_pointer_cas( &p->entries[ i], value, MULLE_CONCURRENT_NO_POINTER);
      if( found == MULLE_CONCURRENT_NO_POINTER)
      {
         _mulle_atomic_pointer_increment( &p->n);
         return( 0);
      }

      if( MULLE_C_UNLIKELY( found == REDIRECT_VALUE))
         return( EBUSY);
   }
}


static void   _mulle_concurrent_pointerarraystorage_copy( struct _mulle_concurrent_pointerarraystorage *dst,
                                                          struct _mulle_concurrent_pointerarraystorage *src)
{
   mulle_atomic_pointer_t   *p;
   mulle_atomic_pointer_t   *p_last;
   void                     *value;
   unsigned int             i;
   unsigned int             n;

   n      = (unsigned int) (uintptr_t) _mulle_atomic_pointer_read( &dst->n);
   p      = &src->entries[ n];
   p_last = &src->entries[ src->size];

   for( i = n; p < p_last; p++, i++)
   {
      value = _mulle_atomic_pointer_read( p);
      // value == MULLE_CONCURRENT_NO_POINTER ? because of extract
      if( value == MULLE_CONCURRENT_NO_POINTER ||
          _mulle_atomic_pointer_cas( &dst->entries[ i], value, MULLE_CONCURRENT_NO_POINTER))
         _mulle_atomic_pointer_increment( &dst->n);
   }
}


#pragma mark - _mulle_concurrent_pointerarray

void  _mulle_concurrent_pointerarray_init( struct mulle_concurrent_pointerarray *array,
                                           unsigned int size,
                                           struct mulle_allocator *allocator)
{
   struct _mulle_concurrent_pointerarraystorage   *storage;

   if( ! allocator)
      allocator = &mulle_default_allocator;

   assert( allocator->abafree && allocator->abafree != (int (*)()) abort);

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

   p = _mulle_atomic_pointer_read( &array->storage.pointer);
   return( (unsigned int) p->size);
}


//
// obviously just a snapshot at some recent point in time
//
unsigned int   _mulle_concurrent_pointerarray_get_count( struct mulle_concurrent_pointerarray *array)
{
   struct _mulle_concurrent_pointerarraystorage   *p;

   p = _mulle_atomic_pointer_read( &array->storage.pointer);
   return( (unsigned int) (uintptr_t) _mulle_atomic_pointer_read( &p->n));
}


# pragma mark - multi-threaded

static void  _mulle_concurrent_pointerarray_migrate_storage( struct mulle_concurrent_pointerarray *array,
                                                             struct _mulle_concurrent_pointerarraystorage *p)
{

   struct _mulle_concurrent_pointerarraystorage   *q;
   struct _mulle_concurrent_pointerarraystorage   *alloced;
   struct _mulle_concurrent_pointerarraystorage   *previous;

   assert( p);

   // acquire new storage
   alloced = NULL;
   q       = _mulle_atomic_pointer_read( &array->next_storage.pointer);

   assert( q);

   if( q == p)
   {
      alloced = _mulle_concurrent_alloc_pointerarraystorage( (unsigned int) p->size * 2, 
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
   _mulle_concurrent_pointerarraystorage_copy( q, p);

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
   p     = _mulle_atomic_pointer_read( &array->storage.pointer);
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
//   p     = _mulle_atomic_pointer_read( &array->storage.pointer);
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
   p    = _mulle_atomic_pointer_read( &array->storage.pointer);
   rval = _mulle_concurrent_pointerarraystorage_add( p, value);
   if( MULLE_C_UNLIKELY( rval == EBUSY || rval == ENOSPC))
   {
      _mulle_concurrent_pointerarray_migrate_storage( array, p);
      goto retry;
   }
}


int  mulle_concurrent_pointerarray_add( struct mulle_concurrent_pointerarray *array,
                                        void *value)
{
   if( ! array)
      return( EINVAL);
   if( value == MULLE_CONCURRENT_NO_POINTER || value == MULLE_CONCURRENT_INVALID_POINTER)
      return( EINVAL);

   _mulle_concurrent_pointerarray_add( array, value);
   return( 0);
}


int  mulle_concurrent_pointerarray_find( struct mulle_concurrent_pointerarray *array,
                                         void *value)
{
   if( ! array)
      return( EINVAL);
   if( value == MULLE_CONCURRENT_NO_POINTER || value == MULLE_CONCURRENT_INVALID_POINTER)
      return( EINVAL);
   return( _mulle_concurrent_pointerarray_find( array, value));
}


#pragma mark - not so concurrent enumerator

void  *_mulle_concurrent_pointerarrayenumerator_next( struct mulle_concurrent_pointerarrayenumerator *rover)
{
   void           *value;
   unsigned int   n;

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
   while( value = _mulle_concurrent_pointerarrayenumerator_next( &rover))
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
   while( value = _mulle_concurrent_pointerarrayenumerator_next( &rover))
      (*f)( value, userinfo);
   mulle_concurrent_pointerarrayenumerator_done( &rover);

   return( 0);
}
