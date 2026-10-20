//
//  mulle-concurrent-hashmap.c
//  mulle-concurrent
//
//  Copyright (c) 2018 Nat! - Mulle kybernetiK.
//  Copyright (c) 2016 Codeon GmbH.
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
#include "mulle-concurrent-hashmap.h"

#include "mulle-concurrent-math.h"
#include "mulle-concurrent-types.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>


//
// empty storage is designed, so that
// you can make an optimistic read into entries
//
static const struct _mulle_concurrent_hashmapstorage   mulle_concurrent_empty_storage =
{
   (void *) -1,
   0,
   { { MULLE_CONCURRENT_NO_HASH, NULL } }
};


#define REDIRECT_VALUE   MULLE_CONCURRENT_INVALID_POINTER


//
// Test scaffolding, same idea as MULLE_CONCURRENT_HASHTABLE_RACE_YIELD.
// Defining MULLE_CONCURRENT_HASHMAP_RACE_YIELD widens the migration race
// windows so that the stochastic tests can hit them. Compiles to nothing
// otherwise.
//
#ifdef MULLE_CONCURRENT_HASHMAP_RACE_YIELD

#define  hashmap_race_yield( void)   MULLE_THREAD_UNPLEASANT_RACE_YIELD()
#else
# define hashmap_race_yield()   do {} while( 0)
#endif

//
// Named race points. By default each one is just hashmap_race_yield().
// A test can #define hashmap_race_point( site) and #include this file to park
// a thread at an exact point, which turns a probabilistic race into a
// deterministic interleaving (see test/hashmap/migration_stall.c).
//
#define HASHMAP_RACE_LOOKUP            1   // lookup: storage read, probe not started
#define HASHMAP_RACE_INSERT            2   // insert: storage read, claim not started
#define HASHMAP_RACE_REGISTER          3   // register: storage read, claim not started
#define HASHMAP_RACE_FILL              4   // insert/register: hash claimed, value not filled
#define HASHMAP_RACE_COPY_VIRGIN       5   // copy: virgin slot seen, not frozen yet
#define HASHMAP_RACE_COPY_READ         6   // copy: value read, not put into successor
#define HASHMAP_RACE_COPY_CARRIED      7   // copy: put into successor, source not frozen

#ifndef hashmap_race_point
# define hashmap_race_point( site)   hashmap_race_yield()
#endif

#pragma mark - _mulle_concurrent_hashmapstorage


static inline intptr_t
   _mulle_concurrent_hashvaluepair_get_hash( struct _mulle_concurrent_hashvaluepair *entry)
{
   return( (intptr_t) _mulle_atomic_pointer_read( &entry->hash));
}


//
// claims 'entry' for 'hash' if it is virgin. Returns the hash that owns the
// slot afterwards: 'hash' if we won the claim (or it was already claimed
// for 'hash'), otherwise the foreign hash that beat us to it. Only the
// winner increments n_hashs.
//
// This is the hash-first protocol: the hash word is the claim token and is
// the only thing a slot contender ever writes in order to take ownership.
// Since the hash word never moves backwards, a slot can not be claimed by
// two different hashes for the lifetime of the storage. That is what keeps
// a thread from ever storing its value under a foreign hash.
//
static inline intptr_t
   _mulle_concurrent_hashmapstorage_claim( struct _mulle_concurrent_hashmapstorage *p,
                                           struct _mulle_concurrent_hashvaluepair *entry,
                                           intptr_t hash)
{
   intptr_t   found;

   found = _mulle_concurrent_hashvaluepair_get_hash( entry);
   if( found != MULLE_CONCURRENT_NO_HASH)
      return( found);

   found = (intptr_t) __mulle_atomic_pointer_cas( &entry->hash,
                                                  (void *) hash,
                                                  (void *) MULLE_CONCURRENT_NO_HASH);
   if( found != MULLE_CONCURRENT_NO_HASH)
      return( found);           // lost the claim, 'found' owns the slot

   _mulle_atomic_pointer_increment( &p->n_hashs);
   return( hash);
}


