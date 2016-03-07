#!/bin/bash -m
#
#  run-test.sh
#  mulle-concurrent
#
#  Created by Nat! on 01.11.13.
#  Copyright (c) 2013 Mulle kybernetiK. All rights reserved.
#  (was run-mulle-scion-test)

# check if running a single test or all

executable=`basename "$0"`
executable=`basename "$executable" .sh`

if [ "`basename "$executable"`" = "run-all-tests" ]
then
   TEST=""
   VERBOSE=yes
   if [ "$1" = "-q" ]
   then
      VERBOSE=no
      shift
   fi
else
   TEST="$1"
   [ -z $# ] || shift
fi


if [ -z "${CFLAGS}" ]
then
   CFLAGS="-w -g -O0"
fi

#
# this is system wide, not so great
# and also not trapped...
#
suppress_crashdumping()
{
   local restore

   case `uname` in
      Darwin)
         restore="`defaults read com.apple.CrashReporter DialogType 2> /dev/null`"
         defaults write com.apple.CrashReporter DialogType none
         ;;
      Linux)
         ;;
   esac

   echo "${restore}"
}


restore_crashdumping()
{
   local restore

   restore="$1"

   case `uname` in
      Darwin)
         if [ -z "${restore}" ]
         then
            defaults delete com.apple.CrashReporter DialogType
         else
            defaults write com.apple.CrashReporter DialogType "${restore}"
         fi
         ;;
      Linux)
         ;;
   esac
}


trace_ignore()
{
   restore_crashdumping "$1"
   return 0
}


RESTORE_CRASHDUMP=`suppress_crashdumping`
trap 'trace_ignore "${RESTORE_CRASHDUMP}"' 0 5 6


#
# find runtime and headers
#
MULLE_CONCURRENT_DEPENDENCIES_INCLUDE=/usr/local/bin

lib="`ls -1 ../lib/libmulle_standalone_concurrent.dylib 2> /dev/null | tail -1`"
MULLE_CONCURRENT_DEPENDENCIES_INCLUDE="../include"

if [ ! -x "${lib}" ]
then
   lib="`ls -1 "../build/Products/Debug/libmulle_standalone_concurrent.dylib" | tail -1 2> /dev/null`"
   MULLE_CONCURRENT_DEPENDENCIES_INCLUDE="../dependencies/include"
fi

MULLE_CONCURRENT="${1:-${lib}}"
[ -z $# ] || shift

if [ -z "${MULLE_CONCURRENT}" ]
then
   echo "libmulle_standalone_concurrent.dylib can not be found" >&2
   exit 1
fi

MULLE_CONCURRENT_INCLUDE="`dirname "${MULLE_CONCURRENT}"`"
MULLE_CONCURRENT_INCLUDE="`dirname "${MULLE_CONCURRENT}"`"

if [ -d "${MULLE_CONCURRENT_INCLUDE}/usr/local/include" ]
then
   MULLE_CONCURRENT_INCLUDE="${MULLE_CONCURRENT_INCLUDE}/usr/local/include"
else
   MULLE_CONCURRENT_INCLUDE="${MULLE_CONCURRENT_INCLUDE}/include"
fi


DIR=${1:-`pwd`}
shift

HAVE_WARNED="no"
RUNS=0

search_plist()
{
   local plist
   local root

   dir=`dirname "$1"`
   plist=`basename "$1"`
   root="$2"

   while :
   do
      if [ -f "$dir"/"$plist" ]
      then
         echo "$dir/$plist"
         break
      fi

      if [ "$dir" = "$root" ]
      then
         break
      fi

      next=`dirname "$dir"`
      if [ "$next" = "$dir" ]
      then
         break
      fi
      dir="$next"
   done
}


relpath()
{
   python -c "import os.path; print os.path.relpath('$1', '$2')"
}



absolute_path_if_relative()
{
   case "$1" in
      .*)  echo "`pwd`/$1"
      ;;

      *) echo "$1"
      ;;
   esac
}


maybe_show_diagnostics()
{
   local errput

   errput="$1"

   local contents
   contents="`head -2 "$errput"`" 2> /dev/null
   if [ "${contents}" != "" ]
   then
      echo "DIAGNOSTICS:" >&2
      cat  "$errput"
   fi
}


maybe_show_output()
{
   local output

   output="$1"

   local contents
   contents="`head -2 "$output"`" 2> /dev/null
   if [ "${contents}" != "" ]
   then
      echo "OUTPUT:" >&2
      cat  "$output"
   fi
}


search_for_strings()
{
   local errput
   local strings
   local banner

   banner="$1"
   errput="$2"
   strings="$3"

   local fail
   local expect
   local match

   fail=0
   while read expect
   do
      if [ ! -z "$expect" ]
      then
         match=`grep "$expect" "$errput"`
         if [ "$match" = "" ]
         then
            if [ $fail -eq 0 ]
            then
               echo "${banner}" >&2
               fail=1
            fi
            echo "   $expect" >&2
         fi
      fi
   done < "$strings"

   [ $fail -eq 1 ]
}


