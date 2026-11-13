#!/usr/bin/env python3
"""Make the kernel ld64.lld links into one to boot: what cctools' ld64 does with -image_base, -segment_order
and -static, done after the link.

  place-kernel.py IN OUT [BASE]
  place-kernel.py --program IN OUT    a static program instead: left where it was linked, no dyld

ld64.lld does not implement -image_base, -segaddr, -segment_order or -static (it says so, and links
anyway). What it makes is a position-independent executable linked at 0 with a dyld's load commands:
- every segment is moved up by BASE (0xfffffff007004000, XNU's ARM64_LINK_ADDR) and every pointer its
  rebase information lists is moved with it, so the image is what a link at BASE would have made;
- the segments are in the order XNU asserts (__TEXT, __DATA_CONST, then __TEXT_EXEC: arm_vm_init()) because
  the link names them so that lld puts them there (kernel/host/llvm/ld: __TEXT_EXEC is linked as __DATA,
  which lld places right after __DATA_CONST, and __DATA as __DATX); their names are given back here;
- the entry point is an LC_UNIXTHREAD, as a kernel's is, in place of LC_MAIN; the load commands only a dyld
  reads (the dylinker, the rebase and bind information, encryption) are left out.

Nothing else changes: the code and data are byte for byte what lld wrote, but for the pointers rebased.

With --program the same is done to a user program without moving it (a static executable, which a
DEVELOPMENT kernel runs: tools/qemu-init/ is one): no dylinker, the entry a thread state.
"""
import struct
import sys

LC_SEGMENT_64, LC_SYMTAB, LC_UNIXTHREAD = 0x19, 0x2, 0x5
LC_LOAD_DYLINKER, LC_MAIN, LC_DYLD_INFO_ONLY, LC_ENCRYPTION_INFO_64 = 0xe, 0x80000028, 0x80000022, 0x2c
LC_DYLD_CHAINED_FIXUPS = 0x80000034
DROPPED = {LC_LOAD_DYLINKER, LC_MAIN, LC_DYLD_INFO_ONLY, LC_ENCRYPTION_INFO_64}
MH_NOUNDEFS, MH_PIE = 0x1, 0x200000
ARM_THREAD_STATE64, ARM_THREAD_STATE64_COUNT = 6, 68
# the names the link gives two segments so that lld orders them as XNU wants, and their own
RENAMED = {b'__DATA': b'__TEXT_EXEC', b'__DATX': b'__DATA'}
# their protections, as XNU's link sets them (-segprot)
PROTECTION = {b'__TEXT_EXEC': 5, b'__DATA': 3}


def uleb(data, i):
    value = shift = 0
    while True:
        byte = data[i]
        i += 1
        value |= (byte & 0x7f) << shift
        shift += 7
        if byte < 0x80:
            return value, i


def rebases(opcodes, segments):
    """The file offsets of the pointers dyld's rebase opcodes say are to slide."""
    out = []
    seg = offset = 0
    i = 0
    while i < len(opcodes):
        op, imm = opcodes[i] & 0xf0, opcodes[i] & 0x0f
        i += 1
        if op == 0x00:                                  # DONE
            break
        elif op == 0x10:                                # SET_TYPE_IMM
            if imm != 1:
                raise SystemExit(f'place-kernel: rebase of type {imm}, not a pointer')
        elif op == 0x20:                                # SET_SEGMENT_AND_OFFSET_ULEB
            seg = imm
            offset, i = uleb(opcodes, i)
        elif op == 0x30:                                # ADD_ADDR_ULEB
            add, i = uleb(opcodes, i)
            offset += add
        elif op == 0x40:                                # ADD_ADDR_IMM_SCALED
            offset += imm * 8
        elif op in (0x50, 0x60, 0x70, 0x80):            # DO_REBASE_*
            count, skip = imm, 0
            if op == 0x60:
                count, i = uleb(opcodes, i)
            elif op == 0x70:
                count = 1
                skip, i = uleb(opcodes, i)
            elif op == 0x80:
                count, i = uleb(opcodes, i)
                skip, i = uleb(opcodes, i)
            for _ in range(count):
                out.append(segments[seg]['fileoff'] + offset)
                offset += 8 + skip
        else:
            raise SystemExit(f'place-kernel: rebase opcode {op:#x} not known')
    return out


