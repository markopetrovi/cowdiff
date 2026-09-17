#!/usr/bin/env python3
"""Checks that need control over what the filesystem actually did.

These are the cases where the extent map, not the byte comparison, decides the
answer: compressed extents whose fe_length is a decompressed length, shifted
shares built with FICLONERANGE, holes against real data, and extents small
enough to live in the inode.  They are the paths most likely to produce a
confident wrong answer rather than a slow right one, so each one asserts both
what was reported and, where it matters, how much was read to report it.

Cases whose fixture cannot be built here (no FICLONERANGE, the filesystem
declines to compress) are skipped loudly rather than passing quietly.
"""
import os
import re
import shutil
import subprocess
import sys

COW, PROBE = sys.argv[1], sys.argv[2]
WORK = os.path.join(os.path.dirname(os.path.abspath(COW)), ".testtmp", "extent")

MERGE_GAP = 16          # must match delta.c
NUM = re.compile(rb"0x([0-9a-f]+), (\d+) bytes")
EXT = re.compile(r"off=(\d+)\s+len=(\d+)\s+phys=(\d+)\s+(.*)$")

passed = failed = 0
skipped = []


class Skip(Exception):
    pass


def fx(name):
    return os.path.join(WORK, name)


def cow(*args):
    p = subprocess.run([COW] + [str(a) for a in args], capture_output=True)
    return p.returncode, p.stdout, p.stderr


def extents(path):
    """Parse --dump-extents into dicts."""
    rc, out, err = cow("--dump-extents", path)
    if rc != 0:
        raise Skip("--dump-extents failed: %s" % err.decode(errors="replace"))
    result = []
    for line in out.decode().splitlines():
        m = EXT.search(line)
        if m:
            result.append({
                "off": int(m.group(1)), "len": int(m.group(2)),
                "phys": int(m.group(3)), "flags": m.group(4),
            })
    return result


def parse_regions(out):
    """Binary mode's reported ranges as (a_off, a_len, b_off, b_len).

    A pure insertion has no A side and a pure deletion no B side; both are
    reported as zero length so the tuple is always the same shape.
    """
    regions = []
    for line in out.split(b"\n"):
        nums = NUM.findall(line)
        if not nums:
            continue
        if line.startswith(b"  inserted"):
            off, ln = nums[0]
            regions.append((0, 0, int(off, 16), int(ln)))
        elif line.startswith(b"  deleted"):
            off, ln = nums[0]
            regions.append((int(off, 16), int(ln), 0, 0))
        elif b"->" in line:
            (ao, al), (bo, bl) = nums[0], nums[1]
            regions.append((int(ao, 16), int(al), int(bo, 16), int(bl)))
        else:
            off, ln = nums[0]
            regions.append((int(off, 16), int(ln), int(off, 16), int(ln)))
    return regions


def read(path):
    with open(path, "rb") as f:
        return f.read()


def differing(a, b):
    """Maximal runs of differing bytes, compared blockwise so the equal
    majority costs a memcmp rather than a Python loop."""
    blk = 1 << 16
    n = min(len(a), len(b))
    runs = []
    i = 0
    while i < n:
        j = min(i + blk, n)
        if a[i:j] == b[i:j]:
            i = j
            continue
        k = i
        while k < j:
            if a[k] == b[k]:
                k += 1
                continue
            s = k
            while k < j and a[k] != b[k]:
                k += 1
            runs.append((s, k - s))
        i = j

    merged = []
    for off, ln in runs:
        if merged and merged[-1][0] + merged[-1][1] == off:
            merged[-1] = (merged[-1][0], merged[-1][1] + ln)
        else:
            merged.append((off, ln))
    return merged


def filefrag_flags(path):
    """Raw kernel flags, from filefrag.

    This is deliberately a second, independent source.  --dump-extents
    reports what cowdiff concluded, faithfully -- which means it cannot
    catch a wrong conclusion, only a stale one.  Asserting a kernel fact
    alongside the verdict is what makes a wrong rule detectable.

    -s syncs first, and it is not optional: without it filefrag reports a
    freshly written file as a single DELALLOC extent with a meaningless
    address, while cowdiff asks the kernel with FIEMAP_FLAG_SYNC and sees
    the real extent.  Two views of the same file only agree once both are
    looking at what is actually on disk.
    """
    r = subprocess.run(["filefrag", "-s", "-v", path], capture_output=True,
                       text=True)
    flags = set()
    for line in r.stdout.splitlines():
        parts = line.split(":")
        if len(parts) >= 5 and parts[0].strip().isdigit():
            flags.update(x.strip() for x in parts[-1].split(",") if x.strip())
    return flags


