#! /bin/sh

PROJECT="MulleConcurrent"    # requires camel-case
DESC="Lock- and Wait-free Hashtable (and an Array too) in C"
DEPENDENCIES="mulle-c11
mulle-allocator
mulle-aba
mulle-thread"            # names not camel case
ORIGIN=public            # git repo to push
LANGUAGE=c               # c,cpp, objc

# source mulle-homebrew.sh (clumsily)

. ./bin/mulle-homebrew/mulle-homebrew.sh

# parse options
homebrew_parse_options "$@"

# dial past options
while [ $# -ne 0 ]
do
   case "$1" in
      -*)
         shift
      ;;
      *)
         break;
      ;;
   esac
done

#
# these can usually be deduced, if you follow the conventions
#
NAME="`get_name_from_project "${PROJECT}" "${LANGUAGE}"`"
HEADER="`get_header_from_name "${NAME}"`"
VERSIONNAME="`get_versionname_from_project "${PROJECT}"`"
VERSION="`get_header_version "${HEADER}" "${VERSIONNAME}"`"


# --- HOMEBREW FORMULA ---
# Information needed to construct a proper brew formula
#
HOMEPAGE="https://www.mulle-kybernetik.com/software/git/${NAME}"
ARCHIVEURL='https://www.mulle-kybernetik.com/software/git/${NAME}/tarball/${VERSION}'  # ARCHIVEURL will be evaled later! keep it in single quotes


# --- HOMEBREW TAP ---
# Specify to where and under what bame to publish via your brew tap
#
RBFILE="${NAME}.rb"                    # ruby file for brew
HOMEBREWTAP="../homebrew-software"     # your tap repository path


# --- GIT ---
# tag to tag your release
# and the origin where
TAG="${1:-${TAGPREFIX}${VERSION}}"


main()
{
   git_main "${ORIGIN}" "${TAG}" || exit 1
   homebrew_main
}

main "$@"