// must only be called on an entry already claimed (by us) for the hash we
// are inserting. returns
//
//    MULLE_CONCURRENT_NO_POINTER      : we stored 'value'
//    MULLE_CONCURRENT_INVALID_POINTER : storage is migrating (EBUSY)
//    other                            : the value that is already there
//
static void  *_mulle_concurrent_hashmapstorage_fill( struct _mulle_concurrent_hashvaluepair *entry,
                                                     void *value)
{
   void   *found;

   hashmap_race_point( HASHMAP_RACE_FILL);

   found = __mulle_atomic_pointer_cas( &entry->value, value, MULLE_CONCURRENT_NO_POINTER);
   if( found == MULLE_CONCURRENT_NO_POINTER)
      return( MULLE_CONCURRENT_NO_POINTER);
   if( MULLE_C_UNLIKELY( found == REDIRECT_VALUE))
      return( MULLE_CONCURRENT_INVALID_POINTER);

   return( found);              // existing live value
}


// n is rounded up to a power of two (at least 4)
MULLE_C_NONNULL_RETURN
static struct _mulle_concurrent_hashmapstorage *
   _mulle_concurrent_alloc_hashmapstorage( unsigned int n,
                                           struct mulle_allocator *allocator)
{
   struct _mulle_concurrent_hashmapstorage  *p;

   if( n > ((unsigned int) -1 >> 1) + 1)
      _mulle_allocator_abort( allocator, NULL, (size_t) -1);

   n = (unsigned int) _mulle_concurrent_pow2roundup( (size_t) n);
   if( n < 4)
      n = 4;
   assert( _mulle_concurrent_is_pow2( n));

   p = _mulle_allocator_calloc( allocator, 1,
                               mulle_allocator_size_add( allocator,
                                  mulle_allocator_size_multiply( allocator,
                                                                 n - 1,
                                                                 sizeof( struct _mulle_concurrent_hashvaluepair)),
                                  sizeof( struct _mulle_concurrent_hashmapstorage)));

   p->mask = n - 1;

   /*
    * in theory, one should be able to use different values for NO_POINTER and
    * INVALID_POINTER
    */
   if( MULLE_CONCURRENT_NO_HASH || MULLE_CONCURRENT_NO_POINTER)
   {
      struct _mulle_concurrent_hashvaluepair   *q;
      struct _mulle_concurrent_hashvaluepair   *sentinel;

      q        = p->entries;
      sentinel = &p->entries[ (unsigned int) p->mask];
      while( q <= sentinel)
      {
         _mulle_atomic_pointer_nonatomic_write( &q->hash, (void *) MULLE_CONCURRENT_NO_HASH);
         _mulle_atomic_pointer_nonatomic_write( &q->value, MULLE_CONCURRENT_NO_POINTER);
         ++q;
      }
   }

   return( p);
}


static unsigned int
   _mulle_concurrent_hashmapstorage_get_max_n_hashs( struct _mulle_concurrent_hashmapstorage *p)
{
   unsigned int   size;
   unsigned int   max;

   // Start migration at half capacity. The unused half is the claim reserve
   // for operations that passed the outer occupancy check concurrently; it
   // preserves the virgin slot required to bound every linear probe.
   size = (unsigned int) p->mask + 1;
   max  = size - (size >> 1);
   return( max);
}


