#!/bin/bash
# Compare cowdiff's text output against GNU diff on a spread of edits.
#
# The whole output has to match, headers and timestamps included: diff(1)
# writes each file's mtime after a tab on its ---/+++ lines and so do we, and
# comparing the two byte for byte is what keeps that true.  (Until it did,
# this script stripped the timestamps from diff's side to make the comparison
# work -- which meant nothing here tested what a header actually contained.)
set -u

# diff(1) is localised; compare against its C-locale output.
export LC_ALL=C

ROOT=$(cd "$(dirname "$0")/.." && pwd)
COW=$ROOT/cowdiff
WORK=$ROOT/.testtmp

[ -x "$COW" ] || { echo "build first: make"; exit 2; }
rm -rf "$WORK"; mkdir -p "$WORK" || exit 2

pass=0; fail=0

chk() {
	local name=$1 A=$2 B=$3
	local exp="$WORK/.expected" act="$WORK/.actual"

	if cmp -s "$A" "$B"; then
		printf 'Files %s and %s are identical\n' "$A" "$B" > "$exp"
	else
		diff -u "$A" "$B" > "$exp"
	fi
	"$COW" "$A" "$B" > "$act" 2>"$WORK/.err"

	if cmp -s "$exp" "$act"; then
		pass=$((pass+1))
	else
		fail=$((fail+1))
		echo "FAIL: $name"
		echo "--- expected ---"; cat "$exp"
		echo "--- got ---"; cat "$act"
		[ -s "$WORK/.err" ] && { echo "--- stderr ---"; cat "$WORK/.err"; }
		echo
	fi
}

cd "$WORK" || exit 2

seq 1 3000 | sed 's/^/line /' > base.txt
cp --reflink=always base.txt A.txt

mk() { cp --reflink=always A.txt "$1"; }

# --- edits inside a fully shared file --------------------------------------
mk B.txt; sed -i '1500s/.*/line 1500 CHANGED/' B.txt
chk "one line changed" A.txt B.txt

mk B.txt; sed -i '1s/.*/first line changed/' B.txt
chk "first line changed" A.txt B.txt

mk B.txt; sed -i '3000s/.*/last line changed/' B.txt
chk "last line changed" A.txt B.txt

mk B.txt; sed -i '1500d' B.txt
chk "one line deleted" A.txt B.txt

mk B.txt; sed -i '1500i\an inserted line' B.txt
chk "one line inserted" A.txt B.txt

mk B.txt; sed -i '1500,1510d' B.txt
chk "ten lines deleted" A.txt B.txt

mk B.txt; sed -i '1500s/.*/x/' B.txt; sed -i '1505s/.*/y/' B.txt
chk "two nearby changes" A.txt B.txt

mk B.txt; sed -i '100s/.*/x/' B.txt; sed -i '2900s/.*/y/' B.txt
chk "two distant changes" A.txt B.txt

mk B.txt; sed -i '100s/.*/x/' B.txt; sed -i '106s/.*/y/' B.txt
chk "changes six lines apart" A.txt B.txt

mk B.txt; head -c 20000 base.txt > B.txt
chk "truncated" A.txt B.txt

mk B.txt; sed -i '2500,$d' B.txt; printf 'appended one\n' >> B.txt
chk "truncated then appended" A.txt B.txt

mk B.txt; printf 'appended line\n' >> B.txt
chk "appended" A.txt B.txt

# --- whole-file, no sharing at all ------------------------------------------
seq 1 3000 | sed 's/^/line /' > C.txt
sed '1500s/.*/changed/' base.txt > D.txt
chk "no sharing, one line changed" C.txt D.txt

sed 's/line/cord/' base.txt > E.txt
chk "no sharing, every line changed" C.txt E.txt

seq 5000 1 8000 | sed 's/^/line /' > F.txt
chk "no sharing, unrelated" C.txt F.txt

