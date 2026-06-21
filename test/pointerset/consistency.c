//
// Consistency test: run N threads briefly, stop them, check for duplicates,
// restart. Repeated N_ROUNDS times on the same set.
//
// "Consistent" means: enumerating the set yields each live pointer at most once.
//
#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-testallocator/mulle-testallocator.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define N_POINTERS   10
#define N_THREADS    8
#define N_ROUNDS     100
#define ITERS_PER_BURST  5000

static void  *ptrs[ N_POINTERS];

static mulle_atomic_pointer_t   stop_flag;


static uint64_t   xorshift64star( uint64_t *x)
{
   *x ^= *x >> 12;
   *x ^= *x << 25;
   *x ^= *x >> 27;
   return *x * 0x2545F4914F6CDD1DULL;
}


static void  worker( struct mulle_concurrent_pointerset *set)
{
   uint64_t   rng;
   void       *ptr;

   mulle_aba_register();

   rng = (uint64_t)(uintptr_t) &rng ^ 0xdeadbeefcafe1234ULL;
   if( ! rng) rng = 1;

   while( ! _mulle_atomic_pointer_read( &stop_flag))
   {
      ptr = ptrs[ xorshift64star( &rng) % N_POINTERS ];
      switch( xorshift64star( &rng) % 2)
      {
         case 0: mulle_concurrent_pointerset_insert( set, ptr); break;
         case 1: mulle_concurrent_pointerset_remove( set, ptr); break;
      }
   }

   mulle_aba_unregister();
}


static void  check_consistency( struct mulle_concurrent_pointerset *set)
{
   // enumerate and verify no pointer appears more than once
   int                                            seen[ N_POINTERS];
   struct mulle_concurrent_pointerset_enumerator  rover;
   void                                           *ptr;
   unsigned int                                   i;
   int                                            rval;

   memset( seen, 0, sizeof( seen));

retry:
   memset( seen, 0, sizeof( seen));
   rover = mulle_concurrent_pointerset_enumerate( set);
   while( (rval = mulle_concurrent_pointerset_enumerator_next( &rover, &ptr)) == 1)
   {
      for( i = 0; i < N_POINTERS; i++)
      {
         if( ptrs[ i] != ptr)
            continue;
         if( seen[ i])
         {
            fprintf( stderr, "DUPLICATE pointer %p found in set!\n", ptr);
            exit( 1);
         }
         seen[ i] = 1;
         break;
      }
      if( i == N_POINTERS)
      {
         fprintf( stderr, "UNKNOWN pointer %p found in set!\n", ptr);
         exit( 1);
      }
   }
   mulle_concurrent_pointerset_enumerator_done( &rover);

   if( rval == ECANCELED)
      goto retry;
}


int   main( void)
{
   struct mulle_concurrent_pointerset   set;
   mulle_thread_t                       threads[ N_THREADS];
   unsigned int                         i;
   unsigned int                         round;

   for( i = 0; i < N_POINTERS; i++)
      ptrs[ i] = (void *)(uintptr_t)(i + 1);

   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;

   mulle_aba_init( &mulle_testallocator);
   mulle_allocator_set_aba( &mulle_testallocator,
                            mulle_aba_get_global(),
                            (mulle_allocator_aba_t *) _mulle_aba_free_owned_pointer);
   mulle_aba_register();

   mulle_concurrent_pointerset_init( &set, 4, &mulle_testallocator);

   for( round = 0; round < N_ROUNDS; round++)
   {
      _mulle_atomic_pointer_nonatomic_write( &stop_flag, NULL);

      for( i = 0; i < N_THREADS; i++)
      {
         if( mulle_thread_create( (void *) worker, &set, &threads[ i]))
         {
            perror( "mulle_thread_create");
            abort();
         }
      }

      // let threads run briefly
      mulle_thread_yield();
      for( i = 0; i < ITERS_PER_BURST; i++)
         mulle_thread_yield();

      // stop threads
      __mulle_atomic_pointer_cas( &stop_flag, (void *) 1, NULL);

      for( i = 0; i < N_THREADS; i++)
         mulle_thread_join( threads[ i]);

      // now single-threaded: check consistency
      check_consistency( &set);
   }

   mulle_concurrent_pointerset_done( &set);

   mulle_aba_unregister();
   mulle_allocator_set_aba( &mulle_testallocator, NULL, NULL);
   mulle_aba_done();

   mulle_testallocator_reset();

   printf( "OK\n");
   return( 0);
}
