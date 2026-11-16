#!/usr/bin/env python3
"""
frames.py RUN.log [MORE.log ...]: how evenly the frames of a run's gestures came, and what held up the slow ones.
(More logs: the live ones of the same run, XL_FOLLOW=N XL_FOLLOW_KEEP=1, which keep what the run's log loses.)

Reads the "tl:" lines a run with boot-arg touch_lat=1 writes (see tools/latency.py for what each
says): the samples and buttons of rwtouch (tl: p), the window server's frames (tl: g: began, ended),
their hand-over (tl: s), the display driver's GPU wait and copy (tl: v), and every wait of a program
for the GPU of a millisecond or more (tl: f). Every gesture -- from a touch going down, or a button,
to the next one or 2.5 s later -- is a segment; for each:

  frames a second while frames came; the share of frame-to-frame times (as the frames reached the
  panel's memory) over 16.7 ms (counted from 18.0: a 60 Hz frame missed) and over 33 ms (from 34.5);
  with boot-arg cpuload=MS (tl: u), how busy each processor core was meanwhile, in percent;
  and for the frames over 16.7 ms, where the time between the previous frame and this one went:
    draw      the window server drawing it (its first draw to its glFlush done: CPU, Mesa included)
    gpu       the GPU after the hand-over (SwapEnd to done)
    idle      the window server not drawing at all before it began (waiting for a commit, a fence, ...)
    copy      the display driver's copy into the panel's memory

The thresholds are as the coordinator asked for them; 18.0 rather than 16.7 because a frame of a steady
60 comes 16.6 to 16.8 ms after the one before.
"""
import sys

OVER_ONE, OVER_TWO, SEGMENT_US = 18000, 34500, 2500000


def lines_of(paths):
    seen, out = set(), []
    for path in paths:
      with open(path, 'rb') as f:
        for raw in f:
            text = raw.decode('utf-8', 'replace')
            at = text.find('tl: ')
            if at < 0:
                continue
            line = text[at:].strip()
            if line not in seen:
                seen.add(line)
                out.append(line)
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    gestures, frames, swaps, shows, waits, loads = [], [], {}, {}, [], []
    for line in lines_of(sys.argv[1:]):
        w = line.split()
        try:
            if w[1] == 'p' and len(w) >= 8 and int(w[2]) in (1, 4):
                gestures.append((int(w[7]), int(w[2]), int(w[3]), int(w[4]), int(w[5])))
            elif w[1] == 'g' and len(w) >= 8:
                frames.append((int(w[4]), int(w[6]), int(w[7])))
            elif w[1] == 's' and len(w) >= 5:
                swaps[int(w[2])] = int(w[4])
            elif w[1] == 'v' and len(w) >= 9:
                shows[int(w[2])] = (int(w[4]), int(w[6]), int(w[8]))
            elif w[1] == 'f' and len(w) >= 6:
                waits.append((int(w[5]), w[2], int(w[3])))
            elif w[1] == 'u' and len(w) >= 4:
                loads.append((int(w[3]), {int(a): int(b) for a, b in (x.split(':') for x in w[4:])}))
        except ValueError:
            pass
    gestures.sort()
    frames.sort(key=lambda f: f[1])
    # every frame on the panel, with the drawing it came from
    timeline = []
    for sid in sorted(shows):
        if sid not in swaps:
            continue
        swap = swaps[sid]
        drawn = [f for f in frames if f[1] <= swap]
        if not drawn:
            continue
        began, ended, draws = drawn[-1]
        ready, shown, done = shows[sid]
        timeline.append(dict(began=began, ended=ended, swap=swap, ready=ready, shown=shown, done=done))
    timeline.sort(key=lambda f: f['done'])
    all_counts = [0, 0, 0]
    kinds, previous = {}, ''          # the same gesture done more than once: its frames together
    print(f'frames.py: {len(gestures)} gestures, {len(timeline)} frames on the panel, {len(waits)} waits for the GPU '
          f'of 1 ms or more')
    for i, (start, kind, button, x, y) in enumerate(gestures):
        end = min(gestures[i + 1][0] if i + 1 < len(gestures) else start + SEGMENT_US, start + SEGMENT_US)
        chosen = [f for f in timeline if start <= f['done'] < end]
        what = f'button {button}' if kind == 4 else f'touch at {x / 655.35:.0f}%,{y / 655.35:.0f}%'
        key = f'{what} after {previous}' if kind == 4 else what
        previous = what
        if len(chosen) < 3:
            print(f'{i + 1:2}. {start / 1e6:9.3f} s {what}: {len(chosen)} frames')
            continue
        gaps = [(b['done'] - a['done'], a, b) for a, b in zip(chosen, chosen[1:])]
        moving = [g for g in gaps if g[0] < 100000]             # (a pause is the end of the movement, not a slow frame)
        span = sum(g[0] for g in moving)
        over1 = [g for g in moving if g[0] > OVER_ONE]
        over2 = [g for g in moving if g[0] > OVER_TWO]
        all_counts[0] += len(moving)
        all_counts[1] += len(over1)
        all_counts[2] += len(over2)
        k = kinds.setdefault(key, [0, 0, 0, 0, 0])
        k[0] += 1
        k[1] += len(moving)
        k[2] += span
        k[3] += len(over1)
        k[4] += len(over2)
        fps = len(moving) * 1e6 / span if span else 0
        line = (f'{i + 1:2}. {start / 1e6:9.3f} s {what}: {len(moving)} frames at {fps:4.1f} a second; '
                f'over 16.7 ms {100 * len(over1) / max(1, len(moving)):3.0f}%, over 33 ms {100 * len(over2) / max(1, len(moving)):3.0f}%')
        if over1:
            # where the slow frames' time went (means over them)
            def mean(values):
                return sum(values) / len(values) / 1000 if values else 0
            draw = mean([b['ended'] - b['began'] for _, a, b in over1])
            gpu = mean([b['ready'] - b['swap'] for _, a, b in over1])
            idle = mean([b['began'] - a['ready'] for _, a, b in over1])
            copy = mean([b['done'] - b['shown'] for _, a, b in over1])
            line += f'; the slow ones: draw {draw:4.1f}, gpu {gpu:4.1f}, idle before {idle:5.1f}, copy {copy:3.1f} ms'
            stalls = [w for w in waits if start <= w[0] < end]
            if stalls:
                line += f'; {len(stalls)} waits for the GPU ({sum(w[2] for w in stalls) / 1000:.0f} ms)'
        # the load samples that end within the movement (each covers the cpuload=MS before it)
        within = [l for t, l in loads if chosen[0]['done'] < t <= chosen[-1]['done']]
        if within:
            cores = sorted({c for l in within for c in l}, reverse=True)
            line += '; cores busy ' + ' '.join(f'{c}:{sum(l.get(c, 0) for l in within) / len(within):.0f}%' for c in cores)
        print(line)
    if any(k[0] > 1 for k in kinds.values()):
        print('the same gestures together:')
        for key, (times, steps, span, over1, over2) in kinds.items():
            print(f'  {key:40} {times} times, {steps:4} frames at {steps * 1e6 / span if span else 0:4.1f} a second; '
                  f'over 16.7 ms {100 * over1 / max(1, steps):3.0f}%, over 33 ms {100 * over2 / max(1, steps):3.0f}%')
    if all_counts[0]:
        print(f'all gestures: {all_counts[0]} frame steps, over 16.7 ms {100 * all_counts[1] / all_counts[0]:.0f}%, '
              f'over 33 ms {100 * all_counts[2] / all_counts[0]:.0f}%')


if __name__ == '__main__':
    main()
