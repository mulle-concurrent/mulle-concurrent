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
#define HAVE_MULLE_CONCURRENT_POSEAS_PATCH
#include "mulle-concurrent-hashmap.h"

#include "mulle-concurrent-types.h"
#include <assert.h>
#include <errno.h>
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
   { { NULL, NULL } }
};


#define REDIRECT_VALUE    MULLE_CONCURRENT_INVALID_POINTER
#define TOMBSTONE_VALUE   MULLE_CONCURRENT_TOMBSTONE_POINTER


static inline intptr_t
   _mulle_concurrent_hashvaluepair_get_hash( struct _mulle_concurrent_hashvaluepair *entry)
{
   return( (intptr_t) _mulle_atomic_pointer_read_relaxed( &entry->hash));
}


//
// claims 'entry' for 'hash' if it is virgin. Returns the hash that owns the
// slot afterwards: 'hash' if we won the claim (or it was already claimed
// for 'hash'), otherwise the foreign hash that beat us to it. Only the
// winner increments n_hashs.
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

   found = (intptr_t) __mulle_atomic_pointer_cas_relaxed( &entry->hash,
                                                  (void *) hash,
                                                  (void *) MULLE_CONCURRENT_NO_HASH);
   if( found != MULLE_CONCURRENT_NO_HASH)
      return( found);           // lost the claim, 'found' owns the slot

   _mulle_atomic_pointer_increment_relaxed( &p->n_hashs);
   return( hash);
}


//
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

   found = __mulle_atomic_pointer_cas_relaxed( &entry->value, value, MULLE_CONCURRENT_NO_POINTER);
   if( found == MULLE_CONCURRENT_NO_POINTER)
      return( MULLE_CONCURRENT_NO_POINTER);
   if( MULLE_C_UNLIKELY( found == REDIRECT_VALUE))
      return( MULLE_CONCURRENT_INVALID_POINTER);

   // Values never transition back from TOMBSTONE to live in this storage.
   // This keeps the value state machine monotonic for bounded migration.
   return( found);              // existing live value or TOMBSTONE_VALUE
}

#pragma mark - _mulle_concurrent_hashmapstorage


// n must be a power of 2
MULLE_C_NONNULL_RETURN
static struct _mulle_concurrent_hashmapstorage *
   _mulle_concurrent_alloc_hashmapstorage( unsigned int n,
                                           struct mulle_allocator *allocator)
{
   struct _mulle_concurrent_hashmapstorage  *p;

   assert( (~(n - 1) & n) == n);

   if( n < 4)
      n = 4;

   // noobsies: allocator returns either valid allocation or aborts
   //           p can't be NULL. we don't to ENOMEM
   p = _mulle_allocator_calloc( allocator, 1, sizeof( struct _mulle_concurrent_hashvaluepair) * (n - 1) +
                                sizeof( struct _mulle_concurrent_hashmapstorage));

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
   void                                     *value;
#ifndef NDEBUG
   unsigned int                             sentinel;

   sentinel = (unsigned int) hash + (unsigned int) p->mask + 1;
#endif
   index    = (unsigned int) hash;

   for(;;)
   {
      entry      = &p->entries[ index & (unsigned int) p->mask];
      entry_hash = _mulle_concurrent_hashvaluepair_get_hash( entry);

      if( entry_hash == MULLE_CONCURRENT_NO_HASH)
         return( MULLE_CONCURRENT_NO_POINTER);

      if( entry_hash == hash)
      {
         value = _mulle_atomic_pointer_read_relaxed( &entry->value);
         if( value == TOMBSTONE_VALUE)
            return( MULLE_CONCURRENT_NO_POINTER);
         return( value);            // may be REDIRECT: caller migrates + retries
      }

      ++index;
      assert( index != sentinel);  // can't happen we always leave space
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
#ifndef NDEBUG
   unsigned int                             sentinel;

   sentinel = (unsigned int) hash + (unsigned int) p->mask + 1;
#endif

   assert( hash != MULLE_CONCURRENT_NO_HASH);
   assert( value != MULLE_CONCURRENT_NO_POINTER && value != MULLE_CONCURRENT_INVALID_POINTER);

   index = (unsigned int) hash;

   for(;;)
   {
      entry = &p->entries[ index & (unsigned int) p->mask];

      if( _mulle_concurrent_hashmapstorage_claim( p, entry, hash) == hash)
         return( _mulle_concurrent_hashmapstorage_fill( entry, value));

      ++index;
      assert( index != sentinel);  // can't happen we always leave space
   }
}


//
// insert:
//
//  0      : did insert
//  EEXIST : key already exists (live value present)
//  EBUSY  : this storage can't be written to (migration in progress)
//  EAGAIN : slot is tombstoned, caller should do same-size migration and retry
//
static int   _mulle_concurrent_hashmapstorage_insert( struct _mulle_concurrent_hashmapstorage *p,
                                                      intptr_t hash,
                                                      void *value)
{
   struct _mulle_concurrent_hashvaluepair   *entry;
   void                                     *found;
   unsigned int                             index;
#ifndef NDEBUG
   unsigned int                             sentinel;

   sentinel = (unsigned int) hash + (unsigned int) p->mask + 1;
#endif

   assert( hash != MULLE_CONCURRENT_NO_HASH);
   assert( value != MULLE_CONCURRENT_NO_POINTER && value != MULLE_CONCURRENT_INVALID_POINTER);

   index = (unsigned int) hash;

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
         if( found == TOMBSTONE_VALUE)
            return( EAGAIN);
         return( EEXIST);
      }