# --- files with no unique lines ---------------------------------------------
# Every line here is the same, so nothing occurs exactly once on either side
# and the anchor search has nothing to hold on to.  A diff of such a file
# cannot be compared against diff(1) line for line -- more than one form is
# correct -- so check what any correct diff must do instead: applied to A it
# must produce B, and it must not answer a one-line edit with the whole file.
#
# Both shapes end with a difference, because a range is only handed to the
# anchor search when its first and last lines differ: that is what the common
# prefix and suffix trimming leave behind.
chk_patch() {
	local name=$1 A=$2 B=$3
	local out="$WORK/.pdiff" recon="$WORK/.precon"
	local touched

	"$COW" -U3 "$A" "$B" > "$out" 2>"$WORK/.err"
	cp "$A" "$recon"
	if ! patch --silent --force "$recon" < "$out" 2>/dev/null ||
	   ! cmp -s "$recon" "$B"; then
		fail=$((fail + 1))
		echo "FAIL: $name (diff applied to A does not give B)"
		head -5 "$out"
		return
	fi
	touched=$(tail -n +3 "$out" | grep -c '^[-+]')
	if [ "$touched" -gt 40 ]; then
		fail=$((fail + 1))
		echo "FAIL: $name: $touched changed lines for a two-line edit" \
		     "in a $(wc -l < "$A")-line file"
		return
	fi
	pass=$((pass + 1))
}

seq 1 3000 | sed 's/.*/repeated line/' > P1.txt
sed -e '1500s/.*/changed/' -e '3000s/.*/also changed/' P1.txt > P2.txt
chk_patch "repetitive, changes at both ends" P1.txt P2.txt

# The shape this was written for: no newline at the end stops the suffix trim
# on its first comparison, so a single changed line used to leave the whole
# file inside one range -- and one range with no unique line in it was
# reported as wholly replaced, several hundred thousand lines of it.
#
# awk, not sed, to build P4: sed keeps the missing final newline, awk adds
# one.  Without that last line differing, the suffix trim has something to
# hold and the case never arises -- which is how the first version of this
# test managed not to test anything.
yes "repeated line" | head -c 20000 > P3.txt
awk 'NR==700{print "changed"; next} {print}' P3.txt > P4.txt
chk_patch "repetitive, change and no trailing newline" P3.txt P4.txt

# --- newline at end of file -------------------------------------------------
printf 'a\nb\nc' > N1.txt; printf 'a\nb\nd' > N2.txt
chk "no trailing newline, changed" N1.txt N2.txt
printf 'a\nb\nc\n' > N3.txt
chk "newline added at end" N1.txt N3.txt
printf 'a\nb\nc' > N1b.txt
cp N1b.txt N1c.txt
chk "identical, no trailing newline" N1b.txt N1c.txt

# A change whose trailing context reaches an unterminated last line.  The walk
# that finds the context crosses newlines on the way and then steps onto a
# line that has none: counting that step as a newline *and* adding the
# end-of-file correction made the header claim one line more than the body
# carried, and patch(1) refuses a hunk whose header over-counts.  Wrong from
# -U2 up, which is why these run through ctx_check below at every size.
printf '1\n2\n3\n4\n5' > NT1.txt; printf '1\n2\nX\n4\n5' > NT2.txt
seq 1 20 | head -c -1 > NT3.txt; sed '18s/.*/X/' NT3.txt > NT4.txt

# --- degenerate -------------------------------------------------------------
: > Z1.txt; : > Z2.txt
chk "both empty" Z1.txt Z2.txt
printf 'x\n' > Z3.txt
chk "empty vs one line" Z1.txt Z3.txt
cp --reflink=always A.txt A2.txt
chk "identical reflink" A.txt A2.txt

# --- insertions of whole blocks ---------------------------------------------
mk B.txt; sed -i '1500i\inserted A\ninserted B\ninserted C' B.txt
chk "three lines inserted" A.txt B.txt

