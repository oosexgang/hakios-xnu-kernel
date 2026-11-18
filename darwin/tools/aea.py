#!/usr/bin/env python3
"""Decrypt the disk images of a Darwin 26/27 IPSW on Linux (Apple Encrypted Archive, AEA1, profile 1).

  aea.py list IPSW                       the members of the IPSW (size, method, name)
  aea.py get IPSW MEMBER OUT             one member as it is (inflated if it is compressed)
  aea.py info IPSW MEMBER                an encrypted member's metadata and root header
  aea.py decrypt IPSW MEMBER OUT [N]     an encrypted member, decrypted and decompressed, into OUT
                                         (N: on from cluster N, OUT kept up to where it begins)

IPSW is a local file or the URL of one on Apple's CDN; from a URL only the bytes asked for are fetched
(HTTP range requests), so the 8 GB root file system of an IPSW is read once, while it is decrypted, and
an IPSW is never downloaded whole. A request that breaks off is taken up again where it stopped.

The symmetric key is wrapped in the archive's metadata (com.apple.wkms.fcs-response) with HPKE (RFC 9180:
DHKEM P-256, HKDF-SHA256, AES-256-GCM, base mode) to a private key Apple publishes at the URL in
com.apple.wkms.fcs-key-url. From it: a main key (HKDF with the archive's salt); a root header (sizes,
compression, checksum) under AES-CTR and HMAC-SHA256; then clusters, each a header of segment sizes,
checksums and MACs, chained by MAC, and its segments, each under its own key, compressed with LZFSE.
Every segment is authenticated and its SHA-256 checked; a mismatch stops the run.

Needs: Python's cryptography package, and Apple's LZFSE decoder as a shared library (Ubuntu: liblzfse1;
or LZFSE_LIB=PATH to a liblzfse.so built from github.com/lzfse/lzfse).
"""
import base64
import ctypes
import ctypes.util
import hashlib
import hmac
import json
import os
import struct
import sys
import urllib.error
import urllib.parse
import urllib.request
import zlib

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

ATTEMPTS = 30


# ---- the IPSW: a ZIP64 archive, local or on a web server -------------------------------------------------

class Source:
    """Bytes of an IPSW by position, from a file or by HTTP range requests"""
    def __init__(self, where):
        self.url = where if where.startswith(('http://', 'https://')) else None
        if self.url:
            request = urllib.request.Request(self.url, method='HEAD')
            with urllib.request.urlopen(request, timeout=120) as response:
                self.size = int(response.headers['Content-Length'])
        else:
            self.file = open(where, 'rb')
            self.size = os.fstat(self.file.fileno()).st_size

    def stream(self, start, end):
        """a file-like object reading START..END (inclusive)"""
        if self.url:
            request = urllib.request.Request(self.url, headers={'Range': f'bytes={start}-{end}'})
            return urllib.request.urlopen(request, timeout=120)
        self.file.seek(start)
        return _Limited(self.file, end + 1 - start)

    def read(self, start, size):
        with self.stream(start, start + size - 1) as s:
            return s.read(size)


class _Limited:
    def __init__(self, f, left):
        self.f, self.left = f, left

    def read(self, n=-1):
        n = self.left if n < 0 else min(n, self.left)
        data = self.f.read(n)
        self.left -= len(data)
        return data

    def __enter__(self):
        return self

    def __exit__(self, *_):
        return False


