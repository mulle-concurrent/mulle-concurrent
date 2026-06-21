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
//   INTPTR_MAX (MULLE_CONCURRENT_TOMBSTONE)       = removed, probe chain continues
//   other                                         = live pointer
//
// Why tombstones instead of NULL on removal:
//   With linear probing, pointers that hash to the same slot form a chain.
//   If ptr_A, ptr_B, ptr_C all hash to slot 5, they land at slots 5, 6, 7.
//   Removing ptr_A by writing NULL breaks the chain: a lookup for ptr_C
//   stops at slot 5 (NULL = "nothing here, stop") and returns "not found"
//   even though ptr_C is at slot 7. A tombstone says "something was here,
//   keep probing" and preserves correctness.
//
// Why we do NOT reuse tombstone slots for new inserts:
//   Reuse would require: remember the first tombstone seen, scan forward to
//   confirm no duplicate exists, then go back and CAS the tombstone slot.
//   The scan and the CAS are not atomic, so another thread can insert the
//   same pointer in between, causing a duplicate. Making this race-free
//   requires a two-phase protocol nearly as complex as a lock.
//
//   Note: if two threads race to insert the same ptr_Q into a tombstone slot,
//   the CAS loser re-reads the slot, sees ptr_Q already there, and correctly
//   returns EEXIST — no duplicate. The problem is only with the forward scan
//   for pre-existing duplicates, not the CAS itself.
//
//   So we skip tombstones during insert (probe past them, claim a fresh NULL
//   slot instead). Tombstones accumulate but are dropped for free at migration
//   time — they are simply not copied to the new storage. The only cost is
//   that n_used fills up faster, triggering migrations sooner in remove-heavy
//   workloads.

#define REDIRECT_VALUE   MULLE_CONCURRENT_INVALID_POINTER
#define TOMBSTONE_VALUE  MULLE_CONCURRENT_TOMBSTONE_POINTER


static const struct _mulle_concurrent_pointersetstorage   mulle_concurrent_empty_pointersetstorage =
{
   (void *) -1,   // n_used = const sentinel
   0,             // mask
   { NULL }
};


// Scramble pointer bits to spread across table slots.
// Pointers are often aligned (low bits zero), so we mix.
static inline unsigned int   _pointerset_hash( void *ptr)
{
   uintptr_t   x;

   x = (uintptr_t) ptr;
   x ^= x >> 16;
   x *= 0x45d9f3bUL;
   x ^= x >> 16;
   return( (unsigned int) x);
}


#pragma mark - storage alloc/free

static struct _mulle_concurrent_pointersetstorage *
   _mulle_concurrent_alloc_pointersetstorage( unsigned int n,
                                              struct mulle_allocator *allocator)
{
   struct _mulle_concurrent_pointersetstorage  *p;

   assert( n >= 2 && (n & (n - 1)) == 0);  // power of 2

   if( n < 4)
      n = 4;

   p = _mulle_allocator_calloc( allocator, 1,
         sizeof( struct _mulle_concurrent_pointersetstorage) +
         sizeof( mulle_atomic_pointer_t) * (n - 1));
   p->mask = n - 1;
   // calloc zeroes entries → all NULL = empty, good
   return( p);
}


static unsigned int
   _mulle_concurrent_pointersetstorage_get_max_n_used( struct _mulle_concurrent_pointersetstorage *p)
{
   unsigned int   size;

   size = (unsigned int) p->mask + 1;
   return( size - (size >> 1));   // 50% load
}


#pragma mark - storage operations

