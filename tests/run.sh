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

rm -f "$WORK"/.expected "$WORK"/.actual "$WORK"/.err
echo
echo "passed $pass, failed $fail"
[ "$fail" -eq 0 ]
