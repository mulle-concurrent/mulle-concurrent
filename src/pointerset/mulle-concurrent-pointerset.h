//
//  mulle-concurrent-pointerset.h
//  mulle-concurrent
//
//  Copyright (c) 2026 Nat! - Mulle kybernetiK.
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
//  mulle-concurrent-pointerset.h
//  mulle-concurrent
//
//  A lock- and wait-free set of void * pointers, built on top of the
//  hashmap storage but with a single-word-per-slot layout.
//
//  Slot values:
//    NULL              (MULLE_CONCURRENT_NO_POINTER)      - empty
//    INTPTR_MIN        (MULLE_CONCURRENT_INVALID_POINTER) - REDIRECT (migration)
//    (void *) -1       (MULLE_CONCURRENT_TOMBSTONE)       - removed
//    anything else                                        - live pointer
//
#ifndef mulle_concurrent_pointerset_h__
#define mulle_concurrent_pointerset_h__

#include "include.h"

#include <errno.h>
#include "mulle-concurrent-types.h"


#include <stdint.h>

// sentinel for removed slots; must not be a valid user pointer.
// INTPTR_MAX (high bit clear) is never a valid userspace address on any
// real architecture, making it safer than (void*)-1 which equals UINTPTR_MAX
// and could theoretically appear as a kernel/device mapping address.


struct _mulle_concurrent_pointersetstorage
{
   mulle_atomic_pointer_t   n_used;   // empty + tombstone slots consumed
   uintptr_t                mask;

   mulle_atomic_pointer_t   entries[ 1];
};


static inline int
   _mulle_concurrent_pointersetstorage_is_const( struct _mulle_concurrent_pointersetstorage *p)
{
   return( _mulle_atomic_pointer_nonatomic_read( &p->n_used) == (void *) -1);
}


union mulle_concurrent_atomicpointersetstorage_t
{
   struct _mulle_concurrent_pointersetstorage  *storage;
   mulle_atomic_pointer_t                       pointer;
};


struct mulle_concurrent_pointerset
{
   union mulle_concurrent_atomicpointersetstorage_t   storage;
   union mulle_concurrent_atomicpointersetstorage_t   next_storage;
   mulle_atomic_pointer_t                             allocator;
};


#pragma mark - no-check variants

MULLE__CONCURRENT_GLOBAL
int  _mulle_concurrent_pointerset_init( struct mulle_concurrent_pointerset *set,
                                        unsigned int size,
                                        struct mulle_allocator *allocator);
MULLE__CONCURRENT_GLOBAL
void  _mulle_concurrent_pointerset_done( struct mulle_concurrent_pointerset *set);

MULLE__CONCURRENT_GLOBAL
unsigned int  _mulle_concurrent_pointerset_get_size( struct mulle_concurrent_pointerset *set);

MULLE__CONCURRENT_GLOBAL
void  *_mulle_concurrent_pointerset_register( struct mulle_concurrent_pointerset *set,
                                              void *ptr);

MULLE__CONCURRENT_GLOBAL
int  _mulle_concurrent_pointerset_insert( struct mulle_concurrent_pointerset *set,
                                          void *ptr);

MULLE__CONCURRENT_GLOBAL
int  _mulle_concurrent_pointerset_member( struct mulle_concurrent_pointerset *set,
                                          void *ptr);

MULLE__CONCURRENT_GLOBAL
int  _mulle_concurrent_pointerset_remove( struct mulle_concurrent_pointerset *set,
                                          void *ptr);


#pragma mark - single-threaded

static inline int
   mulle_concurrent_pointerset_init( struct mulle_concurrent_pointerset *set,
                                     unsigned int size,
                                     struct mulle_allocator *allocator)
{
   if( ! set)
      return( EINVAL);
   return( _mulle_concurrent_pointerset_init( set, size, allocator));
}


static inline void
   mulle_concurrent_pointerset_done( struct mulle_concurrent_pointerset *set)
{
   if( set)
      _mulle_concurrent_pointerset_done( set);
}


static inline unsigned int
   mulle_concurrent_pointerset_get_size( struct mulle_concurrent_pointerset *set)
{
   return( set ? _mulle_concurrent_pointerset_get_size( set) : 0);
}


static inline struct mulle_allocator *
   mulle_concurrent_pointerset_get_allocator( struct mulle_concurrent_pointerset *set)
{
   return( set
           ? (struct mulle_allocator *) _mulle_atomic_pointer_read( &set->allocator)
           : NULL);
}



