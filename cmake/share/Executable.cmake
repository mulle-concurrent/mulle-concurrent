# can be included multiple times in theory

if( MULLE_TRACE_INCLUDE)
   message( STATUS "# Include \"${CMAKE_CURRENT_LIST_FILE}\"" )
endif()


if( NOT EXECUTABLE_NAME)
   set( EXECUTABLE_NAME "${PROJECT_NAME}")
endif()

if( NOT EXECUTABLE_SOURCES)
   set( EXECUTABLE_SOURCES "${SOURCES}")
endif()

#
# must be ahead of AllLoadC
#
include( PreExecutable OPTIONAL)

add_library( "_1_${EXECUTABLE_NAME}" OBJECT
   ${EXECUTABLE_SOURCES}
)

set( ALL_OBJECT_FILES
   $<TARGET_OBJECTS:_1_${EXECUTABLE_NAME}>
)

set_property( TARGET "_1_${EXECUTABLE_NAME}" PROPERTY CXX_STANDARD 11)


if( LINK_PHASE)

   add_executable( "${EXECUTABLE_NAME}"
      ${ALL_OBJECT_FILES}
      ${PROJECT_HEADERS}
      ${CMAKE_EDITABLE_FILES}
   )

   add_dependencies( "${EXECUTABLE_NAME}" "_1_${EXECUTABLE_NAME}")

   # useful for mulle-c, but can be commented out
   set_property( TARGET "${EXECUTABLE_NAME}" PROPERTY CXX_STANDARD 11)

   #
   # this will set EXECUTABLE_LIBRARY_LIST if ALL_LOAD is used
   #
   include( ExecutableAux OPTIONAL)

   if( NOT EXECUTABLE_LIBRARY_LIST)
      if( ALL_LOAD_DEPENDENCY_LIBRARIES)
         message( FATAL_ERROR "ALL_LOAD_DEPENDENCY_LIBRARIES \"${ALL_LOAD_DEPENDENCY_LIBRARIES}\" are not linked to ${EXECUTABLE_NAME}")
      endif()

      set( EXECUTABLE_LIBRARY_LIST
         ${DEPENDENCY_LIBRARIES}
         ${OPTIONAL_DEPENDENCY_LIBRARIES}
         ${OS_SPECIFIC_LIBRARIES}
         ${STARTUP_LIBRARY}
      )
   endif()

   target_link_libraries( "${EXECUTABLE_NAME}"
      ${EXECUTABLE_LIBRARY_LIST}
   )

   set( INSTALL_EXECUTABLE_TARGETS
      "${EXECUTABLE_NAME}"
      ${INSTALL_EXECUTABLE_TARGETS}
   )

   include( PostExecutable OPTIONAL)


   ### Install

   install( TARGETS ${INSTALL_EXECUTABLE_TARGETS} DESTINATION "bin")

endif()
