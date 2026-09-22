# Handoff notes

Everything worth knowing before picking this up again. Started at the end of
the session that built the tool, and updated at the end of the session that
worked through both performance targets. Read in full before changing
anything.

Despite the filename this is not only about performance — it covers what the
tool is, the one correctness invariant that must survive any change, how to
measure, and what is left to try.

---

## 1. What this is

`cowdiff` compares two files using their extent maps, so that data which is
physically shared — reflinks, btrfs snapshots, deduplicated blocks — is never
read. A physical address names content rather than position, so a range of A
and a range of B resolving to the same address hold equal bytes even when they
sit at different file offsets; the offset delta is itself the insertion or
deletion. Everything a proof does not cover becomes a gap, and only gaps are
read.

    make            # builds ./cowdiff and tests/probe
    make check      # tests/run.sh, then tests/extentcheck.py

`README.md` documents the design and the limits for a user. This file is for
whoever works on it next.

## 2. The one invariant

**The tool may read more than it needs to, but it must never report two
different files as identical.** Said positively, so there is no way to misread
it: whenever it says two files are identical, they really are. That verdict
has to be *provable* from what the kernel reported, not merely plausible.

The asymmetry is the point. A false "identical" is silent: exit 0, no output,
and whatever asked — a backup verifier, a migration check, a build system —
concludes the files match and proceeds. A false "different" is loud and gets
investigated. So every ambiguity resolves toward reading and comparing.

Concretely, this is why each of these is the way it is:

- **`FIEMAP_FLAG_SYNC` is mandatory.** Without it the kernel reports dirty
  pages as `DELALLOC` extents whose address means nothing — and two files with
  dirty cache would both report that same meaningless address for *different*
  data, which reads as a proof of equality. This is the exact failure mode.
- **`DELALLOC`, `INLINE`, `DATA_TAIL`, `NOT_ALIGNED`, `MERGED` are untrusted.**
  Inline data lives in the inode and reports `fe_physical` as 0; two different
  small files would both be "at address 0". Their bytes get read instead.
- **Unwritten extents are treated as zeros, not by address.** Readable zeros
  are provable; an address is not.
- **Compressed extents match on `(address, length)` together.** `fe_length` is
  the decompressed length, so `[address, address+length)` overstates the space
  the extent occupies and intersecting those ranges invents matches. This bug
  once turned a 23-byte edit into a phantom 12 KB insert plus a 12 KB delete.
- **`same_filesystem` answers "no" when it cannot tell.** Addresses from two
  filesystems are coincidences. btrfs never sets `sb->s_uuid_len`, so the
  generic `FS_IOC_GETFSUUID` returns `ENOTTY` for it and `BTRFS_IOC_FS_INFO`
  is needed; subvolumes of one filesystem have different `st_dev`.

The same standard applies in the line diff, and it is easy to break there
without noticing: a *class number* may only be issued once the two lines'
bytes have been compared, and a range may only be split at a pair that has
been compared. See §7, §9 and §10 — all three are places where the proof is
what makes a heuristic safe.

Nothing here is about test files. It is the property the tool is built around.

## 3. Correctness gate

Run before and after every change:

    make check                                            # both lines below
    ./tests/run.sh                                        # 76 checks
    python3 tests/extentcheck.py ./cowdiff ./tests/probe  # 22 checks

- `tests/run.sh` compares text output against GNU diff byte for byte across
  22 edit shapes (including files whose last line has no newline), checks
  `-U0/1/2/3/7` context against `diff -U` on three of them, checks `-r`
  against `diff -ru`, checks that `-q` is as silent as `diff -q` is, that the
  options diff accepts — including bunched ones like `-rq` — are accepted
  here, that a symlink loop is reported once instead of followed, and that
  `--stats` describes one file rather than the run. It runs
  `tests/bytecheck.py` for `--byte-offsets` and `tests/deltacheck.py` for the
  coarse fallback. Two further checks cover files with no unique lines, where
  more than one answer is correct and the property tested is instead that the
  diff applied to A rebuilds B (§9).
- `tests/extentcheck.py` covers the paths where the extent map rather than the
  byte comparison decides the answer: compressed extents, a shifted share
  built with `FICLONERANGE`, punched holes, zeros against a hole, a hole
  followed by data, inline extents, unwritten extents. Fixtures that cannot be
  built on the filesystem in use **skip loudly** rather than passing quietly —
  a test that silently stops testing its named path is worse than no test.
