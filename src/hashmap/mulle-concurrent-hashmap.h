//
//  mulle_concurrent_hashmap.h
//  mulle-concurrent
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
#ifndef mulle_concurrent_hashmap_h__
#define mulle_concurrent_hashmap_h__

#include "include.h"

#include <errno.h>


struct _mulle_concurrent_hashvaluepair
{
   intptr_t                 hash;
   mulle_atomic_pointer_t   value;
};


struct _mulle_concurrent_hashmapstorage
{
   mulle_atomic_pointer_t   n_hashs;  // with possibly empty values
   uintptr_t                mask;     // easier to read from debugger if void * size

   struct _mulle_concurrent_hashvaluepair  entries[ 1];
};


static inline int
   _mulle_concurrent_hashmapstorage_is_const( struct _mulle_concurrent_hashmapstorage *p)
{
   // non atomic because const
   return( _mulle_atomic_pointer_nonatomic_read( &p->n_hashs) == (void *) -1);
}



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
   mulle_atomic_pointer_t                          allocator;
};

#pragma mark - single-threaded


// Returns:
//   0      : OK
//   EINVAL : invalid argument
//   ENOMEM : out of memory
//
static inline int
   mulle_concurrent_hashmap_init( struct mulle_concurrent_hashmap *map,
                                  unsigned int size,
                                  struct mulle_allocator *allocator)
{
   MULLE__CONCURRENT_GLOBAL
   int  _mulle_concurrent_hashmap_init( struct mulle_concurrent_hashmap *map,
                                        unsigned int size,
                                        struct mulle_allocator *allocator);
   if( ! map)
      return( EINVAL);
   return( _mulle_concurrent_hashmap_init( map, size, allocator));
}


static inline void
   mulle_concurrent_hashmap_done( struct mulle_concurrent_hashmap *map)
{
   MULLE__CONCURRENT_GLOBAL
   void  _mulle_concurrent_hashmap_done( struct mulle_concurrent_hashmap *map);

   if( map)
      _mulle_concurrent_hashmap_done( map);
}


static inline unsigned int
   mulle_concurrent_hashmap_get_size( struct mulle_concurrent_hashmap *map)
{
   MULLE__CONCURRENT_GLOBAL
   unsigned int  _mulle_concurrent_hashmap_get_size( struct mulle_concurrent_hashmap *map);

   if( ! map)
      return( 0);
   return( _mulle_concurrent_hashmap_get_size( map));
}


#pragma mark - multi-threaded

// Return value (rval):
//
//     MULLE_CONCURRENT_NO_POINTER      : means it did insert
//     MULLE_CONCURRENT_INVALID_POINTER : error (check errno)
//     other                            : value that was already registered
//
// Do not use hash=0
// Do not use value=0 or value=INTPTR_MIN
//
MULLE__CONCURRENT_GLOBAL
void   *mulle_concurrent_hashmap_register( struct mulle_concurrent_hashmap *map,
                                           intptr_t hash,
                                           void *value);
// Return value (rval):
//   0      : OK, inserted
//   EEXIST : detected duplicate
//   EINVAL : invalid argument
//   ENOMEM : must be out of memory
//
// Do not use hash=0
// Do not use value=0 or value=INTPTR_MIN
//
MULLE__CONCURRENT_GLOBAL
int   mulle_concurrent_hashmap_insert( struct mulle_concurrent_hashmap *map,
                                       intptr_t hash,
                                       void *value);


// if rval == NULL, not found

static inline void
   *mulle_concurrent_hashmap_lookup( struct mulle_concurrent_hashmap *map,
                                     intptr_t hash)
{
   MULLE__CONCURRENT_GLOBAL
   void  *_mulle_concurrent_hashmap_lookup( struct mulle_concurrent_hashmap *map,
                                           intptr_t hash);

   if( ! map)
      return( NULL);
   return( _mulle_concurrent_hashmap_lookup( map, hash));
}


