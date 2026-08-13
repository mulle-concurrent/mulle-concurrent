//
//  mulle-concurrent-hashmap2.c
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
#include "mulle-concurrent-hashmap2.h"

//
// Define MULLE_CONCURRENT_HASHMAP2_RACE_YIELD to widen the two-word race
// windows. test/hashmap2/lookup_race.c then fails within ~100 iterations
// against an unfixed lookup, instead of never. See dox/HASHMAP2.md.
//
// #define MULLE_CONCURRENT_HASHMAP2_RACE_YIELD   1

#include "mulle-concurrent-types.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>


#define EMPTY_VALUE   ((void *) 0)
#define FROZEN        MULLE_CONCURRENT_HASHMAP2_FROZEN


//
// Test scaffolding. The dangerous windows in this design are two adjacent
// instructions wide (read the hash word, then touch the value word), which no
// test will hit by luck. Defining MULLE_CONCURRENT_HASHMAP2_RACE_YIELD widens
// them so the races become reproducible. Compiles to nothing otherwise.
//
// Deliberately not rand(): glibc's rand() takes a process-global lock, so it
// would inject a lock and a barrier at exactly the window under observation,
// and all threads would share one unseeded sequence consumed in lock order.
// A per-thread xorshift keeps the decisions independent and lock free, so the
// probe perturbs the timing as little as possible.
//
#ifdef MULLE_CONCURRENT_HASHMAP2_RACE_YIELD

# if defined( __GNUC__) || defined( __clang__)
#  define HASHMAP2_RACE_TLS   __thread
# else
#  define HASHMAP2_RACE_TLS   /* shared, still lock free, good enough */
# endif

static inline void   hashmap2_race_yield( void)
{
   static HASHMAP2_RACE_TLS uint32_t   x;

   if( ! x)
      x = (uint32_t) (uintptr_t) &x | 1;   // distinct per thread, never 0

   x ^= x << 13;
   x ^= x >> 17;
   x ^= x << 5;

   if( (x & 0x3) == 0x1)
      mulle_thread_yield();
}
#else
# define hashmap2_race_yield()   do {} while( 0)
#endif


//
// The topmost bit of the hash word is the FROZEN flag, so a stored hash never
// uses it. Folding is not the identity, but a hash is a hash. 0 stays
// reserved as the "unclaimed" token, so a fold to 0 is nudged to 1.
//
// IMPORTANT: The topmost bit is reserved for FROZEN. A hash with the topmost
// bit set will be silently aliased to its & INTPTR_MAX equivalent. The assert
// catches this in debug builds. On 32-bit platforms this means hashes must be
// in [1, 0x7FFFFFFF]; on 64-bit in [1, 0x7FFFFFFFFFFFFFFF]. If you use
// pointer identity as the hash, this is safe on 64-bit (user-space pointers
// never set bit 63) but UNSAFE on 32-bit where addresses above 0x80000000
// will alias with their lower-half counterpart.
//
static inline intptr_t   hashmap2_fold_hash( intptr_t hash)
{
   assert( (hash & FROZEN) == 0 && "hash has FROZEN bit set — will alias!");
   hash &= INTPTR_MAX;
   return( hash ? hash : 1);
}


static inline int   hashmap2_is_frozen( intptr_t hashword)
{
   return( (hashword & FROZEN) != 0);
}


static inline intptr_t   hashmap2_hash_of( intptr_t hashword)
{
   return( hashword & INTPTR_MAX);
}


static inline intptr_t
   _mulle_concurrent_hashmap2pair_get_hash( struct _mulle_concurrent_hashmap2pair *entry)
{
   return( (intptr_t) _mulle_atomic_pointer_read( &entry->hash));
}


#pragma mark - storage

// n must be a power of 2
MULLE_C_NONNULL_RETURN
static struct _mulle_concurrent_hashmap2storage *
   _mulle_concurrent_hashmap2storage_alloc( unsigned int n,
                                            struct mulle_allocator *allocator)
{
   struct _mulle_concurrent_hashmap2storage   *p;

