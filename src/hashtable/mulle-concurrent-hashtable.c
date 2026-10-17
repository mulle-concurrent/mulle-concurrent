//
//  mulle-concurrent-hashtable.c
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
#include "mulle-concurrent-hashtable.h"
#include "mulle-concurrent-math.h"


// THIS CODE IS STILL EXPERIMENTAL, THOUGH USED ALREADY IN THE MULLE-OBJC
// RUNTIME

//
// Define MULLE_CONCURRENT_HASHTABLE_RACE_YIELD to widen the two-word race
// windows. test/hashtable/lookup_race.c then fails within ~100 iterations
// against an unfixed lookup, instead of never. See dox/HASHTABLE.md.
//
// #define MULLE_CONCURRENT_HASHTABLE_RACE_YIELD   1

#include "mulle-concurrent-types.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>


#define EMPTY_VALUE   ((void *) 0)


//
// Test scaffolding. The dangerous windows in this design are two adjacent
// instructions wide (read the hash word, then touch the value word), which no
// test will hit by luck. Defining MULLE_CONCURRENT_HASHTABLE_RACE_YIELD widens
// them so the races become reproducible. Compiles to nothing otherwise.
//
// Deliberately not rand(): glibc's rand() takes a process-global lock, so it
// would inject a lock and a barrier at exactly the window under observation,
// and all threads would share one unseeded sequence consumed in lock order.
// A per-thread xorshift keeps the decisions independent and lock free, so the
// probe perturbs the timing as little as possible.
//
#ifdef MULLE_CONCURRENT_HASHTABLE_RACE_YIELD

#define  hashtable_race_yield( void)   MULLE_THREAD_UNPLEASANT_RACE_YIELD()
#else
# define hashtable_race_yield()   do {} while( 0)
#endif

//
// Named race points. By default each one is just hashtable_race_yield().
// A test can #define hashtable_race_point( site) and #include this file to
// park a thread at an exact point, which turns a probabilistic race into a
// deterministic interleaving (see test/hashtable/migration_stall.c).
//
#define HASHTABLE_RACE_LOOKUP          1   // lookup: hash read, value not yet
#define HASHTABLE_RACE_INSERT          2   // insert: claimed, value CAS not yet
#define HASHTABLE_RACE_REGISTER        3   // register: claimed, value CAS not yet
#define HASHTABLE_RACE_REMOVE          4   // remove: hash read, value CAS not yet
#define HASHTABLE_RACE_COPY_FROZEN     5   // copy: slot frozen, value not sealed yet
#define HASHTABLE_RACE_COPY_CARRIED    6   // copy: carried into the successor
#define HASHTABLE_RACE_COPY_READ       7   // copy: value sealed, not carried yet

#ifndef hashtable_race_point
# define hashtable_race_point( site)   hashtable_race_yield()
#endif


static inline intptr_t   hashtable_unfold_user_hash( uintptr_t hash,
                                                     uintptr_t hash_mask)
{
   return( (intptr_t) (hash & hash_mask));
}


static inline int   hashtable_is_frozen( uintptr_t hashword, uintptr_t frozen_bit)
{
   return( (hashword & frozen_bit) != 0);
}


static inline uintptr_t   hashtable_hash_of( uintptr_t hashword, uintptr_t hash_mask)
{
   return( hashword & hash_mask);
}


static inline int   hashtable_is_sealed( void *value, uintptr_t sealed_bit)
{
   return( ((uintptr_t) value & sealed_bit) != 0);
}


static inline void   *hashtable_unseal( void *value, uintptr_t sealed_bit)
{
   return( (void *) ((uintptr_t) value & ~sealed_bit));
}


// for an unsealed value word: neither "never set" nor "removed"
static inline int   hashtable_is_live( void *value, void *tombstone)
{
   return( value != EMPTY_VALUE && value != tombstone);
}


static inline uintptr_t
   _mulle_concurrent_hashtablepair_get_hash( struct _mulle_concurrent_hashtablepair *entry)
{
   return( (uintptr_t) _mulle_atomic_pointer_read( &entry->hash));
}


static inline uintptr_t
   _mulle_concurrent_hashtablepair_get_hash_acquire( struct _mulle_concurrent_hashtablepair *entry)
{
   return( (uintptr_t) _mulle_atomic_pointer_read_acquire( &entry->hash));
}


