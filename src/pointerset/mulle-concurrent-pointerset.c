//
//  mulle-concurrent-pointerset.c
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
//  mulle-concurrent-pointerset.c
//  mulle-concurrent
//
#include "mulle-concurrent-pointerset.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>


// Slot states:
//   NULL       (MULLE_CONCURRENT_NO_POINTER)      = empty, probe chain stops here
//   INTPTR_MIN (MULLE_CONCURRENT_INVALID_POINTER) = REDIRECT (migration in progress)
//   other                                         = live pointer
//
// The set is insert-only: there is no remove, so no tombstones are needed and
// a NULL is always the true end of a probe chain. INTPTR_MAX remains reserved
// library-wide and is rejected as user data, but it is not a slot state here.

#define REDIRECT_VALUE   MULLE_CONCURRENT_INVALID_POINTER


//
// Test scaffolding, same idea as MULLE_CONCURRENT_HASHTABLE_RACE_YIELD.
// Defining MULLE_CONCURRENT_POINTERSET_RACE_YIELD widens the migration race
// windows so that the stochastic tests can hit them. Compiles to nothing
// otherwise.
//
#ifdef MULLE_CONCURRENT_POINTERSET_RACE_YIELD

#define  pointerset_race_yield( void)   MULLE_THREAD_UNPLEASANT_RACE_YIELD()
#else
# define pointerset_race_yield()   do {} while( 0)
#endif

//
// Named race points. By default each one is just pointerset_race_yield().
// A test can #define pointerset_race_point( site) and #include this file to
// park a thread at an exact point, which turns a probabilistic race into a
// deterministic interleaving (see test/pointerset/migration_stall.c).
//
#define POINTERSET_RACE_INSERT         1   // insert: storage read, slot CAS not yet
#define POINTERSET_RACE_REGISTER       2   // register: storage read, slot CAS not yet
#define POINTERSET_RACE_MEMBER         4   // member: storage read, probe not started
#define POINTERSET_RACE_CLAIMED        5   // register: slot CAS won, n_used not bumped yet
#define POINTERSET_RACE_COPY_EMPTY     6   // copy: empty slot seen, not frozen yet
#define POINTERSET_RACE_COPY_READ      7   // copy: value read, not registered yet
#define POINTERSET_RACE_COPY_CARRIED   8   // copy: carried into successor, not redirected

#ifndef pointerset_race_point
# define pointerset_race_point( site)   pointerset_race_yield()
#endif


static inline uintptr_t   mulle_pointer_hash( void *ptr)
{
   return( (uintptr_t) ptr);
}


static const struct _mulle_concurrent_pointersetstorage   mulle_concurrent_empty_pointersetstorage =
{
   (void *) -1,   // n_used = const sentinel
   0,             // mask
   { NULL }
};


#pragma mark - storage alloc/free

MULLE_C_NONNULL_RETURN
static struct _mulle_concurrent_pointersetstorage *
   _mulle_concurrent_alloc_pointersetstorage( size_t n,
                                              struct mulle_allocator *allocator)
{
   struct _mulle_concurrent_pointersetstorage  *p;

   if( n < 4)
      n = 4;

   assert( (n & (n - 1)) == 0);  // power of 2

   if( n - 1 > (SIZE_MAX - sizeof( struct _mulle_concurrent_pointersetstorage)) /
               sizeof( mulle_atomic_pointer_t))
      _mulle_allocator_abort( allocator, NULL, (size_t) -1);

   p = _mulle_allocator_calloc( allocator, 1,
         sizeof( struct _mulle_concurrent_pointersetstorage) +
         sizeof( mulle_atomic_pointer_t) * (n - 1));
   p->mask = n - 1;
   // calloc zeroes entries → all NULL = empty, good
   return( p);
}


static size_t
   _mulle_concurrent_pointersetstorage_get_max_n_used( struct _mulle_concurrent_pointersetstorage *p)
{
   size_t   size;

   size = (size_t) p->mask + 1;
   return( size - (size >> 1));   // 50% load
}


#pragma mark - storage operations

