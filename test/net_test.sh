#!/bin/sh
# net_pull.py against the reader service over loopback, through the real socket code. Needs
# what test/run.sh needs, plus nc, dd and sha256sum, plus uv (or PYTHON=python3) for net_pull.py.
#
#   ./test/net_test.sh              uses port 28943 (NET_TEST_PORT=<n> for another)
set -e
cd "$(dirname "$0")/.."
: "${CC:=cc}"
: "${PYTHON:=uv run --no-project --quiet python}"
PORT="${NET_TEST_PORT:-28943}"
WORK="$PWD/build-host/net"
rm -rf "$WORK"
mkdir -p "$WORK/images" "$WORK/out"
. ./test/host_env.sh

# shellcheck disable=SC2086
$CC -std=c11 -Wall -Wextra -Werror -g -O1 $SAN $NBE_HOST_CFLAGS source/net.c source/bis.c \
    source/nbe_fmt.c test/shim/shim.c test/test_net.c -o build-host/test_net

# The first READ is cut after 100000 bytes: the header and one whole chunk get through.
# A reader that lets a second pass without a byte going through is let go of, as the app does
# after two minutes.
# shellcheck disable=SC2086
build-host/test_net "$PORT" drop-after=100000 stall-after=1000 \
    "30=$WORK/images/p30.bin:200000:7" "0=$WORK/images/p0.bin:4096:8" > "$WORK/server.log" 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null || true' EXIT
for _ in $(seq 1 50); do
    nc -z 127.0.0.1 "$PORT" 2>/dev/null && break
    sleep 0.1
done

# Every client call gets a minute: here the console answers at once, so a call that is still
# running then is hung, and a hang must fail the test instead of stopping it.
# shellcheck disable=SC2086
P="timeout 60 $PYTHON net_pull.py --host 127.0.0.1 --port $PORT --timeout 10"

echo "ping:"
# shellcheck disable=SC2086
$P ping

echo "list:"
# shellcheck disable=SC2086
$P list --out "$WORK/out/list.txt" 2> "$WORK/out/list.err"
# One row of the list by id, its columns joined with single blanks (the result column is
# empty when the partition opens)
list_row() { awk -v id="$1" '$1 == id { $1 = $1; print }' "$WORK/out/list.txt"; }
[ "$(list_row 0)" = "0 yes 4096 BootPartition1Root" ] \
    || { echo "FAIL: the list has no first row for id 0: '$(list_row 0)'"; exit 1; }
[ "$(list_row 30)" = "30 yes 200000 User" ] \
    || { echo "FAIL: the list has no row for id 30: '$(list_row 30)'"; exit 1; }
[ "$(list_row 28)" = "28 no 0 0x002ee202 CalibrationFile" ] \
    || { echo "FAIL: the list has no row for the partition that cannot be opened: '$(list_row 28)'"; exit 1; }
[ "$(grep -c -E '^ *[0-9]+ ' "$WORK/out/list.txt")" -eq 20 ] \
    || { echo "FAIL: the list has $(grep -c -E '^ *[0-9]+ ' "$WORK/out/list.txt") rows, wanted 20: every id of the engine's table"; exit 1; }

echo "read 30: cut once, then continued, then checked against the console's hash:"
# shellcheck disable=SC2086
$P read 30 0 0 "$WORK/out/p30.bin" --resume --retries 5 2> "$WORK/out/read30.err"
cat "$WORK/out/read30.err"
grep -q "transfer broke" "$WORK/out/read30.err" || { echo "FAIL: the first attempt did not break"; exit 1; }
grep -q "asking again from 64.00 KiB (1 of 5)" "$WORK/out/read30.err" \
    || { echo "FAIL: it did not continue from the 65536 bytes it had"; exit 1; }
grep -q "whole file checks out" "$WORK/out/read30.err" || { echo "FAIL: no HASH check"; exit 1; }
cmp "$WORK/out/p30.bin" "$WORK/images/p30.bin" && echo "  the file is the image"
(cd "$WORK/out" && sha256sum -c p30.bin.sha256)