def members(source):
    """(name, method, compressed size, offset of the local header) of every member"""
    tail = source.read(max(0, source.size - 65536), min(65536, source.size))
    at = tail.rfind(b'PK\x06\x06')
    if at < 0:
        sys.exit('aea: no ZIP64 directory at the end of the IPSW')
    cd_size, cd_offset = struct.unpack_from('<QQ', tail, at + 40)
    cd = source.read(cd_offset, cd_size)
    p = 0
    while cd[p:p + 4] == b'PK\x01\x02':
        method, = struct.unpack_from('<H', cd, p + 10)
        csize, usize, nl, el, cl = struct.unpack_from('<IIHHH', cd, p + 20)
        offset, = struct.unpack_from('<I', cd, p + 42)
        name = cd[p + 46:p + 46 + nl].decode()
        extra, q = cd[p + 46 + nl:p + 46 + nl + el], 0
        while q + 4 <= len(extra):
            tag, length = struct.unpack_from('<HH', extra, q)
            if tag == 1:
                values = list(struct.unpack_from('<' + 'Q' * (length // 8), extra, q + 4))
                if usize == 0xFFFFFFFF:
                    usize = values.pop(0)
                if csize == 0xFFFFFFFF:
                    csize = values.pop(0)
                if offset == 0xFFFFFFFF:
                    offset = values.pop(0)
            q += 4 + length
        yield name, method, csize, offset
        p += 46 + nl + el + cl


def member(source, name):
    """(method, compressed size, where its data starts)"""
    for n, method, csize, offset in members(source):
        if n == name:
            nl, el = struct.unpack_from('<HH', source.read(offset + 26, 4))
            return method, csize, offset + 30 + nl + el
    sys.exit(f'aea: no member {name}')


class Member:
    """A stored member read in order, taken up again where a request broke off; seek() moves on"""
    def __init__(self, source, name):
        method, self.size, self.start = member(source, name)
        if method != 0:
            sys.exit(f'aea: {name} is compressed in the IPSW (method {method}): use "get" first')
        self.source, self.at, self.stream = source, 0, None

    def seek(self, at):
        self.at, self.stream = at, None

    def read(self, n):
        out = b''
        for _ in range(ATTEMPTS):
            try:
                if self.stream is None:
                    self.stream = self.source.stream(self.start + self.at, self.start + self.size - 1)
                while len(out) < n:
                    chunk = self.stream.read(n - len(out))
                    if not chunk:
                        if self.at >= self.size:
                            return out
                        raise IOError('the connection closed early')
                    out += chunk
                    self.at += len(chunk)
                return out
            except (OSError, IOError) as e:
                print(f'aea: the read broke off at {self.at} ({e}): again', file=sys.stderr, flush=True)
                self.stream = None
        sys.exit('aea: the reads keep breaking off')


def get(source, name, out_path):
    method, csize, data = member(source, name)
    inflater = zlib.decompressobj(-15) if method == 8 else None
    with source.stream(data, data + csize - 1) as stream, open(out_path, 'wb') as out:
        while chunk := stream.read(1 << 20):
            out.write(inflater.decompress(chunk) if inflater else chunk)
        if inflater:
            out.write(inflater.flush())


# ---- HKDF and HPKE (RFC 5869, RFC 9180 base mode, the recipient's side) ---------------------------------

def hkdf_extract(salt, ikm):
    return hmac.new(salt or b'\0' * 32, ikm, hashlib.sha256).digest()


def hkdf_expand(prk, info, length):
    out, block, i = b'', b'', 0
    while len(out) < length:
        i += 1
        block = hmac.new(prk, block + info + bytes([i]), hashlib.sha256).digest()
        out += block
    return out[:length]


def hkdf(ikm, salt, info, length):
    return hkdf_expand(hkdf_extract(salt, ikm), info, length)


KEM_ID, KDF_ID, AEAD_ID = 0x0010, 0x0001, 0x0002     # DHKEM(P-256, HKDF-SHA256), HKDF-SHA256, AES-256-GCM


def hpke_open(private_key, enc, ciphertext):
    def extract(suite, salt, label, ikm):
        return hkdf_extract(salt, b'HPKE-v1' + suite + label + ikm)

    def expand(suite, prk, label, info, length):
        return hkdf_expand(prk, struct.pack('>H', length) + b'HPKE-v1' + suite + label + info, length)

    kem = b'KEM' + struct.pack('>H', KEM_ID)
    dh = private_key.exchange(ec.ECDH(), ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), enc))
    mine = private_key.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
    shared = expand(kem, extract(kem, b'', b'eae_prk', dh), b'shared_secret', enc + mine, 32)
    suite = b'HPKE' + struct.pack('>HHH', KEM_ID, KDF_ID, AEAD_ID)
    context = b'\0' + extract(suite, b'', b'psk_id_hash', b'') + extract(suite, b'', b'info_hash', b'')
    secret = extract(suite, shared, b'secret', b'')
    key, nonce = expand(suite, secret, b'key', context, 32), expand(suite, secret, b'base_nonce', context, 12)
    return AESGCM(key).decrypt(nonce, ciphertext, b'')


# ---- the archive ----------------------------------------------------------------------------------------

def header(f):
    magic, profile, auth_size = struct.unpack('<4sII', f.read(12))
    if magic != b'AEA1':
        sys.exit('aea: not an AEA1 archive')
    auth = f.read(auth_size)
    meta, p = {}, 0
    # each field: its length (its own four bytes included), then KEY NUL VALUE. A length under 4 would read the
    # next field from inside this one (0: this one again, for ever), one past the end of the metadata a field
    # that is not all there: either is an archive that is not what it says
    while p < len(auth):
        if len(auth) - p < 4:
            sys.exit(f'aea: the metadata ends inside a field\'s length, at {p}')
        length, = struct.unpack_from('<I', auth, p)
        if not 4 <= length <= len(auth) - p:
            sys.exit(f'aea: a metadata field of {length} bytes at {p}, in {len(auth)}')
        key, _, value = auth[p + 4:p + length].partition(b'\0')
        meta[key.decode()] = value
        p += length
    return profile, auth, meta


class HttpsOnly(urllib.request.HTTPRedirectHandler):
    """redirects followed to https:// alone"""
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if urllib.parse.urlsplit(newurl).scheme != 'https':
            raise urllib.error.HTTPError(newurl, code, 'aea: a redirect away from https:// is not followed', headers, fp)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def symmetric_key(meta):
    # The URL comes from the archive's metadata, which nothing has authenticated yet: https:// only (no file://,
    # no plain http:// that anyone on the way could answer), redirects included.
    url = meta.get('com.apple.wkms.fcs-key-url', b'').decode(errors='replace')
    if urllib.parse.urlsplit(url).scheme != 'https':
        sys.exit(f'aea: the key URL is not https://: {url!r}')
    with urllib.request.build_opener(HttpsOnly).open(url, timeout=60) as response:
        private_key = serialization.load_pem_private_key(response.read(), None)
    wrapped = json.loads(meta['com.apple.wkms.fcs-response'])
    return hpke_open(private_key, base64.b64decode(wrapped['enc-request']), base64.b64decode(wrapped['wrapped-key']))


def ctr(data, key, iv):
    d = Cipher(algorithms.AES(key), modes.CTR(iv)).decryptor()
    return d.update(data) + d.finalize()


def key_material(key, info, extract=True):
    """MAC key, AES key and IV"""
    m = hkdf(key, b'', info, 80) if extract else hkdf_expand(key, info, 80)
    return m[:32], m[32:64], m[64:80]


def open_archive(source, name):
    f = Member(source, name)
    profile, auth, meta = header(f)
    if profile & 0xffffff != 1:
        sys.exit(f'aea: profile {profile & 0xffffff}: only 1 (symmetric) is done here')
    salt = f.read(32)
    main = hkdf(symmetric_key(meta), salt, b'AEA_AMK' + struct.pack('<I', profile), 32)
    root_mac, root_data, cluster_mac = f.read(32), f.read(48), f.read(32)
    k_mac, k_aes, k_iv = key_material(main, b'AEA_RHEK')
    expected = hmac.new(k_mac, cluster_mac + auth + root_data + struct.pack('<Q', 32 + len(auth)), hashlib.sha256).digest()
    if not hmac.compare_digest(expected, root_mac):
        sys.exit('aea: the root header does not authenticate: wrong key')
    root = ctr(root_data, k_aes, k_iv)
    file_size, encrypted_size, segment_size, per_cluster = struct.unpack_from('<QQII', root)
    info = dict(file_size=file_size, encrypted_size=encrypted_size, segment_size=segment_size,
                segments_per_cluster=per_cluster, compression=chr(root[24]), checksum=root[25])
    return f, meta, main, cluster_mac, info


def lzfse():
    path = os.environ.get('LZFSE_LIB') or ctypes.util.find_library('lzfse')
    if not path:
        sys.exit('aea: no LZFSE library (Ubuntu: apt install liblzfse1; or LZFSE_LIB=PATH)')
    lib = ctypes.CDLL(path)
    lib.lzfse_decode_buffer.restype = ctypes.c_size_t
    lib.lzfse_decode_buffer.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p]
    return lib