   if( n < 4)
      n = 4;
   assert( (~(n - 1) & n) == n);

   // calloc gives us hash == 0 and value == NULL, which is the virgin state.
   // the allocator either returns valid memory or aborts
   p = _mulle_allocator_calloc( allocator, 1,
                                sizeof( struct _mulle_concurrent_hashmap2pair) * (n - 1) +
                                sizeof( struct _mulle_concurrent_hashmap2storage));
   p->mask = n - 1;
   return( p);
}


static unsigned int
   _mulle_concurrent_hashmap2storage_get_max_n_hashs( struct _mulle_concurrent_hashmap2storage *p)
{
   unsigned int   size;

   // migrate at half claimed capacity, so a virgin slot always remains and
   // every linear probe terminates
   size = (unsigned int) p->mask + 1;
   return( size - (size >> 1));
}


static unsigned int
   _mulle_concurrent_hashmap2storage_get_migration_size( struct _mulle_concurrent_hashmap2storage *p)
{
   unsigned int   size;

   size = (unsigned int) p->mask + 1;
   if( size > (unsigned int) -1 / 2)
      abort();
   return( size * 2);
}


//
// Claims 'entry' for 'hash' if it is virgin. Returns the hash word that owns
// the slot afterwards: 'hash' if we won (or it was already ours), a foreign
// hash if we lost, or a frozen word if a migration retired the slot.
//
static inline intptr_t
   _mulle_concurrent_hashmap2storage_claim( struct _mulle_concurrent_hashmap2storage *p,
                                            struct _mulle_concurrent_hashmap2pair *entry,
                                            intptr_t hash)
{
   intptr_t   found;

   found = _mulle_concurrent_hashmap2pair_get_hash( entry);
   if( found != MULLE_CONCURRENT_NO_HASH)
      return( found);

   found = (intptr_t) __mulle_atomic_pointer_cas( &entry->hash,
                                                  (void *) hash,
                                                  (void *) MULLE_CONCURRENT_NO_HASH);
   if( found != MULLE_CONCURRENT_NO_HASH)
      return( found);

   _mulle_atomic_pointer_increment( &p->n_hashs);
   return( hash);
}


//
// Set the FROZEN bit. This is the commit point of the migration for this slot:
// afterwards no writer may modify the value, so whatever the value is at that
// moment is final and can be read without going stale.
//
// The hash word only ever moves NO_HASH -> h -> h|FROZEN, so this loop can
// lose at most twice. That is what bounds copy's per-slot work, independently
// of how often the value changes.
//
static intptr_t
   _mulle_concurrent_hashmap2storage_freeze( struct _mulle_concurrent_hashmap2pair *entry)
{
   intptr_t   found;
   intptr_t   target;

   for(;;)
   {
      found = _mulle_concurrent_hashmap2pair_get_hash( entry);
      if( hashmap2_is_frozen( found))
         return( found);

      target = found | FROZEN;
      if( (intptr_t) __mulle_atomic_pointer_cas( &entry->hash,
                                                 (void *) target,
                                                 (void *) found) == found)
         return( target);
   }
}


