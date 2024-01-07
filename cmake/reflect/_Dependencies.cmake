# This file will be regenerated by `mulle-sourcetree-to-cmake` via
# `mulle-sde reflect` and any edits will be lost.
#
# This file will be included by cmake/share/Files.cmake
#
# Disable generation of this file with:
#
# mulle-sde environment set MULLE_SOURCETREE_TO_CMAKE_DEPENDENCIES_FILE DISABLE
#
if( MULLE_TRACE_INCLUDE)
   message( STATUS "# Include \"${CMAKE_CURRENT_LIST_FILE}\"" )
endif()

#
# Generated from sourcetree: 0A5FA4B8-AB96-4A8A-A855-E40FCC951603;mulle-aba;no-all-load,no-cmake-searchpath,no-import,no-singlephase;
# Disable with : `mulle-sourcetree mark mulle-aba no-link`
# Disable for this platform: `mulle-sourcetree mark mulle-aba no-cmake-platform-${MULLE_UNAME}`
# Disable for a sdk: `mulle-sourcetree mark mulle-aba no-cmake-sdk-<name>`
#
if( NOT MULLE__ABA_LIBRARY)
   find_library( MULLE__ABA_LIBRARY NAMES
      ${CMAKE_STATIC_LIBRARY_PREFIX}mulle-aba${CMAKE_DEBUG_POSTFIX}${CMAKE_STATIC_LIBRARY_SUFFIX}
      ${CMAKE_STATIC_LIBRARY_PREFIX}mulle-aba${CMAKE_STATIC_LIBRARY_SUFFIX}
      mulle-aba
      NO_CMAKE_SYSTEM_PATH NO_SYSTEM_ENVIRONMENT_PATH
   )
   if( NOT MULLE__ABA_LIBRARY AND NOT DEPENDENCY_IGNORE_SYSTEM_LIBARIES)
      find_library( MULLE__ABA_LIBRARY NAMES
         ${CMAKE_STATIC_LIBRARY_PREFIX}mulle-aba${CMAKE_DEBUG_POSTFIX}${CMAKE_STATIC_LIBRARY_SUFFIX}
         ${CMAKE_STATIC_LIBRARY_PREFIX}mulle-aba${CMAKE_STATIC_LIBRARY_SUFFIX}
         mulle-aba
      )
   endif()
   message( STATUS "MULLE__ABA_LIBRARY is ${MULLE__ABA_LIBRARY}")
   #
   # The order looks ascending, but due to the way this file is read
   # it ends up being descending, which is what we need.
   #
   if( MULLE__ABA_LIBRARY)
      #
      # Add MULLE__ABA_LIBRARY to DEPENDENCY_LIBRARIES list.
      # Disable with: `mulle-sourcetree mark mulle-aba no-cmake-add`
      #
      list( APPEND DEPENDENCY_LIBRARIES ${MULLE__ABA_LIBRARY})
      #
      # Inherit information from dependency.
      # Encompasses: no-cmake-searchpath,no-cmake-dependency,no-cmake-loader
      # Disable with: `mulle-sourcetree mark mulle-aba no-cmake-inherit`
      #
      # temporarily expand CMAKE_MODULE_PATH
      get_filename_component( _TMP_MULLE__ABA_ROOT "${MULLE__ABA_LIBRARY}" DIRECTORY)
      get_filename_component( _TMP_MULLE__ABA_ROOT "${_TMP_MULLE__ABA_ROOT}" DIRECTORY)
      #
      #
      # Search for "Definitions.cmake" and "DependenciesAndLibraries.cmake" to include.
      # Disable with: `mulle-sourcetree mark mulle-aba no-cmake-dependency`
      #
      foreach( _TMP_MULLE__ABA_NAME "mulle-aba")
         set( _TMP_MULLE__ABA_DIR "${_TMP_MULLE__ABA_ROOT}/include/${_TMP_MULLE__ABA_NAME}/cmake")
         # use explicit path to avoid "surprises"
         if( IS_DIRECTORY "${_TMP_MULLE__ABA_DIR}")
            list( INSERT CMAKE_MODULE_PATH 0 "${_TMP_MULLE__ABA_DIR}")
            # we only want top level INHERIT_OBJC_LOADERS, so disable them
            if( NOT NO_INHERIT_OBJC_LOADERS)
               set( NO_INHERIT_OBJC_LOADERS OFF)
            endif()
            list( APPEND _TMP_INHERIT_OBJC_LOADERS ${NO_INHERIT_OBJC_LOADERS})
            set( NO_INHERIT_OBJC_LOADERS ON)
            #
            include( "${_TMP_MULLE__ABA_DIR}/DependenciesAndLibraries.cmake" OPTIONAL)
            #
            list( GET _TMP_INHERIT_OBJC_LOADERS -1 NO_INHERIT_OBJC_LOADERS)
            list( REMOVE_AT _TMP_INHERIT_OBJC_LOADERS -1)
            list( REMOVE_ITEM CMAKE_MODULE_PATH "${_TMP_MULLE__ABA_DIR}")
            #
            unset( MULLE__ABA_DEFINITIONS)
            include( "${_TMP_MULLE__ABA_DIR}/Definitions.cmake" OPTIONAL)
            list( APPEND INHERITED_DEFINITIONS ${MULLE__ABA_DEFINITIONS})
            break()
         else()
            message( STATUS "${_TMP_MULLE__ABA_DIR} not found")
         endif()
      endforeach()
   else()
      # Disable with: `mulle-sourcetree mark mulle-aba no-require-link`
      message( FATAL_ERROR "MULLE__ABA_LIBRARY was not found")
   endif()
endif()
