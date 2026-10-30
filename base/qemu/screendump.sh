#!/bin/bash
# Save the screen of a running QEMU (started with -device ramfb -monitor tcp:127.0.0.1:4444,server,nowait) as a PNG.
# usage: screendump.sh [out.png]      (FULL=1: the whole mode, not only the phone-sized corner)
out=$(realpath -m "${1:-$(dirname "$(readlink -f "$0")")/../logs/qemu-screen.png}")   # QEMU writes it: absolute
mkdir -p "$(dirname "$out")"
here=$(dirname "$(readlink -f "$0")")
python3 - "$out" "${QEMU_INSTANCE:-$(cat "$here/instance" 2>/dev/null || echo 0)}" <<'PY'
import socket, struct, sys, time, zlib, os
out = sys.argv[1]; ppm = out + '.ppm'
s = socket.create_connection(('127.0.0.1', 4444 + 10 * int(sys.argv[2]))); s.settimeout(2)
s.sendall(b'\n'); time.sleep(0.3)
s.sendall(f'screendump {ppm}\n'.encode()); time.sleep(1.5)
d = open(ppm, 'rb').read(); os.unlink(ppm)
magic, size, maxval, pix = d.split(b'\n', 3)
w, h = map(int, size.split())
# the kernel uses the top left 1125x2436 of a bigger mode as the phone's screen (boot-args fbwidth/fbheight)
cw, ch = (min(w, 1125), min(h, 2436)) if os.environ.get('FULL') != '1' else (w, h)
# QEMU pads every row of the PPM to a multiple of 4 bytes (wrong for the format, but that is what it writes)
stride = len(pix) // h
rows = b''.join(b'\0' + pix[y * stride:y * stride + cw * 3] for y in range(ch))
w, h = cw, ch
chunk = lambda k, b: struct.pack('>I', len(b)) + k + b + struct.pack('>I', zlib.crc32(k + b))
open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                      chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))
print(out, w, h)
PY
