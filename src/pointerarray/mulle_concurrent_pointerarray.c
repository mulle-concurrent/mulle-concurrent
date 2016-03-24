//
//  mulle_concurrent_pointerarray.c
//  mulle-concurrent
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
#include "mulle_concurrent_pointerarray.h"

#include "mulle_concurrent_types.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>


struct _mulle_concurrent_pointerarraystorage
{
   mulle_atomic_pointer_t   n;
   unsigned int             size;
   
   mulle_atomic_pointer_t   entries[ 1];
};


//
// the nice thing about the redirect is, that another thread that is
// trying to add at the same time cooperates
// The negative is, that we encure extra write cycles, if there is no
// other thread
//
#define REDIRECT_VALUE   MULLE_CONCURRENT_INVALID_POINTER


#pragma mark -
#pragma mark _mulle_concurrent_pointerarraystorage


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
   if( ! p)
      return( p);
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
      sentinel = &p->entries[ p->size];
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
   assert( value != MULLE_CONCURRENT_NO_POINTER && value != MULLE_CONCURRENT_INVALID_POINTER);

   for(;;)
   {
      i = (unsigned int) (uintptr_t) _mulle_atomic_pointer_read( &p->n);
      if( i >= p->size)
         return( ENOSPC);

      found = __mulle_atomic_pointer_compare_and_swap( &p->entries[ i], value, MULLE_CONCURRENT_NO_POINTER);
      if( found == MULLE_CONCURRENT_NO_POINTER)
      {
         _mulle_atomic_pointer_increment( &p->n);
         return( 0);
      }
      
      if( found == REDIRECT_VALUE)
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
      if( _mulle_atomic_pointer_compare_and_swap( &dst->entries[ i], value, MULLE_CONCURRENT_NO_POINTER))
         _mulle_atomic_pointer_increment( &dst->n);
   }
}


#pragma mark -
#pragma mark _mulle_concurrent_pointerarray

int  _mulle_concurrent_pointerarray_init( struct mulle_concurrent_pointerarray *array,
                                          unsigned int size,
                                          struct mulle_allocator *allocator)
{
   struct _mulle_concurrent_pointerarraystorage   *storage;
   
   if( ! allocator)
      allocator = &mulle_default_allocator;

   assert( allocator->abafree && allocator->abafree != (void *) abort);
   
   if( ! allocator->abafree || allocator->abafree == (void *) abort)
   {
      errno = EINVAL;
      return( -1);
   }
   
   array->allocator = allocator;
   storage          = _mulle_concurrent_alloc_pointerarraystorage( size, allocator);
   
   _mulle_atomic_pointer_nonatomic_write( &array->storage, storage);
   _mulle_atomic_pointer_nonatomic_write( &array->next_storage, storage);
   
   if( ! storage)
      return( -1);
   return( 0);
}


//
// this is called when you know, no other threads are accessing it anymore
//
void  _mulle_concurrent_pointerarray_done( struct mulle_concurrent_pointerarray *array)
{
   struct _mulle_concurrent_pointerarraystorage   *storage;
   struct _mulle_concurrent_pointerarraystorage   *next_storage;
   
   storage      = _mulle_atomic_pointer_nonatomic_read( &array->storage);
   next_storage = _mulle_atomic_pointer_nonatomic_read( &array->next_storage);
   
   _mulle_allocator_abafree( array->allocator, storage);
   if( storage != next_storage)
      _mulle_allocator_abafree( array->allocator, next_storage);
}


static int  _mulle_concurrent_pointerarray_migrate_storage( struct mulle_concurrent_pointerarray *array,
                                                      struct _mulle_concurrent_pointerarraystorage *p)
{

   struct _mulle_concurrent_pointerarraystorage   *q;
   struct _mulle_concurrent_pointerarraystorage   *alloced;
   struct _mulle_concurrent_pointerarraystorage   *previous;

   assert( p);
   
   // acquire new storage
   alloced = NULL;
   q       = _mulle_atomic_pointer_read( &array->next_storage);

   assert( q);
   
