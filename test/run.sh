#!/bin/sh
# Host tests for the dump engine: source/bis.c compiled unchanged against test/shim, with
# small part/chunk sizes so the multi-part paths run in seconds. Needs a C compiler (CC,
# default cc) and sha256sum (coreutils). Address/UB sanitizers are used when the compiler
# can run them here.
#
# The simulated console lives in build-host/tmp and is removed afterwards. Set NBE_KEEP_TMP=1
# to look at it after a failed run.
set -e
cd "$(dirname "$0")/.."
: "${CC:=cc}"
mkdir -p build-host
rm -rf build-host/tmp
. ./test/host_env.sh

# shellcheck disable=SC2086
$CC -std=c11 -Wall -Wextra -Werror -g -O1 $SAN $NBE_HOST_CFLAGS \
    "-DNBE_TEST_TMPDIR=\"$PWD/build-host/tmp\"" \
    source/bis.c source/nbe_fmt.c source/net.c test/shim/shim.c test/test_bis.c \
    -o build-host/test_bis
echo "sanitizers: ${SAN:-none}"

rc=0
./build-host/test_bis || rc=$?
if [ "${NBE_KEEP_TMP:-0}" = 1 ]; then
    [ -d build-host/tmp ] && echo "kept: build-host/tmp"
else
    rm -rf build-host/tmp
fi
exit $rc
