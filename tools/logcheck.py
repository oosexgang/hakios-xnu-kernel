#!/usr/bin/env python3
"""What a run's log says, part by part: which came up, which stopped and why, which were not asked for -- the first
thing to look at after a run on a phone, and what to send back when one did not work.

  tools/logcheck.py [LOG...]        the logs named; the newest of logs/ (./bench's) when none is
  tools/logcheck.py --all LOG       every line each part said, not only the one that decides
  tools/logcheck.py --against OLD NEW   what changed from one run to the other: each part whose state is not the same,
                                    the kernel's start, panic and return; status 1 when something that was ok is not

A part is a driver by the prefix of its lines ("adreno:", "bt-uart:" ...). Its state is the line that decides it:
  ok        a line that says it came up (its driver's own words for it, below)
  STOPPED   a line that says it did not, or gave up -- that line is shown
  disabled  "PART: disabled: ..." -- the board or the boot arguments did not ask for it, or could not
  said      lines of it, and none of those
and the kernel's own: whether it started, whether it panicked (the panic's line), whether it went back to the loader.
Status 1 when something stopped or the kernel panicked.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# (part, its lines' prefix, what says it came up, what says it did not): each driver's own words (drivers/board/*.c)
PARTS = [
    ('Identity (loader)', 'identity', [r'from the payload \('], [r"the payload's is not one"]),
    ('GPU', 'adreno', [r'A PICTURE DRAWN BY THE GPU IS ON THE PANEL', r'the GPU runs'],
     [r'THE GPU IS NOT STARTED', r'startup stops', r'FAILED']),
    ('Bluetooth controller', 'bt-uart', [r"the controller is Darwin's from here on", r'identification complete', r'bt-uart: done'],
     [r'bring-up stopped', r'the firmware was not taken', r'refused', r'timed out']),
    ('Bluetooth for Darwin', 'bt', [r'connected by bluetoothd', r'the channel opened', r"is Darwin's: service"],
     [r'cannot be (made|registered)']),
    ('Bluetooth service', 'bt', [r'service "bluetooth" registered'], [r'no service "bluetooth"']),
    ('Storage (own driver)', 'ufs', [r'native check .*: \d+ spans, .* 0 NOT THE SAME, 0 failed reads', r'native: .*transfer list'],
     [r'DIFFERENT at byte', r': STOP', r'NOT AS IT WAS', r"this kernel's own read failed"]),
    ('Flashlight', 'torch', [r"THE LIGHT IS DARWIN'S"], [r'COULD NOT BE SWITCHED OFF', r'given up on', r'not lit:']),
    ('Vibration', 'haptic', [r'THE MOTOR PLAYS WHAT DARWIN PLAYS'], [r'STILL PLAYING']),
    ('Motion sensor', 'imu', [r'the orientation is followed from here on'], [r'NOTHING MORE IS DONE', r'COULD NOT BE POWERED DOWN']),
    ('Microphones', 'mic', [r"THE MICROPHONE IS DARWIN'S", r'LISTENING'], [r'NO READ HAS COME BACK']),
    ('Sound', 'sound', [r'WHAT DARWIN PLAYS GOES TO THE SPEAKERS', r'THE TONE PLAYS NOW'],
     [r'DID NOT START', r'HAS GIVEN NO PERIOD', r'HAS NOT GIVEN EVERY PERIOD', r'REFUSED', r'DID NOT TAKE']),
    ('Speaker amplifiers', 'speaker', [r'RUNS, unmuted', r'BOTH AMPLIFIERS ARE CONFIGURED'], [r'failed']),
    ('Audio DSP', 'adsp', [r'versions .* the channel'], [r'DOES NOT ACKNOWLEDGE', r'taken down', r'SAYS IT HAS GIVEN UP']),
    ('Wi-Fi DSP', 'wifi', [r'QRTR: the DSP is node'], [r'NOT A WORD FROM THE DSP', r'SAYS IT HAS GIVEN UP']),
    ('Wi-Fi driver', 'lx', [r'using the early radio pool'],
     [r'THE DRIVER IS NOT BUILT IN', r'THE HANDSHAKE DID NOT COME THROUGH', r"MICHAEL'S CHECK FAILED TWICE"]),
    ('Touch (Goodix)', 'goodix', [r'touch screen ready'], [r'failed', r'no (goodix|focaltech) touch screen']),
    ('Touch (FocalTech)', 'fts', [r'touchscreen ready'], [r'stopped after repeated', r'failed', r'touchscreen unavailable']),
    ('Touch (FocalTech on I2C)', 'ft5x06', [r'touch screen ready'], [r'stopped: the chip does not come back', r'touch screen unavailable']),
    ('Touch (FocalTech firmware)', 'focaltech', [], [r'failed', r'is not started']),
    ('Buttons', 'buttons', [r'believed from here on'],
     [r'giving up', r'of the arbiter only: no buttons', r'which has no keys this driver reads', r'has not been read for']),
    ('Camera', 'camera', [r'/dev/hakios-camera: the'], [r'REFUSED', r'not ready', r'nothing more is written in this run',
                                                     r'no frames in this run']),
    ('CPU frequency', 'cpufreq', [r'group \d+ .*MHz'], [r'failed', r'not in charge of its clock']),
    ('Memory bandwidth', 'ddr', [r'memory bandwidth vote \d+ on'], [r'no vote']),
    ('Temperature', 'thermal', [r'trip \d'], [r'does not hold', r'answered before (is|are) silent']),
    ('Brightness', 'backlight', [r'brightness'], [r'reports an error', r'switched off: no more']),
    ('Battery', 'battery', [r'%'], [r'failed']),
]
PANIC = re.compile(r'panic\(cpu \d+ caller|^panic:|Kernel panic|Debugger called: <panic>')


def states(text):
    """{part: (state, the line that decides it, all its lines)} of a log, and the kernel's (started, panic, back)."""
    lines = text.split('\n')
    kernel = (any('Darwin Kernel Version' in l for l in lines),
              next(((n, l) for n, l in enumerate(lines, 1) if PANIC.search(l)), None),
              any(re.search(r'kernel returned: log at', l) for l in lines))
    parts = {}
    for name, prefix, good, stop in PARTS:
        own = [l.strip() for l in lines if re.match(rf'\s*(\[[^\]]*\]\s*)?{re.escape(prefix)}: ', l)]
        if not own:
            continue
        if prefix == 'bt' and not any(re.search(p, l) for l in own for p in good + stop):
            continue                    # (a line of bt: that is the other row's)
        state, why = 'said', own[-1]
        disabled = next((l for l in own if re.search(rf'{re.escape(prefix)}: disabled', l)), None)
        stopped = next((l for l in own for p in stop if re.search(p, l)), None)
        # (the first of a part's words that is there: they are in the order of how far each says it got)
        came_up = next((l for p in good for l in own if re.search(p, l)), None)
        if stopped:
            state, why = 'STOPPED', stopped
        elif came_up:
            state, why = 'ok', came_up
        elif disabled:
            state, why = 'disabled', disabled
        parts[name] = (state, why, own)
    return kernel, parts