#pragma mark - storage

// n is rounded up to a power of two (at least 4)
MULLE_C_NONNULL_RETURN
static struct _mulle_concurrent_hashtablestorage *
   _mulle_concurrent_hashtablestorage_alloc( size_t n,
                                             struct mulle_allocator *allocator)
{
   struct _mulle_concurrent_hashtablestorage   *p;

   if( n > ((size_t) -1 >> 1) + 1)
      _mulle_allocator_abort( allocator, NULL, (size_t) -1);

   n = _mulle_concurrent_pow2roundup( n);
   if( n < 4)
      n = 4;
   assert( _mulle_concurrent_is_pow2( n));

   // calloc gives us hash == 0, value == EMPTY and next == NULL, which is the
   // virgin state. the allocator either returns valid memory or aborts
   p = _mulle_allocator_calloc( allocator, 1,
                                mulle_allocator_size_add( allocator,
                                   mulle_allocator_size_multiply( allocator,
                                                                  n - 1,
                                                                  sizeof( struct _mulle_concurrent_hashtablepair)),
                                   sizeof( struct _mulle_concurrent_hashtablestorage)));
   p->mask = n - 1;
   return( p);
}


static size_t
   _mulle_concurrent_hashtablestorage_get_max_n_hashs( struct _mulle_concurrent_hashtablestorage *p)
{
   size_t   size;

   // migrate at half claimed capacity, so a virgin slot always remains and
   // every linear probe terminates
   size = (size_t) p->mask + 1;
   return( size - (size >> 1));
}


static size_t
   _mulle_concurrent_hashtablestorage_get_migration_size( struct _mulle_concurrent_hashtablestorage *p,
                                                          struct mulle_allocator *allocator)
{
   size_t   size;

   size = (size_t) p->mask + 1;
   if( size > (size_t) -1 / 2)
      _mulle_allocator_abort( allocator, NULL, (size_t) -1);
   return( size * 2);
}


//
// Claims 'entry' for 'hash' if it is virgin. Returns the hash word that owns
// the slot afterwards: 'hash' if we won (or it was already ours), a foreign
// hash if we lost, or a frozen word if a migration retired the slot.
//
static inline uintptr_t
   _mulle_concurrent_hashtablestorage_claim( struct _mulle_concurrent_hashtablestorage *p,
                                            struct _mulle_concurrent_hashtablepair *entry,
                                            uintptr_t hash)
{
   uintptr_t   found;

   found = _mulle_concurrent_hashtablepair_get_hash( entry);
   if( found != MULLE_CONCURRENT_NO_HASH)
      return( found);

   found = (uintptr_t) __mulle_atomic_pointer_cas( &entry->hash,
                                                  (void *) hash,
                                                  (void *) MULLE_CONCURRENT_NO_HASH);
   if( found != MULLE_CONCURRENT_NO_HASH)
      return( found);

   _mulle_atomic_pointer_increment( &p->n_hashs);
   return( hash);
}


//
// Set the FROZEN bit. Afterwards no new writer operation touches this slot:
// writers check the hash word first and help migrate when it is frozen. A
// virgin slot can not be claimed anymore.
//
// The hash word only ever moves NO_HASH -> h -> h|FROZEN, so this loop can
// lose at most twice.
//
static uintptr_t
   _mulle_concurrent_hashtablestorage_freeze( struct _mulle_concurrent_hashtablepair *entry,
                                             uintptr_t frozen_bit)
{
   uintptr_t   found;
   uintptr_t   target;

   for(;;)
   {
      found = _mulle_concurrent_hashtablepair_get_hash( entry);
      if( hashtable_is_frozen( found, frozen_bit))
         return( found);

      target = found | frozen_bit;
      if( (uintptr_t) __mulle_atomic_pointer_cas( &entry->hash,
                                                 (void *) target,
                                                 (void *) found) == found)
         return( target);
   }
}


