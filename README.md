# cowdiff

A diff that asks the filesystem what it already knows.

Two files that share data — a snapshot and its origin, a reflink copy, blocks
merged by `bees` or `duperemove` — point at the *same bytes on disk*. FIEMAP
reports where those bytes live, so an address can be used as an identity for
the content stored there. Bytes whose address is shared by both files are
equal and never need to be read. Only what remains gets read and compared.

On a pair of reflinked 4 MB files with a 4 KB insertion, this reads 4 KB where
`diff` reads 8 MB.

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
   be greedy: a worse choice only means larger gaps to compare, never a wrong
   answer.

4. **Gaps.** Whatever is left between anchors. Holes and unwritten extents
   need no read. A hole against data is settled by testing the data side for
   zeros — an extent full of zeros is not a difference, and reporting it as
   one would be a false positive. Only data against data reads two buffers and
   compares them.

## Building and testing

    make            # builds ./cowdiff and tests/probe
    make check      # compares text output against GNU diff across 22 cases

`tests/probe` is a fixture helper doing exactly one thing:
`probe clone SRC DST SRCOFF LEN DSTOFF`, which performs `FICLONERANGE`. It
exists because btrfs has no `FALLOC_FL_INSERT_RANGE`, so the only way to build
a *shifted* share — a tail pointing at the original's extents but at file
offsets differing by the insert size — is to insert the bytes the ordinary way
and then re-clone the tail from the original.

`tests/extentcheck.py` covers the paths where the extent map rather than the
byte comparison decides the answer: compressed extents, shifted shares,
punched holes, zeros against a hole, inline extents, unwritten extents.
Fixtures that cannot be built on the filesystem in use skip loudly rather than
passing quietly.

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

**Line numbers cost a pass over both files.** A unified diff hunk header
names line numbers, and a line number can only be had by counting the newlines
before it. On a 32 MB reflinked pair with a 23-byte edit, printing that header
means reading 48% of both files — against 0.45% for `--byte-offsets` and 0.39%
for binary mode. A factor of 106, spent entirely on the header. So text mode
in its default form wins on CPU and memory but not on I/O.

`--byte-offsets` keeps the body identical and swaps the header for byte
offsets, which are already known from the extent map, so it keeps the win.
The price is output that `patch(1)` cannot consume and a header that a reader
could mistake for line numbers.

**Without sharing, it is currently slower than `diff`.** When nothing is
shared the whole file becomes one gap. GNU diff is fast there because it
compares line hashes rather than bytes, strips common prefix and suffix, runs
`discard_confusing_lines()` to exclude most of the search space up front, and
cuts Myers off with a cost threshold rather than always finding a minimal
diff. This tool memcmps the gap at byte level and *then* builds and sorts
hashed line arrays for both sides, at every level of the anchor search. That
needs the same treatment before it is a safe drop-in for unrelated files.

**Compressed extents are matched by exact identity only.** For a compressed
extent `fe_physical` is the real address of the compressed data but
`fe_length` is the length the data will have once *decompressed*. So
`[address, address+length)` overstates the space the extent occupies, and two
extents can report overlapping ranges while occupying quite separate disk
space. Intersecting those ranges invents matches that do not exist, which is
how this tool once reported a phantom 12 KB insertion plus a 12 KB deletion
for a 23-byte edit. Compressed extents are therefore matched on
`(address, length)` together, which names one extent and cannot be faked; file
offsets may still differ, so shifted anchors survive.

**Sharing is lost by ordinary edits.** Editors rewrite through a temp file, or
rewrite the tail; either way btrfs allocates fresh extents and there is nothing
to skip. The headline win is snapshots and reflink copies, not edited files.
An insertion with a re-shared tail is reported without reading the tail, but
reaching that state takes an explicit `FICLONERANGE`.

**Two filesystems are not comparable.** Equal addresses from separate
filesystems are a coincidence. Files are checked with `st_dev` first, then
`FS_IOC_GETFSUUID`, then `BTRFS_IOC_FS_INFO` — btrfs never assigns
`s_uuid_len`, so the generic ioctl returns `ENOTTY` for it and the btrfs one is
needed. If the question cannot be answered the answer is no, which costs only
the address optimisation.
