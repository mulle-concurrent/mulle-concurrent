//
//  mulle-concurrent-hashmap2.h
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
#ifndef mulle_concurrent_hashmap2_h__
#define mulle_concurrent_hashmap2_h__

#include "include.h"

#include <errno.h>
#include <stdint.h>


//
// EXPERIMENTAL. See dox/REMOVE-MISERY-HASHMAP.md for why this exists.
//
// mulle_concurrent_hashmap2 is a resizable, wait-free hashmap whose migration
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
// The remaining subtlety is that a writer reads the hash word and then CASes
// the value word, so a freeze can slip in between. Writers therefore re-read
// the hash word after a successful value CAS; if it turned frozen they carry
// their own value forward (or, for remove, redo the removal in the newer
// generation). See "post-check" in the implementation.
//
#define MULLE_CONCURRENT_HASHMAP2_FROZEN   ((intptr_t) INTPTR_MIN)


struct _mulle_concurrent_hashmap2pair
{
   mulle_atomic_pointer_t   hash;    // intptr_t, 0 == unclaimed, FROZEN bit
   mulle_atomic_pointer_t   value;   // payload or NULL
};


struct _mulle_concurrent_hashmap2storage
{
   mulle_atomic_pointer_t   n_hashs;   // claimed slots, live or emptied
   uintptr_t                mask;

   struct _mulle_concurrent_hashmap2pair   entries[ 1];
};


union mulle_concurrent_atomichashmap2storage_t
{
   struct _mulle_concurrent_hashmap2storage   *storage;
   mulle_atomic_pointer_t                     pointer;
};


struct mulle_concurrent_hashmap2
{
   union mulle_concurrent_atomichashmap2storage_t   storage;
   union mulle_concurrent_atomichashmap2storage_t   next_storage;
   mulle_atomic_pointer_t                           allocator;
};


#pragma mark - single-threaded

//
//  0      : OK
//  EINVAL : invalid argument
//
MULLE__CONCURRENT_GLOBAL
int   mulle_concurrent_hashmap2_init( struct mulle_concurrent_hashmap2 *map,
                                      unsigned int size,
                                      struct mulle_allocator *allocator);

MULLE__CONCURRENT_GLOBAL
void  mulle_concurrent_hashmap2_done( struct mulle_concurrent_hashmap2 *map);


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
int   mulle_concurrent_hashmap2_insert( struct mulle_concurrent_hashmap2 *map,
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
int   mulle_concurrent_hashmap2_register( struct mulle_concurrent_hashmap2 *map,
                                         intptr_t hash,
                                         void *value,
                                         void **p_old);

//
//  0      : removed
//  ENOENT : the (hash,value) pair is not present
//  EINVAL : invalid argument
//
MULLE__CONCURRENT_GLOBAL
int   mulle_concurrent_hashmap2_remove( struct mulle_concurrent_hashmap2 *map,
                                       intptr_t hash,
                                       void *value);

//
// NULL if absent, otherwise the registered value.
//
MULLE__CONCURRENT_GLOBAL
void  *mulle_concurrent_hashmap2_lookup( struct mulle_concurrent_hashmap2 *map,
                                         intptr_t hash);

MULLE__CONCURRENT_GLOBAL
unsigned int   mulle_concurrent_hashmap2_get_size( struct mulle_concurrent_hashmap2 *map);

MULLE__CONCURRENT_GLOBAL
unsigned int   mulle_concurrent_hashmap2_count( struct mulle_concurrent_hashmap2 *map);


//
// Retire the current generation into a fresh one of the same size. Primarily a
// test hook: it lets a probe thread force continuous generation changes without
// doubling memory each time, which is what makes the remove-versus-copy race
// reproducible. See dox/HASHMAP2.md.
//
MULLE__CONCURRENT_GLOBAL
void   _mulle_concurrent_hashmap2_migrate_same_size( struct mulle_concurrent_hashmap2 *map);


#pragma mark - limited multi-threaded

struct mulle_concurrent_hashmap2enumerator
{
   struct mulle_concurrent_hashmap2   *map;
   void                               *storage;   // compared only, never dereferenced
   unsigned int                       index;
};


//  1          : OK
//  0          : nothing left
//  ECANCELED  : the storage migrated, restart the enumeration
//  EINVAL     : wrong parameter value
//
MULLE__CONCURRENT_GLOBAL
int   _mulle_concurrent_hashmap2enumerator_next( struct mulle_concurrent_hashmap2enumerator *rover,
                                                 intptr_t *p_hash,
                                                 void **p_value);


static inline struct mulle_concurrent_hashmap2enumerator
   mulle_concurrent_hashmap2_enumerate( struct mulle_concurrent_hashmap2 *map)
{
   struct mulle_concurrent_hashmap2enumerator   rover;

   rover.map     = map;
   rover.storage = NULL;
   rover.index   = map ? 0 : (unsigned int) -1;

   return( rover);
}


static inline int
   mulle_concurrent_hashmap2enumerator_next( struct mulle_concurrent_hashmap2enumerator *rover,
                                             intptr_t *p_hash,
                                             void **p_value)
{
   if( ! rover)
      return( EINVAL);
   if( rover->index == (unsigned int) -1)
      return( 0);
   return( _mulle_concurrent_hashmap2enumerator_next( rover, p_hash, p_value));
}


static inline void
   mulle_concurrent_hashmap2enumerator_done( struct mulle_concurrent_hashmap2enumerator *rover)
{
   MULLE_C_UNUSED( rover);
}

#endif
