/*
 *   This file will be regenerated by `mulle-project-versioncheck`.
 *   Any edits will be lost.
 */
#if defined( MULLE_ABA_VERSION)
# if MULLE_ABA_VERSION < ((2 << 20) | (0 << 8) | 22)
#  error "mulle-aba is too old"
# endif
# if MULLE_ABA_VERSION >= ((3 << 20) | (0 << 8) | 0)
#  error "mulle-aba is too new"
# endif
#endif
