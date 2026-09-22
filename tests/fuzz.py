#!/usr/bin/env python3
"""Fuzz cowdiff against the properties that do not need a hand-built shape.

The rest of the suite is shapes a person thought of, and §15 of the notes is
what happened the last time that was all there was: the worst bug the tool has
had lived in a path no shape reached.  This builds pairs at random instead and
checks the things that are true of every correct answer.

Four oracles, in the order they matter:

  * **The invariant (§2).**  `-q` returning 0 must mean the bytes really are
    equal.  A false "identical" is silent — exit 0, no output, and whatever
    asked concludes the files match — so this is the one that counts.  Every
    case runs it.
  * **patch(1) applied to A must rebuild B.**  The text output claims to be a
    unified diff; this is that claim, tested without needing GNU diff to agree
    (more than one diff can be correct, and this one is allowed to differ).
  * **The exit status must agree with diff(1)** — 0, 1 or 2 for the same pair.
  * **Binary mode must not omit a difference**: every run of differing bytes
    must sit inside a reported region, on both sides, and the regions must not
    overlap or run past the end of a file.  Only shapes the tool compares at
    the same offsets are checked this way, since where it aligned *differently*
    offsets there is nothing to compare byte for byte.

A case that fails keeps its two files under .testtmp/fuzz/keep/ and prints
what to run to see it again.  Seeds are the only source of variation, so a
failure reproduces from its seed:
    python3 tests/fuzz.py ./cowdiff --seed 7 --cases 150
"""
import argparse
import os
import random
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(ROOT, ".testtmp", "fuzz")
NUM = re.compile(rb"0x([0-9a-f]+), (\d+) bytes")

MODES = ["text", "text", "binary", "same-length", "sparse", "sparse", "reflink"]


# ---- shapes ---------------------------------------------------------------

def edit_text(rng, lines, alphabet, edits, binary=False):
    """A line-structured file, then a few edits applied to a copy of it."""
    body = []
    for _ in range(lines):
        k = rng.randrange(alphabet)
        body.append(("line %d " % k).encode() + rng.choice([b"", b"x", b"yy"]))
    a = b"\n".join(body)
    if rng.random() < 0.8:
        a += b"\n"
    b = bytearray(a)
    for _ in range(edits):
        op = rng.choice(["del", "ins", "subst", "trunc", "append", "nl"])
        if op == "del" and len(b) > 4:
            i = rng.randrange(len(b))
            del b[i:i + rng.randint(1, 8)]
        elif op == "ins":
            i = rng.randrange(len(b) + 1)
            b[i:i] = rng.choice([b"new line\n", b"X", b"\n", b"ins\n"]) * rng.randint(1, 3)
        elif op == "subst" and len(b) > 4:
            b[rng.randrange(len(b) - 1)] = (rng.randrange(256) if binary
                                            else ord("Z"))
        elif op == "trunc" and len(b) > 4:
            b = b[:rng.randrange(1, len(b))]
        elif op == "append":
            b += rng.choice([b"appended\n", b"tail", b"\n"])
        else:                                   # nl: add or drop the last one
            if b.endswith(b"\n"):
                b = b[:-1]
            else:
                b += b"\n"
    return bytes(a), bytes(b)


