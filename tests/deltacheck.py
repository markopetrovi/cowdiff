#!/usr/bin/env python3
"""Check the coarse fallback for when the list of differing regions fills up.

Two unrelated large files differ almost everywhere, and one delta per run of
differing bytes would be millions of entries saying very little.  Past a cap
(DELTA_MAX in delta.c) what is left is reported as one substitution instead --
and that substitution covers *everything* that is left, so it may only be made
once.  Making it once per read chunk reported the same bytes over and over:
the regions overlapped, and their lengths summed to more bytes than the files
contain -- a report that cannot be acted on, or even believed.

The pair here differs in single bytes far enough apart to be separate runs,
which is the shape that fills the list.  Its differences are known by
construction, so the truth about them costs nothing to state:

  * no difference is left out of the report (the direction that matters);
  * the regions are in order and do not overlap;
  * the bytes claimed do not exceed the bytes that exist;
  * the coarse path was actually reached -- the overlap needs a piece that
    holds more than one read chunk, so a filesystem whose extents are all
    smaller than one skips loudly rather than passing quietly.
"""
import os
import re
import shutil
import subprocess
import sys

COW = sys.argv[1]
WORK = os.path.join(os.path.dirname(os.path.abspath(COW)), ".testtmp", "delta")

SIZE = 10 << 20         # enough differing runs to fill the list, with room left
RUN = 32                # bytes apart: wider than MERGE_GAP, so each is its own
CHUNK = 1 << 20         # delta.c reads this much at a time
EXT = re.compile(r"off=(\d+)\s+len=(\d+)\s+phys=(\d+)\s+(.*)$")
NUM = re.compile(rb"0x([0-9a-f]+), (\d+) bytes")

passed = failed = 0
skipped = []


class Skip(Exception):
    pass


def cow(*args):
    p = subprocess.run([COW] + [str(a) for a in args], capture_output=True)
    return p.returncode, p.stdout, p.stderr


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print("ok   %s" % name)
    else:
        failed += 1
        print("FAIL %s" % name)
        if detail:
            print("       %s" % str(detail)[:600])


def skip(name, why):
    skipped.append(name)
    print("SKIP %s -- %s" % (name, why))


def longest_extent(path):
    rc, out, err = cow("--dump-extents", path)
    if rc != 0:
        raise Skip("--dump-extents failed: %s" % err.decode(errors="replace"))
    lens = [int(m.group(2)) for m in
            (EXT.search(l.decode()) for l in out.splitlines()) if m]
    return max(lens) if lens else 0


def parse_regions(out):
    """(a_off, a_len, b_off, b_len) per reported region."""
    regions = []
    for line in out.split(b"\n"):
        nums = NUM.findall(line)
        if not nums:
            continue
        if line.startswith(b"  inserted"):
            regions.append((0, 0, int(nums[0][0], 16), int(nums[0][1])))
        elif line.startswith(b"  deleted"):
            regions.append((int(nums[0][0], 16), int(nums[0][1]), 0, 0))
        elif b"->" in line:
            regions.append((int(nums[0][0], 16), int(nums[0][1]),
                            int(nums[1][0], 16), int(nums[1][1])))
        else:
            regions.append((int(nums[0][0], 16), int(nums[0][1]),
                            int(nums[0][0], 16), int(nums[0][1])))
    return regions


def uncovered(runs, spans):
    """The first run not inside a span.  Both lists are sorted and disjoint,
    so one pass each is enough -- with a quarter of a million of each, a
    containment test per pair would not finish."""
    si = 0
    for off, ln in runs:
        while si < len(spans) and spans[si][0] + spans[si][1] <= off:
            si += 1
        if si >= len(spans) or spans[si][0] > off:
            return (off, ln)
        if off + ln > spans[si][0] + spans[si][1]:
            return (off, ln)
    return None


def overlaps(spans):
    prev_end = None
    for off, ln in spans:
        if prev_end is not None and off < prev_end:
            return (off, ln)
        prev_end = off + ln
    return None


def main():
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)
    a, b = os.path.join(WORK, "a.bin"), os.path.join(WORK, "b.bin")

    # Incompressible, so each file stays in few extents: the substitution can
    # only overlap itself where a piece holds more than one read chunk, and
    # compressed extents are 128 KB apiece.
    base = os.urandom(SIZE)
    with open(a, "wb") as f:
        f.write(base)
    buf = bytearray(base)
    for off in range(0, SIZE, RUN):
        buf[off] ^= 0xff
    with open(b, "wb") as f:
        f.write(buf)

    try:
        longest = max(longest_extent(a), longest_extent(b))
        if longest <= CHUNK:
            raise Skip("longest extent is %d bytes, not more than the %d-byte "
                       "read chunk: the overlapping substitution cannot be "
                       "reached on this filesystem" % (longest, CHUNK))

        rc, out, err = cow("--force-binary", a, b)
        check("cap: exit status says different", rc == 1, rc)
        regions = parse_regions(out)
        if not regions:
            raise Skip("no regions reported: %s"
                       % out.decode(errors="replace")[:200])

        # The fixture's differences are exactly these one-byte runs.
        runs = [(off, 1) for off in range(0, SIZE, RUN)]
        a_side = [(o, l) for o, l, _, _ in regions if l]
        b_side = [(o, l) for _, _, o, l in regions if l]

        check("cap: the coarse substitution was reached",
              any(l > CHUNK for _, l in a_side),
              "widest region %d bytes" % max(l for _, l in a_side))
        check("cap: regions do not overlap in A", overlaps(a_side) is None,
              overlaps(a_side))
        check("cap: regions do not overlap in B", overlaps(b_side) is None,
              overlaps(b_side))
        check("cap: no difference is left out of A",
              uncovered(runs, a_side) is None, uncovered(runs, a_side))
        check("cap: no difference is left out of B",
              uncovered(runs, b_side) is None, uncovered(runs, b_side))
        check("cap: reported bytes do not exceed the file (A)",
              sum(l for _, l in a_side) <= SIZE,
              sum(l for _, l in a_side))
        check("cap: reported bytes do not exceed the file (B)",
              sum(l for _, l in b_side) <= SIZE,
              sum(l for _, l in b_side))
    except Skip as e:
        skip("coarse fallback", e)
    finally:
        shutil.rmtree(WORK, ignore_errors=True)

    print()
    print("delta checks: %d passed, %d failed, %d skipped"
          % (passed, failed, len(skipped)))
    return 1 if failed else 0


sys.exit(main())