//
// Install (hash,value) in generation 'p', never overwriting what is already
// there. If 'p' turns out to be retired, follow the frontier forward. Each hop
// lands in a strictly newer generation, so this is bounded by the number of
// remaining capacity doublings.
//
//
// Install (hash,value) in generation 'p', never overwriting what is already
// there. If 'p' turns out to be retired, follow the frontier forward. Each hop
// lands in a strictly newer generation, so this is bounded by the number of
// remaining capacity doublings.
//
// Returns the value that is registered for 'hash' afterwards: 'value' if we
// installed it, otherwise the value that was already there. Callers need this
// to keep their contract: a writer whose value loses to an existing one must
// report EEXIST rather than success.
//
static void *
   _mulle_concurrent_hashmap2_carry( struct mulle_concurrent_hashmap2 *map,
                                     struct _mulle_concurrent_hashmap2storage *p,
                                     intptr_t hash,
                                     void *value)
{
   struct _mulle_concurrent_hashmap2pair      *entry;
   struct _mulle_concurrent_hashmap2storage   *q;
   intptr_t                                   found;
   unsigned int                               index;
   void                                       *old;
#ifndef NDEBUG
   unsigned int                               sentinel;
#endif

   for(;;)
   {
      index = (unsigned int) hash;
#ifndef NDEBUG
      sentinel = (unsigned int) hash + (unsigned int) p->mask + 1;
#endif
      for(;;)
      {
         entry = &p->entries[ index & (unsigned int) p->mask];
         found = _mulle_concurrent_hashmap2storage_claim( p, entry, hash);
         if( hashmap2_is_frozen( found))
            break;

         if( found == hash)
         {
            old = __mulle_atomic_pointer_cas( &entry->value, value, EMPTY_VALUE);
            if( old != EMPTY_VALUE)
               return( old);   // destination already holds a value for 'hash',
                               // ours is redundant. Never chase on with it,
                               // that would reinject it past a newer removal
            if( ! hashmap2_is_frozen( _mulle_concurrent_hashmap2pair_get_hash( entry)))
               return( value);
            break;          // installed into a generation that is retiring,
                            // the freezer may have missed it, so chase on
         }
         ++index;
         assert( index != sentinel);   // see the note in storage_insert
      }

      q = _mulle_atomic_pointer_read( &map->next_storage.pointer);
      if( q == p)
         return( value);
      p = q;
   }
}


//
//  0     : '*p_value' is the value, or NULL if absent
//  EBUSY : storage is migrating
//
static int
   _mulle_concurrent_hashmap2storage_lookup( struct _mulle_concurrent_hashmap2storage *p,
                                             intptr_t hash,
                                             void **p_value)
{
   struct _mulle_concurrent_hashmap2pair   *entry;
   intptr_t                                found;
   unsigned int                            index;
#ifndef NDEBUG
   unsigned int                            sentinel;

   sentinel = (unsigned int) hash + (unsigned int) p->mask + 1;
#endif

   index = (unsigned int) hash;
   for(;;)
   {
      entry = &p->entries[ index & (unsigned int) p->mask];
      found = _mulle_concurrent_hashmap2pair_get_hash( entry);

      if( found == MULLE_CONCURRENT_NO_HASH)
      {
         // copy freezes virgin slots, so an unclaimed slot proves the cursor
         // has not passed here and our hash is not further down the chain
         *p_value = EMPTY_VALUE;
         return( 0);
      }

      if( hashmap2_is_frozen( found))
         return( EBUSY);

      if( found == hash)
      {
         hashmap2_race_yield();   // let a migration freeze, carry and consume

         *p_value = _mulle_atomic_pointer_read( &entry->value);
         if( *p_value != EMPTY_VALUE)
            return( 0);

         //
         // An empty value means either genuinely absent, or that a migration
         // froze this slot, carried the value into a newer generation and
         // consumed it here, all after we read the hash word as unfrozen.
         // Re-read the gate to tell the two apart, otherwise we would report
         // a live entry as absent.
         //
         // NOTE: the window is two adjacent instructions wide and test
         // lookup_race.c does not reproduce it. This is reasoned, not measured.
         //
         if( hashmap2_is_frozen( _mulle_concurrent_hashmap2pair_get_hash( entry)))
            return( EBUSY);
         return( 0);
      }
      ++index;
      assert( index != sentinel);   // see the note in storage_insert
   }
}