def decrypt(source, name, out_path, start=0):
    f, meta, main, cluster_mac, info = open_archive(source, name)
    if info['compression'] not in ('-', 'e', 'z'):
        sys.exit(f"aea: compression {info['compression']!r} is not done here")
    if info['checksum'] not in (0, 2):
        sys.exit(f"aea: checksum {info['checksum']} is not done here")
    decoder = lzfse() if info['compression'] == 'e' else None
    per, seg = info['segments_per_cluster'], info['segment_size']
    entry = 8 + (32 if info['checksum'] == 2 else 0)
    written, cluster = 0, 0
    with open(out_path, 'r+b' if start and os.path.exists(out_path) else 'wb') as out:
        if start:
            out.truncate(start * per * seg)
            out.seek(start * per * seg)
        while written < info['file_size']:
            ckey = hkdf(main, b'', b'AEA_CK' + struct.pack('<I', cluster), 32)
            h_mac, h_aes, h_iv = key_material(ckey, b'AEA_CHEK')
            block = f.read(entry * per + 32 + 32 * per)
            headers, macs = block[:entry * per], block[entry * per:]
            if not hmac.compare_digest(hmac.new(h_mac, macs + headers + struct.pack('<Q', len(macs)), hashlib.sha256).digest(),
                                       cluster_mac):
                sys.exit(f'aea: cluster {cluster} does not authenticate')
            cluster_mac = macs[:32]
            headers = ctr(headers, h_aes, h_iv)
            sizes = [struct.unpack_from('<II', headers, i * entry) for i in range(per)]
            if cluster < start:                         # walked: its segments skipped, not fetched
                written += sum(usize for usize, _ in sizes)
                f.seek(f.at + sum(csize for _, csize in sizes))
                cluster += 1
                continue
            prk = hkdf_extract(b'', ckey)
            for i, (usize, csize) in enumerate(sizes):
                if usize == 0:
                    continue
                data = f.read(csize)
                s_mac, s_aes, s_iv = key_material(prk, b'AEA_SK' + struct.pack('<I', i), extract=False)
                if not hmac.compare_digest(hmac.new(s_mac, data + struct.pack('<Q', 0), hashlib.sha256).digest(),
                                           macs[32 + 32 * i:64 + 32 * i]):
                    sys.exit(f'aea: segment {i} of cluster {cluster} does not authenticate')
                plain = ctr(data, s_aes, s_iv)
                if csize != usize:
                    if decoder is None:
                        plain = zlib.decompress(plain)
                    else:
                        buffer = ctypes.create_string_buffer(usize + 1)
                        n = decoder.lzfse_decode_buffer(buffer, usize + 1, plain, len(plain), None)
                        if n != usize:
                            sys.exit(f'aea: segment {i} of cluster {cluster}: LZFSE gave {n} bytes, not {usize}')
                        plain = buffer.raw[:usize]
                if entry > 8 and hashlib.sha256(plain).digest() != headers[i * entry + 8:i * entry + 40]:
                    sys.exit(f'aea: segment {i} of cluster {cluster}: checksum')
                out.write(plain)
                written += len(plain)
            cluster += 1
            if cluster % 16 == 0:
                print(f"aea: {written >> 20} of {info['file_size'] >> 20} MB", file=sys.stderr, flush=True)
    if written != info['file_size']:
        sys.exit(f"aea: {written} bytes written, the archive says {info['file_size']}")


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ('list', 'get', 'info', 'decrypt'):
        sys.exit(__doc__)
    source = Source(sys.argv[2])
    if sys.argv[1] == 'list':
        for name, method, csize, _ in members(source):
            print(f'{csize:>12} {method} {name}')
    elif sys.argv[1] == 'get':
        get(source, sys.argv[3], sys.argv[4])
    elif sys.argv[1] == 'info':
        _, meta, _, _, info = open_archive(source, sys.argv[3])
        for key, value in meta.items():
            print(f'{key}: {value[:100]!r}')
        print(info)
    else:
        decrypt(source, sys.argv[3], sys.argv[4], int(sys.argv[5]) if len(sys.argv) > 5 else 0)


if __name__ == '__main__':
    main()
