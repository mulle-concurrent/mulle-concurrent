#! /bin/sh

mulle-bootstrap build "$@"

#
# fucking Xcode stupidity if we build with -scheme
# stuff gets dumped into "build/Products/Release"
# w/o -scheme stuff gets dumped into "build/Release"
#
MAKE=`which "make"`
CMAKE=`which "cmake"`

#
# Just build stuff, let user install manually
#
if [ "`uname`"  = "Darwin" ]
then
   xcodebuild -configuration Debug -scheme Libraries -project mulle-concurrent.xcodeproj
   xcodebuild -configuration Release -scheme Libraries -project mulle-concurrent.xcodeproj
else
   if [ ! -z "${CMAKE}" ]
   then
      mkdir -p build 2> /dev/null
      cd build || exit 1
      cmake ..

      if [ ! -z "${MAKE}" ]
      then
         make
      fi
   fi
fi
