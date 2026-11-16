#!/usr/bin/env python3
"""kernel/host/llvm/place-kernel.py on a Mach-O shaped as ld64.lld makes the kernel: linked at 0, __TEXT_EXEC
under the name __DATA and __DATA under __DATX, a pointer to rebase, a symbol, LC_MAIN and a dyld's load
commands. What is held: the image is moved to the base, the pointer with it and nothing else of the
contents; the segments have their names and protections back; the entry is an LC_UNIXTHREAD at the base;
what only dyld reads is gone; and what cannot be made into a kernel is refused with the reason. With
--program, a static user program: the same without moving it."""
import importlib.util
import struct
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('place_kernel', ROOT / 'kernel/host/llvm/place-kernel.py')
place_kernel = importlib.util.module_from_spec(spec)
spec.loader.exec_module(place_kernel)

BASE = 0xfffffff007004000
PAGE = 0x4000


def segment(name, vmaddr, size, fileoff, prot, sections=()):
    body = struct.pack('<16sQQQQiiII', name, vmaddr, size, fileoff, size if fileoff or name == b'__TEXT' else 0,
                       prot, prot, len(sections), 0)
    for sectname, addr, sect_size, offset in sections:
        body += struct.pack('<16s16sQQIIIIIIII', sectname, name, addr, sect_size, offset, 2, 0, 0, 0, 0, 0, 0)
    return struct.pack('<II', 0x19, 8 + len(body)) + body


def image(bind=b'', main_offset=PAGE + 0x10, chained=False):
    """__TEXT (header) | __DATA (code, linked as such) | __DATX (one pointer) | __LINKEDIT (rebase, symtab)"""
    linkedit = PAGE * 3
    # rebase: type pointer, segment 2 (__DATX) offset 8, one pointer; done
    rebase = bytes([0x11, 0x22, 0x08, 0x51, 0x00]) + bytes(3)
    symtab = struct.pack('<IBBHQ', 1, 0x0f, 2, 0, PAGE + 0x10)          # N_SECT | N_EXT, in section 2
    strings = b'\0_start\0'
    rebase_off, symoff, stroff = linkedit, linkedit + 16, linkedit + 32
    commands = [
        segment(b'__TEXT', 0, PAGE, 0, 1),
        segment(b'__DATA', PAGE, PAGE, PAGE, 5, [(b'__text', PAGE, 0x20, PAGE)]),
        segment(b'__DATX', PAGE * 2, PAGE, PAGE * 2, 3, [(b'__data', PAGE * 2, 0x10, PAGE * 2)]),
        segment(b'__LINKEDIT', linkedit, PAGE, linkedit, 1),
        struct.pack('<III12s', 0xe, 24, 12, b'/usr/lib/dyld'),                  # LC_LOAD_DYLINKER
        struct.pack('<IIQQ', 0x80000028, 24, main_offset, 0),                       # LC_MAIN
        struct.pack('<II10I', 0x80000022, 48, rebase_off, len(rebase), linkedit + 40 if bind else 0, len(bind),
                    0, 0, 0, 0, 0, 0),                                              # LC_DYLD_INFO_ONLY
        struct.pack('<IIIIII', 0x2, 24, symoff, 1, stroff, len(strings)),           # LC_SYMTAB
    ]
    if chained:
        commands.append(struct.pack('<IIII', 0x80000034, 16, 0, 0))
    commands = b''.join(commands)
    out = bytearray(PAGE * 4)
    count = 8 + (1 if chained else 0)
    struct.pack_into('<IiiIIIII', out, 0, 0xfeedfacf, 0x0100000c, 0, 2, count, len(commands), 0x00200085, 0)
    out[32:32 + len(commands)] = commands
    out[PAGE:PAGE + 4] = struct.pack('<I', 0x14000000)                          # b . (code: left alone)
    struct.pack_into('<QQ', out, PAGE * 2, 0x1234, PAGE + 0x10)                 # data, then a pointer to code
    out[rebase_off:rebase_off + len(rebase)] = rebase
    out[symoff:symoff + 16] = symtab
    out[stroff:stroff + len(strings)] = strings
    if bind:
        out[linkedit + 40:linkedit + 40 + len(bind)] = bind
    return bytes(out)


def load_commands(data):
    ncmds = struct.unpack_from('<I', data, 16)[0]
    at, found = 32, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, at)
        found.append((cmd, data[at:at + size]))
        at += size
    return found


