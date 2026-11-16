#!/usr/bin/env python3
"""A phone's identity: the seed the loader makes its serial numbers, ECID and addresses from.

  tools/identity.py NAME          this phone's seed, made at random the first time and kept after: the boot
                                  argument identity=SEED, and what the loader makes of it
  tools/identity.py --seed SEED   what a seed gives

The loader (arch/arm64/boot/src/unit.c) takes all of a unit's identity from one 64-bit seed: the serial number (10
characters, digits and capitals without I and O, as Apple's randomised ones), the logic board's serial (17),
the ECID (52 bits), the Wi-Fi and Bluetooth addresses (unicast, locally administered) and the boot manifest's
hash. Given the same seed at every boot, the phone stays the same device to Darwin; without one it is another at
each. A seed is kept per phone in $XDG_CONFIG_HOME/hakios/identity/NAME (~/.config/... by default), outside
any checkout: it is this phone's own, and not for a repository.
"""
import argparse
import os
import secrets
import sys
from pathlib import Path

ALPHABET = '0123456789ABCDEFGHJKLMNPQRSTUVWXYZ'      # 34: no I, no O
MASK = (1 << 64) - 1


class SplitMix64:
    """The loader's generator (unit_next()): the same numbers from the same seed."""

    def __init__(self, seed):
        self.state = seed & MASK

    def next(self):
        self.state = (self.state + 0x9e3779b97f4a7c15) & MASK
        z = self.state
        z = ((z ^ (z >> 30)) * 0xbf58476d1ce4e5b9) & MASK
        z = ((z ^ (z >> 27)) * 0x94d049bb133111eb) & MASK
        return z ^ (z >> 31)


def unit(seed):
    """What the loader makes of a seed, as unit_from_seed() does."""
    r = SplitMix64(seed)

    def characters(count):
        return ''.join(ALPHABET[r.next() % len(ALPHABET)] for _ in range(count))

    def address():
        value = r.next()
        octets = [(value >> (8 * i)) & 0xff for i in range(6)]
        octets[0] = (octets[0] & ~3 & 0xff) | 2
        return octets

    serial, mlb = characters(10), characters(17)
    ecid = (r.next() & 0x000fffffffffffff) | 0x0001000000000000
    wifi, bluetooth = address(), address()
    if bluetooth[5] == wifi[5]:
        bluetooth[5] ^= 1
    manifest = b''.join(r.next().to_bytes(8, 'little') for _ in range(6))
    return {'serial': serial, 'mlb': mlb, 'ecid': ecid, 'wifi': wifi, 'bluetooth': bluetooth, 'manifest_hash': manifest}


def store(name):
    base = Path(os.environ.get('XDG_CONFIG_HOME') or Path.home() / '.config')
    return base / 'hakios' / 'identity' / name


def seed_of(name, path=None):
    """The phone's seed: the one kept for it, or a new one, kept from now on."""
    path = path or store(name)
    if path.is_file():
        return int(path.read_text().strip(), 16)
    seed = secrets.randbits(64)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f'{seed:016x}\n')
    path.chmod(0o600)
    return seed


def describe(seed):
    u = unit(seed)

    def mac(octets):
        return ':'.join(f'{o:02x}' for o in octets)

    return '\n'.join([f'identity={seed:016x}',
                      f'  serial number  {u["serial"]}',
                      f'  MLB serial     {u["mlb"]}',
                      f'  ECID           {u["ecid"]:#x}',
                      f'  Wi-Fi          {mac(u["wifi"])}',
                      f'  Bluetooth      {mac(u["bluetooth"])}'])


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    p.add_argument('name', nargs='?', help='the phone (its package name)')
    p.add_argument('--seed', help='16 hex digits: say what this seed gives instead')
    options = p.parse_args(argv)
    if options.seed is not None:
        if len(options.seed) != 16 or any(c not in '0123456789abcdefABCDEF' for c in options.seed):
            raise SystemExit('a seed is 16 hex digits')
        print(describe(int(options.seed, 16)))
    elif options.name:
        print(describe(seed_of(options.name)))
        print(f'(kept in {store(options.name)})')
    else:
        p.error('a phone\'s name, or --seed')


if __name__ == '__main__':
    main(sys.argv[1:])
