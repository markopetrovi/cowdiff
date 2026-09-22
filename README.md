# cowdiff

A diff that asks the filesystem what it already knows.

Two files that share data — a snapshot and its origin, a reflink copy, blocks
merged by `bees` or `duperemove` — point at the *same bytes on disk*. FIEMAP
reports where those bytes live, so an address can be used as an identity for
the content stored there. Bytes whose address is shared by both files are
equal and never need to be read. Only what remains gets read and compared.

On a pair of reflinked 4 MB files with a 4 KB insertion, this reads 4 KB where
`diff` reads 8 MB. On a 10 GB reflinked pair with one 4 KB extent changed
halfway in, it reads 8 KB and answers in under a millisecond, where `cmp` and
`diff` must read 10 GB — from the start of both files to the difference — and
take about twenty seconds doing it.

## How it works

1. **Extent maps.** `FIEMAP` on both files, with `FIEMAP_FLAG_SYNC`. Without
   the sync the kernel reports dirty pages as `DELALLOC` extents with a
   meaningless address — and two files would report the *same* meaningless
   address for different data, which would read as a proof of equality. That
   is the one direction of error this tool must never make.

2. **Trust.** An extent's address is a content identity only when the flags
   say it is. Inline data lives in metadata, `DELALLOC` has not been placed
   yet, and a filesystem with no native extent list synthesises the extent.
   Any of those, and the bytes go in a gap and get compared. An *unwritten*
   extent is different: it reads as zeros whatever the address holds, so it
   is proved equal without a read rather than trusted by address.

3. **Anchors.** The two sets of physical ranges are intersected. A match
   gives `(a_off, b_off, len)` of proven-equal content — and the file offsets
   are allowed to differ. Inserting bytes into a file shifts everything after
   them, so the tail of the new file is still the old file's data at a
   different offset; that still matches, and the offset delta *is* the
   insertion. Matches are then filtered to a chain that never moves backwards
   in either file. Any such chain is a correct alignment, so the selection can
   be greedy: a worse choice only means larger gaps to compare.

   A chain is not by itself a verdict, and that distinction is one this tool
   got wrong. A match at differing offsets proves the *content* is equal, not
   that those bytes agree where they sit — and where they sit is what
   "identical" means. Two files of the same length whose shares are crossed
   (A's block at one offset shared with B's at another, and vice versa) can
   only be chained out of shifted matches, and the gaps such a chain leaves are
   one-sided insertions, which are reported as differences without being read.
   So when the two files are the same length, matches at differing offsets are
   dropped and their ranges compared instead. When the lengths differ the
   length already settles the verdict, and the shifted matches stay, because
   they are what saves reading the tail after an insertion.

4. **Gaps.** Whatever is left between anchors. Holes and unwritten extents
   need no read. A hole against data is settled by testing the data side for
   zeros — an extent full of zeros is not a difference, and reporting it as
   one would be a false positive. Only data against data reads two buffers and
   compares them.

## Building and testing

    make            # builds ./cowdiff and tests/probe
    make check      # tests/run.sh, then tests/extentcheck.py

`tests/probe` is a fixture helper doing exactly one thing:
`probe clone SRC DST SRCOFF LEN DSTOFF`, which performs `FICLONERANGE`. It
exists because btrfs has no `FALLOC_FL_INSERT_RANGE`, so the only way to build
a *shifted* share — a tail pointing at the original's extents but at file
offsets differing by the insert size — is to insert the bytes the ordinary way
and then re-clone the tail from the original.

`tests/extentcheck.py` covers the paths where the extent map rather than the
byte comparison decides the answer — the ones whose fixtures have to make the
filesystem do something specific. `PERFORMANCE.md` §3 lists the cases and what
each one asserts. Fixtures that cannot be built on the filesystem in use skip
loudly rather than passing quietly, including two that skip on the *layout*
they got rather than on the filesystem itself.

`tests/deltacheck.py` covers what happens once the list of differing regions
fills up, which takes a pair of files tens of megabytes long: past a cap the
rest of the file is reported as one coarse substitution, and what it checks is
that the regions do not overlap, that none runs past the end of a file, and
that no byte that differs is left out of them.

`tests/fuzz.py` builds pairs at random and tests the properties that hold of
*every* correct answer rather than comparing against a shape — first among them
that a verdict of "identical" really means the bytes are equal.
`PERFORMANCE.md` §3 lists the oracles and the shapes.
A few hundred cases run in seconds; `--cases` and `--seeds` ask for more, a
failure keeps its two files, and one seed reproduces it exactly.

`tests/bench.sh` times a shape-by-shape overview and `tests/measure.py` is for
numbers — it repeats each command and, when comparing two binaries, pairs them,
because a single run on a laptop varies by more than 2x.

`COWDIFF_EIO_AT="offset:length"` makes every read of that range fail with
`EIO`. It exists so the unreadable-block paths can be tested without a damaged
disk, and nothing outside `tests/run.sh` sets it.

**[`PERFORMANCE.md`](PERFORMANCE.md)** is the working notebook: what each
optimisation measured, and the wrong predictions that had to be measured to be
corrected. Six of them so far. It also records the bugs that were not slow but
useless — a diff that claimed a million lines changed for a one-line edit, a
binary report that named the whole file, a third that named byte-identical
bytes as changed — since "true" is not the same as "worth printing".

The tables here and in §4 measure against different baselines — this one at
equal context, §4's against `diff -u` — and both come from the session that
built the tool, so read them as ratios within a session, not as current
absolutes.

