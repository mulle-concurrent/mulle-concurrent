//
// Stress test: N threads hammering insert/remove/member on pointers 1..10.
// The set is tiny and heavily contended, forcing lots of CAS retries and
// migrations.
//
#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-testallocator/mulle-testallocator.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>


#define N_POINTERS   10
#define N_ITERS      100000

// pointers we store: (void*)1 .. (void*)10
static void  *ptrs[ N_POINTERS];


/* xorshift64star for cheap per-thread randomness */
static uint64_t   xorshift64star( uint64_t *x)
{
   *x ^= *x >> 12;
   *x ^= *x << 25;
   *x ^= *x >> 27;
   return *x * 0x2545F4914F6CDD1DULL;
}


static void  tester( struct mulle_concurrent_pointerset *set)
{
   uint64_t   rng;
   unsigned   i;
   void       *ptr;
   int        rval;

   mulle_aba_register();

   // seed with thread stack address for uniqueness
   rng = (uint64_t)(uintptr_t) &rng ^ 0xdeadbeefcafe1234ULL;
   if( ! rng) rng = 1;

   for( i = 0; i < N_ITERS; i++)
   {
      ptr = ptrs[ xorshift64star( &rng) % N_POINTERS ];

      switch( xorshift64star( &rng) % 3)
      {
         case 0:
            mulle_concurrent_pointerset_insert( set, ptr);
            break;

         case 1:
            mulle_concurrent_pointerset_remove( set, ptr);
            break;

         case 2:
            mulle_concurrent_pointerset_member( set, ptr);
            break;
      }
   }

   mulle_aba_unregister();
}


static void  multi_threaded_test( unsigned int n_threads)
{
   struct mulle_concurrent_pointerset   set;
   mulle_thread_t                       threads[ 32];
   unsigned int                         i;

   assert( n_threads <= 32);

   mulle_aba_init( &mulle_testallocator);
   mulle_allocator_set_aba( &mulle_testallocator,
                            mulle_aba_get_global(),
                            (mulle_allocator_aba_t *) _mulle_aba_free_owned_pointer);
   mulle_aba_register();

   mulle_concurrent_pointerset_init( &set, 4, &mulle_testallocator);

   for( i = 0; i < n_threads; i++)
   {
      if( mulle_thread_create( (void *) tester, &set, &threads[ i]))
      {
         perror( "mulle_thread_create");
         abort();
      }
   }
   for( i = 0; i < n_threads; i++)
      mulle_thread_join( threads[ i]);

   mulle_concurrent_pointerset_done( &set);

   mulle_aba_unregister();
   mulle_allocator_set_aba( &mulle_testallocator, NULL, NULL);
   mulle_aba_done();
}


int   main( void)
{
   unsigned int   i;

   for( i = 0; i < N_POINTERS; i++)
      ptrs[ i] = (void *)(uintptr_t)(i + 1);

   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;

   multi_threaded_test( 1);
   mulle_testallocator_reset();

   multi_threaded_test( 2);
   mulle_testallocator_reset();

   multi_threaded_test( 8);
   mulle_testallocator_reset();

   multi_threaded_test( 32);
   mulle_testallocator_reset();

   printf( "OK\n");
   return( 0);
}
