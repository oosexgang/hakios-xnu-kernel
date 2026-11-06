#!/usr/bin/env python3
"""Check what the loader read from the phone's storage against the ROM's own images.

usage: ufs-survey.py [LOG] [FW_ZIP]
  LOG     the loader's log of a run with ufs=1 among XL_CMDLINE (default logs/last.log)
  FW_ZIP  the ROM's firmware archive (default fw/fw.zip)

With ufs=1 the loader (xnuloader/loader.c, ufs_survey) lists the phone's logical units and GPT
partitions as Qualcomm's own UFS driver in UEFI sees them, and for the partitions there are images
of in the ROM's firmware-update/ it prints an FNV-1a hash of exactly as many bytes as the image
has. This computes the same hash from the images and says, partition by partition, whether the
two agree: where they do, reading through that driver gives the bytes that are on the flash.
With ufs=2 ufs_check=1 the kernel then reads the same partitions through the loader's page
(bsd/dev/redwood_ufs.c) and prints the same hashes: those are shown as kernel:NAME.
Nothing is read from the phone here; it only reads the log.
"""
import re
import sys
import zipfile


def fnv(data, h=0xcbf29ce484222325):
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


def main():
    log_path = sys.argv[1] if len(sys.argv) > 1 else 'logs/last.log'
    zip_path = sys.argv[2] if len(sys.argv) > 2 else 'fw/fw.zip'
    text = open(log_path, encoding='utf-8', errors='replace').read()

    units = re.findall(r'^ufs: unit (\d+): .*$', text, re.M)
    parts = re.findall(r'^ufs: part (\S+): lba (\d+)\.\.(\d+), (\d+) KB$', text, re.M)
    hashes = re.findall(r'^ufs: (\S+): fnv ([0-9a-f]+) over (\d+) bytes, (\d+) ms$', text, re.M)
    # the same partitions read again by the kernel through the loader's page (ufs=2 ufs_check=1)
    checks = re.findall(r'ufs: check (\S+): fnv ([0-9a-f]+) over (\d+) bytes, (\d+) ms', text)
    hashes += [('kernel:' + name, value, size, ms) for name, value, size, ms in checks]
    errors = re.findall(r'^ufs: (\S+): (reading at block .*|.* the partition is smaller)$', text, re.M)
    if not parts:
        sys.exit(f'ufs-survey.py: no "ufs:" lines in {log_path} (was the run made with ufs=1?)')
    print(f'{len(units)} logical units, {len(parts)} partitions in the log')

    images = {}
    with zipfile.ZipFile(zip_path) as z:
        for info in z.infolist():
            m = re.match(r'firmware-update/(.+)\.img$', info.filename)
            if m:
                images[m.group(1)] = info.filename
        agreed = differed = 0
        for name, value, size, ms in hashes:
            base = re.sub(r'_[ab]$', '', name.removeprefix('kernel:'))
            if base not in images:
                print(f'  {name}: no image named {base}.img in {zip_path}')
                continue
            data = z.read(images[base])
            if len(data) != int(size):
                print(f'  {name}: the image has {len(data)} bytes, the loader hashed {size}')
                differed += 1
                continue
            mine = fnv(data)
            same = mine == int(value, 16)
            agreed += same
            differed += not same
            rate = int(size) / max(int(ms), 1) / 1000
            print(f'  {name:<21} {"same" if same else "DIFFERENT":<9} {int(size):>11} bytes '
                  f'in {ms:>5} ms ({rate:.0f} MB/s)  flash {value}  image {mine:x}')
    for name, what in errors:
        print(f'  {name}: {what}')
    print(f'{agreed} agree, {differed} differ, {len(errors)} not read')
    return 0 if differed == 0 and not errors and agreed > 0 else 1


if __name__ == '__main__':
    sys.exit(main())