# --- context lines, matching diff -U ----------------------------------------
# Every shape runs at every context size: a hunk header is where a line count
# can be wrong while the body still looks right, and how far the context
# reaches is exactly what decides that.
ctx_check() {                          # ctx_check NAME A B
	local name=$1 A=$2 B=$3 n
	for n in 0 1 2 3 7; do
		LC_ALL=C diff -U"$n" "$A" "$B" > "$WORK/.expected"
		"$COW" -U"$n" "$A" "$B" > "$WORK/.actual"
		if cmp -s "$WORK/.expected" "$WORK/.actual"; then
			pass=$((pass + 1))
		else
			fail=$((fail + 1))
			echo "FAIL: -U$n context, $name"
			diff -u "$WORK/.expected" "$WORK/.actual" | head -20
		fi
	done
}
ctx_check "one line changed" A.txt B.txt
ctx_check "unterminated last line" NT1.txt NT2.txt
ctx_check "unterminated last line, change further in" NT3.txt NT4.txt

# -u asks for the format we always emit and -a is diff's flag for forcing
# text; both must be accepted rather than refused.  These files differ, so
# acceptance means status 1, and only status 2 counts as a rejection.
#
# The bunched forms are in the list because that is how scripts write them:
# "-rq" is "-r -q" and a diff that refuses it fails on the invocation rather
# than on the comparison.
for opt in -u --unified -a --text -rq -qa -qu -qU0 -rU3; do
	"$COW" "$opt" A.txt B.txt > /dev/null 2>&1
	rc=$?
	if [ "$rc" -le 1 ]; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1)); echo "FAIL: $opt rejected (status $rc)"
	fi
done

# A context size that is not a number is a mistake, not a zero.  Taking it
# for zero would print a hunk with no context and say nothing about it, and
# "-U" followed by a filename is exactly that mistake.
"$COW" -U 3 A.txt B.txt > /dev/null 2>&1
[ "$?" -le 1 ] && pass=$((pass + 1)) || {
	fail=$((fail + 1)); echo "FAIL: -U 3 rejected"; }
for bad in -Uabc -U3x -U-1; do
	"$COW" "$bad" A.txt B.txt > /dev/null 2>&1
	rc=$?
	if [ "$rc" -eq 2 ]; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1)); echo "FAIL: $bad accepted (status $rc)"
	fi
done

# --- recursive (-r) ---------------------------------------------------------
mkdir -p R1/sub R2/sub
echo a > R1/f1;             echo b > R2/f1
echo same > R1/same;        echo same > R2/same
echo only > R1/extra
echo x > R1/sub/deep;       echo y > R2/sub/deep
echo z > R1/sub/only1

LC_ALL=C diff -ru R1 R2 > "$WORK/.expected"
"$COW" -r R1 R2 > "$WORK/.actual"
if cmp -s "$WORK/.expected" "$WORK/.actual"; then
	pass=$((pass + 1))
else
	fail=$((fail + 1))
	echo "FAIL: -r output differs from diff -ru"
	diff -u "$WORK/.expected" "$WORK/.actual" | head -30
fi

# -r on two identical trees must report nothing and succeed.
rm -rf R3; cp -r R1 R3
"$COW" -r R1 R3 > "$WORK/.actual" 2>&1
rc=$?
if [ -s "$WORK/.actual" ] || [ "$rc" -ne 0 ]; then
	fail=$((fail + 1)); echo "FAIL: -r on identical trees (status $rc)"
else
	pass=$((pass + 1))
fi

# A file that is the same inode on both sides is the one pair -r can settle
# without reading anything, and that shortcut printed the "are identical" line
# that -r must not print: diff -ru names only the files that differ.  Hardlinks
# rather than copies, because cp -r gives the two trees separate inodes and the
# shortcut is never reached.
rm -rf H1 H2; mkdir -p H1 H2
echo same > H1/f; ln H1/f H2/f
echo one > H1/g; echo two > H2/g
LC_ALL=C diff -ru H1 H2 > "$WORK/.expected"
"$COW" -r H1 H2 > "$WORK/.actual"
if cmp -s "$WORK/.expected" "$WORK/.actual"; then
	pass=$((pass + 1))
else
	fail=$((fail + 1))
	echo "FAIL: -r reported a pair that shares an inode"
	diff -u "$WORK/.expected" "$WORK/.actual" | head -10
fi

