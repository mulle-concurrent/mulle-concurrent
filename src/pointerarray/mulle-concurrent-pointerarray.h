//
//  mulle_concurrent_pointerarray.h
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
#ifndef mulle_concurrent_pointerarray_h__
#define mulle_concurrent_pointerarray_h__

#include "include.h"

#include <errno.h>


struct _mulle_concurrent_pointerarraystorage;


union mulle_concurrent_atomicpointerarraystorage_t
{
   struct _mulle_concurrent_pointerarraystorage  *storage;
   mulle_atomic_pointer_t                        pointer;
};


struct mulle_concurrent_pointerarray
{
   union mulle_concurrent_atomicpointerarraystorage_t   storage;
   union mulle_concurrent_atomicpointerarraystorage_t   next_storage;
   struct mulle_allocator                               *allocator;
};


#pragma mark - single-threaded

// Returns:
//   0      : OK
//   EINVAL : invalid argument
//   ENOMEM : out of memory
//
static inline int  mulle_concurrent_pointerarray_init( struct mulle_concurrent_pointerarray *array,
                                                       unsigned int size,
                                                       struct mulle_allocator *allocator)
{
   MULLE__CONCURRENT_GLOBAL
   void  _mulle_concurrent_pointerarray_init( struct mulle_concurrent_pointerarray *array,
                                              unsigned int size,
                                              struct mulle_allocator *allocator);
   if( ! array)
      return( EINVAL);

   _mulle_concurrent_pointerarray_init( array, size, allocator);
   return( 0);
}


static inline void  mulle_concurrent_pointerarray_done( struct mulle_concurrent_pointerarray *array)
{
   MULLE__CONCURRENT_GLOBAL
   void  _mulle_concurrent_pointerarray_done( struct mulle_concurrent_pointerarray *array);

   if( array)
      _mulle_concurrent_pointerarray_done( array);
}


static inline unsigned int  mulle_concurrent_pointerarray_get_size( struct mulle_concurrent_pointerarray *array)
{
   MULLE__CONCURRENT_GLOBAL
   unsigned int  _mulle_concurrent_pointerarray_get_size( struct mulle_concurrent_pointerarray *array);

   if( ! array)
      return( 0);
   return( _mulle_concurrent_pointerarray_get_size( array));
}


static inline unsigned int  mulle_concurrent_pointerarray_get_count( struct mulle_concurrent_pointerarray *array)
{
   MULLE__CONCURRENT_GLOBAL
   unsigned int  _mulle_concurrent_pointerarray_get_count( struct mulle_concurrent_pointerarray *array);

   if( ! array)
      return( 0);
   return( _mulle_concurrent_pointerarray_get_count( array));
}


#pragma mark - multi-threaded

// Returns:
//   0      : OK
//   EINVAL : invalid argument
//   ENOMEM : out of memory
//
MULLE__CONCURRENT_GLOBAL
int  mulle_concurrent_pointerarray_add( struct mulle_concurrent_pointerarray *array,
                                        void *value);

static inline void  *mulle_concurrent_pointerarray_get( struct mulle_concurrent_pointerarray *array,
                                          unsigned int i)
{
   MULLE__CONCURRENT_GLOBAL
   void  *_mulle_concurrent_pointerarray_get( struct mulle_concurrent_pointerarray *array,
                                              unsigned int index);
   if( ! array)
      return( NULL);
   return( _mulle_concurrent_pointerarray_get( array, i));
}


MULLE__CONCURRENT_GLOBAL
int  mulle_concurrent_pointerarray_find( struct mulle_concurrent_pointerarray *array,
                                         void *value);

#pragma mark - enumerator

struct mulle_concurrent_pointerarrayenumerator
{
   struct mulle_concurrent_pointerarray   *array;
   unsigned int                            index;
};

struct mulle_concurrent_pointerarrayreverseenumerator
{
   struct mulle_concurrent_pointerarray   *array;
   unsigned int                           index;
};

//
// the specific retuned enumerator is only useable for the calling thread
//
static inline struct mulle_concurrent_pointerarrayenumerator
   mulle_concurrent_pointerarray_enumerate( struct mulle_concurrent_pointerarray *array)
{
   struct mulle_concurrent_pointerarrayenumerator   rover;

   rover.array = array;
   rover.index = array ? 0 : (unsigned int) -1;

   return( rover);
}