static void   *_mulle_concurrent_hashmapstorage_lookup( struct _mulle_concurrent_hashmapstorage *p,
                                                        intptr_t hash)
{
   struct _mulle_concurrent_hashvaluepair   *entry;
   intptr_t                                 entry_hash;
   unsigned int                             index;
   unsigned int                             sentinel;
   void                                     *value;

   index    = (unsigned int) hash;
   sentinel = (unsigned int) hash + (unsigned int) p->mask + 1;

   for(;;)
   {
      entry      = &p->entries[ index & (unsigned int) p->mask];
      entry_hash = _mulle_concurrent_hashvaluepair_get_hash( entry);

      if( entry_hash == MULLE_CONCURRENT_NO_HASH)
         return( MULLE_CONCURRENT_NO_POINTER);

      if( entry_hash == hash)
      {
         value = _mulle_atomic_pointer_read( &entry->value);
         return( value);            // may be REDIRECT: caller migrates + retries
      }

      ++index;
      if( MULLE_C_UNLIKELY( index == sentinel))
      {
         // Probe exhausted: the table has no virgin slot left (more
         // concurrent claimers than the reserved half). Report REDIRECT so
         // the caller migrates and retries in a larger generation, rather
         // than spin forever in a release (NDEBUG) build.
         return( REDIRECT_VALUE);
      }
   }
}


static struct _mulle_concurrent_hashvaluepair  *
    _mulle_concurrent_hashmapstorage_next_pair( struct _mulle_concurrent_hashmapstorage *p,
                                                unsigned int *index)
{
   struct _mulle_concurrent_hashvaluepair   *entry;
   struct _mulle_concurrent_hashvaluepair   *sentinel;

   entry    = &p->entries[ *index];
   sentinel = &p->entries[ (unsigned int) p->mask + 1];

   while( entry < sentinel)
   {
      if( _mulle_concurrent_hashvaluepair_get_hash( entry) == MULLE_CONCURRENT_NO_HASH)
      {
         ++entry;
         continue;
      }

      *index = (unsigned int) (entry - p->entries) + 1;
      return( entry);
   }
   return( NULL);
}


//
// register:
//
//  return value is either
//     MULLE_CONCURRENT_NO_POINTER      : means it did insert
//     MULLE_CONCURRENT_INVALID_POINTER : is busy
//     old                              : value that is already registered
//
static void   *_mulle_concurrent_hashmapstorage_register( struct _mulle_concurrent_hashmapstorage *p,
                                                          intptr_t hash,
                                                          void *value)
{
   struct _mulle_concurrent_hashvaluepair   *entry;
   unsigned int                             index;
   unsigned int                             sentinel;

   assert( hash != MULLE_CONCURRENT_NO_HASH);
   assert( value != MULLE_CONCURRENT_NO_POINTER && value != MULLE_CONCURRENT_INVALID_POINTER);

   index    = (unsigned int) hash;
   sentinel = (unsigned int) hash + (unsigned int) p->mask + 1;

   for(;;)
   {
      entry = &p->entries[ index & (unsigned int) p->mask];

      if( _mulle_concurrent_hashmapstorage_claim( p, entry, hash) == hash)
         return( _mulle_concurrent_hashmapstorage_fill( entry, value));

      ++index;
      if( MULLE_C_UNLIKELY( index == sentinel))
         return( MULLE_CONCURRENT_INVALID_POINTER);  // EBUSY, caller migrates
   }
}


//
// insert:
//
//  0      : did insert
//  EEXIST : key already exists (can't replace currently)
//  EBUSY  : this storage can't be written to
//
static int   _mulle_concurrent_hashmapstorage_insert( struct _mulle_concurrent_hashmapstorage *p,
                                                      intptr_t hash,
                                                      void *value)
{
   struct _mulle_concurrent_hashvaluepair   *entry;
   void                                     *found;
   unsigned int                             index;
   unsigned int                             sentinel;

   assert( hash != MULLE_CONCURRENT_NO_HASH);
   assert( value != MULLE_CONCURRENT_NO_POINTER && value != MULLE_CONCURRENT_INVALID_POINTER);

   index    = (unsigned int) hash;
   sentinel = (unsigned int) hash + (unsigned int) p->mask + 1;

   for(;;)
   {
      entry = &p->entries[ index & (unsigned int) p->mask];

      if( _mulle_concurrent_hashmapstorage_claim( p, entry, hash) == hash)
      {
         found = _mulle_concurrent_hashmapstorage_fill( entry, value);
         if( found == MULLE_CONCURRENT_NO_POINTER)
            return( 0);
         if( MULLE_C_UNLIKELY( found == MULLE_CONCURRENT_INVALID_POINTER))
            return( EBUSY);
         return( EEXIST);
      }

      ++index;
      if( MULLE_C_UNLIKELY( index == sentinel))
         return( EBUSY);
   }
}