static int
   _mulle_concurrent_pointersetstorage_member( struct _mulle_concurrent_pointersetstorage *p,
                                               void *ptr)
{
   void          *found;
   uintptr_t     index;
   uintptr_t     probes;

   index = mulle_pointer_hash( ptr);
   for( probes = 0;; probes++)
   {
      found = _mulle_atomic_pointer_read( &p->entries[ index & (uintptr_t) p->mask]);
      if( found == MULLE_CONCURRENT_NO_POINTER)
         return( 0);
      if( found == ptr)
         return( 1);
      if( MULLE_C_UNLIKELY( found == REDIRECT_VALUE))
         return( EBUSY);
      // other pointer: keep probing
      if( MULLE_C_UNLIKELY( probes >= p->mask))
         return( 0);       // full cycle without a match or a redirect
      ++index;
   }
}


// Returns:
//   MULLE_CONCURRENT_NO_POINTER      : inserted
//   MULLE_CONCURRENT_INVALID_POINTER : REDIRECT (storage busy)
//   ptr                              : already present
//
static void *
   _mulle_concurrent_pointersetstorage_register( struct _mulle_concurrent_pointersetstorage *p,
                                                 void *ptr)
{
   void          *found;
   uintptr_t     index;
   uintptr_t     probes;

   assert( ptr != MULLE_CONCURRENT_NO_POINTER);
   assert( ptr != MULLE_CONCURRENT_INVALID_POINTER);
   assert( ptr != MULLE_CONCURRENT_TOMBSTONE_POINTER);

   index = mulle_pointer_hash( ptr);
   for( probes = 0;; probes++)
   {
      found = _mulle_atomic_pointer_read( &p->entries[ index & (uintptr_t) p->mask]);

      if( found == ptr)
         return( ptr);   // already present

      if( found == MULLE_CONCURRENT_NO_POINTER)
      {
         // try to claim this empty slot
         found = __mulle_atomic_pointer_cas( &p->entries[ index & (uintptr_t) p->mask],
                                             ptr,
                                             MULLE_CONCURRENT_NO_POINTER);
         if( found == MULLE_CONCURRENT_NO_POINTER)
         {
            pointerset_race_point( POINTERSET_RACE_CLAIMED);
            // claimed: bump n_used. A stalled increment only makes the load
            // threshold advisory; it must never be a probe-loop exit.
            _mulle_atomic_pointer_increment( &p->n_used);
            return( MULLE_CONCURRENT_NO_POINTER);  // inserted
         }
         // lost the CAS — re-examine what's there now
         if( MULLE_C_UNLIKELY( found == REDIRECT_VALUE))
            return( MULLE_CONCURRENT_INVALID_POINTER);
         if( found == ptr)
            return( ptr);
         // something else landed here, keep probing
      }
      else if( MULLE_C_UNLIKELY( found == REDIRECT_VALUE))
         return( MULLE_CONCURRENT_INVALID_POINTER);
      // collision: probe on

      if( MULLE_C_UNLIKELY( probes >= p->mask))
         return( MULLE_CONCURRENT_INVALID_POINTER);  // full cycle: migrate
      ++index;
   }
}


static int
   _mulle_concurrent_pointersetstorage_insert( struct _mulle_concurrent_pointersetstorage *p,
                                               void *ptr)
{
   void   *result;

   result = _mulle_concurrent_pointersetstorage_register( p, ptr);
   if( result == MULLE_CONCURRENT_NO_POINTER)
      return( 0);
   if( result == MULLE_CONCURRENT_INVALID_POINTER)
      return( EBUSY);
   return( EEXIST);
}


// Copy live entries from src to dst; freeze the src slots, set them to REDIRECT
// once their value has been carried. Tombstones are dropped (not copied).
//
// Ignoring the result of register() is safe here: every value this pass can
// still see in src was either frozen-and-carried by the pass that retired src
// (then it is already in dst or in dst's successor), or this pass is the one
// that freezes it. If dst is itself being migrated, INVALID means "already in
// a newer generation", not "lost".
static void
   _mulle_concurrent_pointersetstorage_copy( struct _mulle_concurrent_pointersetstorage *dst,
                                             struct _mulle_concurrent_pointersetstorage *src)
{
   mulle_atomic_pointer_t   *entry;
   mulle_atomic_pointer_t   *sentinel;
   void                     *value;
   void                     *actual;

