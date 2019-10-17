#
# The following includes include definitions generated
# during `mulle-sde update`. Don't edit those files. They are
# overwritten frequently.
#
# === MULLE-SDE START ===

message( STATUS "MULLE_ABA_LIBRARY is ${MULLE_ABA_LIBRARY}")
message( STATUS "CMAKE_C_LIBRARY_ARCHITECTURE is ${CMAKE_C_LIBRARY_ARCHITECTURE}")
message( STATUS "CMAKE_LIBRARY_ARCHITECTURE is ${CMAKE_LIBRARY_ARCHITECTURE}")
message( STATUS "CMAKE_PREFIX_PATH is ${CMAKE_PREFIX_PATH}")
message( STATUS "CMAKE_LIBRARY_PATH is ${CMAKE_LIBRARY_PATH}")
message( STATUS "CMAKE_SYSROOT is ${CMAKE_SYSROOT}")

include( _Dependencies)
include( _Libraries)

# === MULLE-SDE END ===
#

#
# If you need more find_library() statements, that you dont want to manage
# with the sourcetree, add them here.
#
# Add OS specific dependencies to OS_SPECIFIC_LIBRARIES
# Add all other dependencies (rest) to DEPENDENCY_LIBRARIES
#