   if( q == p)
   {
      alloced = _mulle_concurrent_alloc_pointerarraystorage( p->size * 2, array->allocator);
      if( ! alloced)
         return( -1);

      // make this the next world, assume that's still set to 'p' (SIC)
      q = __mulle_atomic_pointer_compare_and_swap( &array->next_storage, alloced, p);
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
   previous = __mulle_atomic_pointer_compare_and_swap( &array->storage, q, p);

   // ok, if we succeed free old, if we fail alloced is
   // already gone
   if( previous == p)
      _mulle_allocator_abafree( array->allocator, previous);
   
   return( 0);
}


void  *_mulle_concurrent_pointerarray_get( struct mulle_concurrent_pointerarray *array,
                                           unsigned int index)
{
   struct _mulle_concurrent_pointerarraystorage   *p;
   void                                     *value;
   
retry:
   p     = _mulle_atomic_pointer_read( &array->storage);
   value = _mulle_concurrent_pointerarraystorage_get( p, index);
   if( value == REDIRECT_VALUE)
   {
      if( _mulle_concurrent_pointerarray_migrate_storage( array, p))
         return( MULLE_CONCURRENT_NO_POINTER);
      goto retry;
   }
   return( value);
}


int  _mulle_concurrent_pointerarray_add( struct mulle_concurrent_pointerarray *array,
                                         void *value)
{
   struct _mulle_concurrent_pointerarraystorage   *p;

   assert( value);
   assert( value != REDIRECT_VALUE);
   
retry:
   p = _mulle_atomic_pointer_read( &array->storage);
   switch( _mulle_concurrent_pointerarraystorage_add( p, value))
   {
   case EBUSY   :
   case ENOSPC  :
      if( _mulle_concurrent_pointerarray_migrate_storage( array, p))
         return( -1);
      goto retry;
   }

   return( 0);
}


unsigned int  _mulle_concurrent_pointerarray_get_size( struct mulle_concurrent_pointerarray *array)
{
   struct _mulle_concurrent_pointerarraystorage   *p;
   
   p = _mulle_atomic_pointer_read( &array->storage);
   return( p->size);
}


//
// obviously just a snapshot at some recent point in time
//
unsigned int  mulle_concurrent_pointerarray_get_count( struct mulle_concurrent_pointerarray *array)
{
   struct _mulle_concurrent_pointerarraystorage   *p;
   
   if( ! array)
      return( 0);
   
   p = _mulle_atomic_pointer_read( &array->storage);
   return( (unsigned int) (uintptr_t) _mulle_atomic_pointer_read( &p->n));
}


#pragma mark -
#pragma mark not so concurrent enumerator

int  _mulle_concurrent_pointerarrayenumerator_next( struct mulle_concurrent_pointerarrayenumerator *rover,
                                              void **p_value)
{
   void           *value;
   unsigned int   n;
   
   n = mulle_concurrent_pointerarray_get_count( rover->array);
   if( rover->index >= n)
      return( 0);
   
   value = _mulle_concurrent_pointerarray_get( rover->array, rover->index);
   if( value == MULLE_CONCURRENT_NO_POINTER)
      return( -1);

   ++rover->index;
   if( p_value)
      *p_value = value;

   return( 1);
}


int  _mulle_concurrent_pointerarrayreverseenumerator_next( struct mulle_concurrent_pointerarrayreverseenumerator *rover,
                                                     void **p_value)
{
   void   *value;
   
   if( ! rover->index)
      return( 0);
   
   value = _mulle_concurrent_pointerarray_get( rover->array, --rover->index);
   if( value == MULLE_CONCURRENT_NO_POINTER)
      return( -1);

   if( p_value)
      *p_value = value;

   return( 1);
}



int   _mulle_concurrent_pointerarray_find( struct mulle_concurrent_pointerarray *array,
                                       void *search)
{
   struct mulle_concurrent_pointerarrayenumerator   rover;
   int                                              found;
   void                                             *value;
   
   found = 0;
   rover = mulle_concurrent_pointerarray_enumerate( array);
   while( _mulle_concurrent_pointerarrayenumerator_next( &rover, (void **) &value) == 1)
   {
      if( value == search)
      {
         found = 1;
         break;
      }
   }
   _mulle_concurrent_pointerarrayenumerator_done( &rover);
   
   return( found);
}


int   mulle_concurrent_pointerarray_map( struct mulle_concurrent_pointerarray *list,
                                                void (*f)( void *, void *),
                                                void *userinfo)
{
   struct mulle_concurrent_pointerarrayenumerator  rover;
   void                                            *value;
   
   rover = mulle_concurrent_pointerarray_enumerate( list);
   for(;;)
   {
      switch( _mulle_concurrent_pointerarrayenumerator_next( &rover, &value))
      {
      case -1 : return( -1);
      case  1 : (*f)( value, userinfo); continue;
      }
      break;
   }
   _mulle_concurrent_pointerarrayenumerator_done( &rover);
   return( 0);
}