fail_test()
{
   local m_source
   local a_out
   local stdin

   m_source="$1"
   a_out="$2"
   stdin="$3"

   echo "DEBUG: " >&2
   echo "DYLD_FALLBACK_LIBRARY_PATH=\"${DYLD_FALLBACK_LIBRARY_PATH}\" \
LD_LIBRARY_PATH=\"${LD_LIBRARY_PATH}\" lldb ${a_out}" >&2
   if [ "${stdin}" != "/dev/null" ]
   then
      echo "run < ${stdin}" >&2
   fi
   exit 1
}



run()
{
   local m_source
   local makefile
   local root
   local stdin
   local stdout
   local stderr
   local ccdiag

   m_source="$1"
   makefile="$2"
   root="$3"
   stdin="$4"
   stdout="$5"
   stderr="$6"
   ccdiag="$7"

   local output
   local errput
   local random
   local fail
   local match

   random=`mktemp -t "mulle-objc-runtime"`
   a_out="$random.exe"
   output="$random.stdout"
   errput="$random.stderr"
   errors=`basename $m_source .c`.errors

   local owd

   owd=`pwd`
   pretty_source=`relpath "$owd"/"$m_source" "$root"`
   if [ "$VERBOSE" = "yes" ]
   then
      echo "$pretty_source" >&2
   fi

   RUNS=`expr "$RUNS" + 1`

   # plz2shutthefuckup bash
   set +m
   set +b
   set +v
   # denied, will always print TRACE/BPT

   local  TMP_CFLAGS

   TMP_CFLAGS="${CFLAGS} \
-w \
-I${MULLE_CONCURRENT_INCLUDE} \
-I${MULLE_CONCURRENT_DEPENDENCIES_INCLUDE}"


   if [ -z "${makefile}" ]
   then
      cc ${TMP_CFLAGS} -o "${a_out}" "${m_source}" "${MULLE_CONCURRENT}"
 > "$errput" 2>&1
   else
      CFLAGS="${TMP_CFLAGS}" LDFLAGS="${MULLE_CONCURRENT}" OUTPUT="${a_out}" make
   fi

   if [ $? -ne 0 ]
   then
      if [ "$ccdiag" = "-" ]
      then
         echo "COMPILER ERRORS: \"$pretty_source\"" >&2
         maybe_show_diagnostics "$errput"
         fail_test "${m_source}" "${a_out}" "${stdin}"
      else
         search_for_strings "COMPILER FAILED TO PRODUCE ERRORS: \"$pretty_source\" ($errput)" \
                            "$errput" "$ccdiag"
         if [ $? -ne 0 ]
         then
            return 0
         fi
         maybe_show_diagnostics "$errput" >&2
         fail_test "${m_source}" "${a_out}" "${stdin}"
      fi
   fi

   "${a_out}" < "$stdin" > "$output" 2> "$errput"

   if [ $? -ne 0 ]
   then
      if [ ! -f "$errors" ]
      then
         echo "TEST CRASHED: \"$pretty_source\" (${a_out}, ${errput})" >&2
         maybe_show_diagnostics "$errput" >&2
         fail_test "${m_source}" "${a_out}" "${stdin}"
      else
         search_for_strings "TEST FAILED TO PRODUCE ERRORS: \"$pretty_source\" (${a_out}, $errput)" \
                            "$errput" "$errors"
         if [ $? -ne 0 ]
         then
            return 0
         fi
         maybe_show_diagnostics "$errput" >&2
         fail_test "${m_source}" "${a_out}" "${stdin}"
      fi
   else
      if [ -f "$errors" ]
      then
         echo "TEST FAILED TO CRASH: \"$pretty_source\" (${a_out})" >&2
         maybe_show_diagnostics "$errput" >&2
         fail_test "${m_source}" "${a_out}" "${stdin}"
      fi
   fi

   if [ "$stdout" != "-" ]
   then
      result=`diff -q "$stdout" "$output"`
      if [ "$result" != "" ]
      then
         white=`diff -q -w "$stdout" "$output"`
         if [ "$white" != "" ]
         then
            echo "FAILED: \"$pretty_source\" produced unexpected output (${a_out})" >&2
            echo "DIFF: ($stdout vs. $output)" >&2
            diff -y "$stdout" "$output" >&2
         else
            echo "FAILED: \"$pretty_source\" produced different whitespace output (${a_out})" >&2
            echo "DIFF: ($stdout vs. $output)" >&2
            od -a "$output" > "$output.actual.hex"
            od -a "$stdout" > "$output.expect.hex"
            diff -y "$output.expect.hex" "$output.actual.hex" >&2
         fi

         maybe_show_diagnostics "$errput" >&2
         maybe_show_output "$output"

         fail_test "${m_source}" "${a_out}" "${stdin}"
      fi
   else
      contents="`head -2 "$output"`" 2> /dev/null
      if [ "${contents}" != "" ]
      then
         echo "WARNING: \"$pretty_source\" produced unexpected output (${a_out}, $output)" >&2

         maybe_show_diagnostics "$errput" >&2
         maybe_show_output "$output"

         fail_test "${m_source}" "${a_out}" "${stdin}"
      fi
   fi

   if [ "$stderr" != "-" ]
   then
      result=`diff "$stderr" "$errput"`
      if [ "$result" != "" ]
      then
         echo "WARNING: \"$pretty_source\" produced unexpected diagnostics (${a_out}, $errput)" >&2
         echo "" >&2
         diff "$stderr" "$errput" >&2

         maybe_show_diagnostics "$errput"
         fail_test "${m_source}" "${a_out}" "${stdin}"
      fi
   fi
}