//
// put is only called from copy (migration). unlike register/insert it must
// never clobber what is already in the destination slot: the destination can
// already have been written by a live thread after the storage swap. copy is
// a best-effort "do not leave anyone behind", not an overwrite.
//
// p can itself be mid-migration (a slot we want can already be frozen to
// REDIRECT by p's own copy cursor, if p was swapped in as map->storage and
// is now being migrated away by other threads). When that happens, follow
// the chain into map->next_storage and retry there instead of giving up:
// that storage generation is fixed once it exists, so this recursion is
// bounded by how many times the table has grown since our caller's copy
// started, not by contention. This keeps put (and therefore copy, and
// therefore migrate_storage) wait-free rather than merely lock-free.
//
static int   _mulle_concurrent_hashmapstorage_put( struct mulle_concurrent_hashmap *map,
                                                   struct _mulle_concurrent_hashmapstorage *p,
                                                   intptr_t hash,
                                                   void *value)
{
   struct _mulle_concurrent_hashmapstorage   *q;
   struct _mulle_concurrent_hashvaluepair    *entry;
   void                                      *found;
   unsigned int                              index;
   unsigned int                              sentinel;

   assert( value);

   index    = (unsigned int) hash;
   sentinel = (unsigned int) hash + (unsigned int) p->mask + 1;

   for(;;)
   {
      entry = &p->entries[ index & (unsigned int) p->mask];

      if( _mulle_concurrent_hashmapstorage_claim( p, entry, hash) == hash)
      {
         found = __mulle_atomic_pointer_cas( &entry->value, value, MULLE_CONCURRENT_NO_POINTER);
         if( MULLE_C_UNLIKELY( found == REDIRECT_VALUE))
         {
            // 'p' is itself being migrated away and our slot is already
            // frozen; the value belongs in whatever storage superseded 'p'
            q = _mulle_atomic_pointer_read( &map->next_storage.pointer);
            assert( q != p);
            return( _mulle_concurrent_hashmapstorage_put( map, q, hash, value));
         }
         return( 0);    // stored, or dst already holds a newer value
      }

      ++index;
      if( MULLE_C_UNLIKELY( index == sentinel))
         return( EBUSY);   // caller (copy) drops it, see comment above
   }
}


//
// Copy freezes every source slot. A virgin or claim-in-flight slot is frozen
// directly; a racing writer then observes REDIRECT and retries in the newer
// generation. A live value is put into the destination before its source
// slot is frozen ("no one gets left behind").
//
// put() follows REDIRECT through map->next_storage if the destination is
// itself migrating, so copy fully drains the source in one pass.
//
static void
   _mulle_concurrent_hashmapstorage_copy( struct mulle_concurrent_hashmap *map,
                                          struct _mulle_concurrent_hashmapstorage *dst,
                                          struct _mulle_concurrent_hashmapstorage *src)
{
   struct _mulle_concurrent_hashvaluepair   *p;
   struct _mulle_concurrent_hashvaluepair   *p_last;
   intptr_t                                 hash;
   void                                     *actual;
   void                                     *value;

   p      = src->entries;
   p_last = &src->entries[ src->mask];

   //
   // the shared read-only empty storage (mulle_concurrent_empty_storage) can
   // be a migration source (mulle_concurrent_hashmap_init( map, 0, ...)),
   // but it is a `static const` object: nothing can ever claim a slot in it
   // (there's only the one virgin sentinel entry), so it must never be
   // written to, not even to freeze it.
   //
   if( _mulle_concurrent_hashmapstorage_is_const( src))
      return;

