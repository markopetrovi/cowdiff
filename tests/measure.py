#!/usr/bin/env python3
"""Time cowdiff on the benchmark shapes, on a machine that is doing other work.

tests/bench.sh runs each command once, which is not enough to see a change of
a few percent: the same shape has been measured varying by more than 2x
between runs when something else was running, and GNU diff -- which no one was
editing -- has moved 40% between two runs of bench.sh.  Two things fix it.

  * Repetition.  The minimum of N runs is the run that was disturbed least,
    which is the closest thing available to the true cost.
  * Pairing.  When comparing two binaries, run them alternately and take the
    ratio of each adjacent pair.  The machine's speed drifts over a session --
    it is a laptop, and it is often on battery -- and a ratio taken from two
    runs seconds apart cancels that drift.  The median of those ratios has
    been stable to about a tenth of a percent across sessions, where the
    spread of the individual ratios was 8-35%.

Nothing here needs privileges.  SCHED_FIFO was tried and made the individual
runs *noisier*, not quieter: preempting everything else perturbs more than it
protects, and the residual variation is the CPU's clock, which no scheduling
class can hold still.

The fixtures are the ones tests/bench.sh builds, and they are large, so run
that first if .bench is empty.

usage: measure.py [-n N] [--against OTHER] [BINARY]
"""

import argparse
import os
import resource
import statistics
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH = os.path.join(ROOT, ".bench")

# (label, file A, file B) -- the shapes bench.sh builds and why they matter.
SHAPES = [
    ("reflink+one change", "S_A.txt", "S_B.txt"),
    ("identical unshared", "A.txt", "B_ident.txt"),
    ("scattered changes", "A.txt", "B_scatter.txt"),
    ("scattered+length", "A.txt", "B_scatlen.txt"),
    ("one change", "A.txt", "B_one.txt"),
    ("insert near top", "A.txt", "B_insert.txt"),
    ("unrelated", "A.txt", "B_unrelated.txt"),
    ("no unique lines", "R_A.txt", "R_B.txt"),
]


def run_timed(argv):
    """Child CPU seconds (user + system) for one run.

    CPU time rather than wall time: it ignores the stretches where the process
    was descheduled, which is most of what another program's work costs us.
    """
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    subprocess.run(argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    return ((after.ru_utime - before.ru_utime) +
            (after.ru_stime - before.ru_stime))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", type=int, default=9,
                    help="repetitions per command (default 9)")
    ap.add_argument("--against", metavar="OTHER",
                    help="compare BINARY against this binary, paired")
    ap.add_argument("binary", nargs="?", default=os.path.join(ROOT, "cowdiff"))
    args = ap.parse_args()

    for name in ("A.txt", "S_A.txt", "R_A.txt"):
        if not os.path.exists(os.path.join(BENCH, name)):
            sys.exit(f"{BENCH} has no fixtures: run tests/bench.sh first")

    print(f"{args.binary}  ({args.n} runs per command, CPU seconds)")

    if args.against:
        print(f"paired against {args.against}; ratio is the median of the "
              f"per-pair ratios,\nbelow 1 meaning the first binary is faster\n")
        print(f"{'shape':<20} {'other':>8} {'this':>8} {'ratio':>7}  "
              f"{'min':>6} {'med':>6} {'max':>6}")
    else:
        print(f"\n{'shape':<20} {'cowdiff -U0':>12} {'diff -u':>9}")

    for label, fa, fb in SHAPES:
        pa, pb = os.path.join(BENCH, fa), os.path.join(BENCH, fb)
        mine = ["-U0", pa, pb]

        if not args.against:
            ours = min(run_timed([args.binary] + mine)
                       for _ in range(args.n))
            theirs = min(run_timed(["diff", "-u", pa, pb])
                         for _ in range(args.n))
            print(f"{label:<20} {ours:>12.3f} {theirs:>9.3f}")
            continue

        ratios, others, ourses = [], [], []
        for _ in range(args.n):
            other = run_timed([args.against] + mine)
            ours = run_timed([args.binary] + mine)
            ratios.append(ours / other)
            others.append(other)
            ourses.append(ours)
        med = statistics.median(ratios)
        print(f"{label:<20} {min(others):>8.3f} {min(ourses):>8.3f} "
              f"{med:>7.3f}  {min(ratios):>6.3f} {med:>6.3f} "
              f"{max(ratios):>6.3f}")


if __name__ == "__main__":
    main()