      ++index;
      assert( index != sentinel);  // can't happen we always leave space
   }
}


//
// put is only called from copy (migration). unlike register/insert it must
// never clobber what is already in the destination slot and must never
// resurrect a tombstone there: the destination can already have been
// written by a live thread after the storage swap, or that key can already
// have been removed there. copy is a best-effort "do not leave anyone
// behind", not an overwrite.
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
   struct _mulle_concurrent_hashvaluepair     *entry;
   void                                        *found;
   unsigned int                               index;
#ifndef NDEBUG
   unsigned int                               sentinel;

   sentinel = (unsigned int) hash + (unsigned int) p->mask + 1;
#endif

   assert( value);

   index = (unsigned int) hash;

   for(;;)
   {
      entry = &p->entries[ index & (unsigned int) p->mask];

      if( _mulle_concurrent_hashmapstorage_claim( p, entry, hash) == hash)
      {
         found = __mulle_atomic_pointer_cas_relaxed( &entry->value, value, MULLE_CONCURRENT_NO_POINTER);
         if( MULLE_C_UNLIKELY( found == REDIRECT_VALUE))
         {
            // 'p' is itself being migrated away and our slot is already
            // frozen; the value belongs in whatever storage superseded 'p'
            q = _mulle_atomic_pointer_read_relaxed( &map->next_storage.pointer);
            assert( q != p);
            return( _mulle_concurrent_hashmapstorage_put( map, q, hash, value));
         }
         return( 0);    // stored, or dst already holds a newer value/tombstone
      }

      ++index;
      assert( index != sentinel);  // can't happen we always leave space
   }
}



static int
	_mulle_concurrent_hashmapstorage_remove( struct _mulle_concurrent_hashmapstorage *p,
                                            intptr_t hash,
                                            void *value)
{
   struct _mulle_concurrent_hashvaluepair   *entry;
   intptr_t                                 entry_hash;
   void                                     *found;
   unsigned int                             index;
#ifndef NDEBUG
   unsigned int                             sentinel;

   sentinel = (unsigned int) hash + (unsigned int) p->mask + 1;
#endif

   index = (unsigned int) hash;
   for(;;)
   {
      entry      = &p->entries[ index & (unsigned int) p->mask];
      entry_hash = _mulle_concurrent_hashvaluepair_get_hash( entry);

      if( entry_hash == hash)
      {
         found = __mulle_atomic_pointer_cas_relaxed( &entry->value, TOMBSTONE_VALUE, value);
         if( MULLE_C_UNLIKELY( found == REDIRECT_VALUE))
            return( EBUSY);
         if( found != value)
            return( ENOENT);
         return( 0);
      }

      if( entry_hash == MULLE_CONCURRENT_NO_HASH)
         return( ENOENT);

      ++index;
      assert( index != sentinel);  // can't happen we always leave space
   }
}


