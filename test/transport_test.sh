#!/bin/sh
# Host tests for the transport of the network reader (source/net.c): what happens while a
# reader makes the app wait. The linker's --wrap sends the transport's fcntl, poll, send and
# recv to the scripted ones in test/test_transport.c, so no socket is involved. Needs what
# test/run.sh needs.
set -e
cd "$(dirname "$0")/.."
: "${CC:=cc}"
mkdir -p build-host
. ./test/host_env.sh

# shellcheck disable=SC2086
$CC -std=c11 -Wall -Wextra -Werror -g -O1 $SAN $NBE_HOST_CFLAGS \
    source/net.c source/bis.c source/nbe_fmt.c test/shim/shim.c test/test_transport.c \
    -Wl,--wrap=poll -Wl,--wrap=send -Wl,--wrap=recv -Wl,--wrap=fcntl \
    -o build-host/test_transport
# A minute at most: these checks take well under a second, so one that is still running then
# waits for something that never comes, and that must fail the suite instead of stopping it.
timeout 60 ./build-host/test_transport