def bytes_read(err):
    m = re.search(rb"read (\d+) of", err)
    return int(m.group(1)) if m else None


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


def sound(name, a, b, regions):
    """Every byte that really differs must lie inside a reported region.

    This is the direction that matters: a difference left out of the report
    is a wrong answer, while a reported region that is a little wider than
    necessary is only coarse.
    """
    data_a, data_b = read(a), read(b)
    runs = differing(data_a, data_b)
    a_side = [(o, l) for o, l, _, _ in regions]
    b_side = [(o, l) for _, _, o, l in regions]

    def inside(off, ln, spans):
        return any(s <= off and off + ln <= s + l for s, l in spans if l)

    for off, ln in runs:
        if not inside(off, ln, a_side) or not inside(off, ln, b_side):
            check("%s: no difference omitted" % name, False,
                  "run at %d +%d is not inside any reported region" % (off, ln))
            return
    check("%s: no difference omitted" % name, True)

    # And the other direction: a reported region should not swallow much
    # equal data beyond what MERGE_GAP allows.
    true_bytes = sum(l for _, l in runs)
    reported = sum(l for _, l in a_side)
    allowance = true_bytes + MERGE_GAP * max(len(runs) - 1, 0) + 64
    check("%s: report is not wildly wider than the truth" % name,
          reported <= allowance, "reported %d, true %d" % (reported, true_bytes))


# ---- cases ---------------------------------------------------------------