//
// Copy freezes every source slot. A virgin or claim-in-flight slot is frozen
// directly; a racing writer then observes REDIRECT and retries in the newer
// generation. A tombstone is frozen and dropped. A live value is put into the
// destination before its source slot is frozen ("no one gets left behind").
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
         // Freeze a virgin slot. If this loses to a racing fill, hash-first
         // publication guarantees that the fill's hash is visible now; carry
         // the returned state into the ordinary copy/freeze path below.
         actual = __mulle_atomic_pointer_cas_relaxed( &p->value,
                                               REDIRECT_VALUE,
                                               MULLE_CONCURRENT_NO_POINTER);
         if( actual == MULLE_CONCURRENT_NO_POINTER || actual == REDIRECT_VALUE)
            continue;

         hash = _mulle_concurrent_hashvaluepair_get_hash( p);
         assert( hash != MULLE_CONCURRENT_NO_HASH);
         value = actual;
      }
      else
         value = _mulle_atomic_pointer_read_relaxed( &p->value);
      for(;;)
      {
         if( value == MULLE_CONCURRENT_NO_POINTER)
         {
            actual = __mulle_atomic_pointer_cas_relaxed( &p->value, REDIRECT_VALUE, MULLE_CONCURRENT_NO_POINTER);
            if( actual == MULLE_CONCURRENT_NO_POINTER)
               break;
            value = actual;
            continue;
         }
         if( value == TOMBSTONE_VALUE)
         {
            // dropped here: the free cleanup the tombstone design promises
            actual = __mulle_atomic_pointer_cas_relaxed( &p->value, REDIRECT_VALUE, TOMBSTONE_VALUE);
            if( actual == TOMBSTONE_VALUE)
               break;
            value = actual;
            continue;
         }
         if( MULLE_C_UNLIKELY( value == REDIRECT_VALUE))
            break;

         // it's important that we copy over first so
         // No One Gets Left Behind.
         //
         // 'value' is a read from before this point, so a remove can have
         // tombstoned the source in the meantime and this put would then
         // resurrect a removed pair in 'dst'. remove closes that window
         // itself: after it tombstones a slot it plants a tombstone in the
         // newer generation too (see _mulle_concurrent_hashmap_remove).
         // Since put never overwrites a non-virgin destination slot, the two
         // orders commute and both end up removed.
         _mulle_concurrent_hashmapstorage_put( map, dst, hash, value);

         actual = __mulle_atomic_pointer_cas_relaxed( &p->value, REDIRECT_VALUE, value);
         if( actual == value)
            break;

         value = actual;
      }
   }
}


#pragma mark - _mulle_concurrent_hashmap

int  _mulle_concurrent_hashmap_init( struct mulle_concurrent_hashmap *map,
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

   assert( allocator->abafree && (int (*)(void)) allocator->abafree != (int (*)(void)) abort);

   _mulle_atomic_pointer_nonatomic_write( &map->allocator, allocator);
   if( size == 0)
      storage = (void *) &mulle_concurrent_empty_storage;
   else
      storage = _mulle_concurrent_alloc_hashmapstorage( size, allocator);

   _mulle_atomic_pointer_nonatomic_write( &map->storage.pointer, storage);
   _mulle_atomic_pointer_nonatomic_write( &map->next_storage.pointer, storage);

   return( 0);
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

   p = _mulle_atomic_pointer_read_relaxed( &map->storage.pointer);
   return( (unsigned int) p->mask + 1);
}


static unsigned int
   _mulle_concurrent_hashmapstorage_get_migration_size( struct _mulle_concurrent_hashmapstorage *p)
{
   unsigned int   size;

   // Strict growth bounds the number of storage generations an operation
   // can encounter. Tombstones are dropped while live entries are copied.
   size = (unsigned int) p->mask + 1;
   if( size > (unsigned int) -1 / 2)
      abort();
   return( size * 2);
}