- `tests/fuzz.py` builds pairs at random and checks what is true of every
  correct answer rather than comparing against a shape: §2's invariant on
  every case, the patch oracle, the exit status against `diff`, and binary
  mode's report. Its seeds are the only source of variation, so a failure
  reproduces from its seed (the failing pair is kept and the invocation
  printed). Default is a few hundred cases and a couple of seconds;
  `--cases`/`--seeds` ask for more, which is worth doing when the extent or
  line machinery changes — both of §15's fuzz-found bugs were within the first
  few hundred cases, and neither is a shape anyone had thought to write.
- **A new test must be checked against the binary it is meant to catch.** Both
  of the §9 tests were run against the previous binary first; the first
  version of one of them passed against it, and was not testing anything.
  Every test added for §15 was run the same way, and the count of failures it
  is expected to produce is written down with it.

`tests/bytecheck.py` asserts the two things that matter for `--byte-offsets`:
the body is identical to the line-number form, and each hunk's offsets point
at the content that hunk shows.

## 4. Performance: where it stands

Measured with `tests/measure.py`, which repeats each command and reports the
minimum child CPU time of 9 runs. Absolute numbers move with the machine's
power state — on battery the CPU sits at about half its scaling MHz and
everything, GNU diff included, is roughly twice as slow — so compare ratios
within one session, not absolutes across sessions.

| shape | diff -u | before | after |
|---|---|---|---|
| reflinked + one change | 0.08 | 0.111 | **0.036** |
| identical, unshared | 0.08 | 0.013 | **0.014** |
| scattered changes | 0.65 | 0.208 | **0.121** |
| line inserted near the top | 0.08 | 0.334 | **0.018** |
| one change | 0.09 | 0.422 | **0.050** |
| unrelated | 1.12 | 1.722 | **1.037** |
| no unique lines | 0.42 | 1.165 | 0.579 |
| scattered + length change | 0.65 | 1.267 | 1.013 |

Ahead of GNU diff on six of the eight, including the two that were more than
4× behind before §10. The two it still loses are the two where the whole file
is genuinely one delta — every line of "scattered + length change" differs,
and "no unique lines" has nothing to anchor on — so the line search really
does have to run over all of it.  Compare at equal context: those rows are
`cowdiff -U0` against `diff -u`, and `diff -U0` is the fair partner.

`-q` stops at the first difference it proves (0d4e5c9), which is the one
place the walk may be cut short: on 64 MB of unrelated equal-sized files,
0.060s → 0.0020s.  What it must never do is stop on the way to saying
"identical" — that verdict has to cover every byte not proven shared, so
identical files cost the same as they always did, and twice as fast as
`diff -q` because `diff -q` reads them too.

Already done, newest first:

    e0b903c  Fix four bugs, and five smaller things
    e33d557  Two things the review of 019907e turned up
    3855ed7  Order the notes properly
    e5723a9  Document what happens when a block cannot be read
    019907e  Carry on when a block cannot be read
    a8a4502  Add a license, and point at the notes from the README
    703cc7b  Correct what the benchmark says diff does with binary files
    0e406b5  Correct the check count in the gate
    8d3754f  Document that the binary path no longer reports whole spans
    f8e232a  Report what a shifted span shares, not the whole span
    23f0a3e  Fix a stale figure cross-referenced from §8
    7c72730  Bring the handoff notes up to date
    3d2de4c  Update the limits section for where the tool now stands
    4754824  Count newlines eight bytes at a time
    7e186aa  Benchmark the binary and -q paths
    e10f74f  Write the file's timestamp in the ---/+++ header, as diff does
    29a926b  Note the -q shortcut in the handoff notes
    0d4e5c9  Let -q stop at the first difference it proves
    95e4f22  Record the delta trim, and fix the notes' numbering
    0685481  Trim a delta to the part that differs before diffing it
    39b081e  Update the handoff notes for where things now stand
    e11d53a  Add a benchmark harness that can see a few percent
    cb309b5  Let the newline counts vectorise
    2250ed0  Hash lines a word at a time, and print through a buffer
    885a2b8  Split at the middle when nothing is unique, instead of giving up
    720950b  Assign line classes by sorting, not by hashing
    db11c4b  Compare line classes, not line text