echo "verify 30: the console hashes the range again, in 64 KiB pieces, against the file:"
# shellcheck disable=SC2086
$P verify 30 0 200000 "$WORK/out/p30.bin" --piece 65536 > "$WORK/out/verify.txt" 2> "$WORK/out/verify.err"
tail -n 1 "$WORK/out/verify.err"
[ "$(grep -c '^ok ' "$WORK/out/verify.txt")" -eq 4 ] || { echo "FAIL: not four pieces were checked"; exit 1; }
grep -q '^MISMATCH' "$WORK/out/verify.txt" && { echo "FAIL: a piece of the right file did not match"; exit 1; }
[ "$(awk '{print $1, $2, $3}' "$WORK/out/verify.txt" | tr '\n' ';')" = "ok 0 65536;ok 65536 65536;ok 131072 65536;ok 196608 3392;" ] \
    || { echo "FAIL: the pieces are not these four"; cat "$WORK/out/verify.txt"; exit 1; }

echo "verify 30 against a copy with one byte changed in its second piece:"
cp "$WORK/out/p30.bin" "$WORK/out/p30-bad.bin"
printf '\377\377\377\377' | dd of="$WORK/out/p30-bad.bin" bs=1 seek=70000 conv=notrunc status=none
# shellcheck disable=SC2086
if $P verify 30 0 200000 "$WORK/out/p30-bad.bin" --piece 65536 --reread 1 > "$WORK/out/verify-bad.txt" 2> "$WORK/out/verify-bad.err"; then
    echo "FAIL: a changed file passed"
    exit 1
fi
tail -n 1 "$WORK/out/verify-bad.err"
[ "$(grep -c '^MISMATCH 65536 ' "$WORK/out/verify-bad.txt")" -eq 1 ] && [ "$(grep -c '^ok ' "$WORK/out/verify-bad.txt")" -eq 3 ] \
    || { echo "FAIL: not exactly the second piece was reported"; cat "$WORK/out/verify-bad.txt"; exit 1; }
grep -q '  read again: the console now hashes it [0-9a-f]*\. 4 bytes differ from the file, in 1 of 16 4 KiB blocks, from +0x1000 to +0x1000$' \
    "$WORK/out/verify-bad.txt" || { echo "FAIL: the changed piece was not read again and compared"; cat "$WORK/out/verify-bad.txt"; exit 1; }

echo "verify 30, only its first and its last piece (--count 2):"
# shellcheck disable=SC2086
$P verify 30 0 200000 "$WORK/out/p30.bin" --piece 65536 --count 2 2> /dev/null > "$WORK/out/verify-2.txt"
[ "$(grep -c '^ok ' "$WORK/out/verify-2.txt")" -eq 2 ] && grep -q '^ok 196608 ' "$WORK/out/verify-2.txt" \
    || { echo "FAIL: --count 2 did not take the first and the last piece"; cat "$WORK/out/verify-2.txt"; exit 1; }

echo "read 30, a range in the middle:"
# shellcheck disable=SC2086
$P read 30 1000 500 "$WORK/out/mid.bin" 2> /dev/null
dd if="$WORK/images/p30.bin" of="$WORK/out/mid-want.bin" bs=1 skip=1000 count=500 status=none
cmp "$WORK/out/mid.bin" "$WORK/out/mid-want.bin" && echo "  the file is those bytes of the image"

echo "the same with the id and the start in hex:"
# shellcheck disable=SC2086
$P read 0x1e 0x3e8 500 "$WORK/out/mid-hex.bin" 2> /dev/null
cmp "$WORK/out/mid-hex.bin" "$WORK/out/mid-want.bin" && echo "  the file is those bytes of the image"

echo "read 0, all of it:"
# shellcheck disable=SC2086
$P read 0 0 end "$WORK/out/p0.bin" 2> /dev/null
cmp "$WORK/out/p0.bin" "$WORK/images/p0.bin" && echo "  the file is the image"

echo "verify 0 with end, which is the whole file:"
# shellcheck disable=SC2086
$P verify 0 0 end "$WORK/out/p0.bin" --piece 4096 2> /dev/null | grep -c '^ok 0 4096 ' | grep -x 1 > /dev/null \
    || { echo "FAIL: verify with end did not check the whole file"; exit 1; }

echo "read 99, an id the console does not have:"
# shellcheck disable=SC2086
if $P read 99 0 0 "$WORK/out/none.bin" 2> "$WORK/out/refused.txt"; then
    echo "FAIL: it was not refused"
    exit 1