   for( ; p <= p_last; p++)
   {
      hash = _mulle_concurrent_hashvaluepair_get_hash( p);
      if( hash == MULLE_CONCURRENT_NO_HASH)
      {
         hashmap_race_point( HASHMAP_RACE_COPY_VIRGIN);

         // Freeze a virgin slot. If this loses to a racing fill, hash-first
         // publication guarantees that the fill's hash is visible now; carry
         // the returned state into the ordinary copy/freeze path below.
         actual = __mulle_atomic_pointer_cas( &p->value,
                                               REDIRECT_VALUE,
                                               MULLE_CONCURRENT_NO_POINTER);
         if( actual == MULLE_CONCURRENT_NO_POINTER || actual == REDIRECT_VALUE)
            continue;

         hash = _mulle_concurrent_hashvaluepair_get_hash( p);
         assert( hash != MULLE_CONCURRENT_NO_HASH);
         value = actual;
      }
      else
         value = _mulle_atomic_pointer_read( &p->value);

      for(;;)
      {
         if( value == MULLE_CONCURRENT_NO_POINTER)
         {
            // claimed but not yet filled (or repopulated slot): freeze it
            actual = __mulle_atomic_pointer_cas( &p->value, REDIRECT_VALUE, MULLE_CONCURRENT_NO_POINTER);
            if( actual == MULLE_CONCURRENT_NO_POINTER)
               break;
            value = actual;
            continue;
         }
         if( MULLE_C_UNLIKELY( value == REDIRECT_VALUE))
            break;

         hashmap_race_point( HASHMAP_RACE_COPY_READ);

         // it's important that we copy over first so
         // No One Gets Left Behind
         _mulle_concurrent_hashmapstorage_put( map, dst, hash, value);

         hashmap_race_point( HASHMAP_RACE_COPY_CARRIED);

         actual = __mulle_atomic_pointer_cas( &p->value, REDIRECT_VALUE, value);
         if( actual == value)
            break;

         value = actual;
      }
   }
}


#pragma mark - _mulle_concurrent_hashmap

void  _mulle_concurrent_hashmap_init( struct mulle_concurrent_hashmap *map,
                                      unsigned int size,
                                      struct mulle_allocator *allocator)
{
   struct _mulle_concurrent_hashmapstorage   *storage;

   //
   // check assumption that we can use EINVAL ENOMEM ECANCELED and
   // not clash with 0/1
   //
   assert( EINVAL != 1 && EINVAL != 0);
   assert( ENOMEM != 1 && ENOMEM != 0);
   assert( ECANCELED != 1 && ECANCELED != 0);
   assert( EBUSY != 1 && EBUSY != 0);

   if( ! allocator)
      allocator = &mulle_default_allocator;

   _mulle_atomic_pointer_nonatomic_write( &map->allocator, allocator);
   if( size == 0)
      storage = (void *) &mulle_concurrent_empty_storage;
   else
      storage = _mulle_concurrent_alloc_hashmapstorage( size, allocator);

   _mulle_atomic_pointer_nonatomic_write( &map->storage.pointer, storage);
   _mulle_atomic_pointer_nonatomic_write( &map->next_storage.pointer, storage);
}


//
// this is called when you know, no other threads are accessing it anymore
//
void  _mulle_concurrent_hashmap_done( struct mulle_concurrent_hashmap *map)
{
   struct _mulle_concurrent_hashmapstorage   *storage;
   struct _mulle_concurrent_hashmapstorage   *next_storage;
   struct mulle_allocator                    *allocator;
   // ABA!

   storage      = _mulle_atomic_pointer_nonatomic_read( &map->storage.pointer);
   next_storage = _mulle_atomic_pointer_nonatomic_read( &map->next_storage.pointer);
   allocator    = _mulle_atomic_pointer_nonatomic_read( &map->allocator);

   if( next_storage != storage && ! _mulle_concurrent_hashmapstorage_is_const( next_storage))
      _mulle_allocator_abafree( allocator, next_storage);
   if( ! _mulle_concurrent_hashmapstorage_is_const( storage))
      _mulle_allocator_abafree( allocator, storage);
}