## 5. Method — read this before touching performance

**Profile first. Six separate predictions about where the time went were
wrong across these two sessions, and `perf` found the real cause each time.**
The one regression introduced during the first session (building equivalence
classes eagerly) was caught by the benchmark, not by reasoning about it. The
one introduced during the second (numbering classes in fingerprint order) was
a 44% regression that the profile caught; reasoning had said it was an
improvement.

    perf record -q -g -o /tmp/pd ./cowdiff -U0 A.txt B.txt
    perf report --stdio -i /tmp/pd --sort=symbol --no-children --percent-limit 3

`perf annotate` plus `addr2line -e ./cowdiff -i -f 0xADDR` will name the exact
line for a hot instruction, which is how the 36%-of-runtime load in §7's
predecessor was found. For "is this loop vectorised", count SIMD instructions
in the one function:

    objdump -d ./cowdiff | awk '/<lc_count>:/,/^$/' | grep -cE 'pcmpeqd|pmovmskb'

Two flags isolate phases with no instrumentation at all:

- `--force-binary` runs the extent work and the byte comparison only. It is
  **0.00s on every slow shape** — so all remaining cost is line-level work,
  never I/O or extent handling. Check this first; it redirects the search
  every time.
- `--byte-offsets` does the line diff but skips the line-number scan, so
  `-U0` minus `--byte-offsets` is what line numbers cost. That subtraction is
  what showed the scan was 98% of the reflinked case (§8).

**Measuring is harder than it looks on this machine.** `tests/bench.sh` runs
each command once, and one run is not a measurement: the same shape varies by
more than 2x when something else is running, and GNU diff moved 40% between
two runs of it. Use `tests/measure.py`, which repeats and, when comparing two
binaries, pairs them and takes the ratio of adjacent runs — the median of
those ratios is stable to about a tenth of a percent where the individual
ratios spread by 8-35%. Scheduling tricks do not help: SCHED_FIFO made
individual runs *noisier* (8% → 30% spread) and pinning only changes the
number. The variation is the CPU clock, not contention.

GNU diff's own source is worth reading and is easy to fetch
(`git clone --depth 1 https://git.savannah.gnu.org/git/diffutils.git`). The
things that make it fast: the search compares *integers* (equivalence-class
numbers, `EQUAL(x,y) ((x) == (y))`), `discard_confusing_lines` removes lines
that match nothing in the other file before searching, and `too_expensive`
(≈√input, min 4096) makes it abandon minimality rather than search forever.

## 6. Benchmark traps — all of these have already bitten

- **`cp` reflinks by default on btrfs here.** `cp a b` produces a *shared*
  pair, not an unshared one. An early "no sharing" benchmark was measuring
  the shared fast path and reporting it as the degraded one. Build unshared
  pairs by generating both files from scratch.
- **Replacements must be the same length as what they replace.** Writing a
  shorter word shifts every later offset, the files end up different sizes,
  and the whole thing collapses into one unequal-length gap — a completely
  different and much harder shape. This hid a working optimisation until the
  fixture was fixed.
- **`sed` preserves a missing final newline; `awk` adds one.** A regression
  test for §9 was written with `sed`, which made the two files agree at the
  end, which meant the case it was written for never arose — it passed against
  the broken binary. If a fixture is meant to have a differing last line, build
  it with `awk`.
- **Differences under ~0.05s are noise** on `bench.sh`'s single runs, and
  everything is noise without repetition. See §5.
- `.bench/` holds tens of MB of fixtures and **must stay in `.gitignore`**;
  400 MB of them were once committed and had to be filtered out of history.

## 7. Target 1 — length-changing scattered edits (done: 720950b)

Was 1.27s against diff's 0.41s; the profile was `ctab_slot` 43%, `diff_rec`
20%, `lineset_build` 16%. Class assignment was the bottleneck, not the diff:
a hash table sized to 2× the line count, probed once per line — 16.7M slots,
402 MB, a cache and TLB miss per probe, and 100k page faults on the
allocation (0.31s of the 1.03s run was system time).

