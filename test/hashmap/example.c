#include <mulle-concurrent/mulle-concurrent.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>


static void   fail( char *s)
{
   perror( s);
   exit( 1);
}


int   main(int argc, const char * argv[])
{
   struct mulle_concurrent_hashmap             map;
   struct mulle_concurrent_hashmapenumerator   rover;
   int                                         rval;
   void                                        *value;
   void                                        *other;
   intptr_t                                    hash;

   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_hashmap_init( &map, 0, NULL);

   {
      rval = mulle_concurrent_hashmap_insert( &map, 0x1, (void *) 1848);
      if( rval)
         fail( "mulle_concurrent_hashmap_insert");
      rval = mulle_concurrent_hashmap_insert( &map, 0x2, (void *) 1849);
      if( rval)
         fail( "mulle_concurrent_hashmap_insert");

      value = mulle_concurrent_hashmap_lookup( &map, 0x2);
      if( value != (void *) 1849)
      {
         errno = ENOENT;
         fail( "mulle_concurrent_hashmap_lookup");
      }

      // remove wrong value, should not work
      rval = mulle_concurrent_hashmap_remove( &map, 0x2, (void *) 1000);
      if( rval != ENOENT)
         fail( "mulle_concurrent_hashmap_remove");

      rval = mulle_concurrent_hashmap_remove( &map, 0x2, value);
      if( rval)
      {
         errno = ENOENT;
         fail( "mulle_concurrent_hashmap_remove");
      }

retry:
      rover = mulle_concurrent_hashmap_enumerate( &map);
      while( (rval = mulle_concurrent_hashmapenumerator_next( &rover, &hash, &value)) == 1)
         printf( "%lu : %p\n", (unsigned long) hash, value);
      mulle_concurrent_hashmapenumerator_done( &rover);

      switch( rval)
      {
      case 0:
         break;

      case EBUSY:
         goto retry;

      default :
         fail( "mulle_concurrent_hashmapenumerator_next");
      }

   }

   mulle_concurrent_hashmap_done( &map);

   mulle_aba_unregister();
   mulle_aba_done();

   return( 0);
}
