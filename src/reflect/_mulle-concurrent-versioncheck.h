/*
 *   This file will be regenerated by `mulle-project-versioncheck`.
 *   Any edits will be lost.
 */
#ifndef mulle_concurrent_versioncheck_h__
#define mulle_concurrent_versioncheck_h__

#if defined( MULLE__ABA_VERSION)
# ifndef MULLE__ABA_VERSION_MIN
#  define MULLE__ABA_VERSION_MIN  ((3UL << 20) | (1 << 8) | 2)
# endif
# ifndef MULLE__ABA_VERSION_MAX
#  define MULLE__ABA_VERSION_MAX  ((4UL << 20) | (0 << 8) | 0)
# endif
# if MULLE__ABA_VERSION < MULLE__ABA_VERSION_MIN || MULLE__ABA_VERSION >= MULLE__ABA_VERSION_MAX
#  pragma message("MULLE__ABA_VERSION     is " MULLE_C_STRINGIFY_MACRO( MULLE__ABA_VERSION))
#  pragma message("MULLE__ABA_VERSION_MIN is " MULLE_C_STRINGIFY_MACRO( MULLE__ABA_VERSION_MIN))
#  pragma message("MULLE__ABA_VERSION_MAX is " MULLE_C_STRINGIFY_MACRO( MULLE__ABA_VERSION_MAX))
#  if MULLE__ABA_VERSION < MULLE__ABA_VERSION_MIN
#   error "mulle-aba is too old"
#  else
#   error "mulle-aba is too new"
#  endif
# endif
#endif

#endif