//
// After a successful value CAS the slot may have been frozen in between, in
// which case we wrote into a retired generation. The copier that froze it may
// have read the value before us and carried nothing, so we carry it ourselves
// and then consume it, so that the retired slot ends up empty like any other
// drained slot.
//
//
// After a successful value CAS the slot may have been frozen in between, in
// which case we wrote into a retired generation. The copier that froze it may
// have read the value before us and carried nothing, so we carry it ourselves
// and then consume it, so that the retired slot ends up empty like any other
// drained slot.
//
// Returns the value that is registered for 'hash' afterwards, which is not
// necessarily ours: the newer generation may already hold a value carried out
// of this one. Callers must report that case instead of claiming success.
//
static void *
   _mulle_concurrent_hashmap2_post_check( struct mulle_concurrent_hashmap2 *map,
                                          struct _mulle_concurrent_hashmap2storage *p,
                                          struct _mulle_concurrent_hashmap2pair *entry,
                                          intptr_t hash,
                                          void *value)
{
   struct _mulle_concurrent_hashmap2storage   *q;
   void                                       *actual;

   if( ! hashmap2_is_frozen( _mulle_concurrent_hashmap2pair_get_hash( entry)))
      return( value);

   q = _mulle_atomic_pointer_read( &map->next_storage.pointer);
   if( q == p)
      return( value);

   actual = _mulle_concurrent_hashmap2_carry( map, q, hash, value);
   __mulle_atomic_pointer_cas( &entry->value, EMPTY_VALUE, value);
   return( actual);
}


//
//  0      : did insert
//  EEXIST : a live value is present
//  EBUSY  : storage is migrating
//
static int
   _mulle_concurrent_hashmap2storage_insert( struct mulle_concurrent_hashmap2 *map,
                                             struct _mulle_concurrent_hashmap2storage *p,
                                             intptr_t hash,
                                             void *value)
{
   struct _mulle_concurrent_hashmap2pair   *entry;
   intptr_t                                found;
   unsigned int                            index;
   void                                    *old;
#ifndef NDEBUG
   unsigned int                            sentinel;

   sentinel = (unsigned int) hash + (unsigned int) p->mask + 1;
#endif

   index = (unsigned int) hash;
   for(;;)
   {
      entry = &p->entries[ index & (unsigned int) p->mask];
      found = _mulle_concurrent_hashmap2storage_claim( p, entry, hash);
      if( hashmap2_is_frozen( found))
         return( EBUSY);

      if( found == hash)
      {
         hashmap2_race_yield();   // let a migration freeze this slot

         // the value word has only two states, empty or live, so a failed CAS
         // unambiguously means "already registered"
         old = __mulle_atomic_pointer_cas( &entry->value, value, EMPTY_VALUE);
         if( old != EMPTY_VALUE)
            return( EEXIST);

         // we may have written into a slot that was being retired, in which
         // case the newer generation decides whose value survives
         if( _mulle_concurrent_hashmap2_post_check( map, p, entry, hash, value) != value)
            return( EEXIST);
         return( 0);
      }
      ++index;

      //
      // Migration starts at half claimed capacity, so the unused half is the
      // reserve for threads that passed the occupancy check concurrently and a
      // virgin slot normally remains. That is NOT unconditional: the check and
      // the claim are not atomic, so with more than size/2 threads in flight
      // past the check, n_hashs can overshoot and this probe can run out of
      // slots. Inherited from the original hashmap, see dox/HASHMAP2.md.
      //
      assert( index != sentinel);
   }
}


//
//  0      : '*p_old' is NULL if we inserted, else the pre-existing value
//  EBUSY  : storage is migrating
//
static int
   _mulle_concurrent_hashmap2storage_register( struct mulle_concurrent_hashmap2 *map,
                                               struct _mulle_concurrent_hashmap2storage *p,
                                               intptr_t hash,
                                               void *value,
                                               void **p_old)
{
   struct _mulle_concurrent_hashmap2pair   *entry;
   intptr_t                                found;
   unsigned int                            index;
   void                                    *actual;
   void                                    *old;
#ifndef NDEBUG
   unsigned int                            sentinel;

   sentinel = (unsigned int) hash + (unsigned int) p->mask + 1;
#endif

   index = (unsigned int) hash;
   for(;;)
   {
      entry = &p->entries[ index & (unsigned int) p->mask];
      found = _mulle_concurrent_hashmap2storage_claim( p, entry, hash);
      if( hashmap2_is_frozen( found))
         return( EBUSY);

      if( found == hash)
      {
         hashmap2_race_yield();   // let a migration freeze this slot

         old = __mulle_atomic_pointer_cas( &entry->value, value, EMPTY_VALUE);
         if( old == EMPTY_VALUE)
         {
            // if the slot was being retired, the newer generation may already
            // hold a value carried out of this one, and that one wins
            actual = _mulle_concurrent_hashmap2_post_check( map, p, entry, hash, value);
            old    = (actual == value) ? EMPTY_VALUE : actual;
         }

         *p_old = old;
         return( 0);
      }
      ++index;
      assert( index != sentinel);   // see the note in storage_insert
   }
}


