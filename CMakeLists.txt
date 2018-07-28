cmake_minimum_required( VERSION 3.1)

project( mulle-concurrent C)


### mulle-sde environment

# add cmake module paths to search path
list( INSERT CMAKE_MODULE_PATH 0 "${PROJECT_SOURCE_DIR}/cmake/share")
list( INSERT CMAKE_MODULE_PATH 0 "${PROJECT_SOURCE_DIR}/cmake")

include( Environment)


### Files

include( PreFiles OPTIONAL)

set( MULLE_CONCURRENT_LIBRARY ON)

#
# Include all non underscored .cmake files in ./cmake
#
file( GLOB LOCAL_CMAKE_INCLUDES
   "cmake/[A-Za-z0-9]*.cmake"
)

foreach( FILE ${LOCAL_CMAKE_INCLUDES} )
   include( "${FILE}")
endforeach()


include_directories( ${INCLUDE_DIRS})

include( PostFiles OPTIONAL)

set( PROJECT_FILES
   ${PROJECT_FILES}
   ${SOURCES}
   ${PUBLIC_HEADERS}
   ${PRIVATE_HEADERS}
   ${CMAKE_EDITABLE_FILES}
)

### Library

include( PreLibrary OPTIONAL)

# ##################################
# Mainly of interest for Objective-C
# ##################################
# Libraries are built in two stages:
#
# In the first step the PROJECT_FILES are compiled.
# In the second step STAGE2_SOURCES are added.
# This allows PostLibrary to run an analysis step over PROJECT_FILES and
# generate files to be included by STAGE2_SOURCES. If there are no
# STAGE2_SOURCES then this is just a more verbose way of doing it.
# OBJC_LOADER_INC is the generated analysis step.
#
#
add_library( "_1_mulle-concurrent" OBJECT
   ${PROJECT_FILES}
)

set( ALL_OBJECT_FILES
   $<TARGET_OBJECTS:_1_mulle-concurrent>
)

set_property( TARGET "_1_mulle-concurrent" PROPERTY CXX_STANDARD 11)

if( STAGE2_SOURCES)
   add_library( "_2_mulle-concurrent" OBJECT
      ${STAGE2_SOURCES}
      ${STAGE2_HEADERS}
   )
   set( ALL_OBJECT_FILES
      ${ALL_OBJECT_FILES}
      $<TARGET_OBJECTS:_2_mulle-concurrent>
   )
   set_property( TARGET "_2_mulle-concurrent" PROPERTY CXX_STANDARD 11)
else()
   if( STAGE2_HEADERS)
      message( FATAL_ERROR "No STAGE2_SOURCES found but STAGE2_HEADERS exist")
   endif()
endif()


add_library( "mulle-concurrent"
   ${ALL_OBJECT_FILES}
)


include( Library OPTIONAL)


set( INSTALL_LIBRARY_TARGETS
   "mulle-concurrent"
   ${INSTALL_LIBRARY_TARGETS}
)

include( PostLibrary OPTIONAL)


#
# defined by cmake -DBUILD_SHARED_LIBS=ON
#
if( BUILD_SHARED_LIBS)
   set( SHARED_LIBRARY_LIST
      ${DEPENDENCY_LIBRARIES}
      ${OPTIONAL_DEPENDENCY_LIBRARIES}
      ${OS_SPECIFIC_LIBRARIES}
   )

   include( PostSharedLibrary OPTIONAL) # additional hook

   target_link_libraries( mulle-concurrent
      ${SHARED_LIBRARY_LIST}
   )
endif()


### Install

include( PreInstall OPTIONAL)

install( TARGETS ${INSTALL_LIBRARY_TARGETS} DESTINATION "lib")
install( FILES ${PUBLIC_HEADERS}  DESTINATION "include/mulle-concurrent")
install( FILES ${PRIVATE_HEADERS} DESTINATION "include/mulle-concurrent/private")

install( FILES ${CMAKE_INCLUDES}  DESTINATION "include/mulle-concurrent/cmake")

include( PostInstall OPTIONAL)