//
// Set the SEALED bit on the value word. This is the commit point of the
// migration for this slot: afterwards every writer's value CAS fails, so the
// returned (unsealed) value is final, and every helper carries the same one.
//
// Only writers that read the hash word before the freeze can still change the
// value, and each of those does at most one value CAS, so this loop is bounded
// by the number of threads.
//
static void *
   _mulle_concurrent_hashtablestorage_seal( struct _mulle_concurrent_hashtablepair *entry,
                                           uintptr_t sealed_bit)
{
   void   *found;

   for(;;)
   {
      found = _mulle_atomic_pointer_read( &entry->value);
      if( hashtable_is_sealed( found, sealed_bit))
         return( hashtable_unseal( found, sealed_bit));

      if( __mulle_atomic_pointer_cas( &entry->value,
                                     (void *) ((uintptr_t) found | sealed_bit),
                                     found) == found)
         return( found);
   }
}


//
// Install (hash,value) into generation 'q', but only into a destination value
// word that is still EMPTY, i.e. has never held anything. Anything else means
// the slot is already settled: another helper carried the same final value, or
// it was carried and has since been removed (TOMB) or replaced in 'q'. Never
// overwriting is what keeps a lagging copier from resurrecting a removal.
//
// If the destination is itself retired before anything arrived (sealed EMPTY,
// or a frozen virgin in the probe chain), go on in q's own successor.
//
static void
   _mulle_concurrent_hashtable_carry( struct mulle_concurrent_hashtable *map,
                                     struct _mulle_concurrent_hashtablestorage *q,
                                     uintptr_t hash,
                                     void *value)
{
   struct _mulle_concurrent_hashtablepair   *entry;
   uintptr_t                                found;
   uintptr_t                                hash_mask;
   void                                     *sealed_empty;
   size_t                                   index;
#ifndef NDEBUG
   size_t                                   sentinel;
#endif

   hash_mask    = map->hash_mask;
   sealed_empty = (void *) map->sealed_bit;

   for(;;)
   {
      index = (size_t) hash;
#ifndef NDEBUG
      sentinel = (size_t) hash + (size_t) q->mask + 1;
#endif
      for(;;)
      {
         entry = &q->entries[ index & (size_t) q->mask];
         found = hashtable_hash_of( _mulle_concurrent_hashtablestorage_claim( q, entry, hash),
                                    hash_mask);
         if( found == hash)
         {
            if( __mulle_atomic_pointer_cas( &entry->value, value, EMPTY_VALUE) != sealed_empty)
               return;     // installed, or already settled
            break;         // retired while still empty
         }
         if( found == MULLE_CONCURRENT_NO_HASH)
            break;         // frozen virgin: retired, not here

         ++index;
         assert( index != sentinel);
      }

      q = _mulle_atomic_pointer_read( &q->next);
      assert( q);          // a frozen or sealed slot implies a successor
   }
}


//
// Freeze and seal every slot of 'src', then carry the live ones into 'dst',
// which is src->next. Re-running this over a generation that has already been
// copied is harmless: the seals are in place, and carry never overwrites a
// destination that has held a value.
//
static void
   _mulle_concurrent_hashtablestorage_copy( struct mulle_concurrent_hashtable *map,
                                           struct _mulle_concurrent_hashtablestorage *dst,
                                           struct _mulle_concurrent_hashtablestorage *src)
{
   struct _mulle_concurrent_hashtablepair   *entry;
   struct _mulle_concurrent_hashtablepair   *sentinel;
   uintptr_t                                frozen_bit;
   uintptr_t                                hash;
   uintptr_t                                hash_mask;
   uintptr_t                                sealed_bit;
   void                                     *tombstone;
   void                                     *value;

   frozen_bit = map->frozen_bit;
   hash_mask  = map->hash_mask;
   sealed_bit = map->sealed_bit;
   tombstone  = map->tombstone;
   entry      = src->entries;
   sentinel   = &src->entries[ (size_t) src->mask + 1];

   for( ; entry < sentinel; entry++)
   {
      hash = hashtable_hash_of( _mulle_concurrent_hashtablestorage_freeze( entry, frozen_bit), hash_mask);
      if( hash == MULLE_CONCURRENT_NO_HASH)
         continue;                  // retired virgin, nothing was ever here

      hashtable_race_point( HASHTABLE_RACE_COPY_FROZEN);    // let an in-flight writer land

      value = _mulle_concurrent_hashtablestorage_seal( entry, sealed_bit);
      if( ! hashtable_is_live( value, tombstone))
         continue;                  // never set, or removed

      hashtable_race_point( HASHTABLE_RACE_COPY_READ);      // let others finish and remove

      _mulle_concurrent_hashtable_carry( map, dst, hash, value);

      hashtable_race_point( HASHTABLE_RACE_COPY_CARRIED);
   }
}


