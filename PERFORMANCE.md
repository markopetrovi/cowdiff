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
    make check      # tests/run.sh

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

    ./tests/run.sh                                        # 36 checks
    python3 tests/extentcheck.py ./cowdiff ./tests/probe  # 16 checks

- `tests/run.sh` compares text output against GNU diff byte for byte across
  22 edit shapes, checks `-U0/1/2/3/7` context against `diff -U`, checks `-r`
  against `diff -ru`, and runs `tests/bytecheck.py` for `--byte-offsets`.
  Two further checks cover files with no unique lines, where more than one
  answer is correct and the property tested is instead that the diff applied
  to A rebuilds B (§9).
- `tests/extentcheck.py` covers the paths where the extent map rather than the
  byte comparison decides the answer: compressed extents, a shifted share
  built with `FICLONERANGE`, punched holes, zeros against a hole, inline
  extents, unwritten extents. Fixtures that cannot be built on the filesystem
  in use **skip loudly** rather than passing quietly — a test that silently
  stops testing its named path is worse than no test.
- **A new test must be checked against the binary it is meant to catch.** Both
  of the §9 tests were run against the previous binary first; the first
  version of one of them passed against it, and was not testing anything.

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

    0685481  Trim a delta to the part that differs before diffing it
    e11d53a  Add a benchmark harness that can see a few percent
    cb309b5  Let the newline counts vectorise
    2250ed0  Hash lines a word at a time, and print through a buffer
    885a2b8  Split at the middle when nothing is unique, instead of giving up
    720950b  Assign line classes by sorting, not by hashing
    db11c4b  Compare line classes, not line text
    0ee236b  Skip the line diff when the alignment is already known
    fb1f6d2  Measure before optimising, then fix what the measurement found
    9175049  Add -r, and give the extent checks an oracle independent of cowdiff

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
runtime, though of a much smaller number (0.034s) — see §13.

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

- On the reflinked shape the newline count is still 94% of the runtime,
  though of a much smaller number (0.034s): roughly 32 MB of `pread` to reach
  a hunk near the end of the file. The vectorised count should manage that in
  a few ms, so something else in that path is worth a look (§8).
- The `GROUP_MAX` cap in `classes_assign` has never run. Reaching it needs 64
  distinct lines sharing one 32-bit fingerprint, which is not reachable by
  accident and would take a crafted file.
- There is no randomized testing. The 36 checks in `tests/run.sh` are
  hand-built shapes, and §9's bug — the worst one found so far — lived in a
  fallback path that no hand-built shape exercised. A differential fuzz
  against GNU diff, using "applying the output to A rebuilds B" as the
  oracle rather than a byte-for-byte match, is the obvious next thing to
  build.
- Files with more than 2^31 lines would truncate a line index; `struct lref`
  and `struct cent` before it both store it in 32 bits.
- The `---`/`+++` header has no timestamp, where `diff(1)` writes the file's
  mtime after a tab. `tests/run.sh` strips it before comparing. Nobody has
  established whether that was a decision or an omission; `patch(1)` ignores
  it either way, and the output being reproducible run to run is a side
  effect that a test suite likes.
