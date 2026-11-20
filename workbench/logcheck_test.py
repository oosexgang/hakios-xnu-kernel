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

    def test_a_dangling_last_log_does_not_crash_the_newest_pick(self):
        # a fetch that failed can leave logs/last.log pointing at a file that is not there; auto-pick must skip it, not
        # crash stat()-ing it
        with tempfile.TemporaryDirectory() as work:
            logs = Path(work) / 'logs'
            logs.mkdir()
            (logs / '0001-run.log').write_text(GOOD)
            (logs / 'last.log').symlink_to('does-not-exist.log')
            old = logcheck.ROOT
            logcheck.ROOT = Path(work)
            try:
                said = io.StringIO()
                with contextlib.redirect_stdout(said):
                    code = logcheck.main([])
            finally:
                logcheck.ROOT = old
            self.assertEqual(code, 0, said.getvalue())
            self.assertIn('0001-run.log', said.getvalue())

    def test_a_named_log_that_cannot_be_read_is_reported_not_a_traceback(self):
        with tempfile.TemporaryDirectory() as work:
            missing = Path(work) / 'gone.log'
            said = io.StringIO()
            with contextlib.redirect_stdout(said):
                code = logcheck.main([str(missing)])
            self.assertEqual(code, 1, said.getvalue())
            self.assertIn('not read', said.getvalue())

    def test_what_the_common_drivers_say_of_a_block_they_do_not_know(self):
        code, said = run('Darwin Kernel Version 12377.121.6: ...\n'
                         'cpufreq: group 0 (4 of our cores): row 2 -> 7 of the frequency table, 1804 MHz\n'
                         'cpufreq: group 1 (3 of our cores): the hardware is not in charge of its clock: left alone\n'
                         'buttons: PMIC arbiter version 0x30000000, 512 channels\n'
                         'buttons: this driver knows versions 5, 7 and 8 of the arbiter only: no buttons\n')
        self.assertEqual(code, 1, said)
        self.assertRegex(said, r'CPU frequency +STOPPED +cpufreq: group 1 .*not in charge of its clock')
        self.assertRegex(said, r'Buttons +STOPPED +buttons: this driver knows versions 5, 7 and 8')
        _, said = run('buttons: ... a power-on block of subtype 0x5, which has no keys this driver reads: left alone\n')
        self.assertRegex(said, r'Buttons +STOPPED')

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

    def test_which_identity_the_loader_gave(self):
        _, said = run('identity: serial number ABC, ECID 1\nidentity: from the payload (t8030, 828x1792)\nDarwin Kernel Version 1\n')
        self.assertRegex(said, r'Identity \(loader\) +ok +identity: from the payload \(t8030, 828x1792\)')
        code, said = run("identity: serial number ABC, ECID 1\nidentity: the payload's is not one (100 bytes): the one built in\n"
                         'Darwin Kernel Version 1\n')
        self.assertRegex(said, r"Identity \(loader\) +STOPPED +identity: the payload's is not one")
        self.assertEqual(code, 1)
        _, said = run('identity: serial number ABC, ECID 1\nDarwin Kernel Version 1\n')
        self.assertRegex(said, r'Identity \(loader\) +said +identity: serial number ABC', 'the one built in: said, no more')

    def test_a_look_that_went_through_is_ok(self):
        # (bt=-1 reads and says, and is done: nothing else says it worked)
        code, said = run('Darwin Kernel Version 1\nbt-uart: gpio24 cfg 0 level 0\nbt-uart: done\n')
        self.assertRegex(said, r'Bluetooth controller +ok +bt-uart: done')
        code, said = run('Darwin Kernel Version 1\nbt-uart: SE protocol is not UART (2); stopping\nbt-uart: bring-up stopped; powering down\n'
                         'bt-uart: done\n')
        self.assertRegex(said, r'Bluetooth controller +STOPPED +bt-uart: bring-up stopped')

    def test_a_touch_screen_only_looked_at_is_said_and_one_that_did_not_start_is_stopped(self):
        # (touchscreen=-1, phone_look's: the state is said, and nothing says it worked or failed)
        code, said = run('Darwin Kernel Version 1\nfts: as found (touchscreen=-1: looking only): votes 0x0, clocks 0x80000000\n'
                         'fts: touchscreen=-1: nothing powered, configured or sent\n')
        self.assertRegex(said, r'Touch \(FocalTech\) +said +fts: touchscreen=-1')
        code, said = run('Darwin Kernel Version 1\nfts: no application ID 0x5652 (last 0x0000); no firmware is flashed\n'
                         'fts: touchscreen unavailable\n')
        self.assertRegex(said, r'Touch \(FocalTech\) +STOPPED +fts: touchscreen unavailable')
        code, said = run('Darwin Kernel Version 1\nfocaltech: no firmware built in (focaltech_fw.h is empty): the chip is not started\n'
                         'goodix: no focaltech touch screen\n')
        self.assertRegex(said, r'Touch \(FocalTech firmware\) +STOPPED +focaltech: no firmware built in')
        self.assertRegex(said, r'Touch \(Goodix\) +STOPPED +goodix: no focaltech touch screen')
        # the controller on I2C: a look says, a bring-up that found no chip is stopped, one that reads reports is ok
        code, said = run('Darwin Kernel Version 1\nft5x06: touchscreen=-1: looking only\n'
                         'ft5x06: the chip at 0x38 answers identity 0x5422, an FT5x46; firmware 0x1e, mode 0x00\n'
                         'ft5x06: touchscreen=-1: nothing powered, configured or written\n')
        self.assertRegex(said, r'Touch \(FocalTech on I2C\) +said +ft5x06: touchscreen=-1: nothing powered')
        code, said = run('Darwin Kernel Version 1\nft5x06: nobody answers at 0x38: a chip that is not there, not powered or held in reset\n'
                         'ft5x06: touch screen unavailable\nft5x06: powered off: in reset, what was switched on for it off again, the pins as found\n')
        self.assertRegex(said, r'Touch \(FocalTech on I2C\) +STOPPED +ft5x06: touch screen unavailable')
        code, said = run('Darwin Kernel Version 1\nft5x06: step 5: touch screen ready, 1080 x 2340, 10 fingers; the interrupt line '
                         '(gpio59) is looked at every 8 ms\n')
        self.assertRegex(said, r'Touch \(FocalTech on I2C\) +ok +ft5x06: step 5: touch screen ready')

    def test_a_kernel_that_never_started(self):
        code, said = run('loader: platform phone\nno kernel in payload\n')
        self.assertEqual(code, 1)
        self.assertIn('NOT STARTED', said)
        self.assertIn('no line of any part', said)

    def test_what_gives_a_part_up_on_the_phone(self):
        # the lines that say a part stopped working in the middle of a run, each its driver's own
        log = """Darwin Kernel Version 12377.121.6: ...
thermal: trip 72.0 C, clear 60.0 C, critical 90.0 C
thermal: 1 sensor that answered before is silent: held back 3 of 5, nothing given back until every one answers again
buttons: buttons and keys believed from here on
buttons: power let go: the power-on block has not been read for 62 looks in a row
camera: /dev/hakios-camera: the back camera
camera: no frame in its time: nothing more is written in this run
adsp: 9: versions agreed; the channel is open
adsp: link: THE AUDIO DSP SAYS IT HAS GIVEN UP ("slave-kernel" = 0x1)
lx: using the early radio pool
lx: THE HANDSHAKE DID NOT COME THROUGH in 8000 ms: its message 3 was refused: its RSN element is not the beacon's (2 of its messages came)
"""
        code, said = run(log)
        self.assertEqual(code, 1, said)
        self.assertRegex(said, r'Temperature +STOPPED +thermal: 1 sensor that answered before is silent')
        self.assertRegex(said, r'Buttons +STOPPED +buttons: power let go')
        self.assertRegex(said, r'Camera +STOPPED +camera: no frame in its time')
        self.assertRegex(said, r'Audio DSP +STOPPED +adsp: link: THE AUDIO DSP SAYS IT HAS GIVEN UP')
        self.assertRegex(said, r'Wi-Fi driver +STOPPED +lx: THE HANDSHAKE DID NOT COME THROUGH')

    def test_the_parts_words_are_the_drivers(self):
        # each part's prefix is one its driver writes its lines with
        # (or the loader: the identity is its)
        sources = ''.join(p.read_text(errors='replace') for p in [*(ROOT / 'drivers/board').glob('*.c'), *(ROOT / 'drivers/iokit').glob('*.cpp'),
                                                                   *(ROOT / 'arch/arm64/boot/src').glob('*.c')])
        for name, prefix, _, _ in logcheck.PARTS:
            self.assertTrue(f'"{prefix}: ' in sources, f'{name}: nothing writes lines beginning {prefix}:')


if __name__ == '__main__':
    unittest.main()
