#!/usr/bin/env python3
"""Check --byte-offsets output.

Two properties have to hold.  The mode is supposed to change only the hunk
header, so the bodies must be byte-identical to the line-number form.  And the
offsets in each header have to actually point at the content the hunk shows --
which is the one thing a header full of numbers can get wrong while still
looking plausible.
"""
import subprocess
import sys


def run(cow, args):
    return subprocess.run([cow] + args, capture_output=True).stdout


def parse(out):
    """Split a unified diff into (a_off, a_len, b_off, b_len, body lines)."""
    hunks, cur = [], None
    for line in out.split(b"\n"):
        if line.startswith(b"@@"):
            f = line.split()
            ao, al = (int(x) for x in f[1][1:].split(b","))
            bo, bl = (int(x) for x in f[2][1:].split(b","))
            cur = (ao, al, bo, bl, [])
            hunks.append(cur)
        elif cur is not None and line[:1] in (b" ", b"-", b"+", b"\\"):
            # The backslash keeps "\ No newline at end of file" with its
            # hunk; dropping it would silently add a newline back.
            cur[4].append(line)
    return hunks


def body_of(hunks):
    return [line for h in hunks for line in h[4]]


def side(body, marks):
    """Rebuild one side of the hunk from its prefixed lines."""
    out = []
    for i, line in enumerate(body):
        if line.startswith(b"\\"):
            # "\ No newline at end of file" retracts the newline of the line
            # immediately before it -- but only if that line is on this side,
            # since each side gets its own marker.
            if i > 0 and body[i - 1][:1] in marks and out:
                out[-1] = out[-1][:-1]
            continue
        if line[:1] in marks:
            out.append(line[1:] + b"\n")
    return b"".join(out)


def main():
    cow, pairs = sys.argv[1], sys.argv[2:]
    bad = 0

    for i in range(0, len(pairs), 2):
        a, b = pairs[i], pairs[i + 1]
        line = parse(run(cow, [a, b]))
        byte = parse(run(cow, ["--byte-offsets", a, b]))

        if body_of(line) != body_of(byte):
            print("FAIL %s %s: body differs from line-number mode" % (a, b))
            bad += 1
            continue

        data_a = open(a, "rb").read()
        data_b = open(b, "rb").read()
        for a_off, a_len, b_off, b_len, body in byte:
            if side(body, (b" ", b"-")) != data_a[a_off:a_off + a_len]:
                print("FAIL %s %s: hunk at %d does not match %s there"
                      % (a, b, a_off, a))
                bad += 1
                break
            if side(body, (b" ", b"+")) != data_b[b_off:b_off + b_len]:
                print("FAIL %s %s: hunk at %d does not match %s there"
                      % (a, b, b_off, b))
                bad += 1
                break

    print("byte-offset checks: %d failed" % bad)
    return 1 if bad else 0


sys.exit(main())
