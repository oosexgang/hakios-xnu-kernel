#!/usr/bin/env python3
"""The helpers workbench/qemu_boot_test.sh judges with, against files made here: tone_in_wav.py (the frequency
of what the sound device played) and screen_shows_draw.py (whether the screen shows the shell's picture)."""
import importlib.util
import math
import struct
import sys
import tempfile
import unittest
import wave
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent


def load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / f'{name}.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


tone_in_wav = load('tone_in_wav')
screen_shows_draw = load('screen_shows_draw')


def write_wav(path, hz, seconds, rate=44100, silence=0.1, channels=2):
    """A sine of `hz` for `seconds`, with `silence` seconds of nothing either side."""
    quiet = [0] * int(silence * rate)
    sound = [int(8000 * math.sin(2 * math.pi * hz * i / rate)) for i in range(int(seconds * rate))]
    with wave.open(str(path), 'wb') as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b''.join(struct.pack('<' + 'h' * channels, *([v] * channels)) for v in quiet + sound + quiet))


def write_png(path, width, height, pixel):
    """An 8-bit RGB PNG, unfiltered rows, of pixel(x, y) -> (r, g, b)."""
    rows = b''.join(b'\0' + bytes(c for x in range(width) for c in pixel(x, y)) for y in range(height))
    chunk = lambda kind, body: struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


class ToneInWav(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = Path(self.dir.name) / 'tone.wav'

    def tearDown(self):
        self.dir.cleanup()

    def test_the_frequency_and_the_length_of_the_sound_are_found(self):
        write_wav(self.path, 440, 1.0)
        hz, seconds = tone_in_wav.tone(str(self.path))
        self.assertAlmostEqual(hz, 440, delta=4)
        self.assertAlmostEqual(seconds, 1.0, delta=0.01)
        self.assertEqual(tone_in_wav.main(['tone_in_wav', str(self.path), '440']), 0)

    def test_another_frequency_is_not_the_one_asked_for(self):
        write_wav(self.path, 500, 1.0)
        self.assertEqual(tone_in_wav.main(['tone_in_wav', str(self.path), '440']), 1)

    def test_silence_or_too_short_a_sound_is_no_tone(self):
        write_wav(self.path, 440, 0.0)
        self.assertEqual(tone_in_wav.tone(str(self.path)), (0.0, 0.0))
        write_wav(self.path, 440, 0.3)
        self.assertEqual(tone_in_wav.main(['tone_in_wav', str(self.path), '440']), 1)

    def test_one_channel_and_another_rate(self):
        write_wav(self.path, 1000, 1.0, rate=48000, channels=1)
        self.assertEqual(tone_in_wav.main(['tone_in_wav', str(self.path), '1000']), 0)


class ScreenShowsDraw(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = Path(self.dir.name) / 'screen.png'

    def tearDown(self):
        self.dir.cleanup()

    def test_the_picture_is_recognised(self):
        # (as draw paints it: a white frame, and at the middle no red, half green, half blue)
        write_png(self.path, 40, 60, lambda x, y: (255, 255, 255) if min(x, y, 39 - x, 59 - y) < 8 else (0, x * 255 // 40, y * 255 // 60))
        self.assertEqual(screen_shows_draw.main(['screen_shows_draw', str(self.path)]), 0)

    def test_the_text_console_is_not_it(self):
        write_png(self.path, 40, 60, lambda x, y: (0, 0, 0) if (x + y) % 7 else (200, 200, 200))
        self.assertEqual(screen_shows_draw.main(['screen_shows_draw', str(self.path)]), 1)

    def test_what_is_not_an_rgb_png_is_said(self):
        self.path.write_bytes(b'not a picture')
        with self.assertRaises(ValueError):
            screen_shows_draw.pixels(str(self.path))


if __name__ == '__main__':
    sys.exit(unittest.main())
