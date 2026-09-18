#!/bin/bash
# Benchmark cowdiff against GNU diff across shapes that stress different
# parts of it.  One shape is not a benchmark: a single change in a huge file
# is handled entirely by common prefix/suffix trimming and says nothing about
# the search, while scattered changes defeat that trim and exercise it.
#
# Every shape is built by writing both files from scratch, so none of them
# share extents unless the shape says so.  (Note that cp(1) reflinks by
# default on btrfs, so "cp a b" is NOT how you build an unshared pair.)
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
COW=$ROOT/cowdiff
W=${BENCH_DIR:-$ROOT/.bench}
SIZE=${BENCH_SIZE:-67108864}          # bytes per file
LINES=$((SIZE / 24))

[ -x "$COW" ] || { echo "build first: make"; exit 2; }
rm -rf "$W"; mkdir -p "$W" || exit 2
cd "$W" || exit 2

# A file of distinct lines: something for the anchor search to work with.
uniq_lines() { seq 1 "$LINES" | sed 's/^/line /'; }
# A file of one repeated line: nothing unique to split on, which is the shape
# that defeats patience-style anchoring.
repetitive() { yes "the quick brown fox jumps over the lazy dog 0123456789" |
	head -c "$SIZE"; }

now() { date +%s.%N; }
since() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%.2f", b-a}'; }

run() {                                # run LABEL CMD...
	local label=$1; shift
	local s e
	s=$(now)
	"$@" > /dev/null 2>&1
	local rc=$?
	e=$(now)
	printf '  %-22s %6ss  (status %d)\n' "$label" "$(since "$s" "$e")" "$rc"
}

shape() {                              # shape NAME
	echo "$1"
}

echo "### files are ${SIZE} bytes each ($((SIZE / 1048576)) MiB), $LINES lines"
echo

uniq_lines > A.txt
uniq_lines > B_ident.txt
uniq_lines | sed "${LINES}p" > /dev/null
uniq_lines | awk -v n="$LINES" 'NR==n{print "CHANGED"} NR!=n' > B_one.txt
uniq_lines | awk 'NR%1000==0{print "CHANGED"; next} {print}' > B_scatter.txt
uniq_lines | awk -v at='"$LINES"*0' '1' > /dev/null
uniq_lines | awk -v at=$((LINES / 10)) 'NR==at{print "INSERTED"} {print}' > B_insert.txt
seq 1 "$LINES" | sed 's/^/totally different content /' > B_unrelated.txt
repetitive > R_A.txt
repetitive | awk 'NR==50000{print "CHANGED"; next} {print}' > R_B.txt

# Control: a reflinked pair, which is the case cowdiff exists for.  The edit
# has to be made in place -- writing S_B through a shell redirect would build
# a fresh file and throw away the sharing this shape exists to measure.
cp --reflink=always A.txt S_A.txt
cp --reflink=always A.txt S_B.txt
printf 'CHANGED-CHANGED-CHANGED' |
	dd of=S_B.txt bs=1 seek=$((SIZE / 2)) conv=notrunc status=none

shape "identical, unshared        (must read everything, then confirm equal)"
run "diff -u" diff -u A.txt B_ident.txt
run "cowdiff -U0" "$COW" -U0 A.txt B_ident.txt
run "cowdiff --byte-offsets" "$COW" --byte-offsets A.txt B_ident.txt
echo

shape "one change                 (prefix/suffix trim does the work)"
run "diff -u" diff -u A.txt B_one.txt
run "cowdiff -U0" "$COW" -U0 A.txt B_one.txt
run "cowdiff --byte-offsets" "$COW" --byte-offsets A.txt B_one.txt
run "cowdiff --force-binary" "$COW" --force-binary A.txt B_one.txt
echo

shape "scattered changes          (defeats trimming)"
run "diff -u" diff -u A.txt B_scatter.txt
run "cowdiff -U0" "$COW" -U0 A.txt B_scatter.txt
run "cowdiff --byte-offsets" "$COW" --byte-offsets A.txt B_scatter.txt
run "cowdiff --force-binary" "$COW" --force-binary A.txt B_scatter.txt
echo

shape "line inserted near the top (misaligns everything after it)"
run "diff -u" diff -u A.txt B_insert.txt
run "cowdiff -U0" "$COW" -U0 A.txt B_insert.txt
run "cowdiff --byte-offsets" "$COW" --byte-offsets A.txt B_insert.txt
echo

shape "unrelated                  (nothing in common)"
run "diff -u" diff -u A.txt B_unrelated.txt
run "cowdiff -U0" "$COW" -U0 A.txt B_unrelated.txt
run "cowdiff --byte-offsets" "$COW" --byte-offsets A.txt B_unrelated.txt
echo

shape "no unique lines            (nothing for the anchor search to hold)"
run "diff -u" diff -u R_A.txt R_B.txt
run "cowdiff -U0" "$COW" -U0 R_A.txt R_B.txt
run "cowdiff --byte-offsets" "$COW" --byte-offsets R_A.txt R_B.txt
echo

shape "reflinked + one change     (the case cowdiff exists for)"
run "diff -u" diff -u S_A.txt S_B.txt
run "cowdiff -U0" "$COW" -U0 S_A.txt S_B.txt
run "cowdiff --byte-offsets" "$COW" --byte-offsets S_A.txt S_B.txt
echo

echo "### correctness: cowdiff must agree with diff about whether they differ"
for pair in "A.txt B_ident.txt" "A.txt B_one.txt" "A.txt B_scatter.txt" \
	    "A.txt B_insert.txt" "A.txt B_unrelated.txt" "R_A.txt R_B.txt" \
	    "S_A.txt S_B.txt"; do
	set -- $pair
	diff -q "$1" "$2" > /dev/null 2>&1; want=$?
	"$COW" -q "$1" "$2" > /dev/null 2>&1; got=$?
	printf '  %-28s diff=%d cowdiff=%d %s\n' "$pair" "$want" "$got" \
		"$([ "$want" -eq "$got" ] && echo ok || echo MISMATCH)"
done
