#!/usr/bin/env python3
"""frames.py RUN.log [AFTER_S]: frame pacing while something moves, from a run with boot-args gpu_log=1 touch_lat=1
(rc.jobs.fps drives the gestures).

A run's log keeps only its last minute or two of tl: lines, so the snapshots bench takes during the run
(BENCH_FOLLOW_KEEP=1: RUN.log.live, RUN.log.live.N) are read first and the tl: lines they share counted once.
The gestures are the bursts of touches (tl: i), and the gestures less than 2 s apart one phase of the run (the
home screen's pages, Settings' list). For each phase, over the AFTER_S seconds after each gesture (0.7):

  frames a second    the panel's frames (tl: v, the presenter's copy done) over the time they took
  within 18 ms       the share of frame-to-frame times a 60 Hz panel shows as one refresh (within 10: at 120 Hz)
  gaps over 50 ms    the hitches one sees
  GPU                the window server's command buffers (tl: M), median and 90th percentile
  battery            with battery=1 (tl: b): the charge shown, the current into the battery, whether on a charger
  heat               with the kernel's thermal lines (tl: h, thermal=1, the default): the hottest sensor, and the
                     least each group of cores was allowed by the SoC's own limits hardware (below what was asked
                     for: throttled by heat) and the GPU's clock, over the phase

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
    panel, touches, buffers, swaps, heat, battery = set(), set(), set(), set(), set(), set()
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
            continue
        m = re.match(r'h at (\d+) hottest (-?\d+) sensor \d+ limit (\S+) asked (\S+) gpu (\d+)', t)
        if m:
            heat.add((int(m.group(1)), int(m.group(2)), m.group(3), m.group(4), int(m.group(5))))
            continue
        m = re.match(r'b at (\d+) shown (-?\d+) mv (-?\d+) ma (-?\d+) c (-?\d+) external (\d)', t)
        if m:
            battery.add(tuple(int(x) for x in m.groups()))
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
                f"within 18 ms ({100 * sum(1 for x in gaps if x <= 10000) // len(gaps)}% within 10), "
                f"{sum(1 for x in gaps if x >= 50000):3d} gaps over 50 ms (the longest {max(gaps) / 1e3:.0f})")
        if gpu:
            line += f"; GPU {st.median(gpu) / 1e3:.1f} ms, 90% {q(gpu, 0.9) / 1e3:.1f}"
        print(line)
        hot = [h for h in heat if ph[0] - 1e6 <= h[0] <= ph[-1] + after + 1e6]
        if hot:
            least = asked = None
            for h in hot:
                if h[2] == '-':
                    continue
                lim = [int(x) for x in h[2].split(',')]
                least = lim if least is None else [min(a, b) for a, b in zip(least, lim)]
                asked = [int(x) for x in h[3].split(',')]
            gpus = [h[4] for h in hot if h[4] > 0]
            print(f"  {'':27s} heat: up to {max(h[1] for h in hot) / 10:.1f} C"
                  + (f"; cores allowed at least {'/'.join(map(str, least))} MHz of {'/'.join(map(str, asked))}" if least else "")
                  + (f"; GPU {min(gpus)}-{max(gpus)} MHz" if gpus else ""))
        bat = sorted(b for b in battery if ph[0] - 5e6 <= b[0] <= ph[-1] + after + 5e6)
        if bat:
            print(f"  {'':27s} battery: {bat[0][1] / 10:.1f}% to {bat[-1][1] / 10:.1f}%, "
                  f"{min(b[3] for b in bat)} to {max(b[3] for b in bat)} mA in (less than 0: out), "
                  f"{'on a charger' if any(b[5] for b in bat) else 'not on a charger'}, {max(b[4] for b in bat)} C")


if __name__ == '__main__':
    main()
