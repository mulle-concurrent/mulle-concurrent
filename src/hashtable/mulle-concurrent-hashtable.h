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

#include <errno.h>
#include <stdint.h>


//
// Maps a "hash" to a value. The top bit of the hash will be lost!
// So you can't use to index with arbitrary intptr_t values.
// E.g. on 16 bit   0x8001 and 0x0001 return the same stored value.
//

// mulle_concurrent_hashtable is a resizable, wait-free hashmap whose migration
// state lives in the *hash* word instead of the value word. The original
// hashmap freezes a slot by overwriting its value with a REDIRECT sentinel,
// which destroys the payload. That forces the copier to install a value into
// the destination *before* it can validate that the value is still live, and
// that ordering is what lets a successful remove be silently undone.
//
// Here freezing marks the hash word and leaves the value intact:
//
//   | hash word      | value word | meaning
//   |----------------|------------|-----------------------------------------
//   | NO_HASH        | EMPTY      | virgin
//   | FROZEN         | EMPTY      | retired virgin, nothing was ever here
//   | h              | EMPTY      | claimed, empty or removed (reusable!)
//   | h              | V          | live entry
//   | h | FROZEN     | V          | frozen, V is final and still readable
//   | h | FROZEN     | EMPTY      | frozen and drained (or never had a value)
//
// Consequences:
//
//   * Freezing is the commit point. Once the FROZEN bit is set the value is
//     final, so a copier reads it *after* committing and can never carry a
//     stale value. A removal either lands before the freeze (value is EMPTY,
//     nothing is carried) or is rejected and retried in the newer generation.
//   * A frozen slot still holds its payload, so any helper can complete the
//     carry. No value needs to be installed speculatively.
//   * Because a frozen slot preserves its payload, "frozen" cannot double as
//     "already migrated". So a carried value is *consumed*: set back to EMPTY
//     once it is safely in the newer generation. Without that, re-running copy
//     over a stale generation would reinject values that were legitimately
//     removed in a newer generation. (The original hashmap gets this for free,
//     because REDIRECT destroys the payload.)
//   * Per slot, copy performs at most two hash CAS attempts (the hash word
//     only ever goes NO_HASH -> h -> h|FROZEN), one read and one carry,
//     regardless of how much the value churns.
//   * Because copy is bounded without needing a monotonic value word, there
//     are no tombstones. A removed slot keeps its hash claim, so probe chains
//     stay intact, and it is immediately reusable by a plain value CAS.
//   * The value word has exactly two states, empty or live, so NULL is the
//     only reserved payload. INVALID_POINTER and TOMBSTONE_POINTER are not
//     reserved here.
//
// The price is one reserved bit in the hash space: the topmost bit is the
// FROZEN flag, so hashes are folded into the low bits (a hash is a hash, so
// this is harmless, but it is not an identity mapping).
//
// HASH-WIDTH RESTRICTION:
//
//   Legal hash range: [1, INTPTR_MAX]  (topmost bit must be clear, 0 is NO_HASH)
//
//   On LP64 this is 63 usable bits. User-space pointers never have bit 63 set,
//   so "pointer identity as hash" remains safe.
//
//   On ILP32 this is 31 usable bits. Pointers above 0x80000000 CANNOT be used
//   as hashes — they would alias with their & 0x7FFFFFFF counterpart, and since
//   the hash IS the key in this design, two distinct pointers would collide
//   silently. An assert fires in debug builds if the FROZEN bit is set.
//
// The remaining subtlety is that a writer reads the hash word and then CASes
// the value word, so a freeze can slip in between. Writers therefore re-read
// the hash word after a successful value CAS; if it turned frozen they carry
// their own value forward (or, for remove, redo the removal in the newer
// generation). See "post-check" in the implementation.
//
#include <limits.h>

struct _mulle_concurrent_hashtablepair
{
   mulle_atomic_pointer_t   hash;    // intptr_t, 0 == unclaimed, FROZEN bit
   mulle_atomic_pointer_t   value;   // payload or NULL
};


struct _mulle_concurrent_hashtablestorage
{
   mulle_atomic_pointer_t   n_hashs;   // claimed slots, live or emptied
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
   union mulle_concurrent_atomichashtablestorage_t   next_storage;
   mulle_atomic_pointer_t                           allocator;
   uintptr_t                                        frozen_bit;
   uintptr_t                                        hash_mask;
};


#pragma mark - single-threaded

//
// FROZEN bit placement:
//
//   "positive" mode (default): FROZEN = top bit, hashes must be in [1, INTPTR_MAX].
//                              Use for arbitrary hashes. Pointer-as-hash is safe
//                              on LP64 (user-space never sets bit 63), UNSAFE on ILP32.
//
//   "even" mode:               FROZEN = bit 0, hashes must be even and non-zero.
//                              Use when hashes are aligned pointer addresses (always even).
//                              Full address range available, no aliasing on any platform.
//

//
// Default: top bit is FROZEN ("positive" mode).
//
MULLE_C_NONNULL_FIRST
MULLE__CONCURRENT_GLOBAL
void   _mulle_concurrent_hashtable_init_positive( struct mulle_concurrent_hashtable *map,
                                                  size_t size,
                                                  struct mulle_allocator *allocator);
//
// Low bit is FROZEN ("even" mode). All hashes must be even (aligned pointers).
//
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
// NULL if absent, otherwise the registered value.
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
