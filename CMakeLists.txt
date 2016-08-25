cmake_minimum_required (VERSION 3.0)

project (mulle-concurrent)

cmake_policy(SET CMP0054 NEW)

set( CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/mulle-configuration)
message(STATUS "CMAKE_MODULE_PATH is ${CMAKE_MODULE_PATH}")
message(STATUS "CMAKE_BUILD_TYPE is ${CMAKE_BUILD_TYPE}")
if( "${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
  include( Debug)
else()
  include( Release)
endif()

# using include_directories is a little bit shitty
include_directories( src
src/hashmap
src/pointerarray
)

set( HEADERS
src/mulle_concurrent.h
src/mulle_concurrent_types.h
src/pointerarray/mulle_concurrent_pointerarray.h
src/hashmap/mulle_concurrent_hashmap.h
)

add_library( mulle_concurrent
src/pointerarray/mulle_concurrent_pointerarray.c
src/hashmap/mulle_concurrent_hashmap.c
)

add_library( mulle_concurrent_standalone SHARED
src/mulle_concurrent_standalone.c
)

add_dependencies( mulle_concurrent_standalone mulle_concurrent)

TARGET_LINK_LIBRARIES( mulle_concurrent_standalone
${BEGIN_ALL_LOAD}
mulle_concurrent
mulle_aba
mulle_allocator
mulle_test_allocator
${END_ALL_LOAD}
)



INSTALL( TARGETS mulle_concurrent_standalone mulle_concurrent DESTINATION "lib")
INSTALL( FILES ${HEADERS} DESTINATION "include/mulle_concurrent")