   // the shared empty storage has one entry (mask == 0) and is const, so a
   // copy pass would write REDIRECT into .rodata. Never touch it.
   if( _mulle_concurrent_pointersetstorage_is_const( src))
      return;

   entry    = src->entries;
   sentinel = &src->entries[ src->mask + 1];

   for( ; entry < sentinel; entry++)
   {
      value = _mulle_atomic_pointer_read( entry);
      for(;;)
      {
         if( value == MULLE_CONCURRENT_NO_POINTER)
         {
            pointerset_race_point( POINTERSET_RACE_COPY_EMPTY);

            // Freeze the virgin slot so a writer holding this retired storage
            // cannot claim it after the pass. If we lose, the writer's value
            // is visible and is carried below.
            actual = __mulle_atomic_pointer_cas( entry,
                                                 REDIRECT_VALUE,
                                                 MULLE_CONCURRENT_NO_POINTER);
            if( actual == MULLE_CONCURRENT_NO_POINTER ||
                actual == REDIRECT_VALUE)
               break;
            value = actual;
            continue;
         }
         if( MULLE_C_UNLIKELY( value == REDIRECT_VALUE))
            break;

         pointerset_race_point( POINTERSET_RACE_COPY_READ);

         // copy first, then redirect
         _mulle_concurrent_pointersetstorage_register( dst, value);

         pointerset_race_point( POINTERSET_RACE_COPY_CARRIED);

         actual = __mulle_atomic_pointer_cas( entry, REDIRECT_VALUE, value);
         if( actual == value)
            break;
         value = actual;
      }
   }
}


#pragma mark - mulle_concurrent_pointerset

int  _mulle_concurrent_pointerset_init( struct mulle_concurrent_pointerset *set,
                                        size_t size,
                                        struct mulle_allocator *allocator)
{
   struct _mulle_concurrent_pointersetstorage   *storage;

   assert( EINVAL != 1 && EINVAL != 0);
   assert( ECANCELED != 1 && ECANCELED != 0);
   assert( EBUSY != 1 && EBUSY != 0);

   if( ! allocator)
      allocator = &mulle_default_allocator;

   assert( allocator->abafree && (void (*)(void)) allocator->abafree != (void (*)(void)) abort);

   _mulle_atomic_pointer_nonatomic_write( &set->allocator, allocator);

   if( size == 0)
      storage = (void *) &mulle_concurrent_empty_pointersetstorage;
   else
      storage = _mulle_concurrent_alloc_pointersetstorage( size, allocator);

   _mulle_atomic_pointer_nonatomic_write( &set->storage.pointer, storage);
   _mulle_atomic_pointer_nonatomic_write( &set->next_storage.pointer, storage);

   return( 0);
}


void  _mulle_concurrent_pointerset_done( struct mulle_concurrent_pointerset *set)
{
   struct _mulle_concurrent_pointersetstorage   *storage;
   struct _mulle_concurrent_pointersetstorage   *next_storage;
   struct mulle_allocator                       *allocator;

   storage      = _mulle_atomic_pointer_nonatomic_read( &set->storage.pointer);
   next_storage = _mulle_atomic_pointer_nonatomic_read( &set->next_storage.pointer);
   allocator    = _mulle_atomic_pointer_nonatomic_read( &set->allocator);

   if( next_storage != storage && ! _mulle_concurrent_pointersetstorage_is_const( next_storage))
      _mulle_allocator_abafree( allocator, next_storage);
   if( ! _mulle_concurrent_pointersetstorage_is_const( storage))
      _mulle_allocator_abafree( allocator, storage);
}


size_t  _mulle_concurrent_pointerset_get_size( struct mulle_concurrent_pointerset *set)
{
   struct _mulle_concurrent_pointersetstorage   *p;

   p = _mulle_atomic_pointer_read( &set->storage.pointer);
   return( (size_t) p->mask + 1);
}


