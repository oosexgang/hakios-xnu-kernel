#!/usr/bin/env python3
"""usage: tools/dsc/textdump.py IMAGE OUT.macho   -- the __text of an image of the iOS 16.5 cache as a bare arm64 Mach-O at its own addresses,
so that `otool -arch arm64 -tV OUT.macho` (Apple's, no capstone needed) shows it; also writes OUT.macho.cstring with the __cstring section
and the segment/section addresses on stdout."""
import struct, sys
sys.path.insert(0, sys.argv[0].rsplit('/', 1)[0])
import dsc

c = dsc.Cache()
im = dsc.Image(c, sys.argv[1])
text, size = im.sects[('__TEXT', '__text')]
code = c.read(text, size)
hdr = 32 + 72 + 80
off = 0x4000
macho = struct.pack('<IIIIIIII', 0xfeedfacf, 0x0100000c, 0, 2, 1, 72 + 80, 0, 0)
macho += struct.pack('<II16s4Q', 0x19, 72 + 80, b'__TEXT', text, size, off, size)
macho += struct.pack('<ii', 5, 5)[0:0]
macho = macho[:-0] if False else macho
# segment_command_64: cmd, cmdsize, segname[16], vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, flags
macho = struct.pack('<IIIIIIII', 0xfeedfacf, 0x0100000c, 0, 2, 1, 72 + 80, 0, 0)
macho += struct.pack('<II16sQQQQiiII', 0x19, 72 + 80, b'__TEXT', text, size, off, size, 5, 5, 1, 0)
macho += struct.pack('<16s16sQQIIIIIIII', b'__text', b'__TEXT', text, size, off, 2, 0, 0, 0x80000400, 0, 0, 0)
macho = macho.ljust(off, b'\0') + code
open(sys.argv[2], 'wb').write(macho)
print('text %#x size %#x' % (text, size))
for k, v in sorted(im.sects.items()):
    print(k, hex(v[0]), hex(v[1]))