## Usage

    cowdiff [options] FILE1 FILE2

    With -r, FILE1 and FILE2 may be directories; entries are visited in
    sorted order, a name present on only one side is reported as
    "Only in DIR: NAME", and each file that differs is introduced with the
    "diff -ru A B" line diff prints.  Symlinks are followed, as diff does by
    default, so a broken one is an error rather than a difference.

      -q, --brief          report only whether the files differ
      -r, --recursive      compare directories recursively
          --stats          report how much was actually read
          --byte-offsets   put byte offsets in hunk headers instead of line
                           numbers, which skips the scan line numbers need
      -a, --text           treat the files as text even if they look binary
          --force-binary   report byte ranges even if they look like text
      -U NUM               lines of context around each change (default 3)
      -u, --unified        accepted and ignored; unified is the only format
          --dump-extents   print the extent map and exit

Exit status follows `diff`: 0 identical, 1 different, 2 error.

Text files get a unified diff; binary files get the differing byte ranges.
Binary mode is what gets the full benefit — see the caveat below.

## Limits worth knowing

**Line numbers cost a pass over both files, as far in as the last hunk.**
A unified diff hunk header names line numbers, and a line number can only be
had by counting the newlines before it, so producing one means reading the
file up to that point. On a 35 MB reflinked pair whose edit sits 91% of the
way in, printing the header reads 95.6% of the bytes — against 0.43% for
`--byte-offsets` and 0.37% for binary mode. The percentage is roughly how far
into the file the last change is, and the CPU that goes with it is about 11x
the whole rest of the run. Text mode in its default form therefore wins on
memory but not on I/O, and the extra I/O is spent entirely on the header.

`--byte-offsets` keeps the body identical and swaps the header for byte
offsets, which are already known from the extent map, so it keeps the win.
The price is output that `patch(1)` cannot consume and a header that a reader
could mistake for line numbers.

**Binary mode reads what it needs to locate the change.** Where there is
sharing, the gap is small and almost nothing is read — 0.37% of the bytes on
the reflinked pair above. Where there is not, and the two files differ in
length, there is no alignment to compare against, so the ends they still have
in common are found by reading forward to the first difference and back from
the end. That is what turns "the whole file differs" into "12 bytes at
0x219b655 differ", and it is the same trade the text path makes for its line
numbers: the answer is only as precise as what was read to get it.

**Without sharing, it is close to `diff` and sometimes ahead.** Measured at
equal context against `diff -U0`, on files larger than any cache:

    reflinked, one change        0.33x      identical, unshared    0.28x
    scattered changes            0.18x      unrelated              0.94x
    no unique lines              1.34x      scattered + length     1.61x

The two it still loses are the two where the whole file is genuinely one
delta — every line differs, or nothing occurs once on both sides — so the
line search has to run over all of it. GNU diff is better there: it compares
line hashes rather than bytes, runs `discard_confusing_lines()` to exclude
most of the search space up front, and cuts Myers off with a cost threshold
rather than always finding a minimal diff. Everything else — the byte-level
trim of the common prefix and suffix, the sort-based class assignment, the
anchor search — has been through a profile and a measurement, and the numbers
above are what came out.

(A note on comparing: `diff -u` and `cowdiff -U0` do not do the same amount
of work, since one writes three lines of context and the other none. Use the
same context on both sides, or the comparison flatters whichever writes
less.)

**Compressed extents are matched by exact identity only.** For a compressed
extent `fe_physical` is the real address of the compressed data but
`fe_length` is the length the data will have once *decompressed*. So
`[address, address+length)` overstates the space the extent occupies, and two
extents can report overlapping ranges while occupying quite separate disk
space. Intersecting those ranges invents matches that do not exist, which is
how this tool once reported a phantom 12 KB insertion plus a 12 KB deletion
for a 23-byte edit. Compressed extents are therefore matched on
`(address, length)` together, which names one extent and cannot be faked; file
offsets may still differ, so a compressed extent shared at a different offset
still matches — and, like any other shifted match, is dropped again when the
two files are the same length.

**Sharing is lost by ordinary edits.** Editors rewrite through a temp file, or
rewrite the tail; either way btrfs allocates fresh extents and there is nothing
to skip. The headline win is snapshots and reflink copies, not edited files.
An insertion with a re-shared tail is reported without reading the tail, but
reaching that state takes an explicit `FICLONERANGE`.

**A share that sits at different offsets is compared, not trusted, when the
two files are the same length.** Everything the same-offset matches do not
already cover is then read and compared: one region, or the whole file when
none of them survives. (Step 3 above says why.)

**Storage that cannot be read is reported, not skipped.** A block that returns
EIO cannot be shown to hold equal bytes, so it is reported as a difference —
and said to be a different *kind* of finding, because nothing was seen to
differ there. Where the two files point at the same physical extent *at the
same offset* the block is never read at all, so damage underneath it costs
nothing and the bytes are still proven equal; a share at differing offsets is
compared rather than trusted when the lengths match, so damage there is
reported like any other unreadable region. Two places are coarser than they
would otherwise be: a delta that cannot be line-diffed because a block inside
it is unreadable is reported as a byte range rather than as lines, and line
numbers are given up for byte offsets when a newline count crosses an
unreadable block. Both are coarser, neither is wrong, and both say so where
they happen.

**Two filesystems are not comparable.** Equal addresses from separate
filesystems are a coincidence. Files are checked with `st_dev` first, then
`FS_IOC_GETFSUUID`, then `BTRFS_IOC_FS_INFO` — btrfs never assigns
`s_uuid_len`, so the generic ioctl returns `ENOTTY` for it and the btrfs one is
needed. If the question cannot be answered the answer is no, which costs only
the address optimisation.

## License

GNU General Public License, version 2 — see [`LICENSE`](LICENSE).
