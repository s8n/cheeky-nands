#!/usr/bin/env python3
"""Pull partition bytes from a console that runs nand-backup-experiments with its network
reader on (ZR in the app).

    net_pull.py [--host IP] [--port N] ping
    net_pull.py [--host IP] [--port N] list [--out FILE]
    net_pull.py [--host IP] [--port N] read <id> <start> <length> <file> [--resume] [--retries N]
                                                         [--verify auto|yes|no] [--force]
    net_pull.py [--host IP] [--port N] verify <id> <start> <length> <file> [--piece N] [--count N]
                                                                          [--reread N]

<length> is a number of bytes, or end (the same as 0) for the rest of the partition.
Numbers may be decimal (30, 1048576) or hex with 0x (0x1e, 0x100000).

A read checks every range it receives against the SHA-256 the console sends after the bytes.
When a transfer breaks off, it is asked again from the bytes already in <file>, up to --retries
times. --resume is only needed to continue a file that is there from an earlier run. After a
break, with such a file, or with --verify yes, the whole file is checked once more against a
hash the console computes over the same range without sending it (HASH).

It writes <file>.sha256 next to the file, in sha256sum -c format.

verify has the console hash the range again, piece by piece, without sending the bytes,
and compares every piece with the file. The pieces are small, because the console goes
on writing to its storage: what it wrote since the dump is then apart from a fault of the
transfer. A hash cannot tell the two apart, so on a piece that does not match the file's,
the piece is read once more and its bytes compared with the file's: a few changed blocks
are writes of the console's, a shifted or missing run of megabytes is a fault. With
<length> end the file's own length is the range. --count N takes N pieces spread over
the range instead of all of them. Run it twice, some time apart, and compare what the
console hashes: a piece whose hash changes in between is one the console writes to.

The console only reads NAND. The requests it takes are ping, list, read and hash.
"""
import argparse
import hashlib
import operator
import os
import queue
import socket
import sys
import threading
import time

DEFAULT_HOST = "10.69.68.225"
DEFAULT_PORT = 28900
CHUNK = 1 << 20
PROGRESS_EVERY = 5.0 # seconds between progress lines while bytes come in
HASH_EVERY = 30.0    # seconds between notes while the console hashes and nothing comes


class ProtocolError(Exception):
    """The console answered something other than what the request asked for."""


class Refused(ProtocolError):
    """The console refused the request. Asking again will not help."""


def log(text):
    print(text, file=sys.stderr, flush=True)


def progress_line(text):
    """A line that replaces itself on a terminal, and a line of its own elsewhere."""
    if sys.stderr.isatty():
        sys.stderr.write("\r\033[K" + text)
    else:
        sys.stderr.write(text + "\n")
    sys.stderr.flush()


def on_terminal():
    return sys.stderr.isatty()


def parse_size(text):
    """A number of bytes, or None for the rest of the partition (end, or 0)."""
    try:
        n = 0 if text == "end" else int(text, 0)
    except ValueError:
        n = -1
    if n < 0:
        sys.exit("length must be a number of bytes (decimal or 0x hex), or end for the rest "
                 "of the partition")
    return None if n == 0 else n


def fmt(n):
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if n < 1024 or unit == "TiB":
            return "%d %s" % (n, unit) if unit == "B" else "%.2f %s" % (n, unit)
        n /= 1024.0


class Heartbeat:
    """Logs a note every HASH_EVERY seconds from start() until stop is set."""

    def __init__(self, text):
        self.text = text
        self.stop = threading.Event()

    def start(self):
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        waited = 0
        while not self.stop.wait(HASH_EVERY):
            waited += HASH_EVERY
            log("%s: %d s so far" % (self.text, waited))


