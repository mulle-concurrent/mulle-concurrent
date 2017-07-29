cmake_minimum_required (VERSION 3.0)

project(mulle-concurrent C)

cmake_policy(SET CMP0054 NEW)

set( CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/mulle-configuration/cmake)
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

find_library( MULLE_ABA_LIBRARY            NAMES mulle_aba)
find_library( MULLE_ALLOCATOR_LIBRARY      NAMES mulle_allocator)
find_library( MULLE_TEST_ALLOCATOR_LIBRARY NAMES mulle_test_allocator)
find_library( MULLE_THREAD_LIBRARY         NAMES mulle_thread)

message(STATUS "MULLE_ABA_LIBRARY is ${MULLE_ABA_LIBRARY}")
message(STATUS "MULLE_ALLOCATOR_LIBRARY is ${MULLE_ALLOCATOR_LIBRARY}")
message(STATUS "MULLE_TEST_ALLOCATOR_LIBRARY is ${MULLE_TEST_ALLOCATOR_LIBRARY}")
message(STATUS "MULLE_THREAD_LIBRARY is ${MULLE_THREAD_LIBRARY}")

set( DEPENDENCY_LIBRARIES
${MULLE_ABA_LIBRARY}
${MULLE_ALLOCATOR_LIBRARY}
${MULLE_TEST_ALLOCATOR_LIBRARY}
${MULLE_THREAD_LIBRARY}
)

set( TARGET_ALL_LOAD_LIBRARIES
$<TARGET_FILE:mulle_concurrent>
${DEPENDENCY_LIBRARIES}
)

if( FORCE_LOAD)
   set( TMP)
   foreach( library ${TARGET_ALL_LOAD_LIBRARIES})
      list( APPEND TMP "${FORCE_LOAD} ${library}")
   endforeach( library)
   set( TARGET_ALL_LOAD_LIBRARIES ${TMP})
endif()


TARGET_LINK_LIBRARIES( mulle_concurrent_standalone
${BEGIN_ALL_LOAD}
${TARGET_ALL_LOAD_LIBRARIES}
${END_ALL_LOAD}
)

INSTALL( TARGETS mulle_concurrent_standalone mulle_concurrent DESTINATION "lib")
INSTALL( FILES ${HEADERS} DESTINATION "include/mulle_concurrent")
