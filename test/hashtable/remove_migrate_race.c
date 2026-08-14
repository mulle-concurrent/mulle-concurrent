//
// S6 from dox/HASHTABLE.md: remove racing the freeze.
//
// remove_race.c relies on threshold-driven migrations, which become rare once
// the table is large, so the window almost never coincides with a removal. Here
// a probe thread calls _mulle_concurrent_hashtable_migrate_same_size() in a tight
// loop instead, so generations change constantly at bounded memory cost.
//
// The owner thread owns its key exclusively. Every insert must succeed and every
// remove of a value it just inserted must return 0. If remove reports ENOENT
// because a copier carried and consumed the value first, the removal was lost.
//
#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-testallocator/mulle-testallocator.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>


#define N_ITERS      100000
#define OWNED_KEY    4242
#define OTHER_KEYS   64

static struct mulle_concurrent_hashtable   g_map;
static volatile int                       g_stop;
static int                                g_failed;
static void                               *g_value = (void *) 0xBEEF;


static void  *migrator( void *unused)
{
   intptr_t   i;

   MULLE_C_UNUSED( unused);

   mulle_aba_register();

   for( i = 0; ! g_stop; i++)
   {
      _mulle_concurrent_hashtable_migrate_same_size( &g_map);

      if( (i & 0x3F) == 0)
         mulle_aba_checkin();
   }

   mulle_aba_unregister();
   return( NULL);
}


static void  *owner( void *unused)
{
   int    i;
   int    rval;
   void   *found;

   MULLE_C_UNUSED( unused);

   mulle_aba_register();

   for( i = 0; i < N_ITERS && ! g_failed; i++)
   {
      rval = mulle_concurrent_hashtable_insert( &g_map, OWNED_KEY, g_value);
      if( rval != 0)
      {
         printf( "FAILED: insert returned %d at %d\n", rval, i);
         g_failed = 1;
         break;
      }

      // we own this key, so this must find it
      found = mulle_concurrent_hashtable_lookup( &g_map, OWNED_KEY);
      if( found != g_value)
      {
         printf( "FAILED: lookup lost a live value at %d\n", i);
         g_failed = 1;
         break;
      }

      // and this must remove it, not report it missing
      rval = mulle_concurrent_hashtable_remove( &g_map, OWNED_KEY, g_value);
      if( rval != 0)
      {
         printf( "FAILED: remove returned %d at %d (lost removal)\n", rval, i);
         g_failed = 1;
         break;
      }

      found = mulle_concurrent_hashtable_lookup( &g_map, OWNED_KEY);
      if( found != NULL)
      {
         printf( "FAILED: removed value resurrected at %d\n", i);
         g_failed = 1;
         break;
      }

      if( (i & 0xFF) == 0)
         mulle_aba_checkin();
   }

   g_stop = 1;

   mulle_aba_unregister();
   return( NULL);
}


int   main( void)
{
   mulle_thread_t   threads[ 2];
   intptr_t         i;

   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_hashtable_init( &g_map, 256, NULL);

   // some unrelated live entries, so copy has real work to do on every pass
   for( i = 0; i < OTHER_KEYS; i++)
      mulle_concurrent_hashtable_insert( &g_map, i + 1000, (void *)(uintptr_t)(i + 1));

   if( mulle_thread_create( (void *) migrator, NULL, &threads[ 0]))
   {
      perror( "create migrator" );
      return( 1);
   }
   if( mulle_thread_create( (void *) owner, NULL, &threads[ 1]))
   {
      perror( "create owner" );
      return( 1);
   }

   mulle_thread_join( threads[ 0]);
   mulle_thread_join( threads[ 1]);

   // the unrelated entries must have survived every migration
   for( i = 0; i < OTHER_KEYS; i++)
      if( mulle_concurrent_hashtable_lookup( &g_map, i + 1000) != (void *)(uintptr_t)(i + 1))
      {
         printf( "FAILED: bystander key %ld lost\n", (long) i);
         g_failed = 1;
         break;
      }

   mulle_concurrent_hashtable_done( &g_map);
   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   printf( g_failed ? "FAILED\n" : "PASSED\n");
   return( g_failed ? 1 : 0);
}
