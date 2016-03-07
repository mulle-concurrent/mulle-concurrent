cmake_minimum_required (VERSION 3.0)

project (mulle-concurrent)


# using include_directories is a little bit shitty
include_directories( src
src/hash
)

include_directories( SYSTEM
dependencies/include
)

#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -g -DDEBUG ")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -g -DNDEBUG")

set( HEADERS
src/pointerarray/mulle_concurrent_pointerarray.h
src/hashmap/mulle_concurrent_hashmap.h
)

add_library( mulle_concurrent
src/pointerarray/mulle_concurrent_pointerarray.c
src/hashmap/mulle_concurrent_hashmap.c
)

INSTALL( TARGETS mulle_concurrent DESTINATION "lib")
INSTALL( FILES ${HEADERS} DESTINATION "include/mulle_concurrent")


