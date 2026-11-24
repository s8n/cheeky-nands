#!/bin/sh
# Runs the console UI (source/main.c) without a console: the screen is captured as text and the
# pad answers from a script. Needs what test/run.sh needs. One process per scenario, because the
# UI keeps its state in statics.
#
#   ./test/ui_test.sh              every scenario
#   ./test/ui_test.sh cancel       just these
#
# The simulated console lives in build-host/ui and is removed afterwards (NBE_KEEP_TMP=1 keeps it).
set -e
cd "$(dirname "$0")/.."
: "${CC:=cc}"
WORK="$PWD/build-host/ui"
rm -rf "$WORK"
mkdir -p "$WORK"
. ./test/host_env.sh

# source/main.c holds the app's entry point and the test has a main() of its own, so that one
# file is compiled apart, with its main renamed. (A -D on the command line would apply to
# every file, so it cannot be scoped by position.)
# shellcheck disable=SC2086
UI_CFLAGS="-std=c11 -Wall -Wextra -Werror -g -O1 $SAN $NBE_HOST_CFLAGS -Itest/switch -DNBE_SD_MOUNT=\"$WORK/sdcard\""
# shellcheck disable=SC2086
$CC $UI_CFLAGS -Dmain=nbe_app_main -c source/main.c -o build-host/main_ui.o
# shellcheck disable=SC2086
# test/net_stub.c stands in for source/net.c: no sockets, the requests come from the script.
$CC $UI_CFLAGS source/bis.c source/nbe_fmt.c test/shim/shim.c test/ui_shim.c test/net_stub.c \
    test/test_ui.c build-host/main_ui.o -o build-host/test_ui

if [ "$#" -eq 0 ]; then
    # shellcheck disable=SC2046
    set -- $(build-host/test_ui --list)
fi
rc=0
for s in "$@"; do
    printf '  %-30s ' "$s"
    rm -rf "$WORK/sdcard" "$WORK/images"
    if out=$(build-host/test_ui "$s" "$WORK" 2>&1); then
        echo "ok ($out)"
    else
        echo "FAILED"
        printf '%s\n' "$out" | sed 's/^/      /'
        rc=1
    fi
done
if [ "${NBE_KEEP_TMP:-0}" != 1 ]; then rm -rf "$WORK"; fi
exit $rc