Replaced with a sort: four radix passes over 8-byte refs, then a merge that
compares text only within a run of equal fingerprints. Peak RSS on that shape
fell from 634 MB to 293 MB and the shape runs 1.27s → 1.05s.

**The part worth remembering.** The first version was a 44% *regression*. The
old table handed out class numbers in the order the lines appear; merging
hands them out in fingerprint order; and the anchor search reads
`count_a[c]`, `count_b[c]` and `first_b[c]` for every line in the range it
scans. Those three reads walked forwards under the old numbering and became
random accesses under the new one — `diff_anchor` went from 17% to 55% of the
profile. `classes_renumber` puts the numbers back in file order. Nothing about
the code was wrong; only the order the numbers came out in, and only the
profile showed it.

Also measured and rejected: caching the class representative's line
description in the open-class entry, which the disassembly had argued for as
the hottest instruction in the program. It was a wash on this shape and 7.6%
worse on `unrelated`. Do not re-add it without measuring.

## 8. Target 2 — no unique lines (done: 2250ed0, cb309b5)

Was 1.165s against diff's 0.42s, with the file read three times per file:
`lineset_build` read it and hashed every line, `lc_count` read it again to
count newlines for hunk headers, `print_lines` a third time to emit.

The prescribed fix was to index each file's lines once and derive all three
from that index. **The profile said otherwise, and the profile was right.**
Reading the file a second time was never the cost — reading 35 MB warm takes
under 10 ms. The counting loop was 25× slower than the identical loop in
isolation, because the counter lived behind a pointer, which is enough to stop
the compiler vectorising it. Counting into a local fixed it: on a reflinked
pair, where no byte is read to compare the files and the count is essentially
the entire runtime, 0.105s → 0.034s. On the no-unique-lines shape 1.165s →
0.623s, most of the rest being the two changes in 2250ed0, below.

The other two changes in this target, both from the same profile:

- `hash_line` was FNV-1a, one dependent multiply per byte. Now word at a time,
  with the length mixed in first. Nothing depends on the hash beyond equal
  lines hashing equally — equality is always settled by comparing the bytes.
- `print_lines` assembled each line a byte at a time and then made two locked
  stdio calls per line. It now uses `memchr` and a private buffer.

What is left here: on the reflinked shape the count is still 94% of the
runtime, though of a much smaller number — 0.034s when this was written and
0.019s since 4754824.  See §13 for what is left of it.

Target 2's own shapes are done. What the flag subtraction in §5 showed was
not, in the end, where the remaining time goes on the other slow shapes:
`-U0` minus `--byte-offsets` is only 12% on "one change", and the profile puts
66% in `lineset_build` for a reason that has nothing to do with line numbers
(§10).

## 9. A give-up that was worse than the bug it avoided (885a2b8)

Worth reading as a case study, because the fallback was *documented as
correct* and was still unusable.

When the range left after trimming the common ends contained no line that
occurred exactly once on both sides, the whole range was reported as replaced
— "coarse but never wrong", said the comment. Both halves of that were true,
and it was reached constantly: two files differing in one line, where a
trailing newline also differs, stop the common-suffix trim on its first
comparison and leave the entire tail in one range. A 64 MB file of one
repeated line with a single line changed produced a **131 MB diff**:
1,170,163 lines removed and as many added, for a change of one line and one
newline. `diff(1)` answers it in ten lines.

It now falls back to splitting at the middle of each range, and only reports
the range as replaced if the lines there are *not* equal. This is sound for
the reason §2 exists: a class number is only issued after the bytes have been
compared, so `lines_equal` is a proof, and a proof is all the recursion needs.
What the fallback guesses is which of several equal lines corresponds to
which. A wrong guess splits in the wrong place, costing granularity — both
sides are still diffed exactly — and never truth. The same diff is now 197
bytes at `-U0`, 709 at `-U3`, matching GNU diff's counts.

**The general lesson, since this shape will come up again:** "never wrong" is
not the same as "good enough to ship". The invariant in §2 is about the
direction of error, but an answer that is technically true and practically
useless is its own kind of failure, and this one was in the fallback path
where nobody was looking.