static inline void
   mulle_concurrent_pointerset_reset( struct mulle_concurrent_pointerset *set)
{
   size_t                   size;
   struct mulle_allocator   *allocator;

   size      = mulle_concurrent_pointerset_get_size( set);
   allocator = mulle_concurrent_pointerset_get_allocator( set);
   mulle_concurrent_pointerset_done( set);
   mulle_concurrent_pointerset_init( set, size, allocator);
}



#pragma mark - multi-threaded

// Returns:
//   MULLE_CONCURRENT_NO_POINTER      : inserted (not previously present)
//   MULLE_CONCURRENT_INVALID_POINTER : error (check errno)
//   ptr                              : already present, returns the pointer
//
// Do not pass NULL, INTPTR_MIN, or (void*)-1 as ptr.
//
MULLE__CONCURRENT_GLOBAL
void  *mulle_concurrent_pointerset_register( struct mulle_concurrent_pointerset *set,
                                             void *ptr);

// Returns:
//   0      : inserted
//   EEXIST : already present
//   EINVAL : invalid argument
//   ENOMEM : out of memory
//
MULLE__CONCURRENT_GLOBAL
int   mulle_concurrent_pointerset_insert( struct mulle_concurrent_pointerset *set,
                                          void *ptr);

// Returns 1 if ptr is in the set, 0 if not.
static inline int
   mulle_concurrent_pointerset_member( struct mulle_concurrent_pointerset *set,
                                       void *ptr)
{
   if( ! set || ! ptr)
      return( 0);
   return( _mulle_concurrent_pointerset_member( set, ptr));
}

// Returns:
//   0      : removed
//   ENOENT : not found
//   EINVAL : invalid argument
//   ENOMEM : out of memory
//
MULLE__CONCURRENT_GLOBAL
int   mulle_concurrent_pointerset_remove( struct mulle_concurrent_pointerset *set,
                                          void *ptr);


#pragma mark - limited multi-threaded (enumerator)

struct mulle_concurrent_pointerset_enumerator
{
   struct mulle_concurrent_pointerset   *set;
   unsigned int                          index;
   unsigned int                          mask;
};

MULLE__CONCURRENT_GLOBAL
int  _mulle_concurrent_pointerset_enumerator_next( struct mulle_concurrent_pointerset_enumerator *rover,
                                                   void **ptr);


static inline struct mulle_concurrent_pointerset_enumerator
   mulle_concurrent_pointerset_enumerate( struct mulle_concurrent_pointerset *set)
{
   struct mulle_concurrent_pointerset_enumerator   rover;

   rover.set   = set;
   rover.index = set ? 0 : (unsigned int) -1;
   rover.mask  = 0;

   return( rover);
}


//  1          : OK, *ptr filled
//  0          : done
//  ECANCELED  : mutation detected
//  ENOMEM     : out of memory
//
static inline int
   mulle_concurrent_pointerset_enumerator_next( struct mulle_concurrent_pointerset_enumerator *rover,
                                                void **ptr)
{
   if( ! rover)
      return( EINVAL);
   return( _mulle_concurrent_pointerset_enumerator_next( rover, ptr));
}


static inline void
   mulle_concurrent_pointerset_enumerator_done( struct mulle_concurrent_pointerset_enumerator *rover)
{
   MULLE_C_UNUSED( rover);
}


#pragma mark - conveniences

MULLE__CONCURRENT_GLOBAL
void          *mulle_concurrent_pointerset_lookup_any( struct mulle_concurrent_pointerset *set);

MULLE__CONCURRENT_GLOBAL
unsigned int   mulle_concurrent_pointerset_count( struct mulle_concurrent_pointerset *set);


#define mulle_concurrent_pointerset_for( name, ptr)                                                                              \
   assert( sizeof( ptr) == sizeof( void *));                                                                                     \
   for( struct mulle_concurrent_pointerset_enumerator                                                                            \
           rover__ ## ptr = mulle_concurrent_pointerset_enumerate( name),                                                        \
           *rover__ ## ptr ## __i = (void *) 0;                                                                                  \
        ! rover__ ## ptr ## __i;                                                                                                  \
        rover__ ## ptr ## __i = (mulle_concurrent_pointerset_enumerator_done( &rover__ ## ptr),                                  \
                                 (void *) 1))                                                                                     \
      while( _mulle_concurrent_pointerset_enumerator_next( &rover__ ## ptr, (void **) &ptr) == 1)


#endif /* mulle_concurrent_pointerset_h__ */
