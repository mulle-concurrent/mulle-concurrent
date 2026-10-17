//
//  mulle-concurrent-hashtable.h
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
#ifndef mulle_concurrent_hashtable_h__
#define mulle_concurrent_hashtable_h__

#include "include.h"
#include "mulle-concurrent-types.h"

#include <errno.h>
#include <stdint.h>

// THIS CODE IS STILL EXPERIMENTAL, THOUGH USED ALREADY IN THE MULLE-OBJC
// RUNTIME
//
// EXPERIMENTAL: the migration protocol has had several race bugs so far
// The current design is soak-tested and exhaustively schedule-checked on small
// scenarios (test/hashtable/explore.c), but not under weak memory ordering.
// Prefer mulle_concurrent_hashmap if you can live without a concurrent remove.
// Also this is the only "vibecoded" data structure.
//
// mulle_concurrent_hashtable is a resizable, wait-free map from a "hash" (the
// hash IS the key) to a value, with a concurrent remove. It follows the state
// machine of Cliff Click's NonBlockingHashMap, but marks states with reserved
// bits instead of boxing every value:
//
//   * the hash word has a FROZEN bit (top bit or bit 0, see "hash mode")
//   * the value word has a SEALED bit (top bit or bit 0, see "value mode")
//     and one reserved TOMB value for "removed"
//
// Per generation the slot states are:
//
//   | hash word   | value word  | meaning
//   |-------------|-------------|------------------------------------------
//   | NO_HASH     | EMPTY       | virgin
//   | FROZEN      | EMPTY       | retired virgin, can not be claimed anymore
//   | h           | EMPTY       | claimed, never held a value
//   | h           | V           | live
//   | h           | TOMB        | removed, reusable by insert/register
//   | h (|FROZEN) | x | SEALED  | retired, x (EMPTY, V or TOMB) is final
//
// Migration copies slot by slot: freeze the hash word (writers that read it
// frozen stop and help), seal the value word (writers that were already past
// the hash check now fail their value CAS), then carry a sealed live V into
// the generation's own successor (storage->next), installing it ONLY if the
// destination value word is still EMPTY.
//
// Consequences:
//
//   * The seal is the commit point. After it no writer can change the value,
//     so every helper carries the same final value.
//   * A sealed slot keeps its payload, so lookup simply strips the bit and
//     never has to retry or help.
//   * A destination slot that has ever held a value is never EMPTY again
//     (removal writes TOMB, not EMPTY). So a lagging copier can never reinject
//     a value that was carried and then removed in the newer generation.
//   * Every generation links to its own successor, so a thread that stalled
//     across several migrations helps finish the one right after its
//     generation instead of skipping ahead.
//   * Writers do exactly one value CAS per generation. The FROZEN gate on the
//     hash word means only writers that are already in flight can race the
//     seal, which bounds the copier's work per slot by the number of threads.
//
// HASH MODE (FROZEN bit):
//
//   "positive" (default): FROZEN = top bit, legal hashes are [1, INTPTR_MAX].
//                         Pointer-as-hash is safe on LP64, UNSAFE on ILP32.
//   "even":               FROZEN = bit 0, legal hashes are even and non-zero.
//                         Use for aligned pointer addresses, any platform.
//
// VALUE MODE (SEALED bit and TOMB):
//
//   "even" (default):     SEALED = bit 0, TOMB = MULLE_CONCURRENT_INVALID_POINTER.
//                         Legal values are even, non-NULL and not TOMB.
//                         Use for (aligned) pointers, any platform.
//   "positive":           SEALED = top bit, TOMB = MULLE_CONCURRENT_TOMBSTONE_POINTER.
//                         Legal values are [1, INTPTR_MAX - 1]. Use for
//                         integers.
//
// Illegal hashes or values are rejected (EINVAL, lookup returns NULL), never
// silently folded.
//
#include <limits.h>

struct _mulle_concurrent_hashtablepair
{
   mulle_atomic_pointer_t   hash;    // 0 == unclaimed, may carry FROZEN
   mulle_atomic_pointer_t   value;   // EMPTY, V, TOMB, may carry SEALED
};


struct _mulle_concurrent_hashtablestorage
{
   mulle_atomic_pointer_t   n_hashs;   // claimed slots, live or emptied
   mulle_atomic_pointer_t   next;      // successor generation, NULL if none
   uintptr_t                mask;

   struct _mulle_concurrent_hashtablepair   entries[ 1];
};


union mulle_concurrent_atomichashtablestorage_t
{
   struct _mulle_concurrent_hashtablestorage   *storage;
   mulle_atomic_pointer_t                     pointer;
};


struct mulle_concurrent_hashtable
{
   union mulle_concurrent_atomichashtablestorage_t   storage;
   mulle_atomic_pointer_t                           allocator;
   uintptr_t                                        frozen_bit;
   uintptr_t                                        hash_mask;
   uintptr_t                                        sealed_bit;
   void                                             *tombstone;
};


#pragma mark - single-threaded