static void
   _mulle_concurrent_pointerset_migrate_storage( struct mulle_concurrent_pointerset *set,
                                                 struct _mulle_concurrent_pointersetstorage *p)
{
   struct _mulle_concurrent_pointersetstorage   *q;
   struct _mulle_concurrent_pointersetstorage   *alloced;
   struct _mulle_concurrent_pointersetstorage   *previous;
   struct mulle_allocator                       *allocator;
   size_t                                       old_size;
   size_t                                       new_size;

   // A stale source must not trigger a copy pass. The load-bearing property is
   // that a retry after EBUSY/load-threshold always observes a strictly newer
   // generation; this fast path is what makes the retry chain finite.
   if( _mulle_atomic_pointer_read( &set->storage.pointer) != p)
      return;

   allocator = _mulle_atomic_pointer_read( &set->allocator);

   alloced = NULL;
   q       = _mulle_atomic_pointer_read( &set->next_storage.pointer);
   if( q == p)
   {
      old_size = (size_t) p->mask + 1;
      if( old_size > (SIZE_MAX / 2))
         _mulle_allocator_abort( allocator, NULL, (size_t) -1);

      new_size = old_size * 2;
      // Migration must at least double. The wait-free retry bound relies on a
      // strictly larger generation per retry. The only exception is the const
      // empty storage (size 1), which the allocator clamps to 4.
      assert( new_size == old_size * 2 || old_size < 4);

      alloced = _mulle_concurrent_alloc_pointersetstorage( new_size, allocator);
      q = __mulle_atomic_pointer_cas( &set->next_storage.pointer, alloced, p);
      if( q != p)
      {
         _mulle_allocator_abafree( allocator, alloced);
         alloced = NULL;
      }
      else
         q = alloced;
   }

   _mulle_concurrent_pointersetstorage_copy( q, p);

   previous = __mulle_atomic_pointer_cas( &set->storage.pointer, q, p);
   if( previous == p && ! _mulle_concurrent_pointersetstorage_is_const( previous))
      _mulle_allocator_abafree( allocator, previous);
}


#pragma mark - public multi-threaded ops

void  *_mulle_concurrent_pointerset_register( struct mulle_concurrent_pointerset *set,
                                              void *ptr)
{
   struct _mulle_concurrent_pointersetstorage   *p;
   size_t                                       n;
   size_t                                       max;
   void                                         *result;

retry:
   p   = _mulle_atomic_pointer_read( &set->storage.pointer);
   max = _mulle_concurrent_pointersetstorage_get_max_n_used( p);
   n   = (size_t) (uintptr_t) _mulle_atomic_pointer_read( &p->n_used);

   if( n >= max)
   {
      _mulle_concurrent_pointerset_migrate_storage( set, p);
      goto retry;
   }

   pointerset_race_point( POINTERSET_RACE_REGISTER);

   result = _mulle_concurrent_pointersetstorage_register( p, ptr);
   if( result == MULLE_CONCURRENT_INVALID_POINTER)
   {
      _mulle_concurrent_pointerset_migrate_storage( set, p);
      goto retry;
   }
   return( result);
}


void  *mulle_concurrent_pointerset_register( struct mulle_concurrent_pointerset *set,
                                             void *ptr)
{
   if( ! set || ! ptr ||
       ptr == MULLE_CONCURRENT_INVALID_POINTER ||
       ptr == MULLE_CONCURRENT_TOMBSTONE_POINTER)
   {
      errno = EINVAL;
      return( MULLE_CONCURRENT_INVALID_POINTER);
   }
   return( _mulle_concurrent_pointerset_register( set, ptr));
}


int  _mulle_concurrent_pointerset_insert( struct mulle_concurrent_pointerset *set,
                                          void *ptr)
{
   struct _mulle_concurrent_pointersetstorage   *p;
   size_t                                        n;
   size_t                                        max;
   int                                           rval;

retry:
   p   = _mulle_atomic_pointer_read( &set->storage.pointer);
   max = _mulle_concurrent_pointersetstorage_get_max_n_used( p);
   n   = (size_t) (uintptr_t) _mulle_atomic_pointer_read( &p->n_used);

   if( n >= max)
   {
      _mulle_concurrent_pointerset_migrate_storage( set, p);
      goto retry;
   }

   pointerset_race_point( POINTERSET_RACE_INSERT);

   rval = _mulle_concurrent_pointersetstorage_insert( p, ptr);
   if( MULLE_C_UNLIKELY( rval == EBUSY))
   {
      _mulle_concurrent_pointerset_migrate_storage( set, p);
      goto retry;
   }
   return( rval);
}


