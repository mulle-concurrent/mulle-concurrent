#! /bin/sh

ORIGIN=origin                                    # git repo to push
REMOTEROOTDIR="mulle-objc"
REMOTEHOST="https://github.com"
REMOTEURL="${REMOTEHOST}/${REMOTEROOTDIR}"
ARCHIVEURL='${REMOTEURL}/${NAME}/archive/${VERSION}.tar.gz'  # ARCHIVEURL will be evaled later! keep it in single quotes

:
