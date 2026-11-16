#!/usr/bin/env python3
"""
Build the "XNUP" payload consumed by the boot shim and the XNU loader.

  header : u32 magic "XNUP", u32 size (bytes after the header)
  entries: char name[16], u32 size, u32 reserved, data padded to 16 bytes
  end    : an entry whose name is empty

usage: mkpayload.py -o payload.bin name=file [name=file ...] [--cmdline "..."]
"""
import argparse, struct

ap = argparse.ArgumentParser()
ap.add_argument('-o', '--output', required=True)
ap.add_argument('--cmdline', default='')
ap.add_argument('--physbase', default='', help='hex: force XNU physical base (testing)')
ap.add_argument('--usb', action='store_true', help='make the loader stay resident and take commands over USB')
ap.add_argument('--rdres', default='', help='hex size of a root image already placed at the resident address (QEMU)')
ap.add_argument('--checkpoint', default='', help='kernel checkpoint to stop at; 999 = loader self test')
ap.add_argument('items', nargs='*')
args = ap.parse_args()

body = b''
def add(name, data):
    global body
    body += struct.pack('<16sII', name.encode(), len(data), 0) + data + b'\0' * (-len(data) % 16)

for item in args.items:
    name, path = item.split('=', 1)
    add(name, open(path, 'rb').read())
if args.cmdline:
    add('cmdline', args.cmdline.encode() + b'\0')
if args.physbase:
    add('physbase', args.physbase.lower().replace('0x', '').encode() + b'\0')
if args.rdres:
    add('rdres', args.rdres.lower().replace('0x', '').encode() + b'\0')
if args.usb:
    add('usb', b'1\0')
if args.checkpoint:
    add('checkpoint', args.checkpoint.encode() + b'\0')
body += b'\0' * 24
open(args.output, 'wb').write(struct.pack('<II', 0x50554E58, len(body)) + body)
print(f'{args.output}: {len(body) + 8} bytes')