// if rval == 0, removed
// rval == ENOENT, not found (hash/value pair does not exist (anymore))
// rval == EINVAL, parameter has invalid value
// rval == ENOMEM, must be out of memory

MULLE__CONCURRENT_GLOBAL
int   mulle_concurrent_hashmap_remove( struct mulle_concurrent_hashmap *map,
                                       intptr_t hash,
                                       void *value);


#pragma mark - limited multi-threaded

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
static inline struct mulle_concurrent_hashmapenumerator
   mulle_concurrent_hashmap_enumerate( struct mulle_concurrent_hashmap *map)
{
   struct mulle_concurrent_hashmapenumerator   rover;

   rover.map   = map;
   rover.index = map ? 0 : (unsigned int) -1;
   rover.mask  = 0;

   return( rover);
}


//  1         : OK
//  0         : nothing left
// ECANCELLED : mutation alert
// ENOMEM     : out of memory
// EINVAL     : wrong parameter value

static inline int
  mulle_concurrent_hashmapenumerator_next( struct mulle_concurrent_hashmapenumerator *rover,
                                           intptr_t *hash,
                                           void **value)
{
   MULLE__CONCURRENT_GLOBAL
   int  _mulle_concurrent_hashmapenumerator_next( struct mulle_concurrent_hashmapenumerator *rover,
                                                 intptr_t *hash,
                                                 void **value);
   if( ! rover)
      return( EINVAL);
   return( _mulle_concurrent_hashmapenumerator_next( rover, hash, value));
}


static inline void
   mulle_concurrent_hashmapenumerator_done( struct mulle_concurrent_hashmapenumerator *rover)
{
}


#pragma mark - enumerator conveniences

MULLE__CONCURRENT_GLOBAL
void           *mulle_concurrent_hashmap_lookup_any( struct mulle_concurrent_hashmap *map);

MULLE__CONCURRENT_GLOBAL
unsigned int   mulle_concurrent_hashmap_count( struct mulle_concurrent_hashmap *map);


#pragma mark - various functions, no parameter checks

MULLE__CONCURRENT_GLOBAL
int  _mulle_concurrent_hashmap_init( struct mulle_concurrent_hashmap *map,
                                     unsigned int size,
                                     struct mulle_allocator *allocator);
MULLE__CONCURRENT_GLOBAL
void  _mulle_concurrent_hashmap_done( struct mulle_concurrent_hashmap *map);

MULLE__CONCURRENT_GLOBAL
unsigned int  _mulle_concurrent_hashmap_get_size( struct mulle_concurrent_hashmap *map);


MULLE__CONCURRENT_GLOBAL
void   *_mulle_concurrent_hashmap_register( struct mulle_concurrent_hashmap *map,
                                            intptr_t hash,
                                            void *value);

MULLE__CONCURRENT_GLOBAL
int  _mulle_concurrent_hashmap_insert( struct mulle_concurrent_hashmap *map,
                                       intptr_t hash,
                                       void *value);

MULLE__CONCURRENT_GLOBAL
void  *_mulle_concurrent_hashmap_lookup( struct mulle_concurrent_hashmap *map,
                                         intptr_t hash);

MULLE__CONCURRENT_GLOBAL
int  _mulle_concurrent_hashmap_remove( struct mulle_concurrent_hashmap *map,
                                       intptr_t hash,
                                       void *value);


MULLE__CONCURRENT_GLOBAL
int  _mulle_concurrent_hashmapenumerator_next( struct mulle_concurrent_hashmapenumerator *rover,
                                               intptr_t *hash,
                                               void **value);


#define mulle_concurrent_hashmap_for( array, hash, item, rval)                                                        \
   for( struct mulle_concurrent_hashmapenumerator rover__ ## item = mulle_concurrent_hashmap_enumerate( array); \
      (rval = _mulle_concurrent_hashmapenumerator_next( &rover__ ## item, hash, item)) == 1;)


#endif /* mulle_concurrent_hashmap_h */