fi
tail -n 1 "$WORK/out/refused.txt"
grep -q "refused: ERR" "$WORK/out/refused.txt" || { echo "FAIL: not the console's refusal"; exit 1; }
[ ! -e "$WORK/out/none.bin" ] || { echo "FAIL: a file was written for a refused read"; exit 1; }

echo "read 30 again without --resume, over an existing file:"
# shellcheck disable=SC2086
if $P read 30 0 0 "$WORK/out/p30.bin" 2> /dev/null; then
    echo "FAIL: an existing file was overwritten"
    exit 1
fi

echo "with nothing but nc (-N: it closes its side of the connection after its request):"
# shellcheck disable=SC2086
got=$(printf 'PING\n' | timeout 10 nc -N 127.0.0.1 "$PORT")
[ "$got" = "PONG" ] || { echo "FAIL: nc got '$got'"; exit 1; }

echo "a reader that connects and sends nothing is let go of after a second:"
# nc connects and keeps the connection without a word; the server lets it go after a second.
( sleep 4 | timeout 20 nc 127.0.0.1 "$PORT" ) > /dev/null 2>&1 &
SILENT=$!
let_go=0
for _ in $(seq 1 80); do
    if grep -q '^served "" rc=[0-9a-f]* stage=recv sent=0$' "$WORK/server.log"; then
        let_go=1
        break
    fi
    sleep 0.1
done
wait $SILENT 2>/dev/null || true
[ "$let_go" = 1 ] || { echo "FAIL: the silent reader was not let go of"; exit 1; }
echo "  let go of"

echo "what the console side saw, one line per connection:"
cat "$WORK/server.log"
# Every connection but the port probe (the first two lines), in order. The lines whose result
# code is the transport's own carry RC, so that they are not tied to a number here.
tail -n +4 "$WORK/server.log" \
    | sed -E 's/^(served "READ 30 0 0" rc=)[0-9a-f]+/\1RC/; s/^(served "" rc=)[0-9a-f]+ stage=recv/\1RC stage=recv/' \
    > "$WORK/out/served.txt"
cat > "$WORK/out/served-want.txt" <<EOF
served "PING" rc=00000000 stage=done sent=0
closed
served "LIST" rc=00000000 stage=done sent=0
closed
served "READ 30 0 0" rc=RC stage=send sent=65536
dropped
served "READ 30 65536 0" rc=00000000 stage=done sent=134464
closed
served "HASH 30 0 0" rc=00000000 stage=done sent=0
closed
served "HASH 30 0 65536" rc=00000000 stage=done sent=0
closed
served "HASH 30 65536 65536" rc=00000000 stage=done sent=0
closed
served "HASH 30 131072 65536" rc=00000000 stage=done sent=0
closed
served "HASH 30 196608 3392" rc=00000000 stage=done sent=0
closed
served "HASH 30 0 65536" rc=00000000 stage=done sent=0
closed
served "HASH 30 65536 65536" rc=00000000 stage=done sent=0
closed
served "READ 30 65536 65536" rc=00000000 stage=done sent=65536
closed
served "HASH 30 131072 65536" rc=00000000 stage=done sent=0
closed
served "HASH 30 196608 3392" rc=00000000 stage=done sent=0
closed
served "HASH 30 0 65536" rc=00000000 stage=done sent=0
closed
served "HASH 30 196608 3392" rc=00000000 stage=done sent=0
closed
served "READ 30 1000 500" rc=00000000 stage=done sent=500
closed
served "READ 30 1000 500" rc=00000000 stage=done sent=500
closed
served "READ 0 0 0" rc=00000000 stage=done sent=4096
closed
served "HASH 0 0 4096" rc=00000000 stage=done sent=0
closed
served "READ 99 0 0" rc=002ee202 stage=open sent=0
closed
served "PING" rc=00000000 stage=done sent=0
closed
served "" rc=RC stage=recv sent=0
dropped
EOF
diff "$WORK/out/served-want.txt" "$WORK/out/served.txt" \
    || { echo "FAIL: the console side saw something else"; exit 1; }

rm -rf "$WORK"
echo "net test: ok"