**The same defect was in the binary path, and stayed there for longer**
(f8e232a).  `resolve_gap()` treats a gap whose two spans differ in length, or
hold the same content at different offsets, as uncomparable — which is true —
and reported the whole span as replaced without reading a byte.  Text output
was refined afterwards by the line diff, so nothing showed; binary output has
nothing afterwards, so a six-byte change to a 35 MB file was reported as
35 MB of differing region, in 0.002s.  It now finds what the two spans still
share and reports only the rest, at the cost of reading what has to be
compared to find it.  That is `cmp -l`'s answer to the byte: 12 bytes at
0x219b655 against the whole file.

Two lessons, and the second is the one to carry: the coarse-answer failure
mode hides in the paths with *no* downstream refinement, and the way to find
it is to look at what the tool prints for a small change in a large file and
ask whether a person could act on it.

## 10. A length difference skipped the byte comparison (0685481)

`resolve_gap()` reports a gap whose two spans differ in length as one replaced
span *without comparing any bytes at all*.  For binary output that is the
honest answer and the right call.  For text output it meant the whole file
became a single delta and the line diff had to rediscover the common prefix at
line granularity: 66% of the "one change" shape was `lineset_build`, reading
and hashing 70 MB to find a difference in the last line of one file, because
the replacement was six bytes shorter and so nothing after it lined up.

`--force-binary` is how to see it — it prints one region covering all
35239522 bytes and runs in 0.002s, which is proof that nothing was read.

The fix trims the delta's two line-snapped spans to the region that actually
differs before building the linesets.  Two constraints, both easy to get
wrong, and the second is the subtle one:

  * each side must be cut on one of its own line boundaries, or the lineset
    starts or ends inside a line;
  * both sides must give up the *same number of bytes*.  The lines left
    outside the two regions are then the same lines, which is what lets the
    diff ignore them; cut them by different amounts and it reports trailing
    lines as deleted that are not.

The prefix satisfies both for free — its bytes are equal, so a line boundary
in one file is a line boundary in the other — and the cut goes back to the
start of the line the first difference is on.  The suffix is cut forward to
the first whole line inside it, for the same reason.

Measured, minimum CPU of 9 paired runs: "one change" 0.330s → 0.050s, "line
inserted near the top" 0.290s → 0.017s.  Both went from more than 4× slower
than `diff(1)` to faster than it.  The other six shapes were unchanged, which
is the result worth checking — this only touches deltas the byte level
skipped.

## 11. Constraints a change here must not break

- Section 2's invariant, above all.
- **Build expensive structures lazily.** Eager building made the easy shapes
  twice as slow — twice, once with the hash tables and once with the
  equivalence classes. The common-prefix/suffix trimming resolves those cases
  before the structure is ever needed; `lines_equal()` and `dstate_build()`
  exist for exactly that.
- Keep the `-U`/`-r`/`-a`/`-u` options matching `diff`, and keep `--stats`
  printing *after* the output (printing before hid everything the text path
  reads, and made `--byte-offsets` look worthless).
- **Output must stay byte-identical across a performance change.** Every
  optimisation above was checked against the previous binary on every
  benchmark shape at `-U0/-U1/-U3/-U7` and `--byte-offsets`; the class
  numbering, the hash and the renumbering all change internal representations
  and none of them may change a byte of output.

## 12. Revert strategy

Each optimisation is its own commit and they are independent, so a failed
attempt is `git revert <commit>`, or a reset to the commit before it. Nothing
has ever been pushed; this is a local repository only.

## 13. Not done

- The fuzz is `tests/fuzz.py` now (§15), seeded, a few hundred cases inside
  `make check` and `--cases`/`--seeds` for a longer hunt. Two of §15's four
  bugs were its first few hundred cases — that is the argument for running it
  longer than the default whenever the extent or line machinery changes. What
  it does *not* reach is worth knowing too: the delta list only fills on files
  of eight megabytes and up, so that path is covered by `tests/deltacheck.py`
  instead, and a cost that is merely quadratic is not an oracle at all — §15.4
  was found by timing, not by fuzzing.
- The `GROUP_MAX` cap in `classes_assign` has never run. Reaching it needs 64
  distinct lines sharing one 32-bit fingerprint, which is not reachable by
  accident and would take a crafted file.
- Files with more than 2^31 lines would truncate a line index; `struct lref`
  and `struct cent` before it both store it in 32 bits.