//
// Returns p->next, allocating and publishing it if nobody has yet.
//
static struct _mulle_concurrent_hashtablestorage *
   _mulle_concurrent_hashtablestorage_get_next( struct _mulle_concurrent_hashtablestorage *p,
                                               size_t new_size,
                                               struct mulle_allocator *allocator)
{
   struct _mulle_concurrent_hashtablestorage   *alloced;
   struct _mulle_concurrent_hashtablestorage   *q;

   q = _mulle_atomic_pointer_read( &p->next);
   if( q)
      return( q);

   alloced = _mulle_concurrent_hashtablestorage_alloc( new_size, allocator);
   q       = __mulle_atomic_pointer_cas( &p->next, alloced, NULL);
   if( q)
   {
      _mulle_allocator_free( allocator, alloced);   // never published
      return( q);
   }
   return( alloced);
}


//
// Help migrate 'p' into its own successor. 'p' may be stale (storage has
// moved on already), in which case the copy is a no-op and the storage CAS
// fails. Migration never skips a generation.
//
static void
   _mulle_concurrent_hashtable_migrate_storage_with_size( struct mulle_concurrent_hashtable *map,
                                                         struct _mulle_concurrent_hashtablestorage *p,
                                                         size_t new_size)
{
   struct _mulle_concurrent_hashtablestorage   *q;
   struct mulle_allocator                      *allocator;

   assert( p);

   allocator = _mulle_atomic_pointer_read( &map->allocator);
   q         = _mulle_concurrent_hashtablestorage_get_next( p, new_size, allocator);

   _mulle_concurrent_hashtablestorage_copy( map, q, p);

   if( __mulle_atomic_pointer_cas( &map->storage.pointer, q, p) == p)
      _mulle_allocator_abafree( allocator, p);
}


static inline void
   _mulle_concurrent_hashtable_migrate_storage( struct mulle_concurrent_hashtable *map,
                                               struct _mulle_concurrent_hashtablestorage *p)
{
   struct mulle_allocator   *allocator;

   allocator = _mulle_atomic_pointer_read( &map->allocator);

   _mulle_concurrent_hashtable_migrate_storage_with_size( map, p,
      _mulle_concurrent_hashtablestorage_get_migration_size( p, allocator));
}


//
// Retire the current generation into a fresh one of the *same* size. Exists so
// that tests can force continuous generation changes without doubling memory on
// every call, since migration frequency is the binding constraint for the
// remove-versus-copy races.
//
// Safe as a general operation too: only live values are carried, and the live
// count cannot exceed the threshold that would have grown the table anyway.
//
void   _mulle_concurrent_hashtable_migrate_same_size( struct mulle_concurrent_hashtable *map)
{
   struct _mulle_concurrent_hashtablestorage   *p;

   if( ! map)
      return;

   p = _mulle_atomic_pointer_read( &map->storage.pointer);
   _mulle_concurrent_hashtable_migrate_storage_with_size( map, p,
                                                         (size_t) p->mask + 1);
}


#pragma mark - storage operations

//
// Never fails with EBUSY: a sealed value is final and still readable, and a
// frozen virgin ends the probe chain just like an unfrozen one.
//
static void *
   _mulle_concurrent_hashtablestorage_lookup( struct mulle_concurrent_hashtable *map,
                                             struct _mulle_concurrent_hashtablestorage *p,
                                             uintptr_t hash)
{
   struct _mulle_concurrent_hashtablepair   *entry;
   uintptr_t                                found;
   uintptr_t                                hash_mask;
   void                                     *value;
   size_t                                   index;
#ifndef NDEBUG
   size_t                                   sentinel;

   sentinel = (size_t) hash + (size_t) p->mask + 1;
#endif

   hash_mask = map->hash_mask;
   index     = (size_t) hash;
   for(;;)
   {
      entry = &p->entries[ index & (size_t) p->mask];
      found = hashtable_hash_of( _mulle_concurrent_hashtablepair_get_hash_acquire( entry),
                                 hash_mask);
      if( found == hash)
      {
         hashtable_race_point( HASHTABLE_RACE_LOOKUP);

         value = _mulle_atomic_pointer_read_acquire( &entry->value);
         value = hashtable_unseal( value, map->sealed_bit);
         if( value == map->tombstone)
            return( EMPTY_VALUE);
         return( value);
      }
      if( found == MULLE_CONCURRENT_NO_HASH)
         return( EMPTY_VALUE);   // virgin (frozen or not) ends the chain

      ++index;
      assert( index != sentinel);
   }
}


