#!/usr/bin/env python3
"""Whether a WAV file holds a tone of a given frequency: what QEMU's wav audio back end wrote of what
the machine played (workbench/qemu_boot_test.sh). The frequency is counted from the zero crossings of
the part that is not silence, and has to be within 2% of the one asked for.

    workbench/tone_in_wav.py FILE HZ
"""
import sys
import wave
from array import array


def tone(path):
    """The frequency of the sound in a 16-bit WAV file's first channel, and how many seconds of it there are."""
    with wave.open(path, 'rb') as w:
        if w.getsampwidth() != 2:
            raise ValueError(f"{path}: {8 * w.getsampwidth()}-bit samples, not 16")
        rate, channels = w.getframerate(), w.getnchannels()
        samples = array('h', w.readframes(w.getnframes()))
    if sys.byteorder == 'big':
        samples.byteswap()
    left = samples[::channels]
    sound = [i for i, v in enumerate(left) if abs(v) > 1000]
    if not sound:
        return 0.0, 0.0
    part = left[sound[0]:sound[-1] + 1]
    crossings = sum(1 for a, b in zip(part, part[1:]) if (a < 0) != (b < 0))
    seconds = len(part) / rate
    return crossings / 2 / seconds, seconds


def main(argv):
    if len(argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    try:
        hz, seconds = tone(argv[1])
    except (OSError, EOFError, ValueError, wave.Error) as e:
        print(f"tone_in_wav: {e}", file=sys.stderr)
        return 1
    want = float(argv[2])
    print(f"tone_in_wav: {hz:.1f} Hz for {seconds:.2f} s (asked: {want:g} Hz)")
    return 0 if seconds > 0.5 and abs(hz - want) <= want * 0.02 else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
