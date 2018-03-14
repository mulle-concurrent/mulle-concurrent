if( MULLE_TRACE_INCLUDE)
   message( STATUS "# Include \"${CMAKE_CURRENT_LIST_FILE}\"" )
endif()

#
# Set Search Paths
#
include( CMakeTweaks)


### Additional search paths based on build style

if( CMAKE_BUILD_STYLE STREQUAL "Debug")
   set( CMAKE_LIBRARY_PATH
      "${MULLE_VIRTUAL_ROOT}/dependencies/Debug/lib"
      "${MULLE_VIRTUAL_ROOT}/addictions/Debug/lib"
      ${CMAKE_LIBRARY_PATH}
   )
   set( CMAKE_FRAMEWORK_PATH
      "${MULLE_VIRTUAL_ROOT}/dependencies/Debug/Frameworks"
      "${MULLE_VIRTUAL_ROOT}/addictions/Debug/Frameworks"
      ${CMAKE_FRAMEWORK_PATH}
   )
endif()


# a place to add stuff for ObjC or C++
include( PreFilesCAux OPTIONAL)
