#
# Git mirror and Zip/TGZ cache to conserve bandwidth
# Memo: override in os-specific env file
#
export MULLE_FETCH_MIRROR_DIR="${HOME:-/tmp}/.cache/mulle-fetch/git-mirror"

#
# Git mirror and Zip/TGZ cache to conserve bandwidth
#
export MULLE_FETCH_ARCHIVE_DIR="${HOME:-/tmp}/.cache/mulle-fetch/archive"

#
# PATH to search for git repositories locally
#
export MULLE_FETCH_SEARCH_PATH="${MULLE_VIRTUAL_ROOT}/.."

#
# Prefer symlinking to local git repositories found via MULLE_FETCH_SEARCH_PATH
#
export MULLE_SOURCETREE_SYMLINK="YES"

#
# Use common folder for sharable projects
#
export MULLE_SOURCETREE_STASH_DIRNAME="stash"

#
# Share dependency directory (absolute for ease of use)
#
export DEPENDENCY_DIR="${MULLE_VIRTUAL_ROOT}/dependency"

#
# Share addiction directory (absolute for ease of use)
#
export ADDICTION_DIR="${MULLE_VIRTUAL_ROOT}/addiction"

#
# Use common build directory
#
export KITCHEN_DIR="${MULLE_VIRTUAL_ROOT}/kitchen"
#
#
#
export MULLE_SDE_INSTALLED_VERSION="0.38.0"


