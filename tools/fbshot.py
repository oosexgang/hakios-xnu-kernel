#!/usr/bin/env python3
"""
fbshot.py LOG [OUT.png] [N]: the last whole picture (or picture N) of the frame that a kernel with boot-arg fbshot=SECONDS
printed into LOG (MiniIOMobileFramebuffer.cpp, shot()), as a PNG four times as big.
"""
import base64, re, struct, sys, zlib

text = open(sys.argv[1], 'rb').read().decode('latin-1')
out = sys.argv[2] if len(sys.argv) > 2 else 'fbshot.png'
shots = {}
for m in re.finditer(r'fbshot (\d+): (\S+)(?: (\d+) (\d+))?', text):
    n, word = int(m.group(1)), m.group(2)
    if word == 'begin':
        shots[n] = {'size': (int(m.group(3)), int(m.group(4))), 'data': [], 'end': False}
    elif n in shots and word == 'end':
        shots[n]['end'] = True
    elif n in shots and not shots[n]['end']:
        try: shots[n]["data"].append(base64.b64decode(word))
        except Exception: shots[n]["data"].append(b"\0" * (len(word) * 3 // 4))
whole = [n for n, s in shots.items() if s['end']]
if not whole:
    sys.exit('fbshot.py: no whole picture in the log')
pick = int(sys.argv[3]) if len(sys.argv) > 3 else max(whole)
if pick not in whole:
    sys.exit(f'fbshot.py: no whole picture {pick} in the log')
shot = shots[pick]
w, h = shot['size']
pixels = b''.join(shot['data'])[:w * h * 3]
if len(pixels) < w * h * 3:
    sys.exit(f'fbshot.py: picture {pick} is short: {len(pixels)} of {w * h * 3} bytes')
scale = 4
rows = b''
for y in range(h * scale):
    row = pixels[(y // scale) * w * 3:(y // scale + 1) * w * 3]
    rows += b'\0' + b''.join(row[x * 3:x * 3 + 3] * scale for x in range(w))
def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w * scale, h * scale, 8, 2, 0, 0, 0)) + \
    chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')
open(out, 'wb').write(png)
print(f'fbshot.py: picture {pick}, {w}x{h}: {out}')