//
// Mode bits for _mulle_concurrent_hashtable_init_with_mode. Combine one hash
// mode with one value mode. 0 is the default: positive hashes, even values.
//
#define MULLE_CONCURRENT_HASHTABLE_POSITIVE_HASH    0x0
#define MULLE_CONCURRENT_HASHTABLE_EVEN_HASH        0x1
#define MULLE_CONCURRENT_HASHTABLE_EVEN_VALUE       0x0
#define MULLE_CONCURRENT_HASHTABLE_POSITIVE_VALUE   0x2

//
// The size is a hint: it is rounded up to the next power of two and clamped
// to a minimum of 4, so any size is accepted. mulle_concurrent_hashtable_get_size
// reports the size that was actually used.
//
MULLE_C_NONNULL_FIRST
MULLE__CONCURRENT_GLOBAL
void   _mulle_concurrent_hashtable_init_with_mode( struct mulle_concurrent_hashtable *map,
                                                   size_t size,
                                                   unsigned int mode,
                                                   struct mulle_allocator *allocator);

// positive hashes, even values
MULLE_C_NONNULL_FIRST
MULLE__CONCURRENT_GLOBAL
void   _mulle_concurrent_hashtable_init_positive( struct mulle_concurrent_hashtable *map,
                                                  size_t size,
                                                  struct mulle_allocator *allocator);

// even hashes, even values
MULLE_C_NONNULL_FIRST
MULLE__CONCURRENT_GLOBAL
void   _mulle_concurrent_hashtable_init_even( struct mulle_concurrent_hashtable *map,
                                              size_t size,
                                              struct mulle_allocator *allocator);


static inline
void   mulle_concurrent_hashtable_init( struct mulle_concurrent_hashtable *map,
                                        size_t size,
                                        struct mulle_allocator *allocator)
{
   _mulle_concurrent_hashtable_init_positive( map, size, allocator);
}


static inline
void   mulle_concurrent_hashtable_init_even( struct mulle_concurrent_hashtable *map,
                                             size_t size,
                                             struct mulle_allocator *allocator)
{
   _mulle_concurrent_hashtable_init_even( map, size, allocator);
}


static inline
void   mulle_concurrent_hashtable_init_positive( struct mulle_concurrent_hashtable *map,
                                                 size_t size,
                                                 struct mulle_allocator *allocator)
{
   _mulle_concurrent_hashtable_init_positive( map, size, allocator);
}


static inline
void   mulle_concurrent_hashtable_init_with_mode( struct mulle_concurrent_hashtable *map,
                                                  size_t size,
                                                  unsigned int mode,
                                                  struct mulle_allocator *allocator)
{
   _mulle_concurrent_hashtable_init_with_mode( map, size, mode, allocator);
}


MULLE__CONCURRENT_GLOBAL
void  mulle_concurrent_hashtable_done( struct mulle_concurrent_hashtable *map);


#pragma mark - multi-threaded

//
//  0      : did insert
//  EEXIST : a live value is already registered for 'hash'
//  EINVAL : invalid argument
//
// A previously removed 'hash' is immediately insertable again, no migration
// and no intermediate error state.
//
MULLE__CONCURRENT_GLOBAL
int   mulle_concurrent_hashtable_insert( struct mulle_concurrent_hashtable *map,
                                       intptr_t hash,
                                       void *value);

//
// Insert 'value' if 'hash' is absent. '*p_old' is set to the value that is
// registered afterwards: NULL if we inserted, otherwise the pre-existing
// value (in which case 'value' was not stored). 'p_old' may be NULL.
//
//  0      : OK
//  EINVAL : invalid argument
//
MULLE__CONCURRENT_GLOBAL
int   mulle_concurrent_hashtable_register( struct mulle_concurrent_hashtable *map,
                                         intptr_t hash,
                                         void *value,
                                         void **p_old);

//
//  0      : removed
//  ENOENT : the (hash,value) pair is not present
//  EINVAL : invalid argument
//
MULLE__CONCURRENT_GLOBAL
int   mulle_concurrent_hashtable_remove( struct mulle_concurrent_hashtable *map,
                                       intptr_t hash,
                                       void *value);

//
// Returns non-zero if 'hash' is a legal key for 'map': non-zero and without the
// mode's FROZEN bit (topmost bit in positive mode, bit 0 in even mode). The
// mutating calls reject an illegal hash with EINVAL, and lookup() reports it as
// NULL, exactly like an absent key. A caller that must tell the two apart
// should test with this first.
//
static inline int
   mulle_concurrent_hashtable_hash_is_valid( struct mulle_concurrent_hashtable *map,
                                             intptr_t hash)
{
   return( map &&
           (uintptr_t) hash != MULLE_CONCURRENT_NO_HASH &&
           ((uintptr_t) hash & ~map->hash_mask) == 0);
}

