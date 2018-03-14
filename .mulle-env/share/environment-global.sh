#
# Git mirror and Zip/TGZ cache to conserve bandwidth
# Memo: override in os-specific env file
#
export MULLE_FETCH_MIRROR_DIR="${HOME:-/tmp}/.cache/mulle-fetch/git-mirrors"

#
# Git mirror and Zip/TGZ cache to conserve bandwidth
#
export MULLE_FETCH_ARCHIVE_DIR="${HOME:-/tmp}/.cache/mulle-fetch/archives"

#
# PATH to search for git repositories locally
#
export MULLE_FETCH_SEARCH_PATH="${MULLE_VIRTUAL_ROOT}/.."

#
# Prefer symlinks to local git repositories found via MULLE_FETCH_SEARCH_PATH
#
export MULLE_SYMLINK="YES"

#
# Use common folder for sharable projects
#
export MULLE_SOURCETREE_SHARE_DIR="${MULLE_VIRTUAL_ROOT}/stashes"

#
# Use common build directory
#
export BUILD_DIR="${MULLE_VIRTUAL_ROOT}/build"

#
# Share dependencies directory (absolute for ease of use)
#
export DEPENDENCIES_DIR="${MULLE_VIRTUAL_ROOT}/dependencies"

#
# Share addictions directory (absolute for ease of use)
#
export ADDICTIONS_DIR="${MULLE_VIRTUAL_ROOT}/addictions"