//
// Finds the slot for 'hash', claiming a virgin one on the way.
//
//  0     : '*p_entry' is the slot
//  EBUSY : storage is migrating (or the probe ran around a full table)
//
static int
   _mulle_concurrent_hashtablestorage_find_or_claim( struct mulle_concurrent_hashtable *map,
                                                    struct _mulle_concurrent_hashtablestorage *p,
                                                    uintptr_t hash,
                                                    struct _mulle_concurrent_hashtablepair **p_entry)
{
   struct _mulle_concurrent_hashtablepair   *entry;
   uintptr_t                                found;
   uintptr_t                                frozen_bit;
   size_t                                   index;
   size_t                                   sentinel;

   frozen_bit = map->frozen_bit;
   sentinel   = (size_t) hash + (size_t) p->mask + 1;
   index      = (size_t) hash;
   for(;;)
   {
      entry = &p->entries[ index & (size_t) p->mask];
      found = _mulle_concurrent_hashtablestorage_claim( p, entry, hash);
      if( hashtable_is_frozen( found, frozen_bit))
         return( EBUSY);

      if( found == hash)
      {
         *p_entry = entry;
         return( 0);
      }
      ++index;

      //
      // The occupancy check (n < max) and the slot claim are not atomic: with
      // more threads in flight than size/2, all can pass the check, then all
      // claim slots, filling the table before any migration fires. Return
      // EBUSY so the caller migrates and retries in a larger generation.
      //
      if( index == sentinel)
         return( EBUSY);
   }
}


//
//  0      : did insert
//  EEXIST : a live value is present
//  EBUSY  : storage is migrating, nothing was written
//
static int
   _mulle_concurrent_hashtablestorage_insert( struct mulle_concurrent_hashtable *map,
                                             struct _mulle_concurrent_hashtablestorage *p,
                                             uintptr_t hash,
                                             void *value)
{
   struct _mulle_concurrent_hashtablepair   *entry;
   void                                     *expect;
   void                                     *found;
   int                                      rval;

   rval = _mulle_concurrent_hashtablestorage_find_or_claim( map, p, hash, &entry);
   if( rval)
      return( rval);

   hashtable_race_point( HASHTABLE_RACE_INSERT);

   expect = _mulle_atomic_pointer_read( &entry->value);
   if( hashtable_is_sealed( expect, map->sealed_bit))
      return( EBUSY);
   if( hashtable_is_live( expect, map->tombstone))
      return( EEXIST);

   found = __mulle_atomic_pointer_cas( &entry->value, value, expect);
   if( found == expect)
      return( 0);
   if( hashtable_is_sealed( found, map->sealed_bit))
      return( EBUSY);

   //
   // Either a live value now, or EMPTY went to TOMB, which can only happen via
   // a live value. Both mean another value was live during our call.
   //
   return( EEXIST);
}


//
//  0      : '*p_old' is NULL if we inserted, else the live value
//  EBUSY  : storage is migrating, nothing was written
//
static int
   _mulle_concurrent_hashtablestorage_register( struct mulle_concurrent_hashtable *map,
                                               struct _mulle_concurrent_hashtablestorage *p,
                                               uintptr_t hash,
                                               void *value,
                                               void **p_old)
{
   struct _mulle_concurrent_hashtablepair   *entry;
   void                                     *expect;
   void                                     *found;
   int                                      rval;

   rval = _mulle_concurrent_hashtablestorage_find_or_claim( map, p, hash, &entry);
   if( rval)
      return( rval);

   hashtable_race_point( HASHTABLE_RACE_REGISTER);

   expect = _mulle_atomic_pointer_read( &entry->value);
   for(;;)
   {
      if( hashtable_is_sealed( expect, map->sealed_bit))
         return( EBUSY);
      if( hashtable_is_live( expect, map->tombstone))
      {
         *p_old = expect;
         return( 0);
      }

      found = __mulle_atomic_pointer_cas( &entry->value, value, expect);
      if( found == expect)
      {
         *p_old = EMPTY_VALUE;
         return( 0);
      }

      // unlike insert we must report the winner, so look again. Repeats only
      // if another insert *and* remove completed in between
      expect = found;
   }
}