unsigned int  _mulle_concurrent_hashmap_get_size( struct mulle_concurrent_hashmap *map)
{
   struct _mulle_concurrent_hashmapstorage   *p;

   p = _mulle_atomic_pointer_read( &map->storage.pointer);
   return( (unsigned int) p->mask + 1);
}


static void   _mulle_concurrent_hashmap_migrate_storage( struct mulle_concurrent_hashmap *map,
                                                         struct _mulle_concurrent_hashmapstorage *p)
{

   struct _mulle_concurrent_hashmapstorage   *q;
   struct _mulle_concurrent_hashmapstorage   *alloced;
   struct _mulle_concurrent_hashmapstorage   *previous;
   struct mulle_allocator                    *allocator;
   size_t                                    size;

   assert( p);

   allocator  = _mulle_atomic_pointer_read( &map->allocator);

   // check if we have a chance to succeed
   alloced = NULL;
   q       = _mulle_atomic_pointer_read( &map->next_storage.pointer);
   if( q == p)
   {
      size = (size_t) p->mask + 1;

      // Strict growth bounds the number of storage generations an operation
      // can encounter, which is what makes the retry loops below wait-free.
      if( size > ((unsigned int) -1 >> 1))
         _mulle_allocator_abort( allocator, NULL, (size_t) -1);

      // acquire new storage
      alloced = _mulle_concurrent_alloc_hashmapstorage( (unsigned int) (size * 2),
                                                        allocator);

      // make this the next world, assume that's still set to 'p' (SIC)
      q = __mulle_atomic_pointer_cas( &map->next_storage.pointer, alloced, p);
      if( q != p)
      {
         // someone else produced a next world, use that and get rid of 'alloced'
         _mulle_allocator_abafree( allocator, alloced);  // ABA!!
         alloced = NULL;
      }
      else
         q = alloced;
   }

   // this thread can partake in copying. put() follows the REDIRECT chain
   // on its own if 'q' is itself already being migrated away, so copy
   // always fully drains 'p' in one pass here.
   _mulle_concurrent_hashmapstorage_copy( map, q, p);

   // now update world, giving it the same value as 'next_world'
   previous = __mulle_atomic_pointer_cas( &map->storage.pointer, q, p);

   // ok, if we succeed free old, if we fail alloced is
   // already gone. this must be an ABA free
   if( previous == p && ! _mulle_concurrent_hashmapstorage_is_const( previous))
      _mulle_allocator_abafree( allocator, previous); // ABA!!
}


void  *_mulle_concurrent_hashmap_lookup( struct mulle_concurrent_hashmap *map,
                                         intptr_t hash)
{
   struct _mulle_concurrent_hashmapstorage   *p;
   void                                      *value;

   // won't find invalid hash anyway
retry:
   p     = _mulle_atomic_pointer_read( &map->storage.pointer);
   hashmap_race_point( HASHMAP_RACE_LOOKUP);
   value = _mulle_concurrent_hashmapstorage_lookup( p, hash);
   if( MULLE_C_UNLIKELY( value == REDIRECT_VALUE))
   {
      _mulle_concurrent_hashmap_migrate_storage( map, p);
      goto retry;
   }
   return( value);
}


static int   _mulle_concurrent_hashmap_search_next( struct mulle_concurrent_hashmap *map,
                                                    unsigned int  *expect_mask,
                                                    unsigned int  *index,
                                                    intptr_t *p_hash,
                                                    void **p_value)
{
   struct _mulle_concurrent_hashmapstorage   *p;
   struct _mulle_concurrent_hashvaluepair    *entry;
   void                                      *value;

retry:
   p = _mulle_atomic_pointer_read( &map->storage.pointer);
   if( *expect_mask && (unsigned int) p->mask != *expect_mask)
      return( ECANCELED);

   for(;;)
   {
      entry = _mulle_concurrent_hashmapstorage_next_pair( p, index);
      if( ! entry)
         return( 0);

      value = _mulle_atomic_pointer_read( &entry->value);
      if( MULLE_C_UNLIKELY( value == REDIRECT_VALUE))
      {
         _mulle_concurrent_hashmap_migrate_storage( map, p);
         goto retry;
      }

      if( value != MULLE_CONCURRENT_NO_POINTER)
         break;
   }

   if( p_hash)
      *p_hash = _mulle_concurrent_hashvaluepair_get_hash( entry);
   if( p_value)
      *p_value = value;

   if( ! *expect_mask)
      *expect_mask = (unsigned int) p->mask;

   return( 1);
}


