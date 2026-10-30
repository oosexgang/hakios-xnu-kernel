#!/usr/bin/env python3
# The access point's side of the WPA2 handshake, written against Python's own hashlib and hmac
# and the AES key wrap of the "cryptography" package: an implementation that shares nothing with
# wifi/lx/src/lx_wpa.c. For a number of made-up networks it writes down, as C, the messages an
# access point would send and the answers a correct station gives, byte for byte; test_wpa.c
# has lx_wpa.c answer the same messages and compares. (IEEE 802.11-2020, 12.7.)
import hashlib
import hmac
import random
import struct
import sys

from cryptography.hazmat.primitives.keywrap import aes_key_wrap

rnd = random.Random(int(sys.argv[1]) if len(sys.argv) > 1 else 1)


def prf(key, label, data, length):
    out = b''
    for i in range((length + 19) // 20):
        out += hmac.new(key, label + b'\0' + data + bytes([i]), hashlib.sha1).digest()
    return out[:length]


def key_message(version, info, replay, nonce, rsc, data, kck):
    body = struct.pack('>BHH8s32s16s8s8s16sH', 2, info, 16 if info & 0x80 else 0, replay, nonce, bytes(16), rsc, bytes(8),
                       bytes(16), len(data)) + data
    frame = bytearray(struct.pack('>BBH', version, 3, len(body)) + body)
    if kck is not None:
        frame[81:97] = hmac.new(kck, bytes(frame), hashlib.sha1).digest()[:16]
    return bytes(frame)


def c_bytes(name, data):
    return 'static const unsigned char %s[] = { %s };\n' % (name, ', '.join('0x%02x' % b for b in data) or '0')


def case(n):
    passphrase = bytes(rnd.choice(b'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 !#') for _ in
                       range(rnd.randint(8, 63)))
    name = bytes(rnd.randrange(32, 127) for _ in range(rnd.randint(1, 32)))
    ap, station = bytes([2] + [rnd.randrange(256) for _ in range(5)]), bytes([2] + [rnd.randrange(256) for _ in range(5)])
    anonce, snonce = rnd.randbytes(32), rnd.randbytes(32)
    rsn = bytes.fromhex('30140100000fac040100000fac040100000fac020000')      # CCMP, CCMP, PSK, no capabilities
    group, group_index, group2 = rnd.randbytes(16), rnd.randint(1, 3), rnd.randbytes(16)
    version = rnd.choice([1, 2])
    counter = rnd.randrange(1 << 40)
    replay = lambda k: struct.pack('>Q', counter + k)

    pmk = hashlib.pbkdf2_hmac('sha1', passphrase, name, 4096, 32)
    ptk = prf(pmk, b'Pairwise key expansion', min(ap, station) + max(ap, station) + min(anonce, snonce) + max(anonce, snonce), 48)
    kck, kek, tk = ptk[:16], ptk[16:32], ptk[32:]

    def wrapped(key, index, before=b''):
        # the group key as a "key data encapsulation", padded to a multiple of eight bytes, wrapped with the KEK
        plain = before + bytes([0xdd, 6 + len(key), 0x00, 0x0f, 0xac, 1, index, 0]) + key
        if len(plain) % 8:
            plain += b'\xdd' + bytes(7 - len(plain) % 8)
        return aes_key_wrap(kek, plain)

    rsc = rnd.randbytes(6) + bytes(2)
    m1 = key_message(version, 0x008a, replay(0), anonce, bytes(8), b'', None)
    m2 = key_message(version, 0x010a, replay(0), snonce, bytes(8), rsn, kck)
    m3 = key_message(version, 0x13ca, replay(1), anonce, rsc, wrapped(group, group_index, rsn), kck)
    m4 = key_message(version, 0x030a, replay(1), bytes(32), bytes(8), b'', kck)
    g1 = key_message(version, 0x1382, replay(5), bytes(32), rsc, wrapped(group2, group_index % 3 + 1), kck)
    g2 = key_message(version, 0x0302, replay(5), bytes(32), bytes(8), b'', kck)
    # the same message 3 from an access point that was given another passphrase
    other = hashlib.pbkdf2_hmac('sha1', passphrase + b'x', name, 4096, 32)
    other_ptk = prf(other, b'Pairwise key expansion', min(ap, station) + max(ap, station) + min(anonce, snonce) + max(anonce, snonce), 48)
    m3_wrong = key_message(version, 0x13ca, replay(1), anonce, rsc, aes_key_wrap(other_ptk[16:32], bytes(24)), other_ptk[:16])

    text = ''.join(c_bytes('c%d_%s' % (n, k), v) for k, v in (
        ('passphrase', passphrase), ('name', name), ('ap', ap), ('station', station), ('snonce', snonce), ('rsn', rsn),
        ('pmk', pmk), ('tk', tk), ('group', group), ('group2', group2), ('m1', m1), ('m2', m2), ('m3', m3), ('m4', m4),
        ('g1', g1), ('g2', g2), ('m3_wrong', m3_wrong)))
    entry = '{ %s, %d }' % (', '.join('c%d_%s, sizeof(c%d_%s)' % (n, k, n, k) for k in (
        'passphrase', 'name', 'ap', 'station', 'snonce', 'rsn', 'pmk', 'tk', 'group', 'group2', 'm1', 'm2', 'm3', 'm4', 'g1',
        'g2', 'm3_wrong')), group_index)
    return text, entry


count = 12
cases = [case(n) for n in range(count)]
print(''.join(t for t, _ in cases))
print('static const struct handshake handshakes[] = {\n\t%s\n};' % ',\n\t'.join(e for _, e in cases))

# two things for the pieces on their own, from the same independent implementations
key = bytes(range(32, 48))
print(c_bytes('prf_key', key) + c_bytes('prf_out', prf(key, b'A label', bytes(range(70)), 61)))
print(c_bytes('wrap_kek', key) + c_bytes('wrap_out', aes_key_wrap(key, bytes(range(100, 140)))))