//
//  0      : removed
//  ENOENT : (hash,value) not present
//  EBUSY  : storage is migrating, nothing was removed
//
static int
   _mulle_concurrent_hashtablestorage_remove( struct mulle_concurrent_hashtable *map,
                                             struct _mulle_concurrent_hashtablestorage *p,
                                             uintptr_t hash,
                                             void *value)
{
   struct _mulle_concurrent_hashtablepair   *entry;
   uintptr_t                                found;
   uintptr_t                                frozen_bit;
   void                                     *old;
   size_t                                   index;
#ifndef NDEBUG
   size_t                                   sentinel;

   sentinel = (size_t) hash + (size_t) p->mask + 1;
#endif

   frozen_bit = map->frozen_bit;
   index      = (size_t) hash;
   for(;;)
   {
      entry = &p->entries[ index & (size_t) p->mask];
      found = _mulle_concurrent_hashtablepair_get_hash( entry);

      if( found == MULLE_CONCURRENT_NO_HASH)
         return( ENOENT);

      if( hashtable_is_frozen( found, frozen_bit))
         return( EBUSY);

      if( found == hash)
      {
         hashtable_race_point( HASHTABLE_RACE_REMOVE);

         // TOMB, not EMPTY: a slot that held a value never looks virgin again
         old = __mulle_atomic_pointer_cas( &entry->value, map->tombstone, value);
         if( old == value)
            return( 0);
         if( hashtable_is_sealed( old, map->sealed_bit))
            return( EBUSY);
         return( ENOENT);
      }
      ++index;
      assert( index != sentinel);
   }
}


#pragma mark - single-threaded

void   _mulle_concurrent_hashtable_init_with_mode( struct mulle_concurrent_hashtable *map,
                                                   size_t size,
                                                   unsigned int mode,
                                                   struct mulle_allocator *allocator)
{
   struct _mulle_concurrent_hashtablestorage   *storage;
   uintptr_t                                   top_bit;

   assert( map);

   if( ! allocator)
      allocator = &mulle_default_allocator;

   storage = _mulle_concurrent_hashtablestorage_alloc( size, allocator);
   top_bit = (uintptr_t) 1 << (sizeof( uintptr_t) * CHAR_BIT - 1);

   if( mode & MULLE_CONCURRENT_HASHTABLE_EVEN_HASH)
      map->frozen_bit = 1;
   else
      map->frozen_bit = top_bit;
   map->hash_mask = ~map->frozen_bit;

   if( mode & MULLE_CONCURRENT_HASHTABLE_POSITIVE_VALUE)
   {
      map->sealed_bit = top_bit;
      map->tombstone  = MULLE_CONCURRENT_TOMBSTONE_POINTER;
   }
   else
   {
      map->sealed_bit = 1;
      map->tombstone  = MULLE_CONCURRENT_INVALID_POINTER;
   }

   _mulle_atomic_pointer_nonatomic_write( &map->allocator, allocator);
   _mulle_atomic_pointer_nonatomic_write( &map->storage.pointer, storage);
}


void   _mulle_concurrent_hashtable_init_positive( struct mulle_concurrent_hashtable *map,
                                                  size_t size,
                                                  struct mulle_allocator *allocator)
{
   _mulle_concurrent_hashtable_init_with_mode( map, size,
                                               MULLE_CONCURRENT_HASHTABLE_POSITIVE_HASH |
                                               MULLE_CONCURRENT_HASHTABLE_EVEN_VALUE,
                                               allocator);
}