//
//  0      : removed
//  ENOENT : (hash,value) not present
//  EBUSY  : storage is migrating, nothing was removed
//  EAGAIN : removed, but the slot was retired underneath us, so the removal
//           must be repeated in the newer generation
//
static int
   _mulle_concurrent_hashmap2storage_remove( struct _mulle_concurrent_hashmap2storage *p,
                                             intptr_t hash,
                                             void *value)
{
   struct _mulle_concurrent_hashmap2pair   *entry;
   intptr_t                                found;
   unsigned int                            index;
   void                                    *old;
#ifndef NDEBUG
   unsigned int                            sentinel;

   sentinel = (unsigned int) hash + (unsigned int) p->mask + 1;
#endif

   index = (unsigned int) hash;
   for(;;)
   {
      entry = &p->entries[ index & (unsigned int) p->mask];
      found = _mulle_concurrent_hashmap2pair_get_hash( entry);

      if( found == MULLE_CONCURRENT_NO_HASH)
         return( ENOENT);

      if( hashmap2_is_frozen( found))
         return( EBUSY);

      if( found == hash)
      {
         hashmap2_race_yield();   // let a migration freeze, carry and consume

         old = __mulle_atomic_pointer_cas( &entry->value, EMPTY_VALUE, value);
         if( old != value)
         {
            //
            // An empty value here may mean a migration carried our pair into a
            // newer generation and consumed it, after we read the hash word as
            // unfrozen. Reporting ENOENT would then lose the removal, so check
            // the gate and go on in the newer generation instead.
            //
            // Reproduced by test/hashmap2/remove_migrate_race.c, which forces
            // same-size migrations: without this check it reports a lost
            // removal within a couple of thousand iterations.
            //
            if( old == EMPTY_VALUE &&
                hashmap2_is_frozen( _mulle_concurrent_hashmap2pair_get_hash( entry)))
               return( EBUSY);
            return( ENOENT);
         }

         if( hashmap2_is_frozen( _mulle_concurrent_hashmap2pair_get_hash( entry)))
            return( EAGAIN);
         return( 0);
      }
      ++index;
      assert( index != sentinel);   // see the note in storage_insert
   }
}


//
// Freeze every slot, then carry the live ones. Freezing first is what makes
// the carried value provably final: no speculative install, so a removal can
// never be undone by a lagging copier.
//
static void
   _mulle_concurrent_hashmap2storage_copy( struct mulle_concurrent_hashmap2 *map,
                                           struct _mulle_concurrent_hashmap2storage *dst,
                                           struct _mulle_concurrent_hashmap2storage *src)
{
   struct _mulle_concurrent_hashmap2pair   *entry;
   struct _mulle_concurrent_hashmap2pair   *sentinel;
   intptr_t                                hash;
   void                                    *value;

   entry    = src->entries;
   sentinel = &src->entries[ (unsigned int) src->mask + 1];