- On the reflinked shape the newline count is the runtime (0.019s of it),
  at about 1.8x what `wc -l` spends on the same job and the same bytes. The
  loop is now eight bytes at a time (§4, 4754824); what is left is the read
  underneath it, which 64 KB chunks against `wc`'s larger ones may not be
  describing well. A 1 MB buffer was tried and measured at 1.6%, so the
  difference is somewhere else — worth a `perf` run rather than a guess.
- The two shapes in §4 that this tool still loses are the ones where the
  whole file is one delta. Beating `diff` there means a bounded Myers-style
  search, or `discard_confusing_lines()`-style filtering, and neither is a
  small change.

## 14. Unreadable storage (019907e)

An EIO used to abort the run: no output at all, exit 2, everything already
established thrown away.  It came from a real pair of disk images with a list
of 11016 blocks that read as EIO on the host that imaged them.

The rule now is this tool's own rule, applied one level down.  A block that
cannot be read cannot be *proven* equal, and anything unproven is a
difference -- but it is not a block that was seen to differ, so the output
says which kind it is, per region, and which side failed.  Four things are
worth knowing:

- **Blocks both files point at are still never read.**  Damage underneath a
  shared extent costs nothing and those bytes stay proven equal.  On the real
  pair, 63 of the 11016 listed blocks sit in storage both files share and are
  simply equal.
- **The retry is a block at a time.**  A failed chunk read is redone in 4 KB
  units, so one bad block costs one block of report rather than a megabyte,
  and its neighbours are still compared rather than assumed.
- **Text output is coarser where it has to be.**  A line diff cannot span a
  gap whose contents are unknown, so a delta with a bad block inside it is
  reported as a byte range, and a newline count that crosses one gives up
  line numbers for byte offsets.  Both say so where they happen.  Neither
  fabricates content to fill the gap, which is the one thing that would break
  §2: two unreadable blocks are not equal bytes, they are two unknowns.
- **pread_full now separates EIO from a caller bug.**  A short read means the
  range asked for ran past the end, which is a mistake in this program; EIO
  means the storage underneath is damaged.  They were the same -1, which is
  why this could not be handled where it happens.

`COWDIFF_EIO_AT="offset:length"` makes reads fail on demand, which is how the
suite tests this without a damaged disk; nothing else sets it.

## 15. Four bugs, and five smaller ones

Two of these four came straight out of the differential fuzz §13 kept asking
for, in its first few hundred cases, and both of them broke §2: "identical"
was returned when it was not provable from what the kernel said, or from
anything else. The other two came from reading and measuring what the fuzz was
*not* reaching — a delta list that only fills on files of eight megabytes and
up, and a text path whose cost only appears on files with no newlines in them.

Every fix below has a regression test in the gate (§3), and each of those
tests was run against the binary *before* its fix as well as after.

### 15.1 A hole was taken for zeros all the way to the end of the gap

`gap_equal_range()` cuts a gap into pieces at every extent boundary, so that
each piece has one settled answer on each side. It took the boundary from the
extent *containing* the offset — and when there was none, because the offset
fell in a hole, it took no boundary at all. The hole was therefore treated as
running to the end of the gap, and real data on the far side of it sat inside
a piece whose answer had already been decided to be zeros. Nothing ever read
it.

    X: 65536-byte hole, then 4096 bytes of 'A'
    Y: 69632-byte hole
    $ diff -q X Y       -> differ
    $ cowdiff --stats X Y
    cowdiff: read 0 of 139264 bytes (0.0000%)
    Files X and Y are identical        <- exit 0, nothing read

"Could not be more wrong" is literal: the tool exists so that this verdict is
provable, and this one was produced by not looking. It needed only that a hole
end inside the range being decided — which is what a sparse file *is*, and the
tool's own origin story (§14) is a pair of disk images.

`extmap_next_start()` (new) answers where a hole ends; `extmap_at()` cannot,
because a hole is precisely the absence of an extent. Both sides are now cut
at the end of their zero run. `range_is_zero()`, which had the same assumption
in its own loop, was fixed with it — unreachable from its one caller today,
but the same mistake waiting for a second caller.

### 15.2 A hunk header claimed one line more than its body carried