static inline void   assert_hash_value( intptr_t hash, void *value)
{
   assert( hash != MULLE_CONCURRENT_NO_HASH);
   assert( value != MULLE_CONCURRENT_NO_POINTER);
   assert( value != MULLE_CONCURRENT_INVALID_POINTER);

   MULLE_C_UNUSED( hash);
   MULLE_C_UNUSED( value);
}


//  return value:
//
//     MULLE_CONCURRENT_NO_POINTER      : means it did insert
//     MULLE_CONCURRENT_INVALID_POINTER : error
//     old                              : value that is already registered
//
void   *_mulle_concurrent_hashmap_register( struct mulle_concurrent_hashmap *map,
                                            intptr_t hash,
                                            void *value)
{
   struct _mulle_concurrent_hashmapstorage   *p;
   unsigned int                              n;
   unsigned int                              max;
   void                                      *result;

   assert_hash_value( hash, value);

retry:
   p = _mulle_atomic_pointer_read( &map->storage.pointer);
   assert( p);

   max = _mulle_concurrent_hashmapstorage_get_max_n_hashs( p);
   n   = (unsigned int) (uintptr_t) _mulle_atomic_pointer_read( &p->n_hashs);

   if( n >= max)
   {
      _mulle_concurrent_hashmap_migrate_storage( map, p);
      goto retry;
   }

   hashmap_race_point( HASHMAP_RACE_REGISTER);

   result = _mulle_concurrent_hashmapstorage_register( p, hash, value);
   if( result == MULLE_CONCURRENT_INVALID_POINTER)
   {
      _mulle_concurrent_hashmap_migrate_storage( map, p);
      goto retry;
   }

   return( result);
}


void   *mulle_concurrent_hashmap_register( struct mulle_concurrent_hashmap *map,
                                           intptr_t hash,
                                           void *value)
{
   if( ! map ||
       hash == MULLE_CONCURRENT_NO_HASH ||
       value == MULLE_CONCURRENT_NO_POINTER ||
       value == MULLE_CONCURRENT_INVALID_POINTER)
   {
      return( MULLE_CONCURRENT_INVALID_POINTER);
   }

   return( _mulle_concurrent_hashmap_register( map, hash, value));
}


#pragma mark - insert

int  _mulle_concurrent_hashmap_insert( struct mulle_concurrent_hashmap *map,
                                       intptr_t hash,
                                       void *value)
{
   struct _mulle_concurrent_hashmapstorage   *p;
   unsigned int                              n;
   unsigned int                              max;
   int                                       rval;

   assert_hash_value( hash, value);

retry:
   p = _mulle_atomic_pointer_read( &map->storage.pointer);
   assert( p);

   max = _mulle_concurrent_hashmapstorage_get_max_n_hashs( p);
   n   = (unsigned int) (uintptr_t) _mulle_atomic_pointer_read( &p->n_hashs);

   if( n >= max)
   {
      _mulle_concurrent_hashmap_migrate_storage( map, p);
      goto retry;
   }

   hashmap_race_point( HASHMAP_RACE_INSERT);

   rval = _mulle_concurrent_hashmapstorage_insert( p, hash, value);
   if( MULLE_C_UNLIKELY( rval == EBUSY))
   {
      _mulle_concurrent_hashmap_migrate_storage( map, p);
      goto retry;
   }

   return( rval);
}


