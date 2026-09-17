#!/bin/bash
# Compare cowdiff's text output against GNU diff on a spread of edits.
#
# The hunk bodies and headers have to match exactly; the only permitted
# difference is that diff(1) stamps a timestamp after each filename and we do
# not, so those are stripped before comparing.
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
		diff -u "$A" "$B" | sed -e 's/^\(---\|+++\) \([^\t]*\)\t.*/\1 \2/' > "$exp"
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
	LC_ALL=C diff -U"$n" A.txt B.txt |
		sed -e 's/^\(---\|+++\) \([^\t]*\)\t.*/\1 \2/' > "$WORK/.expected"
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

LC_ALL=C diff -ru R1 R2 |
	sed -e 's/^\(---\|+++\) \([^\t]*\)\t.*/\1 \2/' > "$WORK/.expected"
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

rm -f "$WORK"/.expected "$WORK"/.actual "$WORK"/.err
echo
echo "passed $pass, failed $fail"
[ "$fail" -eq 0 ]