   for( ; entry < sentinel; entry++)
   {
      hash = hashmap2_hash_of( _mulle_concurrent_hashmap2storage_freeze( entry));
      if( hash == MULLE_CONCURRENT_NO_HASH)
         continue;                  // retired virgin, nothing was ever here

      hashmap2_race_yield();        // let a writer land in the frozen slot

      value = _mulle_atomic_pointer_read( &entry->value);
      if( value == EMPTY_VALUE)
         continue;                  // empty, removed, or already drained

      _mulle_concurrent_hashmap2_carry( map, dst, hash, value);

      hashmap2_race_yield();        // let a reader observe the pre-consume slot

      // Consume it. A frozen slot preserves its payload, which is the whole
      // point, but that means "frozen" alone cannot also mean "already
      // drained". Consuming gives the drained state an explicit
      // representation, so re-running copy over a stale generation is a
      // genuine no-op instead of reinjecting values that have been removed
      // in a newer generation since.
      __mulle_atomic_pointer_cas( &entry->value, EMPTY_VALUE, value);
   }
}


static void
   _mulle_concurrent_hashmap2_migrate_storage_with_size( struct mulle_concurrent_hashmap2 *map,
                                                         struct _mulle_concurrent_hashmap2storage *p,
                                                         unsigned int new_size)
{
   struct _mulle_concurrent_hashmap2storage   *alloced;
   struct _mulle_concurrent_hashmap2storage   *previous;
   struct _mulle_concurrent_hashmap2storage   *q;
   struct mulle_allocator                     *allocator;

   assert( p);

   allocator = _mulle_atomic_pointer_read( &map->allocator);

   alloced = NULL;
   q       = _mulle_atomic_pointer_read( &map->next_storage.pointer);
   if( q == p)
   {
      alloced = _mulle_concurrent_hashmap2storage_alloc( new_size, allocator);
      q = __mulle_atomic_pointer_cas( &map->next_storage.pointer, alloced, p);
      if( q != p)
      {
         _mulle_allocator_abafree( allocator, alloced);
         alloced = NULL;
      }
      else
         q = alloced;
   }

   _mulle_concurrent_hashmap2storage_copy( map, q, p);

   previous = __mulle_atomic_pointer_cas( &map->storage.pointer, q, p);
   if( previous == p)
      _mulle_allocator_abafree( allocator, previous);
}


static inline void
   _mulle_concurrent_hashmap2_migrate_storage( struct mulle_concurrent_hashmap2 *map,
                                               struct _mulle_concurrent_hashmap2storage *p)
{
   _mulle_concurrent_hashmap2_migrate_storage_with_size( map, p,
      _mulle_concurrent_hashmap2storage_get_migration_size( p));
}


//
// Retire the current generation into a fresh one of the *same* size. Exists so
// that tests can force continuous generation changes without doubling memory on
// every call, which is what makes the remove-versus-copy race (S6 in
// dox/HASHMAP2.md) reproducible: widening the instruction window is not enough
// there, because migration frequency is the binding constraint.
//
// Safe as a general operation too: only live values are carried, and the live
// count cannot exceed the threshold that would have grown the table anyway.
//
void   _mulle_concurrent_hashmap2_migrate_same_size( struct mulle_concurrent_hashmap2 *map)
{
   struct _mulle_concurrent_hashmap2storage   *p;

   if( ! map)
      return;

   p = _mulle_atomic_pointer_read( &map->storage.pointer);
   _mulle_concurrent_hashmap2_migrate_storage_with_size( map, p,
                                                         (unsigned int) p->mask + 1);
}


#pragma mark - single-threaded

int   mulle_concurrent_hashmap2_init( struct mulle_concurrent_hashmap2 *map,
                                      unsigned int size,
                                      struct mulle_allocator *allocator)
{
   struct _mulle_concurrent_hashmap2storage   *storage;

   if( ! map)
      return( EINVAL);

   if( ! allocator)
      allocator = &mulle_default_allocator;

   storage = _mulle_concurrent_hashmap2storage_alloc( size, allocator);

   _mulle_atomic_pointer_nonatomic_write( &map->allocator, allocator);
   _mulle_atomic_pointer_nonatomic_write( &map->storage.pointer, storage);
   _mulle_atomic_pointer_nonatomic_write( &map->next_storage.pointer, storage);

   return( 0);
}


