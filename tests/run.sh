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
for n in 0 1 2 3 7; do
	LC_ALL=C diff -U"$n" A.txt B.txt > "$WORK/.expected"
	"$COW" -U"$n" A.txt B.txt > "$WORK/.actual"
	if cmp -s "$WORK/.expected" "$WORK/.actual"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "FAIL: -U$n context"
		diff -u "$WORK/.expected" "$WORK/.actual" | head -20
	fi
done

# -u asks for the format we always emit and -a is diff's flag for forcing
# text; both must be accepted rather than refused.  These files differ, so
# acceptance means status 1, and only status 2 counts as a rejection.
for opt in -u --unified -a --text; do
	"$COW" "$opt" A.txt B.txt > /dev/null 2>&1
	rc=$?
	if [ "$rc" -le 1 ]; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1)); echo "FAIL: $opt rejected (status $rc)"
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

rm -f "$WORK"/.expected "$WORK"/.actual "$WORK"/.err
echo
echo "passed $pass, failed $fail"
[ "$fail" -eq 0 ]