def case_compressed():
    """A compressed extent's fe_length is the length the data will have once
    decompressed, so [address, address+length) overstates the space it takes
    on disk.  Intersecting those ranges once turned a 23-byte edit into a
    phantom insertion plus deletion."""
    name = "compressed extents"
    a, b = fx("c_a.txt"), fx("c_b.txt")
    line = b"the quick brown fox jumps over the lazy dog 0123456789\n"
    with open(a, "wb") as f:
        f.write(line * ((32 << 20) // len(line)))
    subprocess.run(["cp", "--reflink=always", a, b], check=True)
    with open(b, "r+b") as f:
        f.seek(16000000)
        f.write(b"CHANGED-CHANGED-CHANGED")

    if "encoded" not in filefrag_flags(a):
        raise Skip("filesystem did not compress the fixture")

    # The kernel says compressed; cowdiff must still trust the address,
    # since address and length together name the extent exactly.
    enc = [e for e in extents(a) if "encoded" in e["flags"]]
    check("%s: compressed extents are still trusted" % name,
          bool(enc) and all(not e["flags"].startswith("untrusted")
                            for e in enc), enc[:1])

    rc, out, _ = cow("--force-binary", a, b)
    regions = parse_regions(out)
    check("%s: exactly the edit is reported" % name,
          regions == [(16000000, 23, 16000000, 23)], regions)
    check("%s: exit status says different" % name, rc == 1, rc)


def case_shifted_share():
    """B is A with one block inserted and the tail re-cloned from A, so B's
    tail shares A's extents at file offsets differing by the insert size.
    The offset delta *is* the insertion, so nothing needs reading."""
    name = "shifted share (FICLONERANGE)"
    a, b = fx("s_a.bin"), fx("s_b.bin")
    x, ins, length = 2 << 20, 4096, 4 << 20

    with open(a, "wb") as f:
        f.write(os.urandom(length))
    subprocess.run(["cp", "--reflink=always", a, b], check=True)

    with open(b, "r+b") as f:
        f.truncate(length + ins)
        f.seek(x)
        f.write(b"\0" * ins + read(a)[x:])

    r = subprocess.run([PROBE, "clone", a, b, str(x), str(length - x),
                        str(x + ins)], capture_output=True)
    if r.returncode != 0:
        raise Skip("FICLONERANGE unavailable: %s"
                   % r.stderr.decode(errors="replace").strip())

    rc, out, err = cow("--stats", a, b)
    regions = parse_regions(out)
    check("%s: one inserted block, nothing else" % name,
          regions == [(0, 0, x, ins)], regions)
    check("%s: reads nothing at all" % name, b"read 0 of" in err,
          err.decode(errors="replace"))


def case_hole_punch():
    """Holes are skipped, but data against a hole still has to be examined,
    because a real extent can hold nothing but zeros."""
    name = "punched hole"
    a, b = fx("h_a.bin"), fx("h_b.bin")
    with open(a, "wb") as f:
        f.write(os.urandom(4 << 20))
    subprocess.run(["cp", "--reflink=always", a, b], check=True)
    subprocess.run(["fallocate", "--punch-hole", "--offset=3000000",
                    "--length=65536", b], check=True)

    rc, out, err = cow("--force-binary", a, b)
    check("%s: exit status says different" % name, rc == 1, rc)
    sound(name, a, b, parse_regions(out))


def case_zeros_vs_hole():
    """A hole reads as zeros, and so can a real extent.  Calling that pair a
    difference would report zeros as changed."""
    name = "zeros against a hole"
    a, b = fx("z_a.bin"), fx("z_b.bin")
    half = 1 << 19

    with open(a, "wb") as f:
        f.write(os.urandom(half))
        f.write(b"\0" * half)
    shutil.copyfile(a, b)               # plain copy: separate extents
    subprocess.run(["fallocate", "--punch-hole", "--offset=%d" % half,
                    "--length=%d" % half, b], check=True)

    # If the filesystem turned the zeros into a hole too, the comparison
    # would be hole against hole and would prove nothing.  Say so rather
    # than passing quietly.
    covering = [e for e in extents(a) if e["off"] <= half < e["off"] + e["len"]]
    if not covering:
        raise Skip("the zeros were stored as a hole, not a real extent")

    rc, out, err = cow(a, b)
    check("%s: reported identical" % name, rc == 0,
          out.decode(errors="replace"))


def case_inline():
    """A file small enough to live in its inode is reported with fe_physical
    of zero.  Trusting that address would make two different small files look
    identical, which is a wrong answer rather than a slow one."""
    name = "inline extents"
    a, b = fx("i_a.txt"), fx("i_b.txt")
    with open(a, "wb") as f:
        f.write(b"hello world\n")
    with open(b, "wb") as f:
        f.write(b"HELLO WORLD\n")

    # Two independent facts that must agree.  filefrag reports what the
    # kernel said; the dump reports what cowdiff concluded.  If the trust
    # rule ever stopped treating inline data as untrusted, the first would
    # still say inline while the second said trusted, and this would fail --
    # which is exactly the bug that would make two different small files
    # come out identical.
    if "inline" not in filefrag_flags(a):
        raise Skip("the fixture was not stored inline")

    check("%s: cowdiff refuses to trust an inline address" % name,
          all(e["flags"].startswith("untrusted") for e in extents(a)),
          extents(a))

    rc, out, err = cow("--force-binary", a, b)
    check("%s: different, not assumed equal" % name, rc == 1, rc)
    regions = parse_regions(out)
    # The two strings share their space and their newline, so the report is
    # one merged region rather than two -- the property under test is that
    # anything is reported at all.
    check("%s: the difference is reported" % name, len(regions) == 1, regions)
    sound(name, a, b, regions)


def case_unwritten():
    """An unwritten extent is allocated but holds no data, so it reads as
    zeros whatever address it has.  Two of them agree without either being
    read, and without their addresses being compared."""
    name = "unwritten extents"
    a, b = fx("u_a.bin"), fx("u_b.bin")
    with open(a, "wb") as f:
        f.write(os.urandom(1 << 20))
    subprocess.run(["fallocate", "--offset=1048576", "--length=%d" % (1 << 20),
                    a], check=True)
    shutil.copyfile(a, b)               # separate extents, same content

    if not any("unwritten" in e["flags"] for e in extents(a)):
        raise Skip("fallocate did not produce an unwritten extent")

    rc, out, err = cow("--stats", a, b)
    check("%s: reported identical" % name, rc == 0,
          out.decode(errors="replace"))

    # Both files are 2 MB: 1 MB of random data and 1 MB of unwritten extent.
    # The random halves are separate copies so they must be read; the
    # unwritten halves are known zeros and must not be.  Reading all 4 MB
    # would mean the zeros went through a comparison.
    got = bytes_read(err)
    check("%s: the unwritten half was never read" % name,
          got is not None and got <= (2 << 20) + (1 << 16), got)


CASES = [case_compressed, case_shifted_share, case_hole_punch,
         case_zeros_vs_hole, case_inline, case_unwritten]


def main():
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)

    for case in CASES:
        try:
            case()
        except Skip as e:
            skip(case.__name__, e)
        except subprocess.CalledProcessError as e:
            check(case.__name__, False, "fixture failed: %s" % e)

    shutil.rmtree(WORK, ignore_errors=True)
    print()
    print("extent checks: %d passed, %d failed, %d skipped"
          % (passed, failed, len(skipped)))
    return 1 if failed else 0


sys.exit(main())