void   _mulle_concurrent_hashtable_init_even( struct mulle_concurrent_hashtable *map,
                                              size_t size,
                                              struct mulle_allocator *allocator)
{
   _mulle_concurrent_hashtable_init_with_mode( map, size,
                                               MULLE_CONCURRENT_HASHTABLE_EVEN_HASH |
                                               MULLE_CONCURRENT_HASHTABLE_EVEN_VALUE,
                                               allocator);
}


void  mulle_concurrent_hashtable_done( struct mulle_concurrent_hashtable *map)
{
   struct _mulle_concurrent_hashtablestorage   *next;
   struct _mulle_concurrent_hashtablestorage   *storage;
   struct mulle_allocator                      *allocator;

   if( ! map)
      return;

   allocator = _mulle_atomic_pointer_nonatomic_read( &map->allocator);
   storage   = _mulle_atomic_pointer_nonatomic_read( &map->storage.pointer);

   // predecessors have been abafreed, successors (normally none) have not
   while( storage)
   {
      next = _mulle_atomic_pointer_nonatomic_read( &storage->next);
      _mulle_allocator_free( allocator, storage);
      storage = next;
   }

   _mulle_atomic_pointer_nonatomic_write( &map->storage.pointer, NULL);
}


#pragma mark - multi-threaded

void  *mulle_concurrent_hashtable_lookup( struct mulle_concurrent_hashtable *map,
                                         intptr_t user_hash)
{
   struct _mulle_concurrent_hashtablestorage   *p;

   if( ! map)
      return( NULL);
   if( ! mulle_concurrent_hashtable_hash_is_valid( map, user_hash))
      return( NULL);

   p = _mulle_atomic_pointer_read_acquire( &map->storage.pointer);
   return( _mulle_concurrent_hashtablestorage_lookup( map, p, (uintptr_t) user_hash));
}


int   mulle_concurrent_hashtable_insert( struct mulle_concurrent_hashtable *map,
                                       intptr_t user_hash,
                                       void *value)
{
   struct _mulle_concurrent_hashtablestorage   *p;
   int                                         rval;
   size_t                                      max;
   size_t                                      n;
   uintptr_t                                   hash;

   if( ! mulle_concurrent_hashtable_value_is_valid( map, value))
      return( EINVAL);
   if( ! mulle_concurrent_hashtable_hash_is_valid( map, user_hash))
      return( EINVAL);

   hash = (uintptr_t) user_hash;

retry:
   p   = _mulle_atomic_pointer_read( &map->storage.pointer);
   max = _mulle_concurrent_hashtablestorage_get_max_n_hashs( p);
   n   = (size_t) (uintptr_t) _mulle_atomic_pointer_read( &p->n_hashs);

   if( n >= max)
   {
      _mulle_concurrent_hashtable_migrate_storage( map, p);
      goto retry;
   }

   rval = _mulle_concurrent_hashtablestorage_insert( map, p, hash, value);
   if( rval == EBUSY)
   {
      _mulle_concurrent_hashtable_migrate_storage( map, p);
      goto retry;
   }
   return( rval);
}


int   mulle_concurrent_hashtable_register( struct mulle_concurrent_hashtable *map,
                                         intptr_t user_hash,
                                         void *value,
                                         void **p_old)
{
   struct _mulle_concurrent_hashtablestorage   *p;
   int                                         rval;
   size_t                                      max;
   size_t                                      n;
   void                                        *old;
   uintptr_t                                   hash;

   if( ! mulle_concurrent_hashtable_value_is_valid( map, value))
      return( EINVAL);
   if( ! mulle_concurrent_hashtable_hash_is_valid( map, user_hash))
      return( EINVAL);

   hash = (uintptr_t) user_hash;

retry:
   p   = _mulle_atomic_pointer_read( &map->storage.pointer);
   max = _mulle_concurrent_hashtablestorage_get_max_n_hashs( p);
   n   = (size_t) (uintptr_t) _mulle_atomic_pointer_read( &p->n_hashs);

   if( n >= max)
   {
      _mulle_concurrent_hashtable_migrate_storage( map, p);
      goto retry;
   }

   rval = _mulle_concurrent_hashtablestorage_register( map, p, hash, value, &old);
   if( rval == EBUSY)
   {
      _mulle_concurrent_hashtable_migrate_storage( map, p);
      goto retry;
   }

   if( p_old)
      *p_old = old;
   return( 0);
}