def ask(a, request, on_bytes=None, progress=None):
    """Send one request line on a connection of its own and read the answer.

    PING: the answer line. LIST: (rows, end line). READ and HASH: (sha256 hex, start, length,
    partition size). While the bytes of a READ come in, progress(bytes so far, length) is
    called every PROGRESS_EVERY seconds.
    Raises Refused or ProtocolError, ConnectionError, socket.timeout or OSError.
    """
    s = socket.create_connection((a.host, a.port), timeout=a.timeout)
    try:
        f = s.makefile("rb")
        s.sendall(request.encode("ascii") + b"\n")

        heart = None
        if request.startswith("HASH"):
            # Nothing comes while the console hashes, and on a large range that takes long.
            s.settimeout(None)
            heart = Heartbeat("the console is hashing")
            heart.start()
        try:
            head = f.readline()
        finally:
            if heart:
                heart.stop.set()
        if not head.endswith(b"\n"):
            raise ConnectionError("the console closed the connection without answering")
        head = head.decode("ascii", "replace").rstrip("\r\n")
        if head.startswith("ERR"):
            raise Refused("the console refused: " + head)

        if request == "PING":
            return head
        if request == "LIST":
            rows = []
            line = head # a LIST answer starts with its first row: there is no header line
            while not line.startswith("END"):
                if not line.startswith("PART "):
                    raise ProtocolError("unexpected line during LIST: " + line)
                rows.append(line[5:])
                raw = f.readline()
                if not raw.endswith(b"\n"):
                    raise ConnectionError("the console closed the connection during LIST")
                line = raw.decode("ascii", "replace").rstrip("\r\n")
            return rows, line

        fields = head.split()
        try:
            if fields[0] != "OK":
                raise ValueError
            start, length, size = (int(x) for x in fields[1:4])
        except (ValueError, IndexError):
            raise ProtocolError("unexpected answer to %s: %s" % (request, head))

        digest = hashlib.sha256()
        left = length if request.startswith("READ") else 0 # a HASH sends no bytes, only its hash
        got = 0
        tick = time.time()
        while left:
            data = f.read(min(CHUNK, left))
            if not data:
                raise ConnectionError(
                    "the console stopped after %d of %d bytes" % (length - left, length))
            digest.update(data)
            left -= len(data)
            if on_bytes:
                on_bytes(data)
                got += len(data)
                now = time.time()
                if progress and now - tick >= PROGRESS_EVERY:
                    progress(got, length)
                    tick = now
        tail = f.readline()
        if not tail.endswith(b"\n"):
            raise ConnectionError("the console closed the connection before its hash")
        tail = tail.decode("ascii", "replace").strip()
        if not tail.startswith("SHA256 ") or len(tail) != 7 + 64:
            raise ProtocolError("no hash after the answer to %s: %r" % (request, tail))
        if request.startswith("READ") and tail[7:] != digest.hexdigest():
            # A READ's hash is of the bytes that came; a HASH's is of the range, and is the result.
            raise ProtocolError("hash mismatch: the console says %s, the bytes hash to %s"
                                % (tail[7:], digest.hexdigest()))
        return tail[7:], start, length, size
    finally:
        s.close()


def cmd_ping(a):
    print(ask(a, "PING"))


def cmd_list(a):
    rows, end = ask(a, "LIST")
    lines = ["%5s  %-4s  %14s  %-10s  %s" % ("id", "open", "size", "result", "name")]
    for row in rows:
        fields = row.split(None, 4)
        if len(fields) != 5:
            lines.append("  (unparsed) " + row)
            continue
        pid, openable, size, rc, name = fields
        shown = "" if rc == "00000000" else "0x" + rc
        lines.append("%5s  %-4s  %14s  %-10s  %s"
                     % (pid, "yes" if openable == "1" else "no", size, shown, name))
    if end != "END":
        lines.append(end)
    lines.append("")
    lines.append("pull one with: net_pull.py read <id> <start> <length|end> <file>")
    text = "\n".join(lines)
    print(text)
    if a.out:
        with open(a.out, "w") as out:
            out.write(text + "\n")


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            data = f.read(CHUNK)
            if not data:
                break
            digest.update(data)
    return digest.hexdigest()


