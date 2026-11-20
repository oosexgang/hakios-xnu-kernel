#!/usr/bin/env python3
"""Match SpringBoard's long main-loop turns to timestamped stack samples.

Usage: tools/turn-stalls.py RUN.log RUN.log.live* [--from 82 --to 84]
Times are seconds since boot; the log files may overlap, so lines are deduplicated.
Requires the `tl: t MS at US woke US ...` format from inject_sb.c.
"""

import argparse


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--from", dest="start", type=float, default=0)
    ap.add_argument("--to", dest="end", type=float, default=float("inf"))
    ap.add_argument("--min-ms", type=float, default=25)
    args = ap.parse_args()
    turns, samples, touches = {}, {}, []
    for path in args.logs:
        with open(path, errors="replace") as log:
            for line in log:
                text = line[line.index("tl: "):].strip() if "tl: " in line else ""
                parts = text.split()
                try:
                    if len(parts) >= 5 and parts[1] == "a":
                        woke, commit, done = map(int, parts[2:5])
                        turns[woke] = (commit, done)
                    elif len(parts) >= 8 and parts[1] == "t" and parts[3] == "at" and parts[5] == "woke":
                        ms, at, woke = int(parts[2]), int(parts[4]), int(parts[6])
                        samples[at] = (woke, ms, " ".join(parts[7:]))
                    elif len(parts) >= 8 and parts[1] == "p" and parts[2] in ("1", "4"):
                        touches.append(int(parts[7]))
                except ValueError:
                    continue
    touches = sorted(set(touches))
    for woke, (commit, done) in sorted(turns.items()):
        if not args.start * 1e6 <= woke <= args.end * 1e6:
            continue
        before = (commit - woke) / 1000
        if before < args.min_ms:
            continue
        nearby = [(at, ms, stack) for at, (start, ms, stack) in samples.items()
                  if start == woke or (woke <= at <= commit and abs(start - woke) < 3000)]
        nearby.sort()
        gesture = max((t for t in touches if t <= woke), default=None)
        age = (woke - gesture) / 1000 if gesture is not None else None
        print(f"{woke / 1e6:8.3f}s: {before:6.1f} ms before commit, "
              f"{(done - commit) / 1000:5.1f} ms commit, "
              f"{age if age is not None else -1:6.0f} ms after gesture")
        for at, ms, stack in nearby:
            print(f"    +{(at - woke) / 1000:5.1f} ms ({ms} measured): {stack}")


if __name__ == "__main__":
    main()