def against(old_text, new_text, out=None):
    """What changed from the run of OLD_TEXT to that of NEW_TEXT; true when something that was ok is not."""
    out = out or sys.stdout
    (old_kernel, old), (new_kernel, new) = states(old_text), states(new_text)
    worse = False
    for what, a, b in (('started', old_kernel[0], new_kernel[0]), ('panicked', old_kernel[1] is not None, new_kernel[1] is not None),
                       ('came back to the loader', old_kernel[2], new_kernel[2])):
        if a != b:
            print(f'kernel {what}: {"yes" if a else "no"} -> {"yes" if b else "no"}', file=out)
            worse |= (what == 'panicked') == b
    changed = 0
    for name in [n for n, _, _, _ in PARTS]:
        a, b = old.get(name, ('(nothing)', '', [])), new.get(name, ('(nothing)', '', []))
        if a[0] != b[0]:
            changed += 1
            print(f'  {name}: {a[0]} -> {b[0]}' + (f'   {b[1][:140]}' if b[1] else ''), file=out)
            worse |= a[0] == 'ok' and b[0] != 'ok'
    if not changed:
        print('  every part as it was', file=out)
    return worse


def check(text, everything=False, out=None):
    out = out or sys.stdout
    (started, panic, back), parts = states(text)
    print(f'kernel: {"started" if started else "NOT STARTED (no Darwin Kernel Version line)"}' +
          (f'; PANIC at line {panic[0]}: {panic[1].strip()}' if panic else '; no panic') +
          ('; back in the loader' if back else ''), file=out)
    width = max((len(name) for name in parts), default=0)
    for name, (state, why, own) in parts.items():
        print(f'  {name:{width}}  {state:8}  {why[:150]}', file=out)
        if everything:
            for line in own:
                print(f'  {"":{width}}            {line[:150]}', file=out)
    if not parts:
        print('  (no line of any part: the run stopped before the drivers, or the log is not a run\'s)', file=out)
    return panic is not None or not started or any(state == 'STOPPED' for state, _, _ in parts.values())


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('logs', nargs='*', metavar='LOG')
    parser.add_argument('--all', action='store_true', help='every line of each part')
    parser.add_argument('--against', metavar='OLD', help='what changed from the run of OLD to that of the LOG named')
    options = parser.parse_args(arguments)
    if options.against:
        if len(options.logs) != 1:
            parser.error('--against OLD NEW: one log to compare with OLD')
        print(f'{options.against} -> {options.logs[0]}:')
        return 1 if against(Path(options.against).read_text(errors='replace'), Path(options.logs[0]).read_text(errors='replace')) else 0
    logs = [Path(p) for p in options.logs]
    if not logs:
        # the files that exist (a dangling logs/last.log from a fetch that failed must not crash the stat below), newest last
        found = sorted((p for p in (ROOT / 'logs').glob('*.log') if p.is_file()), key=lambda p: p.stat().st_mtime)
        if not found:
            parser.error('no log named, and none in logs/ (./bench run leaves them there)')
        logs = found[-1:]
    bad = False
    for log in logs:
        print(f'{log}:')
        try:
            text = log.read_text(errors='replace')
        except OSError as error:
            print(f'  (not read: {error})')
            bad = True
            continue
        bad |= check(text, options.all)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