static void   _mulle_concurrent_hashmap_migrate_storage_with_size( struct mulle_concurrent_hashmap *map,
                                                                   struct _mulle_concurrent_hashmapstorage *p,
                                                                   unsigned int new_size)
{

   struct _mulle_concurrent_hashmapstorage   *q;
   struct _mulle_concurrent_hashmapstorage   *alloced;
   struct _mulle_concurrent_hashmapstorage   *previous;
   struct mulle_allocator                    *allocator;

   assert( p);

   allocator  = _mulle_atomic_pointer_read_relaxed( &map->allocator);

   // check if we have a chance to succeed
   alloced = NULL;
   q       = _mulle_atomic_pointer_read_relaxed( &map->next_storage.pointer);
   if( q == p)
   {
      // acquire new storage
      alloced = _mulle_concurrent_alloc_hashmapstorage( new_size, allocator);
      // make this the next world, assume that's still set to 'p' (SIC)
      q = __mulle_atomic_pointer_cas_relaxed( &map->next_storage.pointer, alloced, p);
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
   previous = __mulle_atomic_pointer_cas_relaxed( &map->storage.pointer, q, p);

   // ok, if we succeed free old, if we fail alloced is
   // already gone. this must be an ABA free
   if( previous == p && ! _mulle_concurrent_hashmapstorage_is_const( previous))
      _mulle_allocator_abafree( allocator, previous); // ABA!!
}


static inline void
   _mulle_concurrent_hashmap_migrate_storage( struct mulle_concurrent_hashmap *map,
                                              struct _mulle_concurrent_hashmapstorage *p)
{
   _mulle_concurrent_hashmap_migrate_storage_with_size( map, p,
      _mulle_concurrent_hashmapstorage_get_migration_size( p));
}


//
// Same-size migration: drops tombstones without growing.  Used when
// insert/register hits a tombstone.  The resulting storage has the same
// capacity but the tombstoned slot is now virgin, so the retry succeeds.
//
// Wait-free argument: a same-size migration can only be triggered once per
// tombstone encounter.  After migration the tombstone is gone and the
// operation completes, so the retry chain adds at most one same-size hop
// before the normal strictly-growing chain.
//
static inline void
   _mulle_concurrent_hashmap_migrate_storage_same_size( struct mulle_concurrent_hashmap *map,
                                                        struct _mulle_concurrent_hashmapstorage *p)
{
   unsigned int  size;

   size = (unsigned int) p->mask + 1;
   _mulle_concurrent_hashmap_migrate_storage_with_size( map, p, size);
}


void  *_mulle_concurrent_hashmap_lookup( struct mulle_concurrent_hashmap *map,
                                         intptr_t hash)
{
   struct _mulle_concurrent_hashmapstorage   *p;
   void                                      *value;

   // won't find invalid hash anyway
retry:
   p     = _mulle_atomic_pointer_read_relaxed( &map->storage.pointer);
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
   p = _mulle_atomic_pointer_read_relaxed( &map->storage.pointer);
   if( *expect_mask && (unsigned int) p->mask != *expect_mask)
      return( ECANCELED);

   for(;;)
   {
      entry = _mulle_concurrent_hashmapstorage_next_pair( p, index);
      if( ! entry)
         return( 0);

      value = _mulle_atomic_pointer_read_relaxed( &entry->value);
      if( MULLE_C_UNLIKELY( value == REDIRECT_VALUE))
      {
         _mulle_concurrent_hashmap_migrate_storage( map, p);
         goto retry;
      }

      if( value != MULLE_CONCURRENT_NO_POINTER && value != TOMBSTONE_VALUE)
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
   assert( value != TOMBSTONE_VALUE);

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
   p = _mulle_atomic_pointer_read_relaxed( &map->storage.pointer);
   assert( p);

   max = _mulle_concurrent_hashmapstorage_get_max_n_hashs( p);
   n   = (unsigned int) (uintptr_t) _mulle_atomic_pointer_read_relaxed( &p->n_hashs);

   if( n >= max)
   {
      _mulle_concurrent_hashmap_migrate_storage( map, p);
      goto retry;
   }

   result = _mulle_concurrent_hashmapstorage_register( p, hash, value);
   if( result == MULLE_CONCURRENT_INVALID_POINTER)
   {
      _mulle_concurrent_hashmap_migrate_storage( map, p);
      goto retry;
   }
   if( result == TOMBSTONE_VALUE)
   {
      _mulle_concurrent_hashmap_migrate_storage_same_size( map, p);
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
       value == MULLE_CONCURRENT_INVALID_POINTER ||
       value == TOMBSTONE_VALUE)
   {
      errno = EINVAL;
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
   p = _mulle_atomic_pointer_read_relaxed( &map->storage.pointer);
   assert( p);

   max = _mulle_concurrent_hashmapstorage_get_max_n_hashs( p);
   n   = (unsigned int) (uintptr_t) _mulle_atomic_pointer_read_relaxed( &p->n_hashs);

   if( n >= max)
   {
      _mulle_concurrent_hashmap_migrate_storage( map, p);
      goto retry;
   }

   rval = _mulle_concurrent_hashmapstorage_insert( p, hash, value);
   if( MULLE_C_UNLIKELY( rval == EBUSY))
   {
      _mulle_concurrent_hashmap_migrate_storage( map, p);
      goto retry;
   }
   if( MULLE_C_UNLIKELY( rval == EAGAIN))
   {
      _mulle_concurrent_hashmap_migrate_storage_same_size( map, p);
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
   if( value == MULLE_CONCURRENT_NO_POINTER || value == MULLE_CONCURRENT_INVALID_POINTER ||
       value == TOMBSTONE_VALUE)
      return( EINVAL);

   return( _mulle_concurrent_hashmap_insert( map, hash, value));
}



#pragma mark - pose

//
// Seed a *private* (not yet reachable) storage with hash->value, using the
// normal claim path so that the entry ends up at the probe position lookups
// will search, and so that n_hashs counts it.
//
static void
   _mulle_concurrent_hashmapstorage_seed( struct _mulle_concurrent_hashmapstorage *q,
                                          intptr_t hash,
                                          void *value)
{
   struct _mulle_concurrent_hashvaluepair   *entry;
   unsigned int                             index;

   index = (unsigned int) hash;
   for(;;)
   {
      entry = &q->entries[ index & (unsigned int) q->mask];
      if( _mulle_concurrent_hashmapstorage_claim( q, entry, hash) == hash)
      {
         // 'q' is private: this CAS cannot fail, the slot is virgin
         _mulle_concurrent_hashmapstorage_fill( entry, value);
         return;
      }
      ++index;
   }
}


//
// "poseAs": replace the value of an existing entry, once, with a value that
// is unique to us and final.  See dox/POSEAS-PATCH.md for the proof.
//
// The naive implementation — CAS the slot in the published storage — is not
// good enough, and neither is any amount of repair afterwards.  A migration
// that read the old value before our CAS carries it forward in a *register*,
// and later stores it into the next generation, silently reverting us after
// we already returned.  Nothing observable in memory reveals that pending
// write, and nothing can decide that the carried value is the stale one:
// an entry is {hash,value} with an opaque payload and no version.
//
// So instead of writing into the current world, we build the next one:
//
//    1. confirm the key currently holds 'expect'
//    2. allocate a fresh storage and seed it with 'value' *while it is
//       still private*
//    3. only then publish it as next_storage
//    4. let the ordinary cooperative copy fill in every other key and
//       publish the result
//
// Step 2 before step 3 is the whole trick.  put() into a destination is
// write-once ("stored, or dst already holds a newer value"), so every
// carrier of the stale 'expect' — including a migrator descheduled since
// before we even started — is *refused* when it tries to copy the key over.
// The property that made the naive version unfixable becomes the mechanism
// that enforces correctness.
//
// Deliberately not wait-free: it retries until it wins or loses outright.
//
int  _mulle_concurrent_hashmap_pose( struct mulle_concurrent_hashmap *map,
                                     intptr_t hash,
                                     void *value,
                                     void *expect)
{
   struct _mulle_concurrent_hashmapstorage   *p;
   struct _mulle_concurrent_hashmapstorage   *q;
   struct _mulle_concurrent_hashmapstorage   *alloced;
   struct _mulle_concurrent_hashmapstorage   *previous;
   struct mulle_allocator                    *allocator;
   void                                      *found;

   assert_hash_value( hash, value);

   allocator = _mulle_atomic_pointer_read_relaxed( &map->allocator);

   for(;;)
   {
      p = _mulle_atomic_pointer_read_relaxed( &map->storage.pointer);
      assert( p);

      //
      // What does the key hold right now?
      //
      found = _mulle_concurrent_hashmapstorage_lookup( p, hash);
      if( MULLE_C_UNLIKELY( found == REDIRECT_VALUE))
      {
         _mulle_concurrent_hashmap_migrate_storage( map, p);
         continue;
      }
      if( found == MULLE_CONCURRENT_NO_POINTER)
         return( ENOENT);      // absent, or removed (tombstone)
      if( found == value)
         return( 0);           // already posed: idempotent by contract
      if( found != expect)
         return( EEXIST);      // wrong expect, or a competing pose won

      //
      // We can only substitute during a migration we start ourselves.  If one
      // is already in flight its destination may hold the stale value, so
      // help it finish and try again from the new world.
      //
      q = _mulle_atomic_pointer_read_relaxed( &map->next_storage.pointer);
      if( q != p)
      {
         _mulle_concurrent_hashmap_migrate_storage( map, p);
         continue;
      }

      //
      // Build the next world privately, with 'value' already in it.
      //
      alloced = _mulle_concurrent_alloc_hashmapstorage(
                     _mulle_concurrent_hashmapstorage_get_migration_size( p),
                     allocator);
      _mulle_concurrent_hashmapstorage_seed( alloced, hash, value);

      //
      // Publish it as the destination. From here on it is an ordinary
      // migration, except that our key is already filled in and therefore
      // immune to being overwritten by a copy of the old value.
      //
      q = __mulle_atomic_pointer_cas_relaxed( &map->next_storage.pointer, alloced, p);
      if( q != p)
      {
         _mulle_allocator_abafree( allocator, alloced);   // ABA!!
         continue;                                       // lost, start over
      }

      _mulle_concurrent_hashmapstorage_copy( map, alloced, p);

      previous = __mulle_atomic_pointer_cas_relaxed( &map->storage.pointer, alloced, p);
      if( previous == p && ! _mulle_concurrent_hashmapstorage_is_const( previous))
         _mulle_allocator_abafree( allocator, previous);  // ABA!!

      //
      // 'value' is ours and final, so this is a decisive test that nobody
      // else's write can satisfy.
      //
      found = _mulle_concurrent_hashmap_lookup( map, hash);
      if( found == value)
         return( 0);
      if( found == expect)
         continue;
      return( found == MULLE_CONCURRENT_NO_POINTER ? ENOENT : EEXIST);
   }
}


#pragma mark - patch (single-threaded only)

//
// Single-threaded patch: unconditionally replace the value of an existing
// entry.  There is no CAS, no migration concern, and no concurrency contract.
// This is cheap and repeatable — use it freely during single-threaded setup
// or teardown phases.  Use remove() to delete entries.
//
int  _mulle_concurrent_hashmap_patch( struct mulle_concurrent_hashmap *map,
                                      intptr_t hash,
                                      void *value)
{
   struct _mulle_concurrent_hashmapstorage   *p;
   struct _mulle_concurrent_hashvaluepair    *entry;
   intptr_t                                  entry_hash;
   void                                      *old;
   unsigned int                              index;
#ifndef NDEBUG
   unsigned int                              sentinel;
#endif

   p = _mulle_atomic_pointer_read_relaxed( &map->storage.pointer);

#ifndef NDEBUG
   sentinel = (unsigned int) hash + (unsigned int) p->mask + 1;
#endif

   index = (unsigned int) hash;
   for(;;)
   {
      entry      = &p->entries[ index & (unsigned int) p->mask];
      entry_hash = _mulle_concurrent_hashvaluepair_get_hash( entry);

      if( entry_hash == MULLE_CONCURRENT_NO_HASH)
         return( ENOENT);

      if( entry_hash == hash)
      {
         old = _mulle_atomic_pointer_read_relaxed( &entry->value);
         if( old == TOMBSTONE_VALUE)
            return( ENOENT);

         _mulle_atomic_pointer_write_relaxed( &entry->value, value);
         return( 0);
      }

      ++index;
      assert( index != sentinel);
   }
}


#pragma mark - remove


int  _mulle_concurrent_hashmap_remove( struct mulle_concurrent_hashmap *map,
                                       intptr_t hash,
                                       void *value)
{
   struct _mulle_concurrent_hashmapstorage   *p;
   int                                       rval;

   assert_hash_value( hash, value);

retry:
   p    = _mulle_atomic_pointer_read_relaxed( &map->storage.pointer);
   rval = _mulle_concurrent_hashmapstorage_remove( p, hash, value);
   if( MULLE_C_UNLIKELY( rval == EBUSY))
   {
      _mulle_concurrent_hashmap_migrate_storage( map, p);
      goto retry;
   }
   return( rval);
}


int  mulle_concurrent_hashmap_remove( struct mulle_concurrent_hashmap *map,
                                      intptr_t hash,
                                      void *value)
{
   if( ! map)
      return( EINVAL);
   if( hash == MULLE_CONCURRENT_NO_HASH)
      return( EINVAL);
   if( value == MULLE_CONCURRENT_NO_POINTER || value == MULLE_CONCURRENT_INVALID_POINTER ||
       value == TOMBSTONE_VALUE)
      return( EINVAL);

   return( _mulle_concurrent_hashmap_remove( map, hash, value));
}


#pragma mark - not so concurrent enumerator

int  _mulle_concurrent_hashmapenumerator_next( struct mulle_concurrent_hashmapenumerator *rover,
                                               intptr_t *p_hash,
                                               void **p_value)
{
   int        rval;
   void       *value;
   intptr_t   hash;

   if( ! rover || ! rover->map)
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
