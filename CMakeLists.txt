cmake_minimum_required( VERSION 3.0)

project( mulle-concurrent C)

set( CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/mulle-configuration/cmake)
message( STATUS "CMAKE_MODULE_PATH is ${CMAKE_MODULE_PATH}")
message( STATUS "CMAKE_BUILD_TYPE is ${CMAKE_BUILD_TYPE}")
if( "${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
  include( Debug)
else()
  include( Release)
endif()


set( MULLE_CONCURRENT_LIBRARY ON)
include( CMakeSourcesAndHeaders.txt)
include( CMakeDependencies.txt)


set( PUBLIC_HEADERS
${HEADERS}
CMakeDependencies.txt
)

set( DEPENDENCY_LIBRARIES
${MULLE_ABA_LIBRARY}
${MULLE_ALLOCATOR_LIBRARY}
${MULLE_TEST_ALLOCATOR_LIBRARY}
${MULLE_THREAD_LIBRARY}
)

###
#
#
add_library( mulle_concurrent
${SOURCES}
)

set( STANDALONE_BASE_NAME "mulle_concurrent")
set( STANDALONE_NAME "mulle_concurrent_standalone")
set( STANDALONE_DEFINITIONS ${MULLE_CONCURRENT_DEFINITIONS})
set( STANDALONE_ALL_LOAD_LIBRARIES
$<TARGET_FILE:mulle_concurrent>
${DEPENDENCY_LIBRARIES}
)


include( Standalone)


INSTALL( TARGETS mulle_concurrent_standalone mulle_concurrent DESTINATION "lib")
INSTALL( FILES ${PUBLIC_HEADERS} DESTINATION "include/mulle_concurrent")

include( MulleTest)
