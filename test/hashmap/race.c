// Race probe: one thread owns a single key and does remove/register/pose
// on it while other threads hammer inserts to force constant migration.
// If the remove-vs-migration interaction loses or resurrects a value, the
// checks below catch it.
#define HAVE_MULLE_CONCURRENT_POSEAS_PATCH
#include <mulle-concurrent/mulle-concurrent.h>

#include <mulle-testallocator/mulle-testallocator.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>


#define N_MIGRATORS   3
#define N_ITERS       200000

static struct mulle_concurrent_hashmap   g_map;
static volatile int                      g_stop;
static intptr_t                          g_key     = 777777;
static void                              *g_value   = (void *) 0x1234;
static void                              *g_posed = (void *) 0x5678;

static int   g_failed;


// only the owner thread calls fail(); stop the migrators so the test
// terminates promptly instead of spinning until the harness timeout
static void   fail( char *format, ...)
{
   va_list   args;

   g_stop   = 1;
   g_failed = 1;
   printf( "FAILED: " );
   va_start( args, format);
   vprintf( format, args);
   va_end( args);
   printf( "\n");
}


static void  *migrator( void *unused)
{
   intptr_t   i;
   intptr_t   hash;

   mulle_aba_register();

   for( i = 0; ! g_stop; i++)
   {
      hash = (intptr_t)((i * 2654435761u) % 1000000) + 1000000;
      mulle_concurrent_hashmap_insert( &g_map, hash, (void *)(uintptr_t)(hash * 2 + 1));
   }

   mulle_aba_unregister();
   return( NULL);
}


static void  *owner( void *unused)
{
   int      i;
   int      rval;
   void     *result;

   mulle_aba_register();

   for( i = 0; i < N_ITERS; i++)
   {
      // remove the key (expect the current value: posed after a pose)
      rval = mulle_concurrent_hashmap_remove( &g_map, g_key, g_posed);
      if( rval != 0 && rval != ENOENT)
      {
         fail( "remove rval %d", rval);
         return( (void *) 1);
      }

      // A tombstone can deny reuse until migration drops it. The migrator
      // threads keep advancing generations, so retry on the next iteration.
      errno  = 0;
      result = mulle_concurrent_hashmap_register( &g_map, g_key, g_value);
      if( result == MULLE_CONCURRENT_INVALID_POINTER && errno == EEXIST)
         continue;
      if( result != MULLE_CONCURRENT_NO_POINTER && result != g_value)
      {
         fail( "register stale at %d: returned %p (not NO_POINTER or %p)",
               i, result, g_value);
         return( (void *) 1);
      }

      // pose the value
      rval = _mulle_concurrent_hashmap_pose( &g_map, g_key, g_posed, g_value);
      if( rval != 0)
      {
         fail( "pose rval %d at %d", rval, i);
         return( (void *) 1);
      }

      // register again: must return the posed value
      result = mulle_concurrent_hashmap_register( &g_map, g_key, g_value);
      if( result != g_posed)
      {
         fail( "register stale at %d: returned %p, expected posed %p",
               i, result, g_posed);
         return( (void *) 1);
      }
   }

   g_stop = 1;

   mulle_aba_unregister();
   return( NULL);
}


int   main( void)
{
   mulle_thread_t   threads[ N_MIGRATORS + 1];
   unsigned int     i;

   mulle_testallocator_initialize();
   mulle_default_allocator = mulle_testallocator;
   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_hashmap_init( &g_map, 4, NULL);
   // seed the key with the posed value so remove() sees it
   mulle_concurrent_hashmap_insert( &g_map, g_key, g_posed);

   for( i = 0; i < N_MIGRATORS; i++)
   {
      if( mulle_thread_create( (void *) migrator, NULL, &threads[ i]))
      {
         perror( "create migrator" );
         return( 1);
      }
   }
   if( mulle_thread_create( (void *) owner, NULL, &threads[ N_MIGRATORS]))
   {
      perror( "create owner" );
      return( 1);
   }

   for( i = 0; i < N_MIGRATORS + 1; i++)
      mulle_thread_join( threads[ i]);

   mulle_concurrent_hashmap_done( &g_map);
   mulle_aba_unregister();
   mulle_aba_done();
   mulle_testallocator_reset();

   printf( g_failed ? "FAILED\n" : "PASSED\n");
   return( g_failed ? 1 : 0);
}