int  mulle_concurrent_pointerset_insert( struct mulle_concurrent_pointerset *set,
                                         void *ptr)
{
   if( ! set || ! ptr ||
       ptr == MULLE_CONCURRENT_INVALID_POINTER ||
       ptr == MULLE_CONCURRENT_TOMBSTONE_POINTER)
      return( EINVAL);
   return( _mulle_concurrent_pointerset_insert( set, ptr));
}


int  _mulle_concurrent_pointerset_member( struct mulle_concurrent_pointerset *set,
                                          void *ptr)
{
   struct _mulle_concurrent_pointersetstorage   *p;
   int                                           rval;

retry:
   p = _mulle_atomic_pointer_read( &set->storage.pointer);
   pointerset_race_point( POINTERSET_RACE_MEMBER);
   rval = _mulle_concurrent_pointersetstorage_member( p, ptr);
   if( MULLE_C_UNLIKELY( rval == EBUSY))
   {
      _mulle_concurrent_pointerset_migrate_storage( set, p);
      goto retry;
   }
   return( rval);
}


#pragma mark - enumerator

static int
   _mulle_concurrent_pointerset_search_next( struct mulle_concurrent_pointerset *set,
                                             uintptr_t *expect_mask,
                                             uintptr_t *index,
                                             void **p_ptr)
{
   struct _mulle_concurrent_pointersetstorage   *p;
   void                                         *value;

retry:
   p = _mulle_atomic_pointer_read( &set->storage.pointer);
   if( *expect_mask && (uintptr_t) p->mask != *expect_mask)
      return( ECANCELED);

   while( *index <= (uintptr_t) p->mask)
   {
      value = _mulle_atomic_pointer_read( &p->entries[ *index]);
      (*index)++;

      if( value == MULLE_CONCURRENT_NO_POINTER)
         continue;

      if( MULLE_C_UNLIKELY( value == REDIRECT_VALUE))
      {
         _mulle_concurrent_pointerset_migrate_storage( set, p);
         goto retry;
      }

      if( p_ptr)
         *p_ptr = value;
      if( ! *expect_mask)
         *expect_mask = (uintptr_t) p->mask;
      return( 1);
   }
   return( 0);
}


int  _mulle_concurrent_pointerset_enumerator_next( struct mulle_concurrent_pointerset_enumerator *rover,
                                                   void **ptr)
{
   if( ! rover || ! rover->set)
      return( 0);

   return( _mulle_concurrent_pointerset_search_next( rover->set,
                                                     &rover->mask,
                                                     &rover->index,
                                                     ptr));
}


#pragma mark - conveniences

size_t  mulle_concurrent_pointerset_count( struct mulle_concurrent_pointerset *set)
{
   struct mulle_concurrent_pointerset_enumerator   rover;
   size_t                                          count;
   int                                             rval;

retry:
   count = 0;
   rover = mulle_concurrent_pointerset_enumerate( set);
   for(;;)
   {
      rval = _mulle_concurrent_pointerset_enumerator_next( &rover, NULL);
      if( rval == 1)
      {
         ++count;
         continue;
      }
      if( ! rval)
         break;
      mulle_concurrent_pointerset_enumerator_done( &rover);
      goto retry;
   }
   mulle_concurrent_pointerset_enumerator_done( &rover);
   return( count);
}


void  *mulle_concurrent_pointerset_lookup_any( struct mulle_concurrent_pointerset *set)
{
   struct mulle_concurrent_pointerset_enumerator   rover;
   void                                            *any;

   any   = NULL;
   rover = mulle_concurrent_pointerset_enumerate( set);
   _mulle_concurrent_pointerset_enumerator_next( &rover, &any);
   mulle_concurrent_pointerset_enumerator_done( &rover);
   return( any);
}