int   mulle_concurrent_hashtable_remove( struct mulle_concurrent_hashtable *map,
                                         intptr_t user_hash,
                                         void *value)
{
   struct _mulle_concurrent_hashtablestorage   *p;
   int                                         rval;

   if( ! mulle_concurrent_hashtable_value_is_valid( map, value))
      return( EINVAL);
   if( ! mulle_concurrent_hashtable_hash_is_valid( map, user_hash))
      return( EINVAL);

retry:
   p    = _mulle_atomic_pointer_read( &map->storage.pointer);
   rval = _mulle_concurrent_hashtablestorage_remove( map, p, (uintptr_t) user_hash, value);
   if( rval == EBUSY)
   {
      _mulle_concurrent_hashtable_migrate_storage( map, p);
      goto retry;
   }
   return( rval);
}


size_t   mulle_concurrent_hashtable_get_size( struct mulle_concurrent_hashtable *map)
{
   struct _mulle_concurrent_hashtablestorage   *p;

   if( ! map)
      return( 0);

   p = _mulle_atomic_pointer_read( &map->storage.pointer);
   return( (size_t) p->mask + 1);
}


//
// Snapshot of the current generation. A sealed value is still the value of
// its slot, so a migration in progress does not force a restart.
//
size_t   mulle_concurrent_hashtable_count( struct mulle_concurrent_hashtable *map)
{
   struct _mulle_concurrent_hashtablepair      *entry;
   struct _mulle_concurrent_hashtablepair      *sentinel;
   struct _mulle_concurrent_hashtablestorage   *p;
   size_t                                      count;
   void                                        *value;

   if( ! map)
      return( 0);

   count    = 0;
   p        = _mulle_atomic_pointer_read( &map->storage.pointer);
   entry    = p->entries;
   sentinel = &p->entries[ (size_t) p->mask + 1];

   for( ; entry < sentinel; entry++)
   {
      if( hashtable_hash_of( _mulle_concurrent_hashtablepair_get_hash( entry),
                             map->hash_mask) == MULLE_CONCURRENT_NO_HASH)
         continue;

      value = hashtable_unseal( _mulle_atomic_pointer_read( &entry->value), map->sealed_bit);
      if( hashtable_is_live( value, map->tombstone))
         ++count;
   }
   return( count);
}


#pragma mark - limited multi-threaded

int   _mulle_concurrent_hashtableenumerator_next( struct mulle_concurrent_hashtableenumerator *rover,
                                                  intptr_t *p_hash,
                                                  void **p_value)
{
   struct _mulle_concurrent_hashtablepair      *entry;
   struct _mulle_concurrent_hashtablestorage   *p;
   struct mulle_concurrent_hashtable           *map;
   uintptr_t                                   found;
   size_t                                      size;
   void                                        *value;

   map  = rover->map;
   p    = _mulle_atomic_pointer_read( &map->storage.pointer);
   size = (size_t) p->mask + 1;

   //
   // 'index' is an offset into the generation we started on. If the storage
   // advanced between calls, applying it to the new generation would silently
   // skip and duplicate entries. Compare the generation instead. The stale
   // pointer is only ever compared, never dereferenced. (ABA caveat: a recycled
   // allocation at the same address would defeat this, which is why the
   // enumerator remains "limited multi-threaded".)
   //
   if( ! rover->storage)
      rover->storage = p;
   else
      if( rover->storage != p)
         return( ECANCELED);

   while( rover->index < size)
   {
      entry = &p->entries[ rover->index];
      ++rover->index;

      found = _mulle_concurrent_hashtablepair_get_hash( entry);
      if( hashtable_is_frozen( found, map->frozen_bit))
         return( ECANCELED);
      if( found == MULLE_CONCURRENT_NO_HASH)
         continue;

      value = _mulle_atomic_pointer_read( &entry->value);
      if( hashtable_is_sealed( value, map->sealed_bit))
         return( ECANCELED);
      if( ! hashtable_is_live( value, map->tombstone))
         continue;

      if( p_hash)
         *p_hash = hashtable_unfold_user_hash( found, map->hash_mask);
      if( p_value)
         *p_value = value;
      return( 1);
   }
   return( 0);
}
