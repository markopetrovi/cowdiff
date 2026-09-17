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

`tests/probe` is a fixture helper: `probe dump FILE` prints an extent map, and
`probe clone SRC DST SRCOFF LEN DSTOFF` performs `FICLONERANGE`. The latter
exists because btrfs has no `FALLOC_FL_INSERT_RANGE`, so the only way to build
a *shifted* share — a tail pointing at the original's extents but at file
offsets differing by the insert size — is to insert the bytes the ordinary way
and then re-clone the tail from the original.

## Usage

    cowdiff [options] FILE1 FILE2

      -q, --brief          report only whether the files differ
          --stats          report how much was actually read
          --force-binary   treat the files as binary
          --force-text     treat them as text even if they look binary
          --dump-extents   print the extent maps and exit

Exit status follows `diff`: 0 identical, 1 different, 2 error.

Text files get a unified diff; binary files get the differing byte ranges.
Binary mode is what gets the full benefit — see the caveat below.

## Limits worth knowing

**Text mode must scan both files.** A unified diff hunk header names line
numbers, and a line number can only be had by counting the newlines before it.
So text output reads both files even when every byte is shared; it wins on CPU
and memory, not on I/O. Binary mode has no such constraint and gets the whole
win. A `--byte-offsets` mode that puts byte offsets in the hunk headers
instead is the intended escape hatch.

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