void  mulle_concurrent_hashmap2_done( struct mulle_concurrent_hashmap2 *map)
{
   struct _mulle_concurrent_hashmap2storage   *next_storage;
   struct _mulle_concurrent_hashmap2storage   *storage;
   struct mulle_allocator                     *allocator;

   if( ! map)
      return;

   allocator    = _mulle_atomic_pointer_nonatomic_read( &map->allocator);
   storage      = _mulle_atomic_pointer_nonatomic_read( &map->storage.pointer);
   next_storage = _mulle_atomic_pointer_nonatomic_read( &map->next_storage.pointer);

   if( storage)
      _mulle_allocator_free( allocator, storage);
   if( next_storage && next_storage != storage)
      _mulle_allocator_free( allocator, next_storage);

   _mulle_atomic_pointer_nonatomic_write( &map->storage.pointer, NULL);
   _mulle_atomic_pointer_nonatomic_write( &map->next_storage.pointer, NULL);
}


#pragma mark - multi-threaded

void  *mulle_concurrent_hashmap2_lookup( struct mulle_concurrent_hashmap2 *map,
                                         intptr_t hash)
{
   struct _mulle_concurrent_hashmap2storage   *p;
   void                                       *value;

   if( ! map || hash == MULLE_CONCURRENT_NO_HASH)
      return( NULL);

   hash = hashmap2_fold_hash( hash);

retry:
   p = _mulle_atomic_pointer_read( &map->storage.pointer);
   if( _mulle_concurrent_hashmap2storage_lookup( p, hash, &value) == EBUSY)
   {
      _mulle_concurrent_hashmap2_migrate_storage( map, p);
      goto retry;
   }
   return( value);
}


int   mulle_concurrent_hashmap2_insert( struct mulle_concurrent_hashmap2 *map,
                                       intptr_t hash,
                                       void *value)
{
   struct _mulle_concurrent_hashmap2storage   *p;
   int                                        rval;
   unsigned int                               max;
   unsigned int                               n;

   if( ! map || hash == MULLE_CONCURRENT_NO_HASH || value == EMPTY_VALUE)
      return( EINVAL);

   hash = hashmap2_fold_hash( hash);

retry:
   p   = _mulle_atomic_pointer_read( &map->storage.pointer);
   max = _mulle_concurrent_hashmap2storage_get_max_n_hashs( p);
   n   = (unsigned int) (uintptr_t) _mulle_atomic_pointer_read( &p->n_hashs);

   if( n >= max)
   {
      _mulle_concurrent_hashmap2_migrate_storage( map, p);
      goto retry;
   }

   rval = _mulle_concurrent_hashmap2storage_insert( map, p, hash, value);
   if( rval == EBUSY)
   {
      _mulle_concurrent_hashmap2_migrate_storage( map, p);
      goto retry;
   }
   return( rval);
}


int   mulle_concurrent_hashmap2_register( struct mulle_concurrent_hashmap2 *map,
                                         intptr_t hash,
                                         void *value,
                                         void **p_old)
{
   struct _mulle_concurrent_hashmap2storage   *p;
   int                                        rval;
   unsigned int                               max;
   unsigned int                               n;
   void                                       *old;

   if( ! map || hash == MULLE_CONCURRENT_NO_HASH || value == EMPTY_VALUE)
      return( EINVAL);

   hash = hashmap2_fold_hash( hash);

retry:
   p   = _mulle_atomic_pointer_read( &map->storage.pointer);
   max = _mulle_concurrent_hashmap2storage_get_max_n_hashs( p);
   n   = (unsigned int) (uintptr_t) _mulle_atomic_pointer_read( &p->n_hashs);

   if( n >= max)
   {
      _mulle_concurrent_hashmap2_migrate_storage( map, p);
      goto retry;
   }

   rval = _mulle_concurrent_hashmap2storage_register( map, p, hash, value, &old);
   if( rval == EBUSY)
   {
      _mulle_concurrent_hashmap2_migrate_storage( map, p);
      goto retry;
   }

   if( p_old)
      *p_old = old;
   return( 0);
}