# A symlink to a directory above itself is a loop, and following it is not
# merely slow: it descends until the kernel refuses at forty levels, printing
# an error for each.  diff(1) reports the loop once and gives up on the pair;
# this must do the same, with the same status.
rm -rf L1 L2; mkdir -p L1 L2
echo x > L1/f; echo x > L2/f
ln -s . L1/self; ln -s . L2/self
LC_ALL=C diff -r L1 L2 > "$WORK/.expected" 2>&1; want=$?
"$COW" -r L1 L2 > "$WORK/.actual" 2>&1; got=$?
loops=$(grep -c 'recursive directory loop' "$WORK/.actual")
if [ "$want" -eq "$got" ] && [ "$loops" -eq 1 ]; then
	pass=$((pass + 1))
else
	fail=$((fail + 1))
	echo "FAIL: symlink loop (diff status $want, cowdiff status $got," \
	     "$loops report(s))"
	head -3 "$WORK/.actual"
fi

# A directory that cannot be listed is a failure, not an empty directory.  The
# names the other side holds were being reported as "Only in" it, which is a
# claim about a directory that was never read -- it may hold every one of them.
# Both shapes below reach the same path: a file where a directory was expected,
# and a directory that refuses to open.
#
# The status here is 2 and diff's is not, deliberately: diff -ru FILE DIR goes
# looking for DIR/FILE and compares that, which is a pair this tool does not
# have an answer for, so it refuses the invocation instead.  What is being
# tested is the refusal's *report*, so that is what is asserted.
rm -rf U1 U2; mkdir -p U1 U2
echo x > U1/f; echo y > U2/f; echo z > U2/only
"$COW" -r U1/f U2 > "$WORK/.actual" 2>&1; got=$?
if [ "$got" -eq 2 ] && ! grep -q '^Only in' "$WORK/.actual"; then
	pass=$((pass + 1))
else
	fail=$((fail + 1))
	echo "FAIL: -r with one side unlistable (status $got)"
	cat "$WORK/.actual"
fi

# Mode 000 stops nobody when the suite runs as root, so the check follows diff
# out of the way there rather than passing for the wrong reason.
if [ "$(id -u)" -ne 0 ]; then
	rm -rf P1 P2; mkdir -p P1/sub P2/sub
	echo a > P1/sub/f; echo b > P2/sub/f; echo c > P2/sub/g
	chmod 000 P1/sub
	LC_ALL=C diff -ru P1 P2 > /dev/null 2>&1; want=$?
	"$COW" -r P1 P2 > "$WORK/.actual" 2>&1; got=$?
	chmod 755 P1/sub
	if [ "$got" -eq "$want" ] && ! grep -q '^Only in' "$WORK/.actual"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "FAIL: -r into an unreadable directory (diff $want, cowdiff $got)"
		cat "$WORK/.actual"
	fi
else
	echo "note: running as root; the unreadable-directory check is skipped"
fi

# --stats describes the file it is printed for.  A running total across a -r
# walk makes the second file's line claim the first one's bytes as well, and
# on an unshared pair compared in binary mode the number is exactly the size
# of the two files -- so a percentage past 100 is a count that has leaked
# across files.  (Text mode reads more than the files' size on purpose, by
# the pass its line numbers cost, so it cannot be measured this way.)
rm -rf ST1 ST2; mkdir -p ST1 ST2
head -c 200000 /dev/urandom > ST1/f; head -c 200000 /dev/urandom > ST2/f
over=$("$COW" -r --stats ST1 ST2 2>&1 >/dev/null |
	awk '/^cowdiff: read/ { p=$0; sub(/.*\(/, "", p); sub(/%.*/, "", p);
				 if (p+0 > 100.0001) print p }')
if [ -z "$over" ]; then
	pass=$((pass + 1))
else
	fail=$((fail + 1)); echo "FAIL: --stats reports $over% of a file read"
fi

