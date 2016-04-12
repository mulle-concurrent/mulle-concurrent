//
//  mulle_concurrent_hashmap.h
//  mulle-concurrent
//
//  Created by Nat! on 04.03.16.
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
#ifndef mulle_concurrent_hashmap_h__
#define mulle_concurrent_hashmap_h__

#include <mulle_thread/mulle_thread.h>
#include <mulle_allocator/mulle_allocator.h>


struct _mulle_concurrent_hashmapstorage;


union mulle_concurrent_atomichashmapstorage_t
{
   struct _mulle_concurrent_hashmapstorage  *storage;
   mulle_atomic_pointer_t                   pointer;
};

//
// basically does: http://preshing.com/20160222/a-resizable-concurrent-map/
// but is wait-free
//
struct mulle_concurrent_hashmap
{
   union mulle_concurrent_atomichashmapstorage_t   storage;
   union mulle_concurrent_atomichashmapstorage_t   next_storage;
   struct mulle_allocator                          *allocator;
};

int  _mulle_concurrent_hashmap_init( struct mulle_concurrent_hashmap *map,
                                     unsigned int size,
                                     struct mulle_allocator *allocator);
void  _mulle_concurrent_hashmap_done( struct mulle_concurrent_hashmap *map);


int  _mulle_concurrent_hashmap_insert( struct mulle_concurrent_hashmap *map,
                                       intptr_t hash,
                                       void *value);

int  _mulle_concurrent_hashmap_remove( struct mulle_concurrent_hashmap *map,
                                       intptr_t hash,
                                       void *value);

void  *_mulle_concurrent_hashmap_lookup( struct mulle_concurrent_hashmap *map,
                                         intptr_t hash);

unsigned int  _mulle_concurrent_hashmap_get_size( struct mulle_concurrent_hashmap *map);
unsigned int  mulle_concurrent_hashmap_get_count( struct mulle_concurrent_hashmap *map);


#pragma mark -
#pragma mark not so concurrent enumerator

struct mulle_concurrent_hashmapenumerator
{
   struct mulle_concurrent_hashmap   *map;
   unsigned int                      index;
   unsigned int                      mask;
};


//
// the specific retuned enumerator is only useable for the calling thread
// if you remove stuff from the map, the enumerator will be unhappy and
// stop (but will tell you). If the map grows, the rover is equally unhappy.
//
static inline struct mulle_concurrent_hashmapenumerator  mulle_concurrent_hashmap_enumerate( struct mulle_concurrent_hashmap *map)
{
   struct mulle_concurrent_hashmapenumerator   rover;
   
   rover.map   = map;
   rover.index = map ? 0 : -1;
   rover.mask  = 0;
   
   return( rover);
}


//  1 : OK
//  0 : nothing left
// -1 : failed to enumerate further
//
int  _mulle_concurrent_hashmapenumerator_next( struct mulle_concurrent_hashmapenumerator *rover,
                                               intptr_t *hash,
                                               void **value);

static inline void  _mulle_concurrent_hashmapenumerator_done( struct mulle_concurrent_hashmapenumerator *rover)
{
}

// convenience using the enumerator

void  *_mulle_concurrent_hashmap_lookup_any( struct mulle_concurrent_hashmap *map);

#endif /* mulle_concurrent_hashmap_h */
