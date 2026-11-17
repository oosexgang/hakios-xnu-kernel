#!/usr/bin/env python3
"""scripts/phone-day.sh with a stand-in for ./bench, in a copy of what it uses: the runs in order, each log looked at,
the summary, a stop at the first run that did not go -- or not, with --keep-going -- and a run that left no log."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# a bench that "runs": a log in logs/ and logs/last.log to it, its kernel's words chosen by the run's name
BENCH = r'''#!/bin/bash
mkdir -p logs
n=$(ls logs/*.log 2> /dev/null | wc -l)
log=logs/$(printf '%03d' "$n")-run.log
case $BENCH_DEFCONFIG in
phone_never) echo "bench: no answer: the kernel hung"; exit 2 ;;
phone_bad) printf 'Darwin Kernel Version 1\nadreno: THE GPU IS NOT STARTED: step 7 failed\nkernel returned: log at 0x1, 2 bytes\n' > "$log" ;;
*) printf 'Darwin Kernel Version 1\ntorch: THE LIGHT IS DARWIN'"'"'S (/dev/hakios-torch)\nkernel returned: log at 0x1, 2 bytes\n' > "$log" ;;
esac
ln -sf "$(basename "$log")" logs/last.log
echo "result: done  [$log]"
'''


class PhoneDay(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir)
        for path in ('scripts/phone-day.sh', 'tools/logcheck.py', 'tools/defconfig.py'):
            (self.dir / path).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / path, self.dir / path)
        shutil.copytree(ROOT / 'arch/arm64/configs', self.dir / 'arch/arm64/configs')
        for name in ('phone_bad', 'phone_never'):
            (self.dir / f'arch/arm64/configs/{name}_defconfig').write_text(f'# {name}, the test\'s\nx=1\n')
        (self.dir / 'bench').write_text(BENCH)
        (self.dir / 'bench').chmod(0o755)

    def day(self, *runs):
        done = subprocess.run([str(self.dir / 'scripts/phone-day.sh'), *runs], cwd=self.dir, capture_output=True, text=True,
                              env={'PATH': '/usr/bin:/bin', 'BENCH': str(self.dir / 'bench')})
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

    def test_a_run_there_is_no_defconfig_of(self):
        done, day = self.day('phone_nothing')
        self.assertEqual(done.returncode, 2)
        self.assertIsNone(day, 'nothing begun')


if __name__ == '__main__':
    unittest.main()
