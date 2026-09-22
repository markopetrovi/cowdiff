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

    filefrag is e2fsprogs, not part of a minimal system, so a machine
    without it skips the cases that need a second opinion rather than
    failing them: nothing is wrong with the binary in that case.
    """
    try:
        r = subprocess.run(["filefrag", "-s", "-v", path], capture_output=True,
                           text=True)
    except FileNotFoundError:
        raise Skip("filefrag is not installed")
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

def block_phys(path, blk):
    """The physical address of each blk-sized block, or None if unanswerable.

    Addresses are contiguous inside an extent, so a block's address is the
    extent's plus its offset in it.  A compressed extent reports the length the
    data will have once decompressed, so that arithmetic would be nonsense
    there; and a hole is not a share at all.  Either way the caller skips
    rather than asserting something it cannot know.
    """
    es = extents(path)
    if any("encoded" in e["flags"] for e in es):
        return None
    size = os.path.getsize(path)
    out = []
    for off in range(0, size, blk):
        e = next((e for e in es if e["off"] <= off < e["off"] + e["len"]), None)
        if e is None:
            return None
        out.append(e["phys"] + (off - e["off"]))
    return out


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


def case_crossed_share():
    """Shares that sit at *different* offsets in the two files.

    A match found by address proves the two ranges hold equal content; it does
    not prove those bytes agree at the same offsets, and the same offsets are
    what the verdict is about.  Cross the shares -- A's first block pointing at
    B's second, and A's second at B's first -- and every chain available is a
    shifted one, whose gaps are one-sided "insertions" and are reported as
    differences *without reading anything*.  Two byte-identical files came out
    "different", exit 1, having read nothing: a wrong "differ" is the loud
    direction, but it is still a wrong answer, and it is the one an exit status
    carries.

    Built by cloning A's two halves into B in the other order.  They have to be
    written in two calls: one write of both makes the filesystem share the
    blocks with each other inside A, and then there is nothing to cross.
    """
    name = "crossed share (FICLONERANGE at a different offset)"
    a, b = fx("x_a.bin"), fx("x_b.bin")
    blk = 4096
    first, second = os.urandom(blk), os.urandom(blk)

    def build(path, lo, hi):
        """Two blocks, written separately so they are separate extents."""
        with open(path, "wb") as f:
            f.write(lo)
        with open(path, "ab") as f:
            f.write(hi)

    def clone(src_off, dst_off):
        r = subprocess.run([PROBE, "clone", a, b, str(src_off), str(blk),
                            str(dst_off)], capture_output=True)
        if r.returncode != 0:
            raise Skip("FICLONERANGE unavailable: %s"
                       % r.stderr.decode(errors="replace").strip())

    def cross():
        """B := A's second block, then A's first."""
        with open(b, "wb") as f:
            f.truncate(2 * blk)
        clone(blk, 0)
        clone(0, blk)

    build(a, first, first)                  # identical bytes, two blocks
    cross()
    pa, pb = block_phys(a, blk), block_phys(b, blk)
    if pa is None or pb is None or pa[0] == pa[1] or \
       pa[0] != pb[1] or pa[1] != pb[0]:
        raise Skip("the filesystem did not lay the blocks out crossed: "
                   "%s vs %s" % (pa, pb))

    rc, out, err = cow("--force-binary", a, b)
    check("%s: identical bytes, reported identical" % name, rc == 0,
          "%s %s" % (rc, out.decode(errors="replace")))
    rc, out, err = cow("-q", a, b)
    check("%s: -q is as quiet as diff -q" % name, rc == 0 and not out,
          (rc, out))

    # The same crossing with blocks that differ: the files really do differ,
    # and at the same offsets, so the report has to cover that.
    build(a, first, second)
    cross()
    rc, out, err = cow("--force-binary", a, b)
    check("%s: different blocks are still reported different" % name, rc == 1,
          rc)
    sound(name + ", different blocks", a, b, parse_regions(out))

    # Control: the same two blocks cloned at matching offsets.  Nothing is
    # crossed, nothing is dropped, and the answer must not change.
    build(a, first, second)
    with open(b, "wb") as f:
        f.truncate(2 * blk)
    clone(0, 0)
    clone(blk, blk)
    rc, out, err = cow("--force-binary", a, b)
    check("%s: matching offsets unchanged" % name, rc == 0,
          "%s %s" % (rc, out.decode(errors="replace")))


def case_shifted_equal_span():
    """A gap between two shifted matches whose two sides hold equal bytes.

    A chain that has moved offsets can leave an interior gap whose spans are
    the same bytes at different offsets: the alignment shifted, the content did
    not.  There is nothing to report there -- the shift is already reported by
    the gap that opened it -- and reporting it anyway named a region as changed
    whose two sides are byte-identical, in binary output where no line diff
    follows to trim the claim back.

    A = S1|M|S2 and B = PAD|S1|M|S2, with S1 and S2 cloned from A one block
    further along.  The middle block has to be a copy that is *not* shared, or
    it becomes an ordinary match at identical offsets and the gap it should
    leave does not exist; writing it in place afterwards gives it an extent of
    its own holding the same bytes.
    """
    name = "shifted span that is byte-identical"
    a, b = fx("e_a.bin"), fx("e_b.bin")
    blk = 4096
    s1, m, s2, pad = (os.urandom(blk) for _ in range(4))

    with open(a, "wb") as f:
        f.write(s1 + m + s2)
    with open(b, "wb") as f:
        f.write(pad + s1 + m + s2)
    with open(b, "r+b") as f:               # an unshared copy of the middle
        f.seek(2 * blk)
        f.write(m)

    for src, dst in ((0, blk), (2 * blk, 3 * blk)):
        r = subprocess.run([PROBE, "clone", a, b, str(src), str(blk), str(dst)],
                           capture_output=True)
        if r.returncode != 0:
            raise Skip("FICLONERANGE unavailable: %s"
                       % r.stderr.decode(errors="replace").strip())

    pa, pb = block_phys(a, blk), block_phys(b, blk)
    if pa is None or pb is None or len(pa) != 3 or len(pb) != 4 or \
       pa[0] != pb[1] or pa[2] != pb[3] or pa[1] == pb[2]:
        raise Skip("the filesystem did not lay the blocks out as the case "
                   "needs: %s vs %s" % (pa, pb))

    rc, out, err = cow("--force-binary", a, b)
    regions = parse_regions(out)
    check("%s: only the insertion is reported" % name,
          regions == [(0, 0, 0, blk)], regions)
    check("%s: exit status says different" % name, rc == 1, rc)

    # The general form of the same property, so that a fixture which drifts
    # into some other shape is still caught: a region with two sides may not
    # claim a difference between bytes that are equal.
    data_a, data_b = read(a), read(b)
    liar = next((r for r in regions
                 if r[1] and r[3]
                 and data_a[r[0]:r[0] + r[1]] == data_b[r[2]:r[2] + r[3]]), None)
    check("%s: no region claims a difference between equal bytes" % name,
          liar is None, liar)


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


def case_hole_then_data():
    """A hole ends where the next extent begins -- not at the end of the range.

    One side is a hole to the end of the file; the other has a hole of the
    same length and real data after it.  Treating the hole as zeros "from here
    on" puts that data inside a piece whose answer was already decided, so
    every byte of the difference was skipped and the pair came out identical.
    That is the one direction of error this tool must never make.
    """
    name = "hole followed by data"
    a, b = fx("hd_a.bin"), fx("hd_b.bin")
    hole, tail = 65536, 4096

    with open(a, "wb") as f:
        f.truncate(hole)                # never written: a hole
        f.seek(hole)
        f.write(b"A" * tail)            # real data straight after it
    with open(b, "wb") as f:
        f.truncate(hole + tail)         # all hole: reads as zeros

    # If the filesystem stored the data as a hole, or left an extent over the
    # hole, the comparison being made is a different one and this case would
    # not be testing its named path.
    if not any(e["off"] == hole for e in extents(a)):
        raise Skip("the data after the hole was not stored as its own extent")
    if any(e["off"] < hole for e in extents(b)):
        raise Skip("B's hole was stored as an extent")

    rc, out, err = cow("--force-binary", a, b)
    check("%s: not reported identical" % name, rc == 1, out)
    sound(name, a, b, parse_regions(out))


def case_hole_then_zeros():
    """The zero test stops where the hole does.

    Both sides start with a hole of the same length; one has data after it and
    the other a real extent of zeros.  "Both read as zeros" is true of the
    hole and false of what follows it, so a piece that spans the boundary
    decides the data's answer with the hole's.
    """
    name = "hole then data against hole then zeros"
    a, b = fx("hz_a.bin"), fx("hz_b.bin")
    hole, tail = 65536, 4096

    with open(a, "wb") as f:
        f.truncate(hole)
        f.seek(hole)
        f.write(b"A" * tail)
    with open(b, "wb") as f:
        f.truncate(hole)
        f.seek(hole)
        f.write(b"\0" * tail)

    if any(e["off"] < hole for e in extents(a) + extents(b)):
        raise Skip("a hole was stored as an extent")

    rc, out, err = cow("--force-binary", a, b)
    check("%s: not reported identical" % name, rc == 1, out)
    sound(name, a, b, parse_regions(out))


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


CASES = [case_compressed, case_shifted_share, case_crossed_share,
         case_shifted_equal_span, case_hole_punch, case_zeros_vs_hole,
         case_hole_then_data, case_hole_then_zeros, case_inline,
         case_unwritten]


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