//
// Returns non-zero if 'value' is a legal payload for 'map': non-NULL, without
// the mode's SEALED bit (bit 0 in even value mode, topmost bit in positive
// value mode) and not the mode's TOMB value. The mutating calls reject an
// illegal value with EINVAL.
//
static inline int
   mulle_concurrent_hashtable_value_is_valid( struct mulle_concurrent_hashtable *map,
                                              void *value)
{
   return( map &&
           value != MULLE_CONCURRENT_NO_POINTER &&
           ((uintptr_t) value & map->sealed_bit) == 0 &&
           value != map->tombstone);
}

//
// NULL if absent or if 'hash' is not a legal key (see
// mulle_concurrent_hashtable_hash_is_valid), otherwise the registered value.
//
MULLE__CONCURRENT_GLOBAL
void  *mulle_concurrent_hashtable_lookup( struct mulle_concurrent_hashtable *map,
                                         intptr_t hash);

MULLE__CONCURRENT_GLOBAL
size_t   mulle_concurrent_hashtable_get_size( struct mulle_concurrent_hashtable *map);

MULLE__CONCURRENT_GLOBAL
size_t   mulle_concurrent_hashtable_count( struct mulle_concurrent_hashtable *map);


//
// Retire the current generation into a fresh one of the same size. Primarily a
// test hook: it lets a probe thread force continuous generation changes without
// doubling memory each time, which is what makes the remove-versus-copy race
// reproducible. See dox/HASHTABLE.md.
//
MULLE__CONCURRENT_GLOBAL
void   _mulle_concurrent_hashtable_migrate_same_size( struct mulle_concurrent_hashtable *map);


#pragma mark - limited multi-threaded

struct mulle_concurrent_hashtableenumerator
{
   struct mulle_concurrent_hashtable   *map;
   void                               *storage;   // compared only, never dereferenced
   unsigned int                       index;
};


//  1          : OK
//  0          : nothing left
//  ECANCELED  : the storage migrated, restart the enumeration
//  EINVAL     : wrong parameter value
//
MULLE__CONCURRENT_GLOBAL
int   _mulle_concurrent_hashtableenumerator_next( struct mulle_concurrent_hashtableenumerator *rover,
                                                 intptr_t *p_hash,
                                                 void **p_value);


static inline struct mulle_concurrent_hashtableenumerator
   mulle_concurrent_hashtable_enumerate( struct mulle_concurrent_hashtable *map)
{
   struct mulle_concurrent_hashtableenumerator   rover;

   rover.map     = map;
   rover.storage = NULL;
   rover.index   = map ? 0 : (unsigned int) -1;

   return( rover);
}


static inline int
   mulle_concurrent_hashtableenumerator_next( struct mulle_concurrent_hashtableenumerator *rover,
                                             intptr_t *p_hash,
                                             void **p_value)
{
   if( ! rover)
      return( EINVAL);
   if( rover->index == (unsigned int) -1)
      return( 0);
   return( _mulle_concurrent_hashtableenumerator_next( rover, p_hash, p_value));
}


static inline void
   mulle_concurrent_hashtableenumerator_done( struct mulle_concurrent_hashtableenumerator *rover)
{
   MULLE_C_UNUSED( rover);
}



#define mulle_concurrent_hashtable_for_rval( name, hash, value, rval)                                                         \
   assert( sizeof( hash) == sizeof( intptr_t));                                                                               \
   assert( sizeof( value) == sizeof( void *));                                                                                \
   for( struct mulle_concurrent_hashtableenumerator                                                                           \
           rover__ ## hash ## __ ## value = mulle_concurrent_hashtable_enumerate( name),                                      \
           *rover__  ## hash ## __ ## value ## __i = (void *) 0;                                                              \
        ! rover__  ## hash ## __ ## value ## __i;                                                                             \
        rover__ ## hash ## __ ## value ## __i = (mulle_concurrent_hashtableenumerator_done( &rover__ ## hash ## __ ## value), \
                                              (void *) 1))                                                                    \
      while( (rval = _mulle_concurrent_hashtableenumerator_next( &rover__ ## hash ## __ ## value,                             \
                                                       (intptr_t *) &hash,                                                    \
                                                       (void **) &value)) == 1)


#define mulle_concurrent_hashtable_for( name, hash, value)                                                                    \
   assert( sizeof( hash) == sizeof( intptr_t));                                                                               \
   assert( sizeof( value) == sizeof( void *));                                                                                \
   for( struct mulle_concurrent_hashtableenumerator                                                                           \
           rover__ ## hash ## __ ## value = mulle_concurrent_hashtable_enumerate( name),                                      \
           *rover__  ## hash ## __ ## value ## __i = (void *) 0;                                                              \
        ! rover__  ## hash ## __ ## value ## __i;                                                                             \
        rover__ ## hash ## __ ## value ## __i = (mulle_concurrent_hashtableenumerator_done( &rover__ ## hash ## __ ## value), \
                                              (void *) 1))                                                                    \
      while( _mulle_concurrent_hashtableenumerator_next( &rover__ ## hash ## __ ## value,                                     \
                                                       (intptr_t *) &hash,                                                    \
                                                       (void **) &value) == 1)

#endif