def reread_note(a, off, n):
    """The console's bytes of one piece, read once more, against the file's bytes of it."""
    data = bytearray()
    try:
        now, _, _, _ = ask(a, "READ %d %d %d" % (a.id, off, n), on_bytes=data.extend)
    except (ProtocolError, ConnectionError, socket.timeout, OSError) as e:
        return "read again, which failed: %s" % e
    blocks = [] # where 4 KiB blocks differ from the file
    differing = 0
    with open(a.file, "rb") as f:
        f.seek(off)
        pos = 0
        while pos < n:
            mine = f.read(min(CHUNK, n - pos))
            if not mine:
                break
            theirs = data[pos:pos + len(mine)]
            if theirs != mine:
                for b in range(0, len(mine), 65536):
                    if theirs[b:b + 65536] == mine[b:b + 65536]:
                        continue
                    for k in range(0, len(mine) - b, 4096):
                        if theirs[b + k:b + k + 4096] != mine[b + k:b + k + 4096]:
                            blocks.append(pos + b + k)
                            differing += sum(map(operator.ne, theirs[b + k:b + k + 4096],
                                                 mine[b + k:b + k + 4096]))
            pos += len(mine)
    text = "read again: the console now hashes it %s. %d bytes differ from the file, in %d of %d 4 KiB blocks" % (
        now, differing, len(blocks), (n + 4095) // 4096)
    if blocks:
        text += ", from +0x%x to +0x%x" % (blocks[0], blocks[-1])
    return text


def cmd_verify(a):
    if os.path.isdir(a.file) or not os.path.exists(a.file):
        sys.exit("%s is not a file" % a.file)
    fsize = os.path.getsize(a.file)
    length = parse_size(a.length)
    total = length if length is not None else fsize
    if total <= 0 or a.start + total > fsize:
        sys.exit("%s holds %d bytes: there are not %d from byte %d in it" % (a.file, fsize, total, a.start))
    pieces = [(off, min(a.piece, a.start + total - off)) for off in range(a.start, a.start + total, a.piece)]
    if 0 < a.count < len(pieces):
        last = len(pieces) - 1
        picks = sorted({(i * last) // (a.count - 1) for i in range(a.count)})
        pieces = [pieces[i] for i in picks]

    # The file is hashed by a thread of its own, one piece ahead of the console: reading the
    # disk and reading the console's NAND then take the same time instead of adding up.
    local = queue.Queue(maxsize=2)

    def hash_file():
        try:
            with open(a.file, "rb") as f:
                for off, n in pieces:
                    f.seek(off)
                    digest = hashlib.sha256()
                    left = n
                    while left:
                        data = f.read(min(CHUNK, left))
                        if not data:
                            break
                        digest.update(data)
                        left -= len(data)
                    local.put(digest.hexdigest() if left == 0 else None)
        except OSError as e:
            local.put(e)
        local.put(None)

    threading.Thread(target=hash_file, daemon=True).start()
    bad = 0
    rereads = 0
    for i, (off, n) in enumerate(pieces):
        want, _, _, size = ask(a, "HASH %d %d %d" % (a.id, off, n))
        have = local.get()
        if have is None or isinstance(have, OSError):
            sys.exit("%s: it could not be read up to byte %d: %s" % (a.file, off + n, have or "it ends early"))
        if a.start + total > size:
            sys.exit("the console says partition %d is %d bytes: the range goes past its end" % (a.id, size))
        if want == have:
            print("ok %d %d %s" % (off, n, want), flush=True)
        else:
            bad += 1
            print("MISMATCH %d %d console %s file %s" % (off, n, want, have), flush=True)
            if rereads < a.reread:
                rereads += 1
                print("  " + reread_note(a, off, n), flush=True)
        done = sum(n for _, n in pieces[:i + 1])
        log("piece %d of %d: %s  (%s of %s so far)" % (i + 1, len(pieces), "ok" if want == have else "MISMATCH",
            fmt(done), fmt(total)))
    log("%s: %d of %d pieces match what the console reads%s" % (
        a.file, len(pieces) - bad, len(pieces), "" if bad == 0 else "; %d DO NOT" % bad))
    sys.exit(0 if bad == 0 else 1)


def cmd_read(a):
    length = parse_size(a.length)
    if os.path.isdir(a.file):
        sys.exit("%s is a directory" % a.file)
    have = 0
    if os.path.exists(a.file) and not a.force:
        if not a.resume:
            sys.exit("%s exists: --resume to continue it, or --force to start over" % a.file)
        have = os.path.getsize(a.file)
        if length is not None and have >= length:
            sys.exit("%s already holds %d bytes; nothing to do" % (a.file, have))
        if have:
            log("%s: continuing from %s (%d bytes)" % (a.file, fmt(have), have))
    if a.force:
        with open(a.file, "wb"):
            pass
        have = 0

    breaks = 1 if have else 0 # what an earlier run left behind was never checked as a whole
    attempts = 0
    size = None
    out = None # opened when the first bytes come in, so that a refused read leaves no file
    last = [0, time.time()] # [bytes last shown, when]: for the rate

    def keep(data):
        nonlocal out
        if out is None:
            out = open(a.file, "ab")
        out.write(data)

    def show(done, _total):
        now = time.time()
        rate = (done - last[0]) / max(now - last[1], 0.001)
        last[0], last[1] = done, now
        progress_line("%s: %s  (%s/s)" % (a.file, fmt(before + done), fmt(rate)))

    try:
        while True:
            before = have
            last[0], last[1] = 0, time.time()
            request = "READ %d %d %s" % (a.id, a.start + before,
                                         "0" if length is None else length - before)
            try:
                _, got_start, got_len, size = ask(a, request, on_bytes=keep, progress=show)
            except Refused:
                raise
            except (ProtocolError, ConnectionError, socket.timeout, OSError) as e:
                if out:
                    out.flush()
                    have = os.path.getsize(a.file)
                if isinstance(e, ProtocolError):
                    # bytes that did not check out are not kept
                    os.truncate(a.file, before)
                    have = before
                if on_terminal():
                    log("")
                if attempts >= a.retries:
                    sys.exit("gave up with %s in %s: %s" % (fmt(have), a.file, e))
                attempts += 1
                breaks += 1
                log("transfer broke (%s); asking again from %s (%d of %d)"
                    % (e, fmt(have), attempts, a.retries))
                time.sleep(1)
                continue
            if out:
                out.flush()
                have = os.path.getsize(a.file)
            if on_terminal():
                log("")
            if length is None:
                if got_start + got_len >= size:
                    break # the partition's end
            elif have >= length:
                break
    finally:
        if out:
            out.close()

    if length is not None and have != length:
        sys.exit("%s holds %d bytes, wanted %d" % (a.file, have, length))
    end = size if length is None else a.start + length

    if a.verify == "yes" or (a.verify == "auto" and breaks):
        log("checking the whole file against the console's hash of the same range")
        want, _, _, _ = ask(a, "HASH %d %d %s" % (a.id, a.start,
                                                  "0" if length is None else length))
        got = file_sha256(a.file)
        if got != want:
            sys.exit("HASH MISMATCH: the file hashes to %s, the console to %s" % (got, want))
        log("the whole file checks out")

    digest = file_sha256(a.file)
    with open(a.file + ".sha256", "w") as side:
        side.write("%s  %s\n" % (digest, os.path.basename(a.file)))
    log("%s: done, %s of partition %d from %d to %d, sha256 %s"
        % (a.file, fmt(have), a.id, a.start, end, digest))


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=DEFAULT_HOST, help="the console (default %(default)s)")
    p.add_argument("--port", type=int, default=DEFAULT_PORT, help="default %(default)s")
    p.add_argument("--timeout", type=float, default=60.0,
                   help="seconds to wait for the console and between its bytes "
                        "(default %(default)s)")
    sub = p.add_subparsers(dest="command", required=True)
    sub.add_parser("ping")
    pl = sub.add_parser("list")
    pl.add_argument("--out", help="also save the list to this file")
    pr = sub.add_parser("read")
    pr.add_argument("id", type=lambda x: int(x, 0),
                    help="partition id as the list shows it (30, or 0x1e)")
    pr.add_argument("start", type=lambda x: int(x, 0), help="first byte to read")
    pr.add_argument("length", help="number of bytes, or end for the rest of the partition")
    pr.add_argument("file", help="where to put the bytes")
    pr.add_argument("--resume", action="store_true",
                    help="continue a file that is there from an earlier run")
    pr.add_argument("--retries", type=int, default=5,
                    help="how often to ask again after a transfer breaks (default %(default)s)")
    pr.add_argument("--verify", choices=("auto", "yes", "no"), default="auto",
                    help="check the whole file against the console's HASH: after a break or "
                         "with a resumed file, always, or never (default %(default)s)")
    pr.add_argument("--force", action="store_true", help="start over even if the file exists")
    pv = sub.add_parser("verify", help="have the console hash the range again and compare with the file")
    pv.add_argument("id", type=lambda x: int(x, 0), help="partition id as the list shows it (30, or 0x1e)")
    pv.add_argument("start", type=lambda x: int(x, 0), help="first byte of the range")
    pv.add_argument("length", help="number of bytes, or end for the file's own length")
    pv.add_argument("file", help="the file to check")
    pv.add_argument("--piece", type=lambda x: int(x, 0), default=256 << 20,
                    help="bytes per piece (default 256 MiB)")
    pv.add_argument("--reread", type=int, default=10,
                    help="when a piece does not match, read it once more and say how its bytes differ "
                         "from the file's; for at most this many pieces (default %(default)s, 0: none)")
    pv.add_argument("--count", type=int, default=0,
                    help="check only this many pieces, spread over the range (default: all)")
    a = p.parse_args()
    try:
        if a.command == "ping":
            cmd_ping(a)
        elif a.command == "list":
            cmd_list(a)
        elif a.command == "verify":
            cmd_verify(a)
        else:
            cmd_read(a)
    except (ProtocolError, ConnectionError, socket.timeout, OSError) as e:
        sys.exit("%s: %s" % (a.command, e))
    except KeyboardInterrupt:
        sys.exit("interrupted")


if __name__ == "__main__":
    main()