run_test()
{
   local m_source
   local root
   local makefile

   m_source="$1.c"
   root="$2"
   makefile="$3"

   local stdin
   local stdout
   local stderr
   local plist

   stdin="$1.stdin"
   if [ ! -f "$stdin" ]
   then
      stdin="provide/$1.stdin"
   fi
   if [ ! -f "$stdin" ]
   then
      stdin="default.stdin"
   fi
   if [ ! -f "$stdin" ]
   then
      stdin="/dev/null"
   fi

   stdout="$1.stdout"
   if [ ! -f "$stdout" ]
   then
      stdout="expect/$1.stdout"
   fi
   if [ ! -f "$stdout" ]
   then
      stdout="default.stdout"
   fi
   if [ ! -f "$stdout" ]
   then
      stdout="-"
   fi

   stderr="$1.stderr"
   if [ ! -f "$stderr" ]
   then
      stderr="expect/$1.stderr"
   fi
   if [ ! -f "$stderr" ]
   then
      stderr="default.stderr"
   fi
   if [ ! -f "$stderr" ]
   then
      stderr="-"
   fi

   ccdiag="$1.ccdiag"
   if [ ! -f "$ccdiag" ]
   then
      ccdiag="expect/$1.ccdiag"
   fi
   if [ ! -f "$ccdiag" ]
   then
      ccdiag="default.ccdiag"
   fi
   if [ ! -f "$ccdiag" ]
   then
      ccdiag="-"
   fi

   run "$m_source" "$makefile" "$root" "$stdin" "$stdout" "$stderr" "$ccdiag"
}


scan_current_directory()
{
   local root

   root="$1"

   local i
   local filename
   local dir

   if [ -f Makefile ]
   then
      run_test "$1" "$root" "Makefile"
      return 0
   fi

   for i in [^_]*
   do
      if [ -d "$i" ]
      then
         dir=`pwd`
         cd "$i"
         scan_current_directory "$root"
         cd "$dir"
      else
         filename=`basename "$i" .c`
         if [ "$filename" != "$i" ]
         then
            run_test "$filename" "$root"
         fi
      fi
   done
}



MULLE_CONCURRENT="`absolute_path_if_relative "$MULLE_CONCURRENT"`"
MULLE_CONCURRENT_INCLUDE="`absolute_path_if_relative "$MULLE_CONCURRENT_INCLUDE"`"
MULLE_CONCURRENT_DEPENDENCIES_INCLUDE="`absolute_path_if_relative "$MULLE_CONCURRENT_DEPENDENCIES_INCLUDE"`"

# OS X
DYLD_FALLBACK_LIBRARY_PATH="`dirname "${MULLE_CONCURRENT}"`" ; export DYLD_FALLBACK_LIBRARY_PATH
# Linux
LD_LIBRARY_PATH="`dirname "${MULLE_CONCURRENT}"`" ; export LD_LIBRARY_PATH


if [ "$TEST" = "" ]
then
   cd "${DIR}"
   scan_current_directory "`pwd -P`"

   if [ "$RUNS" -ne 0 ]
   then
      echo "All tests ($RUNS) passed successfully"
   else
      echo "no tests found" >&2
      exit 1
   fi
else
    dirname=`dirname "$TEST"`
    if [ "$dirname" = "" ]
    then
       dirname="."
    fi
    file=`basename "$TEST"`
    filename=`basename "$file" .c`

    if [ "$file" = "$filename" ]
    then
       echo "error: source file must have .c extension" >&2
       exit 1
    fi

    if [ ! -f "$TEST" ]
    then
       echo "error: source file not found" >&2
       exit 1
    fi

    old="`pwd -P`"
    cd "${dirname}" || exit 1
    run_test "$filename" "${old}"
    rval=$?
    cd "${old}" || exit 1
    exit $rval
fi