def place(image, base, program=False):
    renamed, protection = ({}, {}) if program else (RENAMED, PROTECTION)
    image = bytearray(image)
    magic, cputype, subtype, filetype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<IiiIIIII', image)
    if magic != 0xfeedfacf:
        raise SystemExit('place-kernel: not a 64-bit Mach-O')
    commands, segments, sections, entry, symtab, info = [], [], [], None, None, None
    at = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', image, at)
        if cmd == LC_DYLD_CHAINED_FIXUPS:
            raise SystemExit('place-kernel: chained fixups: link with -no_fixup_chains')
        if cmd == LC_SEGMENT_64:
            name, vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<16sQQQQ', image, at + 8)
            segments.append({'at': at, 'name': name.rstrip(b'\0'), 'vmaddr': vmaddr, 'vmsize': vmsize,
                             'fileoff': fileoff, 'filesize': filesize})
            for n in range(struct.unpack_from('<I', image, at + 64)[0]):
                sect_size, sect_offset = struct.unpack_from('<QI', image, at + 72 + n * 80 + 40)
                if sect_size and sect_offset:
                    sections.append(sect_offset)
        elif cmd == LC_MAIN:
            entry = struct.unpack_from('<Q', image, at + 8)[0]          # a file offset
        elif cmd == LC_SYMTAB:
            symtab = struct.unpack_from('<IIII', image, at + 8)
        elif cmd == LC_DYLD_INFO_ONLY:
            info = struct.unpack_from('<10I', image, at + 8)
        if cmd not in DROPPED:
            commands.append(bytes(image[at:at + size]))
        at += size
    end_of_commands = at
    if entry is None or info is None:
        raise SystemExit('place-kernel: no LC_MAIN or LC_DYLD_INFO_ONLY: not what ld64.lld makes of the kernel')
    rebase_off, rebase_size, bind_off, bind_size, weak_off, weak_size, lazy_off, lazy_size = info[:8]
    # (weak binding only lets dyld put another image's definition in place of this one's: the pointers
    # already hold this image's, and are rebased like any other. What is to be bound is not there.)
    if bind_size or lazy_size:
        raise SystemExit('place-kernel: the link left symbols to bind at run time: a kernel has none')

    # the pointers, then the addresses
    slots = rebases(image[rebase_off:rebase_off + rebase_size], segments)
    for slot in slots:
        struct.pack_into('<Q', image, slot, struct.unpack_from('<Q', image, slot)[0] + base)
    entry_va = next(s['vmaddr'] + entry - s['fileoff'] for s in segments
                    if s['fileoff'] <= entry < s['fileoff'] + s['filesize'] and s['filesize']) + base
    if symtab:
        symoff, nsyms = symtab[0], symtab[1]
        for n in range(nsyms):
            entry_at = symoff + n * 16
            n_type = image[entry_at + 4]
            if n_type & 0x0e == 0x0e:                                   # N_SECT
                struct.pack_into('<Q', image, entry_at + 8, struct.unpack_from('<Q', image, entry_at + 8)[0] + base)

    out = []
    for command in commands:
        command = bytearray(command)
        cmd = struct.unpack_from('<I', command)[0]
        if cmd == LC_SEGMENT_64:
            name = bytes(command[8:24]).rstrip(b'\0')
            name = renamed.get(name, name)
            command[8:24] = name.ljust(16, b'\0')
            struct.pack_into('<Q', command, 24, struct.unpack_from('<Q', command, 24)[0] + base)
            if name in protection:
                struct.pack_into('<ii', command, 56, protection[name], protection[name])
            nsects = struct.unpack_from('<I', command, 64)[0]
            for s in range(nsects):
                sect = 72 + s * 80
                command[sect + 16:sect + 32] = name.ljust(16, b'\0')
                struct.pack_into('<Q', command, sect + 32, struct.unpack_from('<Q', command, sect + 32)[0] + base)
        out.append(bytes(command))
    registers = [0] * 33
    registers[32] = entry_va                                            # pc
    out.append(struct.pack('<IIII', LC_UNIXTHREAD, 16 + ARM_THREAD_STATE64_COUNT * 4, ARM_THREAD_STATE64,
                           ARM_THREAD_STATE64_COUNT) + struct.pack('<33Q', *registers) + bytes(8))
    commands = b''.join(out)
    first_section = min(sections) if sections else len(image)
    if 32 + len(commands) > first_section:
        raise SystemExit('place-kernel: no room for the load commands: link with a larger -headerpad')
    image[32:end_of_commands] = bytes(end_of_commands - 32)
    image[32:32 + len(commands)] = commands
    struct.pack_into('<IIII', image, 12, filetype, len(out), len(commands), MH_NOUNDEFS | (0 if program else MH_PIE))
    return bytes(image), len(slots), entry_va


def main():
    arguments = sys.argv[1:]
    program = arguments[:1] == ['--program']
    arguments = arguments[1:] if program else arguments
    if len(arguments) not in (2, 3) or (program and len(arguments) != 2):
        raise SystemExit(__doc__.split('\n\n')[1])
    base = 0 if program else int(arguments[2], 0) if len(arguments) == 3 else 0xfffffff007004000
    image, slots, entry = place(open(arguments[0], 'rb').read(), base, program)
    open(arguments[1], 'wb').write(image)
    print(f'place-kernel: at {base:#x}, {slots} pointers moved, entry {entry:#x}')


if __name__ == '__main__':
    main()
