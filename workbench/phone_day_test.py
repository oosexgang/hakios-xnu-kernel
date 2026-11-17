#!/usr/bin/env python3
"""scripts/phone-day.sh with a stand-in for ./bench, in a copy of what it uses: the runs in order, each log looked at,
the summary, a stop at the first run that did not go -- or not, with --keep-going -- and a run that left no log; what the
runs need looked at first (a stand-in for scripts/phone-ready.py), and no day begun on a machine that is not ready."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# a bench that "runs": a log in logs/ and logs/last.log to it, its kernel's words chosen by the run's name
BENCH = r'''#!/bin/bash
mkdir -p logs
echo "$1 ${BENCH_DEFCONFIG:-}" >> bench-calls
if [ "$1" = fresh ]; then
	[ -f fresh-fails ] && { echo "bench: the loader did not come up on USB"; exit 1; }
	echo "bench: the loader is up (zeta), no kernel run in it yet"; exit 0
fi
n=$(ls logs/*.log 2> /dev/null | wc -l)
log=logs/$(printf '%03d' "$n")-run.log
case $BENCH_DEFCONFIG in
phone_never) printf 'Darwin Kernel Version 1\nbacklight: board zeta\n' > "$log.live"
	echo "bench: no answer: the kernel hung"; exit 2 ;;
phone_hung) printf 'Darwin Kernel Version 1\ntorch: THE LIGHT IS DARWIN'"'"'S (/dev/hakios-torch)\n' > "$log"
	ln -sf "$(basename "$log")" logs/last.log
	echo "result: hung 1 0  [$log]"; echo "bench: the kernel did not come back from its core; restarting the phone"; exit 2 ;;
phone_bad) printf 'Darwin Kernel Version 1\nadreno: THE GPU IS NOT STARTED: step 7 failed\nkernel returned: log at 0x1, 2 bytes\n' > "$log" ;;
*) printf 'Darwin Kernel Version 1\ntorch: THE LIGHT IS DARWIN'"'"'S (/dev/hakios-torch)\nkernel returned: log at 0x1, 2 bytes\n' > "$log" ;;
esac
ln -sf "$(basename "$log")" logs/last.log
echo "result: done  [$log]"
'''

# a check of what the runs need: ready unless the test left a word in not-ready
READY = r'''#!/bin/bash
echo "phone-ready: zeta, for $*"
if [ -f not-ready ]; then echo "  MISSING  kernel  $(cat not-ready)"; exit 1; fi
echo "  ok       kernel  here"
'''


class PhoneDay(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir)
        for path in ('scripts/phone-day.sh', 'tools/logcheck.py', 'tools/defconfig.py'):
            (self.dir / path).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / path, self.dir / path)
        shutil.copytree(ROOT / 'arch/arm64/configs', self.dir / 'arch/arm64/configs')
        for name in ('phone_bad', 'phone_never', 'phone_hung'):
            (self.dir / f'arch/arm64/configs/{name}_defconfig').write_text(f'# {name}, the test\'s\nx=1\n')
        (self.dir / 'bench').write_text(BENCH)
        (self.dir / 'bench').chmod(0o755)
        (self.dir / 'ready').write_text(READY)
        (self.dir / 'ready').chmod(0o755)

    def day(self, *runs):
        done = subprocess.run([str(self.dir / 'scripts/phone-day.sh'), *runs], cwd=self.dir, capture_output=True, text=True,
                              env={'PATH': '/usr/bin:/bin', 'BENCH': str(self.dir / 'bench'),
                                   'READY': str(self.dir / 'ready')})
        days = sorted((self.dir / 'logs').glob('day-*'))
        return done, days[-1] if days else None

    def test_the_runs_in_order_and_what_each_said(self):
        done, day = self.day('phone_first', 'phone_bt')
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertEqual(sorted(p.name for p in day.iterdir()),
                         ['phone_bt.bench', 'phone_bt.log', 'phone_first.bench', 'phone_first.log', 'summary.md'])
        summary = (day / 'summary.md').read_text()
        self.assertLess(summary.index('## phone_first'), summary.index('## phone_bt'))
        self.assertRegex(summary, r'Flashlight +ok')
        self.assertIn('-- since phone_first:', summary)
        self.assertIn('every part as it was', summary)
        self.assertIn('the folder to send back', done.stdout)

    def test_a_run_that_did_not_go_stops_the_day(self):
        done, day = self.day('phone_first', 'phone_bad', 'phone_bt')
        self.assertEqual(done.returncode, 1)
        summary = (day / 'summary.md').read_text()
        self.assertRegex(summary, r'GPU +STOPPED')
        self.assertNotIn('## phone_bt', summary, 'nothing after the run that stopped')
        self.assertIn('phone_bad did not go all the way', done.stdout)

    def test_or_not_when_told_to_go_on(self):
        done, day = self.day('--keep-going', 'phone_bad', 'phone_never', 'phone_bt')
        self.assertEqual(done.returncode, 1)
        summary = (day / 'summary.md').read_text()
        self.assertIn('## phone_bt', summary)
        self.assertIn('no log: the run did not come back', summary)
        self.assertIn('no answer: the kernel hung', (day / 'phone_never.bench').read_text())

    def test_a_fresh_loader_before_every_run_but_the_first(self):
        done, day = self.day('phone_first', 'phone_look', 'phone_bt')
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        calls = (self.dir / 'bench-calls').read_text().split('\n')
        self.assertEqual([c for c in calls if c], ['run phone_first', 'fresh ', 'run phone_look', 'fresh ', 'run phone_bt'])

    def test_a_loader_that_cannot_be_started_anew_ends_the_day(self):
        (self.dir / 'fresh-fails').write_text('')
        done, day = self.day('phone_first', 'phone_look')
        self.assertEqual(done.returncode, 1)
        summary = (day / 'summary.md').read_text()
        self.assertIn('not run: the loader could not be started anew', summary)
        self.assertNotIn('run phone_look', (self.dir / 'bench-calls').read_text())

    def test_a_kernel_that_hung_on_its_core_is_not_a_run_that_went(self):
        # (the loader fetched what it said, and every part of it is fine: ./bench failing is what tells)
        done, day = self.day('phone_hung', 'phone_bt')
        self.assertEqual(done.returncode, 1)
        summary = (day / 'summary.md').read_text()
        self.assertIn('./bench run did not end well (exit 2)', summary)
        self.assertIn('did not come back from its core', summary)
        self.assertNotIn('## phone_bt', summary)

    def test_what_a_run_that_stopped_answering_said_is_kept(self):
        done, day = self.day('phone_never')
        self.assertEqual(done.returncode, 1)
        self.assertEqual((day / 'phone_never.live.log').read_text(), 'Darwin Kernel Version 1\nbacklight: board zeta\n')
        self.assertIn('what it said while it ran: phone_never.live.log', (day / 'summary.md').read_text())

    def test_what_the_runs_need_is_looked_at_first(self):
        done, day = self.day('phone_first', 'phone_bt')
        summary = (day / 'summary.md').read_text()
        self.assertLess(summary.index('## Before'), summary.index('## phone_first'))
        self.assertIn('phone-ready: zeta, for phone_first phone_bt', summary)
        self.assertIn('ok       kernel  here', summary)

    def test_a_machine_that_is_not_ready_begins_no_run(self):
        (self.dir / 'not-ready').write_text('no kernel: kernel/build.sh')
        done, day = self.day('phone_first')
        self.assertEqual(done.returncode, 2)
        self.assertIn('not ready (above): no run started', done.stdout)
        self.assertEqual([p.name for p in day.iterdir()], ['summary.md'], 'no run')
        self.assertIn('MISSING  kernel  no kernel: kernel/build.sh', (day / 'summary.md').read_text())
        # told to begin all the same: the runs, and nothing looked at
        done, day = self.day('--no-check', 'phone_first')
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertNotIn('## Before', (day / 'summary.md').read_text())
        self.assertTrue((day / 'phone_first.log').is_file())

    def test_a_run_there_is_no_defconfig_of(self):
        done, day = self.day('phone_nothing')
        self.assertEqual(done.returncode, 2)
        self.assertIsNone(day, 'nothing begun')


if __name__ == '__main__':
    unittest.main()