int  mulle_concurrent_hashmap_insert( struct mulle_concurrent_hashmap *map,
                                      intptr_t hash,
                                      void *value)
{
   if( ! map)
      return( EINVAL);
   if( hash == MULLE_CONCURRENT_NO_HASH)
      return( EINVAL);
   if( value == MULLE_CONCURRENT_NO_POINTER || value == MULLE_CONCURRENT_INVALID_POINTER)
      return( EINVAL);

   return( _mulle_concurrent_hashmap_insert( map, hash, value));
}



#pragma mark - patch (single-threaded only)

//
// Single-threaded patch: unconditionally replace the value of an existing
// entry. There is no CAS, no migration concern, and no concurrency contract.
// Because no concurrent thread is running, writing NULL over a live value
// does not re-introduce a non-monotonic value word on any concurrent path.
// Use during single-threaded setup/teardown phases only.
//
int  _mulle_concurrent_hashmap_patch( struct mulle_concurrent_hashmap *map,
                                      intptr_t hash,
                                      void *value)
{
   struct _mulle_concurrent_hashmapstorage   *p;
   struct _mulle_concurrent_hashvaluepair    *entry;
   intptr_t                                  entry_hash;
   unsigned int                              index;
   unsigned int                              sentinel;

   p = _mulle_atomic_pointer_read_relaxed( &map->storage.pointer);

   index    = (unsigned int) hash;
   sentinel = (unsigned int) hash + (unsigned int) p->mask + 1;

   for(;;)
   {
      entry      = &p->entries[ index & (unsigned int) p->mask];
      entry_hash = (intptr_t) _mulle_atomic_pointer_read_relaxed( &entry->hash);

      if( entry_hash == MULLE_CONCURRENT_NO_HASH)
         return( ENOENT);

      if( entry_hash == hash)
      {
         _mulle_atomic_pointer_write_relaxed( &entry->value, value);
         return( 0);
      }

      ++index;
      if( MULLE_C_UNLIKELY( index == sentinel))
         return( ENOENT);
   }
}




#pragma mark - not so concurrent enumerator

int  _mulle_concurrent_hashmapenumerator_next( struct mulle_concurrent_hashmapenumerator *rover,
                                               intptr_t *p_hash,
                                               void **p_value)
{
   int        rval;
   void       *value;
   intptr_t   hash;

   if( ! rover->map)
      return( 0);

   rval = _mulle_concurrent_hashmap_search_next( rover->map, &rover->mask, &rover->index, &hash, &value);

   if( MULLE_C_UNLIKELY( rval != 1))
      return( rval);

   if( MULLE_C_LIKELY( p_hash != NULL))
      *p_hash = hash;
   if( MULLE_C_LIKELY( p_value != NULL))
      *p_value = value;

   return( 1);
}


#pragma mark - enumerator based code

//
// obviously just a snapshot at some recent point in time
//
unsigned int  mulle_concurrent_hashmap_count( struct mulle_concurrent_hashmap *map)
{
   unsigned int                                count;
   int                                         rval;
   struct mulle_concurrent_hashmapenumerator   rover;

retry:
   count = 0;

   rover = mulle_concurrent_hashmap_enumerate( map);
   for(;;)
   {
      rval = _mulle_concurrent_hashmapenumerator_next( &rover, NULL, NULL);
      if( rval == 1)
      {
         ++count;
         continue;
      }

      if( ! rval)
         break;

      mulle_concurrent_hashmapenumerator_done( &rover);
      goto retry;
   }

   mulle_concurrent_hashmapenumerator_done( &rover);
   return( count);
}


void  *mulle_concurrent_hashmap_lookup_any( struct mulle_concurrent_hashmap *map)
{
   struct mulle_concurrent_hashmapenumerator  rover;
   void  *any;

   any   = NULL;

   rover = mulle_concurrent_hashmap_enumerate( map);
   _mulle_concurrent_hashmapenumerator_next( &rover, NULL, &any);
   mulle_concurrent_hashmapenumerator_done( &rover);

   return( any);
}