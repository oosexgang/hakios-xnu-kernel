#!/usr/bin/env python3
"""tools/logcheck.py on logs written here, as a run's log has the drivers' lines: each part's state from its own
words, a panic and a kernel that never started said, the status of a run that went wrong."""
import contextlib
import importlib.util
import io
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('logcheck', ROOT / 'tools/logcheck.py')
logcheck = importlib.util.module_from_spec(spec)
spec.loader.exec_module(logcheck)

GOOD = """loader: platform phone
Darwin Kernel Version 12377.121.6: ...
bt-uart: identification complete; the firmware taken
bt-uart: the controller is Darwin's from here on
bt: service "bluetooth" registered (address 02:00:00:00:00:01, transport H4BC)
bt: com.apple.uart.sk.bluetooth connected by bluetoothd (pid 81, unit 1)
torch: THE LIGHT IS DARWIN'S (/dev/hakios-torch) from here on, until the run ends: at most Android's 125 mA, at most 120 s at a time
[ 12.5] haptic: THE MOTOR PLAYS WHAT DARWIN PLAYS ON ITS ACTUATOR (/dev/hakios-haptic) from here on, until the run ends
cam: something of another part
camera: disabled: this board's board.dts lists no "camera-sm7325"
buttons: next: read the type of pon through channel 3 (2 registers, 0x0804)
kernel returned: log at 0xa0000000, 1234 bytes
"""
BAD = """Darwin Kernel Version 12377.121.6: ...
adreno: 7: step 7: FAILED; the steps after it are not attempted
adreno: THE GPU IS NOT STARTED: step 7 (the GMU) failed; the system draws with its software renderer
ufs: native check: userdata (LUN 0): DIFFERENT at byte 4096 (00 through the loader, ff here): STOP
panic(cpu 2 caller 0xfffffff007a1b2c4): "a test's panic"
"""


def run(text, *more):
    with tempfile.TemporaryDirectory() as work:
        log = Path(work) / 'run.log'
        log.write_text(text)
        said = io.StringIO()
        with contextlib.redirect_stdout(said):
            code = logcheck.main([str(log), *more])
    return code, said.getvalue()


class LogCheck(unittest.TestCase):
    def test_a_run_that_went_well(self):
        code, said = run(GOOD)
        self.assertEqual(code, 0, said)
        self.assertIn('kernel: started; no panic; back in the loader', said)
        self.assertRegex(said, r'Bluetooth controller +ok +bt-uart: the controller is Darwin\'s from here on')
        self.assertRegex(said, r'Bluetooth for Darwin +ok +bt: com.apple.uart.sk.bluetooth connected by bluetoothd')
        self.assertRegex(said, r'Bluetooth service +ok')
        self.assertRegex(said, r"Flashlight +ok +torch: THE LIGHT IS DARWIN'S")
        self.assertRegex(said, r'Vibration +ok', 'a line with the time in front of it is the part\'s too')
        self.assertRegex(said, r'Camera +disabled +camera: disabled: ')
        self.assertRegex(said, r'Buttons +said +buttons: next: ')
        self.assertNotIn('GPU', said, 'a part that said nothing is not listed')

    def test_a_run_that_did_not(self):
        code, said = run(BAD)
        self.assertEqual(code, 1)
        self.assertIn('PANIC at line 5: panic(cpu 2 caller', said)
        self.assertRegex(said, r'GPU +STOPPED +adreno: 7: step 7: FAILED')
        self.assertRegex(said, r'Storage \(own driver\) +STOPPED +ufs: native check: .*DIFFERENT at byte 4096')
        self.assertNotIn('back in the loader', said)

    def test_every_line_of_each_part_when_asked(self):
        _, said = run(BAD, '--all')
        self.assertIn('adreno: THE GPU IS NOT STARTED', said)

    def test_what_changed_from_one_run_to_the_other(self):
        with tempfile.TemporaryDirectory() as work:
            old, new = Path(work) / 'old.log', Path(work) / 'new.log'
            old.write_text(GOOD)
            new.write_text(GOOD.replace("bt-uart: the controller is Darwin's from here on\n", 'bt-uart: bring-up stopped; powering down\n')
                           .replace('bt-uart: identification complete; the firmware taken\n', '')
                           .replace('camera: disabled', 'camera: /dev/hakios-camera: the back camera\'s frames\ncamera: was disabled')
                           .replace('kernel returned: log at', 'returned nothing at'))
            said = io.StringIO()
            with contextlib.redirect_stdout(said):
                code = logcheck.main([str(new), '--against', str(old)])
            text = said.getvalue()
            self.assertEqual(code, 1, text)
            self.assertRegex(text, r'Bluetooth controller: ok -> STOPPED +bt-uart: bring-up stopped')
            self.assertRegex(text, r'Camera: disabled -> ok')
            self.assertIn('kernel came back to the loader: yes -> no', text)
            self.assertNotIn('Flashlight', text, 'what did not change is not said')
            said = io.StringIO()
            with contextlib.redirect_stdout(said):
                code = logcheck.main([str(old), '--against', str(old)])
            self.assertEqual(code, 0)
            self.assertIn('every part as it was', said.getvalue())

    def test_a_kernel_that_never_started(self):
        code, said = run('loader: platform phone\nno kernel in payload\n')
        self.assertEqual(code, 1)
        self.assertIn('NOT STARTED', said)
        self.assertIn('no line of any part', said)

    def test_the_parts_words_are_the_drivers(self):
        # each part's prefix is one its driver writes its lines with
        sources = ''.join(p.read_text(errors='replace') for p in (ROOT / 'drivers/board').glob('*.c'))
        for name, prefix, _, _ in logcheck.PARTS:
            self.assertIn(f'"{prefix}: ', sources, f'{name}: no driver writes lines beginning {prefix}:')


if __name__ == '__main__':
    unittest.main()
