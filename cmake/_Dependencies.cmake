#
# cmake/_Dependencies.cmake is generated by `mulle-sde`. Edits will be lost.
# Disable generation of this file with:
#   mulle-sde environment  --global set MULLE_SOURCETREE_TO_CMAKE_DEPENDENCIES_FILE DISABLE
if( MULLE_TRACE_INCLUDE)
   message( STATUS "# Include \"${CMAKE_CURRENT_LIST_FILE}\"" )
endif()

#
# Generated from sourcetree: mulle-aba;no-all-load,no-import,no-singlephase;
# Disable with: `mulle-sourcetree mark mulle-aba no-link`
#
if( NOT MULLE_ABA_LIBRARY)
   find_library( MULLE_ABA_LIBRARY NAMES ${CMAKE_STATIC_LIBRARY_PREFIX}mulle-aba${CMAKE_STATIC_LIBRARY_SUFFIX} mulle-aba NO_CMAKE_SYSTEM_PATH)
   message( STATUS "MULLE_ABA_LIBRARY is ${MULLE_ABA_LIBRARY}")
   #
   # The order looks ascending, but due to the way this file is read
   # it ends up being descending, which is what we need.
   #
   if( MULLE_ABA_LIBRARY)
      #
      # Add to MULLE_ABA_LIBRARY list.
      # Disable with: `mulle-sourcetree mark mulle-aba no-cmakeadd`
      #
      set( DEPENDENCY_LIBRARIES
         ${DEPENDENCY_LIBRARIES}
         ${MULLE_ABA_LIBRARY}
         CACHE INTERNAL "need to cache this"
      )
      #
      # Inherit ObjC loader and link dependency info.
      # Disable with: `mulle-sourcetree mark mulle-aba no-cmakeinherit`
      #
      # // temporarily expand CMAKE_MODULE_PATH
      get_filename_component( _TMP_MULLE_ABA_ROOT "${MULLE_ABA_LIBRARY}" DIRECTORY)
      get_filename_component( _TMP_MULLE_ABA_ROOT "${_TMP_MULLE_ABA_ROOT}" DIRECTORY)
      #
      #
      # Search for "DependenciesAndLibraries.cmake" to include.
      # Disable with: `mulle-sourcetree mark mulle-aba no-cmakedependency`
      #
      foreach( _TMP_MULLE_ABA_NAME "mulle-aba")
         set( _TMP_MULLE_ABA_DIR "${_TMP_MULLE_ABA_ROOT}/include/${_TMP_MULLE_ABA_NAME}/cmake")
         # use explicit path to avoid "surprises"
         if( EXISTS "${_TMP_MULLE_ABA_DIR}/DependenciesAndLibraries.cmake")
            unset( MULLE_ABA_DEFINITIONS)
            list( INSERT CMAKE_MODULE_PATH 0 "${_TMP_MULLE_ABA_DIR}")
            # we only want top level INHERIT_OBJC_LOADERS, so disable them
            if( NOT NO_INHERIT_OBJC_LOADERS)
               set( NO_INHERIT_OBJC_LOADERS OFF)
            endif()
            list( APPEND _TMP_INHERIT_OBJC_LOADERS ${NO_INHERIT_OBJC_LOADERS})
            set( NO_INHERIT_OBJC_LOADERS ON)
            #
            include( "${_TMP_MULLE_ABA_DIR}/DependenciesAndLibraries.cmake")
            #
            list( GET _TMP_INHERIT_OBJC_LOADERS -1 NO_INHERIT_OBJC_LOADERS)
            list( REMOVE_AT _TMP_INHERIT_OBJC_LOADERS -1)
            #
            list( REMOVE_ITEM CMAKE_MODULE_PATH "${_TMP_MULLE_ABA_DIR}")
            set( INHERITED_DEFINITIONS
               ${INHERITED_DEFINITIONS}
               ${MULLE_ABA_DEFINITIONS}
               CACHE INTERNAL "need to cache this"
            )
            break()
         else()
            message( STATUS "${_TMP_MULLE_ABA_DIR}/DependenciesAndLibraries.cmake not found")
         endif()
      endforeach()
   else()
      message( FATAL_ERROR "MULLE_ABA_LIBRARY was not found")
   endif()
endif()