class PlaceKernel(unittest.TestCase):
    def setUp(self):
        self.before = image()
        self.after, self.moved, self.entry = place_kernel.place(self.before, BASE)

    def test_the_pointer_is_moved_and_nothing_else(self):
        self.assertEqual(self.moved, 1)
        self.assertEqual(struct.unpack_from('<QQ', self.after, PAGE * 2), (0x1234, BASE + PAGE + 0x10))
        self.assertEqual(self.after[PAGE:PAGE * 2], self.before[PAGE:PAGE * 2])          # the code

    def test_the_segments_have_their_names_addresses_and_protections(self):
        segments = {}
        for cmd, body in load_commands(self.after):
            if cmd == 0x19:
                name, vmaddr = struct.unpack_from('<16sQ', body, 8)
                prot = struct.unpack_from('<ii', body, 56)
                segments[name.rstrip(b'\0')] = (vmaddr, prot)
                for n in range(struct.unpack_from('<I', body, 64)[0]):
                    self.assertEqual(body[72 + n * 80 + 16:72 + n * 80 + 32].rstrip(b'\0'), name.rstrip(b'\0'))
        self.assertEqual(segments, {b'__TEXT': (BASE, (1, 1)), b'__TEXT_EXEC': (BASE + PAGE, (5, 5)),
                                    b'__DATA': (BASE + PAGE * 2, (3, 3)), b'__LINKEDIT': (BASE + PAGE * 3, (1, 1))})

    def test_the_entry_is_a_thread_state_and_the_dyld_commands_are_gone(self):
        commands = [cmd for cmd, _ in load_commands(self.after)]
        self.assertNotIn(0x80000028, commands)          # LC_MAIN
        self.assertNotIn(0xe, commands)                 # LC_LOAD_DYLINKER
        self.assertNotIn(0x80000022, commands)          # LC_DYLD_INFO_ONLY
        thread = [body for cmd, body in load_commands(self.after) if cmd == 0x5]
        self.assertEqual(len(thread), 1)
        self.assertEqual(struct.unpack_from('<II', thread[0], 8), (6, 68))
        self.assertEqual(struct.unpack_from('<Q', thread[0], 16 + 32 * 8)[0], BASE + PAGE + 0x10)
        self.assertEqual(self.entry, BASE + PAGE + 0x10)
        self.assertEqual(struct.unpack_from('<I', self.after, 24)[0], 0x00200001)   # MH_NOUNDEFS | MH_PIE

    def test_a_symbol_of_a_section_moves_with_it(self):
        symoff = PAGE * 3 + 16
        self.assertEqual(struct.unpack_from('<Q', self.after, symoff + 8)[0], BASE + PAGE + 0x10)

    def test_what_cannot_be_a_kernel_is_refused(self):
        with self.assertRaisesRegex(SystemExit, 'bind'):
            place_kernel.place(image(bind=b'\x11\x00'), BASE)
        with self.assertRaisesRegex(SystemExit, 'no_fixup_chains'):
            place_kernel.place(image(chained=True), BASE)
        with self.assertRaisesRegex(SystemExit, 'not a 64-bit Mach-O'):
            place_kernel.place(b'\0' * 64, BASE)

    def test_a_program_is_left_where_it_was_linked(self):
        after, moved, entry = place_kernel.place(image(), 0, program=True)
        names = [struct.unpack_from('<16s', body, 8)[0].rstrip(b'\0') for cmd, body in load_commands(after) if cmd == 0x19]
        self.assertEqual(names, [b'__TEXT', b'__DATA', b'__DATX', b'__LINKEDIT'])       # (as linked)
        self.assertEqual(entry, PAGE + 0x10)
        self.assertEqual(struct.unpack_from('<I', after, 24)[0], 0x1)          # MH_NOUNDEFS: no dyld, not slid
        self.assertNotIn(0xe, [cmd for cmd, _ in load_commands(after)])

    def test_the_rebase_opcodes(self):
        segments = [{'fileoff': 0x1000}, {'fileoff': 0x8000}]
        # segment 1 offset 0x10: three pointers, skip 8 after each; then one more 0x20 further
        opcodes = bytes([0x11, 0x21, 0x10, 0x80, 0x03, 0x08, 0x30, 0x20, 0x51, 0x00])
        self.assertEqual(place_kernel.rebases(opcodes, segments), [0x8010, 0x8020, 0x8030, 0x8060])


if __name__ == '__main__':
    unittest.main()
