//
//  mulle-concurrent-math.h
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
#ifndef mulle_concurrent_math_h__
#define mulle_concurrent_math_h__

#include <stddef.h>
#include <stdint.h>


//
// Bit twiddling, same idea as mulle-container-math.h. It is kept separate and
// namespaced here so it cannot collide if both headers ever meet in one TU.
//
// `0` counts as a power of two for these predicates.
//
static inline int   _mulle_concurrent_is_pow2( size_t x)
{
   return( (x & (~x + 1)) == x);
}


//
// Rounds up to the next power of two, `0` promotes to `1`.
//
// The caller guards against overflow: above the largest representable power
// of two the bit trick wraps to 1. The storage allocators route that through
// the allocator's fail vector.
//
static inline uint32_t   _mulle_concurrent_pow2roundup_32( uint32_t v)
{
   v--;
   v |= v >> 1;
   v |= v >> 2;
   v |= v >> 4;
   v |= v >> 8;
   v |= v >> 16;
   v++;

   return( v ? v : 1);
}


static inline uint64_t   _mulle_concurrent_pow2roundup_64( uint64_t v)
{
   v--;
   v |= v >> 1;
   v |= v >> 2;
   v |= v >> 4;
   v |= v >> 8;
   v |= v >> 16;
   v |= v >> 32;
   v++;

   return( v ? v : 1);
}


static inline size_t   _mulle_concurrent_pow2roundup( size_t v)
{
   if( sizeof( size_t) >= sizeof( uint64_t))
      return( (size_t) _mulle_concurrent_pow2roundup_64( (uint64_t) v));
   return( (size_t) _mulle_concurrent_pow2roundup_32( (uint32_t) v));
}


#endif