static inline struct mulle_concurrent_pointerarrayreverseenumerator
   mulle_concurrent_pointerarray_reverseenumerate( struct mulle_concurrent_pointerarray *array, unsigned int n)
{
   struct mulle_concurrent_pointerarrayreverseenumerator   rover;

   rover.array = array;
   rover.index = n;

   return( rover);
}


// Returns:
//   1      : OK
//   0      : nothing left
//   EINVAL : invalid argument
static inline void  *mulle_concurrent_pointerarrayenumerator_next( struct mulle_concurrent_pointerarrayenumerator *rover)
{
   MULLE__CONCURRENT_GLOBAL
   void   *_mulle_concurrent_pointerarrayenumerator_next( struct mulle_concurrent_pointerarrayenumerator *rover);

   if( ! rover)
      return(  NULL);
   return( _mulle_concurrent_pointerarrayenumerator_next( rover));
}


static inline void  *mulle_concurrent_pointerarrayreverseenumerator_next( struct mulle_concurrent_pointerarrayreverseenumerator *rover)
{
   MULLE__CONCURRENT_GLOBAL
   void   *_mulle_concurrent_pointerarrayreverseenumerator_next( struct mulle_concurrent_pointerarrayreverseenumerator *rover);

   if( ! rover)
      return( NULL);
   return( _mulle_concurrent_pointerarrayreverseenumerator_next( rover));
}


static inline void  mulle_concurrent_pointerarrayenumerator_done( struct mulle_concurrent_pointerarrayenumerator *rover)
{
}


static inline void  mulle_concurrent_pointerarrayreverseenumerator_done( struct mulle_concurrent_pointerarrayreverseenumerator *rover)
{
}


#pragma mark - enumerator conveniences

MULLE__CONCURRENT_GLOBAL
int   mulle_concurrent_pointerarray_map( struct mulle_concurrent_pointerarray *list,
                                        void (*f)( void *, void *),
                                        void *userinfo);

#pragma mark - various functions, no parameter checks

MULLE__CONCURRENT_GLOBAL
void  _mulle_concurrent_pointerarray_init( struct mulle_concurrent_pointerarray *array,
                                          unsigned int size,
                                          struct mulle_allocator *allocator);
MULLE__CONCURRENT_GLOBAL
void  _mulle_concurrent_pointerarray_done( struct mulle_concurrent_pointerarray *array);

MULLE__CONCURRENT_GLOBAL
unsigned int  _mulle_concurrent_pointerarray_get_size( struct mulle_concurrent_pointerarray *array);

MULLE__CONCURRENT_GLOBAL
unsigned int  _mulle_concurrent_pointerarray_get_count( struct mulle_concurrent_pointerarray *array);

MULLE__CONCURRENT_GLOBAL
void  _mulle_concurrent_pointerarray_add( struct mulle_concurrent_pointerarray *array,
                                         void *value);

MULLE__CONCURRENT_GLOBAL
void  *_mulle_concurrent_pointerarray_get( struct mulle_concurrent_pointerarray *array,
                                           unsigned int i);

MULLE__CONCURRENT_GLOBAL
int  _mulle_concurrent_pointerarray_find( struct mulle_concurrent_pointerarray *array,
                                          void *value);

MULLE__CONCURRENT_GLOBAL
void   *_mulle_concurrent_pointerarrayenumerator_next( struct mulle_concurrent_pointerarrayenumerator *rover);


MULLE__CONCURRENT_GLOBAL
void  *_mulle_concurrent_pointerarrayreverseenumerator_next( struct mulle_concurrent_pointerarrayreverseenumerator *rover);


#define mulle_concurrent_pointerarray_for( array, item)                                                                   \
   for( struct mulle_concurrent_pointerarrayenumerator rover__ ## item = mulle_concurrent_pointerarray_enumerate( array); \
      (item = _mulle_concurrent_pointerarrayenumerator_next( &rover__ ## item));)

#define mulle_concurrent_pointerarray_for_reverse( array, n, item)                                                                         \
   for( struct mulle_concurrent_pointerarrayreverseenumerator rover__ ## item = mulle_concurrent_pointerarray_reverseenumerate( array, n); \
      (item = _mulle_concurrent_pointerarrayreverseenumerator_next( &rover__ ## item));)

#endif /* mulle_concurrent_pointerarray_h */