static int
   _mulle_concurrent_pointersetstorage_member( struct _mulle_concurrent_pointersetstorage *p,
                                               void *ptr)
{
   void          *found;
   unsigned int   index;
#ifndef NDEBUG
   unsigned int   sentinel;
   sentinel = _pointerset_hash( ptr) + (unsigned int) p->mask + 1;
#endif

   index = _pointerset_hash( ptr);
   for(;;)
   {
      found = _mulle_atomic_pointer_read( &p->entries[ index & (unsigned int) p->mask]);
      if( found == MULLE_CONCURRENT_NO_POINTER)
         return( 0);
      if( found == ptr)
         return( 1);
      // tombstone or other pointer: keep probing
      ++index;
      assert( index != sentinel);
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
   unsigned int   index;
#ifndef NDEBUG
   unsigned int   sentinel;
   sentinel = _pointerset_hash( ptr) + (unsigned int) p->mask + 1;
#endif

   assert( ptr != MULLE_CONCURRENT_NO_POINTER);
   assert( ptr != MULLE_CONCURRENT_INVALID_POINTER);
   assert( ptr != TOMBSTONE_VALUE);

   index = _pointerset_hash( ptr);
   for(;;)
   {
      found = _mulle_atomic_pointer_read( &p->entries[ index & (unsigned int) p->mask]);

      if( found == ptr)
         return( ptr);   // already present

      if( found == MULLE_CONCURRENT_NO_POINTER)
      {
         // try to claim this empty slot
         found = __mulle_atomic_pointer_cas( &p->entries[ index & (unsigned int) p->mask],
                                             ptr,
                                             MULLE_CONCURRENT_NO_POINTER);
         if( found == MULLE_CONCURRENT_NO_POINTER)
         {
            // claimed: bump n_used
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
      // tombstone or collision: probe on
      // (we never reuse tombstone slots — see comment at top of file)

      ++index;
      assert( index != sentinel);
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


static int
   _mulle_concurrent_pointersetstorage_remove( struct _mulle_concurrent_pointersetstorage *p,
                                               void *ptr)
{
   void          *found;
   unsigned int   index;
#ifndef NDEBUG
   unsigned int   sentinel;
   sentinel = _pointerset_hash( ptr) + (unsigned int) p->mask + 1;
#endif

   index = _pointerset_hash( ptr);
   for(;;)
   {
      found = _mulle_atomic_pointer_read( &p->entries[ index & (unsigned int) p->mask]);

      if( found == MULLE_CONCURRENT_NO_POINTER)
         return( ENOENT);

      if( MULLE_C_UNLIKELY( found == REDIRECT_VALUE))
         return( EBUSY);

      if( found == ptr)
      {
         found = __mulle_atomic_pointer_cas( &p->entries[ index & (unsigned int) p->mask],
                                             TOMBSTONE_VALUE,
                                             ptr);
         if( found == ptr)
            return( 0);
         if( MULLE_C_UNLIKELY( found == REDIRECT_VALUE))
            return( EBUSY);
         // someone else removed it already
         return( ENOENT);
      }
      // tombstone or other pointer: keep probing
      ++index;
      assert( index != sentinel);
   }
}


// Copy live entries from src to dst; mark src slots as REDIRECT.
// Tombstones are dropped (not copied) — that's the cleanup.
static void
   _mulle_concurrent_pointersetstorage_copy( struct _mulle_concurrent_pointersetstorage *dst,
                                             struct _mulle_concurrent_pointersetstorage *src)
{
   mulle_atomic_pointer_t   *entry;
   mulle_atomic_pointer_t   *sentinel;
   void                     *value;
   void                     *actual;

   entry    = src->entries;
   sentinel = &src->entries[ src->mask + 1];

   for( ; entry < sentinel; entry++)
   {
      value = _mulle_atomic_pointer_read( entry);
      for(;;)
      {
         if( value == MULLE_CONCURRENT_NO_POINTER || value == TOMBSTONE_VALUE)
            break;   // empty or tombstone: skip — tombstones are dropped here,
                     // which is the free cleanup promised in the design
         if( MULLE_C_UNLIKELY( value == REDIRECT_VALUE))
            break;

         // copy first, then redirect
         _mulle_concurrent_pointersetstorage_register( dst, value);

         actual = __mulle_atomic_pointer_cas( entry, REDIRECT_VALUE, value);
         if( actual == value)
            break;
         value = actual;
      }
   }
}


#pragma mark - mulle_concurrent_pointerset

int  _mulle_concurrent_pointerset_init( struct mulle_concurrent_pointerset *set,
                                        unsigned int size,
                                        struct mulle_allocator *allocator)
{
   struct _mulle_concurrent_pointersetstorage   *storage;

   assert( EINVAL != 1 && EINVAL != 0);
   assert( ENOMEM != 1 && ENOMEM != 0);
   assert( ECANCELED != 1 && ECANCELED != 0);
   assert( EBUSY != 1 && EBUSY != 0);

   if( ! allocator)
      allocator = &mulle_default_allocator;

   assert( allocator->abafree && (int (*)(void)) allocator->abafree != (int (*)(void)) abort);

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


unsigned int  _mulle_concurrent_pointerset_get_size( struct mulle_concurrent_pointerset *set)
{
   struct _mulle_concurrent_pointersetstorage   *p;

   p = _mulle_atomic_pointer_read( &set->storage.pointer);
   return( (unsigned int) p->mask + 1);
}


static int
   _mulle_concurrent_pointerset_migrate_storage( struct mulle_concurrent_pointerset *set,
                                                 struct _mulle_concurrent_pointersetstorage *p)
{
   struct _mulle_concurrent_pointersetstorage   *q;
   struct _mulle_concurrent_pointersetstorage   *alloced;
   struct _mulle_concurrent_pointersetstorage   *previous;
   struct mulle_allocator                       *allocator;

   allocator = _mulle_atomic_pointer_read( &set->allocator);

   alloced = NULL;
   q       = _mulle_atomic_pointer_read( &set->next_storage.pointer);
   if( q == p)
   {
      alloced = _mulle_concurrent_alloc_pointersetstorage( ((unsigned int) p->mask + 1) * 2,
                                                           allocator);
      if( MULLE_C_UNLIKELY( ! alloced))
         return( ENOMEM);

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

   return( 0);
}


#pragma mark - public multi-threaded ops

void  *_mulle_concurrent_pointerset_register( struct mulle_concurrent_pointerset *set,
                                              void *ptr)
{
   struct _mulle_concurrent_pointersetstorage   *p;
   unsigned int                                  n;
   unsigned int                                  max;
   void                                         *result;

retry:
   p   = _mulle_atomic_pointer_read( &set->storage.pointer);
   max = _mulle_concurrent_pointersetstorage_get_max_n_used( p);
   n   = (unsigned int) (uintptr_t) _mulle_atomic_pointer_read( &p->n_used);

   if( n >= max)
   {
      if( _mulle_concurrent_pointerset_migrate_storage( set, p))
      {
         errno = ENOMEM;
         return( MULLE_CONCURRENT_INVALID_POINTER);
      }
      goto retry;
   }

   result = _mulle_concurrent_pointersetstorage_register( p, ptr);
   if( result == MULLE_CONCURRENT_INVALID_POINTER)
   {
      if( _mulle_concurrent_pointerset_migrate_storage( set, p))
      {
         errno = ENOMEM;
         return( MULLE_CONCURRENT_INVALID_POINTER);
      }
      goto retry;
   }
   return( result);
}


void  *mulle_concurrent_pointerset_register( struct mulle_concurrent_pointerset *set,
                                             void *ptr)
{
   if( ! set || ! ptr ||
       ptr == MULLE_CONCURRENT_INVALID_POINTER ||
       ptr == TOMBSTONE_VALUE)
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
   unsigned int                                  n;
   unsigned int                                  max;
   int                                           rval;

retry:
   p   = _mulle_atomic_pointer_read( &set->storage.pointer);
   max = _mulle_concurrent_pointersetstorage_get_max_n_used( p);
   n   = (unsigned int) (uintptr_t) _mulle_atomic_pointer_read( &p->n_used);

   if( n >= max)
   {
      if( _mulle_concurrent_pointerset_migrate_storage( set, p))
         return( ENOMEM);
      goto retry;
   }

   rval = _mulle_concurrent_pointersetstorage_insert( p, ptr);
   if( MULLE_C_UNLIKELY( rval == EBUSY))
   {
      if( _mulle_concurrent_pointerset_migrate_storage( set, p))
         return( ENOMEM);
      goto retry;
   }
   return( rval);
}


int  mulle_concurrent_pointerset_insert( struct mulle_concurrent_pointerset *set,
                                         void *ptr)
{
   if( ! set || ! ptr ||
       ptr == MULLE_CONCURRENT_INVALID_POINTER ||
       ptr == TOMBSTONE_VALUE)
      return( EINVAL);
   return( _mulle_concurrent_pointerset_insert( set, ptr));
}


int  _mulle_concurrent_pointerset_member( struct mulle_concurrent_pointerset *set,
                                          void *ptr)
{
   struct _mulle_concurrent_pointersetstorage   *p;

   p = _mulle_atomic_pointer_read( &set->storage.pointer);
   return( _mulle_concurrent_pointersetstorage_member( p, ptr));
}


int  _mulle_concurrent_pointerset_remove( struct mulle_concurrent_pointerset *set,
                                          void *ptr)
{
   struct _mulle_concurrent_pointersetstorage   *p;
   int                                           rval;

retry:
   p    = _mulle_atomic_pointer_read( &set->storage.pointer);
   rval = _mulle_concurrent_pointersetstorage_remove( p, ptr);
   if( MULLE_C_UNLIKELY( rval == EBUSY))
   {
      if( _mulle_concurrent_pointerset_migrate_storage( set, p))
         return( ENOMEM);
      goto retry;
   }
   return( rval);
}


int  mulle_concurrent_pointerset_remove( struct mulle_concurrent_pointerset *set,
                                         void *ptr)
{
   if( ! set || ! ptr ||
       ptr == MULLE_CONCURRENT_INVALID_POINTER ||
       ptr == TOMBSTONE_VALUE)
      return( EINVAL);
   return( _mulle_concurrent_pointerset_remove( set, ptr));
}


#pragma mark - enumerator

static int
   _mulle_concurrent_pointerset_search_next( struct mulle_concurrent_pointerset *set,
                                             unsigned int *expect_mask,
                                             unsigned int *index,
                                             void **p_ptr)
{
   struct _mulle_concurrent_pointersetstorage   *p;
   void                                         *value;

retry:
   p = _mulle_atomic_pointer_read( &set->storage.pointer);
   if( *expect_mask && (unsigned int) p->mask != *expect_mask)
      return( ECANCELED);

   while( *index <= (unsigned int) p->mask)
   {
      value = _mulle_atomic_pointer_read( &p->entries[ *index]);
      (*index)++;

      if( value == MULLE_CONCURRENT_NO_POINTER || value == TOMBSTONE_VALUE)
         continue;

      if( MULLE_C_UNLIKELY( value == REDIRECT_VALUE))
      {
         if( _mulle_concurrent_pointerset_migrate_storage( set, p))
            return( ENOMEM);
         goto retry;
      }

      if( p_ptr)
         *p_ptr = value;
      if( ! *expect_mask)
         *expect_mask = (unsigned int) p->mask;
      return( 1);
   }
   return( 0);
}


int  _mulle_concurrent_pointerset_enumerator_next( struct mulle_concurrent_pointerset_enumerator *rover,
                                                   void **ptr)
{
   return( _mulle_concurrent_pointerset_search_next( rover->set,
                                                     &rover->mask,
                                                     &rover->index,
                                                     ptr));
}


#pragma mark - conveniences

unsigned int  mulle_concurrent_pointerset_count( struct mulle_concurrent_pointerset *set)
{
   struct mulle_concurrent_pointerset_enumerator   rover;
   unsigned int                                     count;
   int                                              rval;

retry:
   count = 0;
   rover = mulle_concurrent_pointerset_enumerate( set);
   for(;;)
   {
      rval = _mulle_concurrent_pointerset_enumerator_next( &rover, NULL);
      if( rval == 1) { ++count; continue; }
      if( ! rval)    break;
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