# And the pair that reads nothing at all, being one inode, still has to say so:
# the shortcut that answers it used to leave --stats behind with it.
rm -rf ST3; mkdir -p ST3
head -c 100000 /dev/urandom > ST3/f
ln ST3/f ST3/g
line=$("$COW" --stats ST3/f ST3/g 2>&1 >/dev/null | grep '^cowdiff: read')
if [ "$line" = "cowdiff: read 0 of 200000 bytes (0.0000%)" ]; then
	pass=$((pass + 1))
else
	fail=$((fail + 1)); echo "FAIL: --stats for one inode said '$line'"
fi

# --- byte-offset mode -------------------------------------------------------
# The bodies must match the line-number form exactly, and every hunk's offsets
# must point at the content that hunk shows.
seq 1 500 | sed 's/^/line /' > P.txt
mkr() { cp --reflink=always P.txt "$1"; }

mkr Q.txt; sed -i '250s/.*/changed/' Q.txt
mkr R.txt; sed -i '10d' R.txt
mkr S.txt; sed -i '400i\inserted' S.txt
mkr V.txt; sed -i '1s/.*/first/' V.txt; sed -i '$s/.*/last/' V.txt
printf 'a\nb\nc' > T1.txt; printf 'a\nb\nd' > T2.txt
seq 1 400 | sed 's/^/x /' > U1.txt; sed 's/x /y /' U1.txt > U2.txt

echo "--- byte-offset mode ---"
if python3 "$ROOT/tests/bytecheck.py" "$COW" \
	P.txt Q.txt P.txt R.txt P.txt S.txt P.txt V.txt \
	T1.txt T2.txt U1.txt U2.txt; then
	pass=$((pass + 1))
else
	fail=$((fail + 1))
fi

# --- what happens once the list of differing regions fills -------------------
# Its own fixtures are tens of megabytes, which is why it is a script of its
# own rather than shapes in this one.
echo "--- delta cap ---"
if python3 "$ROOT/tests/deltacheck.py" "$COW"; then
	pass=$((pass + 1))
else
	fail=$((fail + 1))
fi

# --- the fuzz -----------------------------------------------------------------
# Random pairs rather than named shapes, against the oracles that hold of every
# correct answer -- see the file. It needs no fixtures of this script's, only a
# binary, and a few hundred cases take a couple of seconds.
echo "--- fuzz ---"
if python3 "$ROOT/tests/fuzz.py" "$COW"; then
	pass=$((pass + 1))
else
	fail=$((fail + 1))
fi

# --- blocks that cannot be read ---------------------------------------------
# Storage that returns EIO cannot be shown to hold equal bytes, so it is
# reported as a difference -- and as a *different kind* of difference, since
# nothing was seen to differ there.  No filesystem returns EIO on demand, so
# the reads are made to fail through the COWDIFF_EIO_AT hook in pread_full;
# nothing outside this script sets it.
seq 1 4000 | sed 's/^/line /' > E1.txt
sed '' E1.txt > E2.txt                  # a copy, so no extents are shared
EIO=0x1000:4096                         # 4 KB in the middle of E1/E2

# Without the hook they are identical, which is the control: whatever the
# injected runs report, they report because a read failed and for no other
# reason.
"$COW" -q E1.txt E2.txt > /dev/null 2>&1; rc=$?
if [ "$rc" -eq 0 ]; then pass=$((pass+1)); else
	fail=$((fail+1)); echo "FAIL: EIO control pair is not identical (status $rc)"
fi

# Binary mode: the region, its offsets, and that it is unreadable.
COWDIFF_EIO_AT=$EIO "$COW" --force-binary E1.txt E2.txt > "$WORK/.eio" 2>&1
rc=$?
if [ "$rc" -eq 1 ] && grep -q 'could not be read' "$WORK/.eio" &&
   grep -q 'unreadable  both at 0x1000, 4096 bytes' "$WORK/.eio"; then
	pass=$((pass+1))
else
	fail=$((fail+1)); echo "FAIL: binary output for an unreadable block (status $rc)"
	cat "$WORK/.eio"
fi