int   mulle_concurrent_hashmap2_remove( struct mulle_concurrent_hashmap2 *map,
                                       intptr_t hash,
                                       void *value)
{
   struct _mulle_concurrent_hashmap2storage   *p;
   int                                        removed;
   int                                        rval;

   if( ! map || hash == MULLE_CONCURRENT_NO_HASH || value == EMPTY_VALUE)
      return( EINVAL);

   hash    = hashmap2_fold_hash( hash);
   removed = 0;

retry:
   p    = _mulle_atomic_pointer_read( &map->storage.pointer);
   rval = _mulle_concurrent_hashmap2storage_remove( p, hash, value);

   if( rval == EAGAIN)
   {
      // we did remove it, but on a slot that was being retired. A copier may
      // have carried the value forward before our CAS landed, so repeat the
      // removal in the newer generation. Legal because we have not returned
      // yet: a concurrent re-insert of the same value may be ordered before us
      removed = 1;
      _mulle_concurrent_hashmap2_migrate_storage( map, p);
      goto retry;
   }

   if( rval == EBUSY)
   {
      _mulle_concurrent_hashmap2_migrate_storage( map, p);
      goto retry;
   }

   if( removed && rval == ENOENT)
      return( 0);
   return( rval);
}


unsigned int   mulle_concurrent_hashmap2_get_size( struct mulle_concurrent_hashmap2 *map)
{
   struct _mulle_concurrent_hashmap2storage   *p;

   if( ! map)
      return( 0);

   p = _mulle_atomic_pointer_read( &map->storage.pointer);
   return( (unsigned int) p->mask + 1);
}


unsigned int   mulle_concurrent_hashmap2_count( struct mulle_concurrent_hashmap2 *map)
{
   struct _mulle_concurrent_hashmap2pair      *entry;
   struct _mulle_concurrent_hashmap2pair      *sentinel;
   struct _mulle_concurrent_hashmap2storage   *p;
   intptr_t                                   found;
   unsigned int                               count;

   if( ! map)
      return( 0);

retry:
   count    = 0;
   p        = _mulle_atomic_pointer_read( &map->storage.pointer);
   entry    = p->entries;
   sentinel = &p->entries[ (unsigned int) p->mask + 1];

   for( ; entry < sentinel; entry++)
   {
      found = _mulle_concurrent_hashmap2pair_get_hash( entry);
      if( hashmap2_is_frozen( found))
      {
         _mulle_concurrent_hashmap2_migrate_storage( map, p);
         goto retry;
      }
      if( found == MULLE_CONCURRENT_NO_HASH)
         continue;
      if( _mulle_atomic_pointer_read( &entry->value) != EMPTY_VALUE)
         ++count;
   }
   return( count);
}


#pragma mark - limited multi-threaded

int   _mulle_concurrent_hashmap2enumerator_next( struct mulle_concurrent_hashmap2enumerator *rover,
                                                 intptr_t *p_hash,
                                                 void **p_value)
{
   struct _mulle_concurrent_hashmap2pair      *entry;
   struct _mulle_concurrent_hashmap2storage   *p;
   intptr_t                                   found;
   unsigned int                               size;
   void                                       *value;

   p    = _mulle_atomic_pointer_read( &rover->map->storage.pointer);
   size = (unsigned int) p->mask + 1;

   //
   // 'index' is an offset into the generation we started on. If the storage
   // advanced between calls, applying it to the new generation would silently
   // skip and duplicate entries, and the new generation is not frozen so the
   // check below would not catch it. Compare the generation instead. The stale
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

      found = _mulle_concurrent_hashmap2pair_get_hash( entry);
      if( hashmap2_is_frozen( found))
         return( ECANCELED);
      if( found == MULLE_CONCURRENT_NO_HASH)
         continue;

      value = _mulle_atomic_pointer_read( &entry->value);
      if( value == EMPTY_VALUE)
         continue;

      if( p_hash)
         *p_hash = found;
      if( p_value)
         *p_value = value;
      return( 1);
   }
   return( 0);
}