A hunk's trailing context is found by stepping forward line by line, and the
lines it covers are added to the newline count taken before the walk. But the
last line of a file need not end in a newline: a walk that stops there has
entered a line without crossing one. `forward_lines()` counted steps, the
caller added the end-of-file correction as well, and a hunk whose context
reached an unterminated last line came out one line too long.

    A = "1\n2\n3\n4\n5"   (no trailing newline)
    B = "1\n2\nX\n4\n5"
    $ diff -u A B | grep @@      ->  @@ -1,5 +1,5 @@
    $ cowdiff  A B | grep @@     ->  @@ -1,6 +1,6 @@

The body was right, so comparisons against GNU diff that looked only at
content passed; the suite did compare headers, but only on shapes whose last
line had a newline. It was wrong from `-U2` up, and `patch(1)` refuses the
result outright ("malformed patch") — so the output was not merely mismatched,
it was unusable.

`line_end()` now reports whether it found a newline or ran out of file, and
`forward_lines()` counts newlines crossed rather than lines entered, so the
"+1 for an unterminated final line" lands exactly once whichever way the walk
ends.

### 15.3 Once the delta list filled, the same bytes were reported over and over

`DELTA_MAX` exists so that two unrelated large files do not report a delta per
run of differing bytes. Past it the rest of the span is reported as one
substitution — but the check sat at the top of `diff_buffers()`, which is
called once per *read chunk*, and the substitution covered everything to the
end of the span. So chunk after chunk pushed another one, each covering more
of the same bytes:

    12 MB pair differing in one byte per 32 (393216 bytes truly differ)
    before:  262148 differing regions (10747904 bytes in A, 10747904 bytes in B)
    after:   262145 differing regions ( 4456448 bytes in A,  4456448 bytes in B)

"10747904 bytes in" a 12582912-byte file is not an answer anyone can use. On a
filesystem whose extents are contiguous the repeated substitutions merged into
one and only the totals gave it away; with larger extents they overlapped and
the regions themselves were nonsense.

`diff_buffers()` now returns `DIFF_COARSE` once it has reported the rest of
the span, `compare_range()` stops reading that piece, and `gap_equal_range()`
moves to the *next piece* rather than to the next gap — every byte not proven
equal still has to land in some delta. It also stops comparing after the cap,
which is the point of having one.

### 15.4 Line boundaries were re-read for every delta

Each delta snapped its byte range out to whole lines by asking where the line
starts and ends, and each question walked to the nearest newline on its own.
With ordinary lines that is a few bytes; with a file that has no newlines in
it at all — minified JSON, CR-only line endings, any single-line file — each
question walks to the end of the file, once per delta.

    1 MB, one differing byte per 64, no newlines
    before:  19.8 s   (8.4M pread calls, from strace -c)
    after:    0.00 s

The 16 MB version never finished. `struct lcache` keeps the last line found,
per file: a question about an offset on that line is free, and a question
about an offset elsewhere is asked the ordinary way and seeds the cache with
*its* line. An edit-dense file therefore stops paying for its line lengths, an
edit-sparse one pays what it did before (measured: within 2% on the benchmark
shapes), and neither gets a wrong answer to save time.

### 15.5 The smaller things, all in the same pass

- **`--stats` counted the run, not the file.** With `-r` the second file's
  line included the first file's bytes and printed a percentage past 100 —
  358% on a two-file walk. `compare_files()` now resets the counter.
- **Bunched short options were refused.** `diff -rq` works, so this must too,
  or a script fails on the invocation rather than on the comparison.
- **`-U` took any garbage for a number.** `-Uabc` silently meant no context —
  a hunk with no context, and no complaint about it; `-U` followed by a
  filename meant the same. The value is now digits or an error, as in diff.
- **`-q` was not quiet.** It printed the "identical" line where `diff -q`
  prints nothing. The flag's whole value is that a script can read the status
  and nothing else.
- **A symlink loop was followed.** A symlink to a directory above itself made
  the walk descend until the kernel refused at forty levels, printing an error
  per level. diff(1) reports "recursive directory loop" once and gives up on
  the pair; so does this now, with the same status.

One thing deliberately left alone: `-r` still has no `--` (end of options), so
a file whose name begins with `-` cannot be named. It is the only remaining
case where an argument diff accepts is refused, and it is a small addition if
anyone needs it.
