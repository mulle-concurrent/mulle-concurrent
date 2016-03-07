//
//  mulle_concurrent_pointerarray.h
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
#ifndef mulle_concurrent_pointerarray_h__
#define mulle_concurrent_pointerarray_h__

#include <mulle_thread/mulle_thread.h>
#include <mulle_allocator/mulle_allocator.h>


struct mulle_aba;

struct mulle_concurrent_pointerarray
{
   mulle_atomic_pointer_t   storage;
   mulle_atomic_pointer_t   next_storage;
   struct mulle_allocator   *allocator;
};

int  _mulle_concurrent_pointerarray_init( struct mulle_concurrent_pointerarray *array,
                                          unsigned int size,
                                          struct mulle_allocator *allocator);
void  _mulle_concurrent_pointerarray_done( struct mulle_concurrent_pointerarray *array);


int  _mulle_concurrent_pointerarray_add( struct mulle_concurrent_pointerarray *array,
                                         void *value);

void  *_mulle_concurrent_pointerarray_get( struct mulle_concurrent_pointerarray *array,
                                           unsigned int n);

int  _mulle_concurrent_pointerarray_find( struct mulle_concurrent_pointerarray *array,
                                            void *value);


unsigned int  _mulle_concurrent_pointerarray_get_size( struct mulle_concurrent_pointerarray *array);
unsigned int  mulle_concurrent_pointerarray_get_count( struct mulle_concurrent_pointerarray *array);


#pragma mark -
#pragma mark not so concurrent enumerator

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
// if you remove stuff from the array, the enumerator will be unhappy and
// stop (but will tell you). If the array grows, the rover is equally unhappy.
//
static inline struct mulle_concurrent_pointerarrayenumerator
   mulle_concurrent_pointerarray_enumerate( struct mulle_concurrent_pointerarray *array)
{
   struct mulle_concurrent_pointerarrayenumerator   rover;
   
   rover.array   = array;
   rover.index = array ? 0 : (unsigned int) -1;
   
   return( rover);
}

static inline struct mulle_concurrent_pointerarrayreverseenumerator
   _mulle_concurrent_pointerarray_reverseenumerate( struct mulle_concurrent_pointerarray *array, unsigned int n)
{
   struct mulle_concurrent_pointerarrayreverseenumerator   rover;
   
   rover.array   = array;
   rover.index = n;
   
   return( rover);
}


//  1 : OK
//  0 : nothing left
// -1 : failed to enumerate further
//
int  _mulle_concurrent_pointerarrayenumerator_next( struct mulle_concurrent_pointerarrayenumerator *rover,
                                              void **value);

int  _mulle_concurrent_pointerarrayreverseenumerator_next( struct mulle_concurrent_pointerarrayreverseenumerator *rover,
                                                     void **value);

static inline void  _mulle_concurrent_pointerarrayenumerator_done( struct mulle_concurrent_pointerarrayenumerator *rover)
{
}

static inline void  _mulle_concurrent_pointerarrayreverseenumerator_done( struct mulle_concurrent_pointerarrayreverseenumerator *rover)
{
}

#endif /* mulle_concurrent_pointerarray_h */
