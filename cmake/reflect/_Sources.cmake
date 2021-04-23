#
# This file will be included by cmake/share/sources.cmake
#
# cmake/reflect/_Sources.cmake is generated by `mulle-sde reflect`.
# Edits will be lost.
#
if( MULLE_TRACE_INCLUDE)
   MESSAGE( STATUS "# Include \"${CMAKE_CURRENT_LIST_FILE}\"" )
endif()

#
# contents selected with patternfile ??-source--sources
#
set( SOURCES
src/hashmap/mulle-concurrent-hashmap.c
src/pointerarray/mulle-concurrent-pointerarray.c
)

#
# contents selected with patternfile ??-source--standalone-sources
#
set( STANDALONE_SOURCES
src/mulle-concurrent-standalone.c
)
