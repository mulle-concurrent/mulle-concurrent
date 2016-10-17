#!/usr/bin/env bash
#
#  run-test.sh
#  MulleObjC
#
#  Created by Nat! on 01.11.13.
#  Copyright (c) 2013 Mulle kybernetiK. All rights reserved.
#  (was run-mulle-scion-test)


PROJECTDIR="`dirname "$PWD"`"
PROJECTNAME="`basename "${PROJECTDIR}"`"
LIBRARY_SHORTNAME="mulle_concurrent"


. "mulle-tests/test-c-common.sh"
RELEASE_CL_CFLAGS="${RELEASE_CL_CFLAGS} -DMULLE_ALLOCATOR_EXTERN_GLOBAL=extern"
DEBUG_CL_CFLAGS="${DEBUG_CL_CFLAGS} -DMULLE_ALLOCATOR_EXTERN_GLOBAL=extern"

. "mulle-tests/test-tools-common.sh"
. "mulle-tests/test-sharedlib-common.sh"
. "mulle-tests/run-test-common.sh"
