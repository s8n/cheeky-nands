#!/usr/bin/env python3
"""Apply one mutation to a copy of a source file. Used by test/mutation_check.sh.

    mutate.py <source> <destination>      (the mutation itself arrives on stdin)

stdin holds the original text, then a line that starts with ===> and continues with the
replacement, so that an indented replacement keeps its indentation:

    if (x) {
===>    if (y) {

The original text must appear exactly once in the source. Otherwise nothing is written and the
exit status is 3.
"""
import sys

SEP = "\n===>"


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    with open(src) as f:
        text = f.read()
    parts = sys.stdin.read().split(SEP)
    if len(parts) != 2:
        print("mutation: expected one '===>' line, found %d" % (len(parts) - 1), file=sys.stderr)
        return 3
    old, new = parts[0].strip("\n"), parts[1].strip("\n")
    n = text.count(old)
    if n != 1:
        first = old.splitlines()[0].strip() if old else ""
        print("target text found %d times (expected 1); first line: %s" % (n, first), file=sys.stderr)
        return 3
    with open(dst, "w") as f:
        f.write(text.replace(old, new, 1))
    return 0


sys.exit(main())