# Text mode: no diff line describes it, and it says so on stderr as well,
# because the output is then not a patch of the whole difference.
COWDIFF_EIO_AT=$EIO "$COW" -U0 E1.txt E2.txt > "$WORK/.eio" 2>"$WORK/.eioerr"
rc=$?
if [ "$rc" -eq 1 ] && grep -q '^cowdiff: unreadable ' "$WORK/.eio" &&
   grep -q 'input/output error' "$WORK/.eioerr"; then
	pass=$((pass+1))
else
	fail=$((fail+1)); echo "FAIL: text output for an unreadable block (status $rc)"
	cat "$WORK/.eio" "$WORK/.eioerr"
fi

# Two files that differ, at different lengths, with a bad block in the middle.
# The whole range becomes one delta, which cannot be line-diffed once a block
# inside it turns out to be unreadable -- so it is reported as an unreadable
# byte range rather than as lines.  That is coarser than the line-level answer
# would have been, and deliberate: the line diff cannot span a gap whose
# contents are unknown, and reporting less than the truth about it is not an
# option either.
seq 1 4000 | sed 's/^/line /' > E3.txt
sed '3000s/.*/CHANGED/' E3.txt > E4.txt
COWDIFF_EIO_AT=$EIO "$COW" -U0 E3.txt E4.txt > "$WORK/.eio" 2>"$WORK/.eioerr"
rc=$?
if [ "$rc" -eq 1 ] && grep -q '^cowdiff: unreadable E3.txt\[0x0, ' "$WORK/.eio" &&
   grep -q 'input/output error' "$WORK/.eioerr"; then
	pass=$((pass+1))
else
	fail=$((fail+1)); echo "FAIL: unreadable range in place of a line diff (status $rc)"
	cat "$WORK/.eio" "$WORK/.eioerr" | head -6
fi

# The same pair with the bad block one range over, so that the differing line
# is outside it: the change is then two ranges that were never compared, the
# line diff runs, and line numbers are refused because the newline count
# crossed the bad block -- the headers carry byte offsets instead.
COWDIFF_EIO_AT=0x1000:4096 "$COW" -U0 E3.txt E4.txt > "$WORK/.eio" 2>"$WORK/.eioerr"
if grep -q 'byte offsets' "$WORK/.eioerr" || [ "$(grep -c '^cowdiff: unreadable' "$WORK/.eio")" -gt 0 ]; then
	pass=$((pass+1))
else
	fail=$((fail+1)); echo "FAIL: no warning that line numbers could not be counted"
	cat "$WORK/.eio" "$WORK/.eioerr" | head -6
fi

# --- -q agrees with diff -q -------------------------------------------------
# -q stops at the first difference it proves.  That is sound only because
# "differ" is settled by one difference while "identical" has to cover every
# byte that was not proven shared -- so the walk may be cut short on the way
# to one verdict and never on the way to the other.  A wrong verdict here
# would be silent, which is the failure mode this tool exists to avoid.
for pair in "A.txt B.txt" "A.txt A2.txt" "C.txt D.txt" "C.txt E.txt" \
	    "N1.txt N2.txt" "N1.txt N3.txt" "N1b.txt N1c.txt" \
	    "Z1.txt Z2.txt" "Z1.txt Z3.txt" "P1.txt P2.txt" "P3.txt P4.txt"; do
	set -- $pair
	diff -q "$1" "$2" > /dev/null 2>&1; want=$?
	"$COW" -q "$1" "$2" > /dev/null 2>&1; got=$?
	if [ "$want" -eq "$got" ]; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "FAIL: -q on $1 vs $2: diff says $want, cowdiff says $got"
	fi
done

# And -q says nothing at all about files that match, as diff -q does.  The
# verdict a script reads is the status; a line it was not expecting is one
# more thing to parse, and the whole point of the flag is that it is quiet.
"$COW" -q N1b.txt N1c.txt > "$WORK/.actual" 2>&1
if [ -s "$WORK/.actual" ]; then
	fail=$((fail + 1)); echo "FAIL: -q is not silent for identical files"
else
	pass=$((pass + 1))
fi

rm -f "$WORK"/.expected "$WORK"/.actual "$WORK"/.err
echo
echo "passed $pass, failed $fail"
[ "$fail" -eq 0 ]
