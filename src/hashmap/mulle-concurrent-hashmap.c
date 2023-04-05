//
//  mulle_concurrent_hashmap.c
//  mulle-concurrent
//
//  Created by Nat! on 04.03.16.
//  Copyright © 2016 Nat! for Mulle kybernetiK.
//  Copyright © 2016 Codeon GmbH.
//  All rights reserved.
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
   { { MULLE_CONCURRENT_NO_HASH, NULL } }
};


#define REDIRECT_VALUE   MULLE_CONCURRENT_INVALID_POINTER

#pragma mark - _mulle_concurrent_hashmapstorage


// n must be a power of 2
static struct _mulle_concurrent_hashmapstorage *
   _mulle_concurrent_alloc_hashmapstorage( unsigned int n,
                                           struct mulle_allocator *allocator)
{
   struct _mulle_concurrent_hashmapstorage  *p;

   assert( (~(n - 1) & n) == n);

   if( n < 4)
      n = 4;

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
         q->hash  = MULLE_CONCURRENT_NO_HASH;
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

   size = (unsigned int) p->mask + 1;
   max  = size - (size >> 1);
   return( max);
}


static void   *_mulle_concurrent_hashmapstorage_lookup( struct _mulle_concurrent_hashmapstorage *p,
                                                        intptr_t hash)
{
   struct _mulle_concurrent_hashvaluepair   *entry;
   unsigned int                             index;
#ifndef NDEBUG
   unsigned int                             sentinel;

   sentinel = (unsigned int) hash + (unsigned int) p->mask + 1;
#endif
   index    = (unsigned int) hash;

   for(;;)
   {
      entry = &p->entries[ index & (unsigned int) p->mask];

      if( entry->hash == MULLE_CONCURRENT_NO_HASH)
         return( MULLE_CONCURRENT_NO_POINTER);

      if( entry->hash == hash)
         return( _mulle_atomic_pointer_read( &entry->value));

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
      if( entry->hash == MULLE_CONCURRENT_NO_HASH)
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

      if( entry->hash == MULLE_CONCURRENT_NO_HASH || entry->hash == hash)
      {
         found = __mulle_atomic_pointer_cas( &entry->value, value, MULLE_CONCURRENT_NO_POINTER);
         if( found != MULLE_CONCURRENT_NO_POINTER)
         {
            if( found == REDIRECT_VALUE)
               return( MULLE_CONCURRENT_INVALID_POINTER);  // EBUSY
            return( found);
         }

         if( ! entry->hash)
         {
            _mulle_atomic_pointer_increment( &p->n_hashs);
            entry->hash = hash;
         }

         return( found); // MULLE_CONCURRENT_NO_POINTER
      }

      ++index;
      assert( index != sentinel);  // can't happen we always leave space
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

      if( entry->hash == MULLE_CONCURRENT_NO_HASH || entry->hash == hash)
      {
         found = __mulle_atomic_pointer_cas( &entry->value, value, MULLE_CONCURRENT_NO_POINTER);
         if( found != MULLE_CONCURRENT_NO_POINTER)
         {
            if( found == REDIRECT_VALUE)
               return( EBUSY);
            return( EEXIST);
         }

         if( ! entry->hash)
         {
            _mulle_atomic_pointer_increment( &p->n_hashs);
            entry->hash = hash;
         }

         return( 0);
      }

      ++index;
      assert( index != sentinel);  // can't happen we always leave space
   }
}


