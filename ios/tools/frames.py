#!/usr/bin/env python3
"""frames.py RUN.log [AFTER_S]: frame pacing while something moves, from a run with boot-args gpu_log=1 touch_lat=1
(rc.jobs.fps drives the gestures).

A run's log keeps only its last minute or two of tl: lines, so the snapshots xl takes during the run
(XL_FOLLOW_KEEP=1: RUN.log.live, RUN.log.live.N) are read first and the tl: lines they share counted once.
The gestures are the bursts of touches (tl: i), and the gestures less than 2 s apart one phase of the run (the
home screen's pages, Settings' list). For each phase, over the AFTER_S seconds after each gesture (0.7):

  frames a second    the panel's frames (tl: v, the presenter's copy done) over the time they took
  within 18 ms       the share of frame-to-frame times a 60 Hz panel shows as one refresh
  gaps over 50 ms    the hitches one sees
  GPU                the window server's command buffers (tl: M), median and 90th percentile

The time between gestures is left out: with nothing moving the window server draws when the clock ticks, and that is
no frame rate."""
import glob, os, re, statistics as st, sys


def lines_of(path):
    """the run's lines, the snapshots first, each tl: line once"""
    snaps = sorted(glob.glob(path + '.live.*'), key=lambda p: int(p.rsplit('.', 1)[1]) if p.rsplit('.', 1)[1].isdigit() else -1)
    seen = set()
    for p in snaps + [path + '.live', path]:
        if not os.path.exists(p):
            continue
        for line in open(p, errors='replace'):
            if 'tl: ' in line:
                if line in seen:
                    continue
                seen.add(line)
            yield line


def q(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p * len(xs)))]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    after = float(sys.argv[2]) * 1e6 if len(sys.argv) > 2 else 0.7e6
    panel, touches, buffers, swaps = set(), set(), set(), set()
    for line in lines_of(sys.argv[1]):
        i = line.find('tl: ')
        if i < 0:
            continue
        t = line[i + 4:]
        m = re.match(r'v \d+ ready \d+ shown \d+ done (\d+)', t)
        if m:
            panel.add(int(m.group(1)))
            continue
        m = re.match(r'i \d+ posted (\d+)', t)
        if m:
            touches.add(int(m.group(1)))
            continue
        m = re.match(r'M made (\d+) committed (\d+) done (\d+) gpu (\d+)', t)
        if m:
            buffers.add(tuple(int(x) for x in m.groups()))
            continue
        m = re.match(r's \d+ at (\d+)', t)
        if m:
            swaps.add(int(m.group(1)))
    if not panel or not touches:
        sys.exit('frames.py: no tl: v or tl: i lines (boot-args gpu_log=1 touch_lat=1?)')
    panel, touches, buffers = sorted(panel), sorted(touches), sorted(buffers)
    t0 = min(swaps) if swaps else panel[0]
    gestures = []
    for x in touches:
        if not gestures or x - gestures[-1] > 300000:
            gestures.append(x)
    phases = []
    for g in gestures:
        if phases and g - phases[-1][-1] < 2e6:
            phases[-1].append(g)
        else:
            phases.append([g])
    print(f"{sys.argv[1]}: {len(gestures)} gestures in {len(phases)} phases; the {after / 1e6:.1f} s after each gesture")
    for ph in phases:
        gaps, gpu = [], []
        for g in ph:
            shown = [v for v in panel if g <= v <= g + after]
            gaps += [b - a for a, b in zip(shown, shown[1:])]
            gpu += [m[3] for m in buffers if g <= m[1] <= g + after]
        head = f"  from {(ph[0] - t0) / 1e6:6.1f} s, {len(ph):2d} gesture{'s' if len(ph) > 1 else ' '}:"
        if not gaps:
            print(head, "no frames")
            continue
        line = (f"{head} {len(gaps) / (sum(gaps) / 1e6):5.1f} frames a second, {100 * sum(1 for x in gaps if x <= 18000) // len(gaps):3d}% "
                f"within 18 ms, {sum(1 for x in gaps if x >= 50000):3d} gaps over 50 ms (the longest {max(gaps) / 1e3:.0f})")
        if gpu:
            line += f"; GPU {st.median(gpu) / 1e3:.1f} ms, 90% {q(gpu, 0.9) / 1e3:.1f}"
        print(line)


if __name__ == '__main__':
    main()
