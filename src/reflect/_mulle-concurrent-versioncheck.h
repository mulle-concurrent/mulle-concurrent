/*
 *   This file will be regenerated by `mulle-project-versioncheck`.
 *   Any edits will be lost.
 */
#if defined( MULLE__ABA_VERSION)
# if MULLE__ABA_VERSION < ((3 << 20) | (0 << 8) | 1)
#  error "mulle-aba is too old"
# endif
# if MULLE__ABA_VERSION >= ((4 << 20) | (0 << 8) | 0)
#  error "mulle-aba is too new"
# endif
#endif
