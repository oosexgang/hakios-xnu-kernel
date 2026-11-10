#!/usr/bin/env python3
"""fbjpg.py LOG OUT.jpg [NAME]: the last whole picture that /redwood/bin/fbgrab -log NAME put on the console (lines "fbjpg NAME I/N: base64"),
as the JPEG it was. Run fbgrab as the last thing of a run: the log has the console's last 261 KB."""
import base64, re, sys

text = open(sys.argv[1], 'rb').read().decode('latin-1')
want = sys.argv[3] if len(sys.argv) > 3 else None
pictures = {}
for m in re.finditer(r'fbjpg (\S+) (\d+)/(\d+): ([A-Za-z0-9+/=]+)', text):
    name, i, n, data = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)
    if want and name != want:
        continue
    pictures.setdefault((name, n), {})[i] = data
whole = [(k, v) for k, v in pictures.items() if len(v) == k[1]]
if not whole:
    sys.exit('fbjpg.py: no whole picture in the log (%d partial)' % len(pictures))
(name, n), lines = whole[-1]
open(sys.argv[2], 'wb').write(base64.b64decode(''.join(lines[i] for i in range(1, n + 1))))
print('fbjpg.py: %s, %d lines' % (name, n))