static int   _mulle_concurrent_hashmapstorage_put( struct _mulle_concurrent_hashmapstorage *p,
                                                   intptr_t hash,
                                                   void *value)
{
   struct _mulle_concurrent_hashvaluepair   *entry;
   void                                     *found;
   void                                     *expect;
   unsigned int                             index;
#ifndef NDEBUG
   unsigned int                             sentinel;

   sentinel = (unsigned int) hash + (unsigned int) p->mask + 1;
#endif

   assert( value);

   index = (unsigned int) hash;

   for(;;)
   {
      entry = &p->entries[ index & (unsigned int) p->mask];

      if( entry->hash == hash)
      {
         expect = MULLE_CONCURRENT_NO_POINTER;
         for(;;)
         {
            found = __mulle_atomic_pointer_cas( &entry->value, value, expect);
            if( found == expect)
               return( 0);
            if( found == REDIRECT_VALUE)
               return( EBUSY);
            expect = found;
         }
      }

      if( entry->hash == MULLE_CONCURRENT_NO_HASH)
      {
         found = __mulle_atomic_pointer_cas( &entry->value, value, MULLE_CONCURRENT_NO_POINTER);
         if( found != MULLE_CONCURRENT_NO_POINTER)
         {
            if( found == REDIRECT_VALUE)
               return( EBUSY);
            return( EEXIST);
         }

         _mulle_atomic_pointer_increment( &p->n_hashs);
         entry->hash = hash;

         return( 0);
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
   void                                     *found;
   unsigned int                             index;
#ifndef NDEBUG
   unsigned int                             sentinel;

   sentinel = (unsigned int) hash + (unsigned int) p->mask + 1;
#endif

   index = (unsigned int) hash;
   for(;;)
   {
      entry  = &p->entries[ index & (unsigned int) p->mask];

      if( entry->hash == hash)
      {
         found = __mulle_atomic_pointer_cas( &entry->value, MULLE_CONCURRENT_NO_POINTER, value);
         if( found == REDIRECT_VALUE)
            return( EBUSY);
         return( found == value ? 0 : ENOENT);
      }

      if( entry->hash == MULLE_CONCURRENT_NO_HASH)
         return( ENOENT);

      ++index;
      assert( index != sentinel);  // can't happen we always leave space
   }
}


static void
   _mulle_concurrent_hashmapstorage_copy( struct _mulle_concurrent_hashmapstorage *dst,
                                          struct _mulle_concurrent_hashmapstorage *src)
{
   struct _mulle_concurrent_hashvaluepair   *p;
   struct _mulle_concurrent_hashvaluepair   *p_last;
   void                                     *actual;
   void                                     *value;

   p      = src->entries;
   p_last = &src->entries[ src->mask];

   for( ;p <= p_last; p++)
   {
      if( ! p->hash)
         continue;

      value = _mulle_atomic_pointer_read( &p->value);
      for(;;)
      {
         if( value == MULLE_CONCURRENT_NO_POINTER)
            break;
         if( value == REDIRECT_VALUE)
            break;

         // it's important that we copy over first so
         // No One Gets Left Behind
         _mulle_concurrent_hashmapstorage_put( dst, p->hash, value);

         actual = __mulle_atomic_pointer_cas( &p->value, REDIRECT_VALUE, value);
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

   assert( allocator->abafree && allocator->abafree != (int (*)()) abort);

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

   p = _mulle_atomic_pointer_read( &map->storage.pointer);
   return( (unsigned int) p->mask + 1);
}


static int  _mulle_concurrent_hashmap_migrate_storage( struct mulle_concurrent_hashmap *map,
                                                       struct _mulle_concurrent_hashmapstorage *p)
{

   struct _mulle_concurrent_hashmapstorage   *q;
   struct _mulle_concurrent_hashmapstorage   *alloced;
   struct _mulle_concurrent_hashmapstorage   *previous;
   struct mulle_allocator                    *allocator;

   assert( p);

   allocator  = _mulle_atomic_pointer_read( &map->allocator);

   // check if we have a chance to succeed
   alloced = NULL;
   q       = _mulle_atomic_pointer_read( &map->next_storage.pointer);
   if( q == p)
   {
      // acquire new storage
      alloced = _mulle_concurrent_alloc_hashmapstorage( ((unsigned int) p->mask + 1) * 2,
                                                        allocator);
      if( ! alloced)
         return( ENOMEM);

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

   // this thread can partake in copying
   _mulle_concurrent_hashmapstorage_copy( q, p);

   // now update world, giving it the same value as 'next_world'
   previous = __mulle_atomic_pointer_cas( &map->storage.pointer, q, p);

   // ok, if we succeed free old, if we fail alloced is
   // already gone. this must be an ABA free
   if( previous == p && ! _mulle_concurrent_hashmapstorage_is_const( previous))
      _mulle_allocator_abafree( allocator, previous); // ABA!!

   return( 0);
}


void  *_mulle_concurrent_hashmap_lookup( struct mulle_concurrent_hashmap *map,
                                         intptr_t hash)
{
   struct _mulle_concurrent_hashmapstorage   *p;
   void                                      *value;

   // won't find invalid hash anyway
retry:
   p     = _mulle_atomic_pointer_read( &map->storage.pointer);
   value = _mulle_concurrent_hashmapstorage_lookup( p, hash);
   if( value == REDIRECT_VALUE)
   {
      if( _mulle_concurrent_hashmap_migrate_storage( map, p))
         return( (void *) MULLE_CONCURRENT_NO_POINTER);
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
      if( value == REDIRECT_VALUE)
      {
         if( _mulle_concurrent_hashmap_migrate_storage( map, p))
            return( ENOMEM);
         goto retry;
      }

      if( value != MULLE_CONCURRENT_NO_POINTER)
         break;
   }

   if( p_hash)
      *p_hash = entry->hash;
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
      if( _mulle_concurrent_hashmap_migrate_storage( map, p))
      {
         errno = ENOMEM;
         return( MULLE_CONCURRENT_INVALID_POINTER);
      }
      goto retry;
   }

   result = _mulle_concurrent_hashmapstorage_register( p, hash, value);
   if( result == MULLE_CONCURRENT_INVALID_POINTER)
   {
      if( _mulle_concurrent_hashmap_migrate_storage( map, p))
      {
         errno = ENOMEM;
         return( MULLE_CONCURRENT_INVALID_POINTER);
      }
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
   p = _mulle_atomic_pointer_read( &map->storage.pointer);
   assert( p);

   max = _mulle_concurrent_hashmapstorage_get_max_n_hashs( p);
   n   = (unsigned int) (uintptr_t) _mulle_atomic_pointer_read( &p->n_hashs);

   if( n >= max)
   {
      if( _mulle_concurrent_hashmap_migrate_storage( map, p))
         return( ENOMEM);
      goto retry;
   }

   rval = _mulle_concurrent_hashmapstorage_insert( p, hash, value);
   if( rval == EBUSY)
   {
      if( _mulle_concurrent_hashmap_migrate_storage( map, p))
         return( ENOMEM);
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


#pragma mark - remove


int  _mulle_concurrent_hashmap_remove( struct mulle_concurrent_hashmap *map,
                                       intptr_t hash,
                                       void *value)
{
   struct _mulle_concurrent_hashmapstorage   *p;
   int                                       rval;

   assert_hash_value( hash, value);

retry:
   p    = _mulle_atomic_pointer_read( &map->storage.pointer);
   rval = _mulle_concurrent_hashmapstorage_remove( p, hash, value);
   if( rval == EBUSY)
   {
      if( _mulle_concurrent_hashmap_migrate_storage( map, p))
         return( ENOMEM);
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
   if( value == MULLE_CONCURRENT_NO_POINTER || value == MULLE_CONCURRENT_INVALID_POINTER)
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

   rval = _mulle_concurrent_hashmap_search_next( rover->map, &rover->mask, &rover->index, &hash, &value);

   if( rval != 1)
      return( rval);

   if( p_hash)
      *p_hash = hash;
   if( p_value)
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