def shape_sparse(rng):
    """Hole, zeros and data at different offsets in the two files.

    This is the shape §15.1 was found in: a hole ends where the next extent
    begins, and a comparison that runs one on to the end of the range reads
    the data after it as zeros.
    """
    size = rng.choice([1, 2, 4, 8]) * 4096
    specs = []
    for _ in range(2):
        spec, pos = [], 0
        while pos < size:
            ln = max(4096, min(rng.choice([4096, 8192, size // 4 or 4096]),
                               size - pos))
            spec.append((rng.choice(["hole", "data", "zeros"]), pos, ln))
            pos += ln
        specs.append(spec)
    return size, specs[0], specs[1]


def write_spec(path, spec, size):
    with open(path, "wb") as f:
        for kind, off, ln in spec:
            f.seek(off)
            if kind == "data":
                f.write(bytes([65 + (off // 4096) % 26]) * ln)
            elif kind == "zeros":
                f.write(b"\0" * ln)
        f.truncate(size)


# ---- reading the output ---------------------------------------------------

def regions_from(out):
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


def differing_runs(a, b):
    """Maximal runs of differing bytes at the same offsets."""
    n = min(len(a), len(b))
    runs, i = [], 0
    while i < n:
        if a[i] == b[i]:
            i += 1
            continue
        s = i
        while i < n and a[i] != b[i]:
            i += 1
        runs.append((s, i - s))
    return runs


def uncovered(runs, spans):
    """The first run not inside a span; both lists are sorted and disjoint."""
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


# ---- the case runner ------------------------------------------------------

def complain(name, detail=""):
    """Say what went wrong.  The case stops at the first complaint it makes:
    the later oracles read the same output, so a second complaint about it is
    noise around the first."""
    print("FAIL %s" % name)
    if detail:
        print("       %s" % str(detail)[:600])


def keep(tag, pa, pb):
    d = os.path.join(WORK, "keep")
    os.makedirs(d, exist_ok=True)
    for src, side in ((pa, "a"), (pb, "b")):
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(d, "%s_%s" % (tag, side)))
    print("       kept: %s/keep/%s_{a,b}  (a and b are the two files)"
          % (WORK, tag))


def run_case(cow, rng, tag, args):
    """Build one pair, run every oracle that applies to it.  Returns True if
    nothing complained."""
    pa, pb = os.path.join(WORK, "a"), os.path.join(WORK, "b")
    mode = rng.choice(MODES)
    same_offsets = False
    want_a = want_b = None

    if mode == "sparse":
        size, sa, sb = shape_sparse(rng)
        write_spec(pa, sa, size)
        write_spec(pb, sb, size)
        same_offsets = True                # unshared, equal length
    elif mode == "same-length":
        # Incompressible, swapped one byte at a time: enough separate runs of
        # differing bytes to fill the delta list, which is where the coarse
        # fallback lives (§15.3).
        n = rng.choice([8192, 65536, 300000])
        a = bytearray(rng.randbytes(n))
        b = bytearray(a)
        for _ in range(rng.randint(1, 3000)):
            b[rng.randrange(n)] = rng.randrange(256)
        want_a, want_b = bytes(a), bytes(b)
        with open(pa, "wb") as f:
            f.write(want_a)
        with open(pb, "wb") as f:
            f.write(want_b)
        same_offsets = True
    elif mode == "reflink":
        a, b = edit_text(rng, rng.choice([1, 2, 5, 40, 400]),
                         rng.choice([1, 2, 20]), rng.randint(1, 6))
        with open(pa, "wb") as f:
            f.write(a)
        # Rewriting in place keeps whatever extents the copy shared, which is
        # the point of the mode; the copy itself is best-effort, since not
        # every filesystem can reflink.
        subprocess.run(["cp", "--reflink=always", pa, pb], check=False,
                       stderr=subprocess.DEVNULL)
        with open(pb, "r+b") as f:
            f.write(b)
    else:
        a, b = edit_text(rng, rng.choice([1, 2, 5, 40, 400, 3000]),
                         rng.choice([1, 2, 3, 20]), rng.randint(1, 6),
                         binary=(mode == "binary"))
        if mode == "binary" and rng.random() < 0.5:
            a += b"\0" + rng.randbytes(rng.randint(0, 200))
            b += b"\0" + rng.randbytes(rng.randint(0, 200))
        with open(pa, "wb") as f:
            f.write(a)
        with open(pb, "wb") as f:
            f.write(b)

    da = open(pa, "rb").read()
    db = open(pb, "rb").read()
    if want_a is not None and (da, db) != (want_a, want_b):
        # A fixture that is not what it was built to be would have the whole
        # case testing something else, so say so rather than compare on.
        complain(name + ": the fixture on disk is not what was built", "")
        return False
    equal = da == db
    name = "%s %s" % (tag, mode)

    # 1. the invariant
    p = subprocess.run([cow, "-q", pa, pb], capture_output=True)
    if p.returncode == 0 and not equal:
        complain(name + ": -q said identical, and they differ",
                 "%d/%d bytes, first difference at %s"
                 % (len(da), len(db),
                    next((i for i in range(min(len(da), len(db)))
                          if da[i] != db[i]), "n/a (length)")))
        keep(tag, pa, pb)
        return False

    # 2. the status agrees with diff
    if args.diff:
        d = subprocess.run([args.diff, "-q", pa, pb], capture_output=True)
        if (p.returncode != 0) != (d.returncode != 0):
            complain(name + ": status disagrees with diff",
                     "cowdiff %d, diff %d" % (p.returncode, d.returncode))
            keep(tag, pa, pb)
            return False

    if equal:
        return True

    # 3. the text output is a patch that rebuilds B
    if args.patch and b"\0" not in da[:4096] and b"\0" not in db[:4096]:
        p = subprocess.run([cow, "-U3", pa, pb], capture_output=True)
        rec = os.path.join(WORK, "recon")
        shutil.copyfile(pa, rec)
        r = subprocess.run([args.patch, "--silent", "--force", rec],
                           input=p.stdout, capture_output=True)
        if r.returncode != 0 or open(rec, "rb").read() != db:
            complain(name + ": patch did not rebuild B",
                     "patch said %d; the diff was %d bytes"
                     % (r.returncode, len(p.stdout)))
            keep(tag, pa, pb)
            with open(os.path.join(WORK, "keep", tag + "_diff"), "wb") as f:
                f.write(p.stdout)
            return False

    # 4. binary mode reports every difference, once
    if same_offsets:
        p = subprocess.run([cow, "--force-binary", pa, pb],
                           capture_output=True)
        regions = regions_from(p.stdout)
        a_side = [(o, l) for o, l, _, _ in regions if l]
        b_side = [(o, l) for _, _, o, l in regions if l]
        runs = differing_runs(da, db)
        missed = (uncovered(runs, a_side), uncovered(runs, b_side))
        bad_overlap = (overlaps(a_side), overlaps(b_side))
        too_much = sum(l for _, l in a_side) > len(da)
        if any(missed) or any(bad_overlap) or too_much:
            complain(name + ": binary report is wrong",
                     "omitted %s/%s, overlap %s/%s, reported %d of %d bytes"
                     % (missed[0], missed[1], bad_overlap[0], bad_overlap[1],
                        sum(l for _, l in a_side), len(da)))
            keep(tag, pa, pb)
            return False

    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cow", nargs="?", default=os.path.join(ROOT, "cowdiff"))
    ap.add_argument("--cases", type=int, default=60,
                    help="cases per seed (default 40)")
    ap.add_argument("--seeds", type=int, default=8,
                    help="how many seeds, starting at 1 (default 4)")
    ap.add_argument("--seed", type=int,
                    help="run this one seed instead of the range")
    args = ap.parse_args()

    if not os.access(args.cow, os.X_OK):
        sys.exit("fuzz: %s is not executable: build first" % args.cow)
    args.patch = shutil.which("patch")
    args.diff = shutil.which("diff")
    if not args.patch:
        print("note: patch(1) is not installed: the text oracle is skipped")
    if not args.diff:
        print("note: diff(1) is not installed: the status oracle is skipped")

    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)

    passed = failed = 0
    seeds = [args.seed] if args.seed else list(range(1, args.seeds + 1))
    for seed in seeds:
        rng = random.Random(seed)
        for i in range(args.cases):
            if run_case(args.cow, rng, "seed%d.%d" % (seed, i), args):
                passed += 1
            else:
                failed += 1
                break       # one failure is enough: stop and report it
        if failed:
            break

    print("fuzz: %d cases passed, %d failed" % (passed, failed))
    if not failed:
        shutil.rmtree(WORK, ignore_errors=True)
    return 1 if failed else 0


sys.exit(main())
