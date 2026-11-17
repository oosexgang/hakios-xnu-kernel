#!/usr/bin/env python3
"""One image for a phone: its kernel, its loader and its boot arguments in one file, to boot, keep and pass around.

  tools/image.py build [--board NAME] [--defconfig NAME] [--identity FILE] [--out DIR]
  tools/image.py build --every-run [--board NAME] [--out DIR]
        one image for each run of a phone (the defconfigs phone_*), each in DIR/DEFCONFIG/: a day with the phone
        (Documentation/phone-test.ru.md) without a build between the runs
        build/images/NAME/: NAME.payload (the loader, the kernel and the boot arguments, as the phone's UEFI starts
        them), boot.img when the phone's UEFI build is there (fastboot boot boot.img: one command, nothing written to
        the phone), manifest.json (what went in, with the hashes), README.md (how to boot it)
  tools/image.py publish IMAGE_DIR DEVICE_REPO [--version TEXT] [--with-uefi]
        the image into DEVICE_REPO/images/VERSION/ and the index DEVICE_REPO/images/README.md: for a phone's own
        repository (tools/device.py add --from), which others then boot from
  tools/image.py personalize IMAGE --identity FILE [-o OUT]
        an image someone else built, given the identity of your own iPhone's IPSW (below): a payload or a boot.img
  tools/image.py show FILE
        what an image holds, and whether it is what its manifest says
  tools/image.py verify DIR
        every image under DIR (a phone's images/) is what its manifest says, and what publish would take: no identity,
        no system image, a loader without Apple's data; status 1 when one is not (the phone's repository's check runs it)
  tools/image.py extract FILE [--payload OUT] [--loader OUT]
        its payload and its loader, as files (tools/qemu-kernel.sh --image boots an image in QEMU with them)

The kernel is kernel/build.sh's (--kernel, or BENCH_KERNEL as for ./bench), the loader arch/arm64/boot/build.sh's
(--loader). The boot arguments are the board's own (boot { } of its board.dts) and then the defconfig's
(arch/arm64/configs, phone_first unless told), each replacing one of the same name, as ./bench does.

What never goes into an image that is published: Apple's data. Which iPhone Darwin is told it runs on (the identity)
is taken from an IPSW; a loader built with it (identity.h), or a payload carrying it, is for its builder's own phone.
`publish` refuses both, and the system image (bench ramdisk) is never part of an image. Whoever boots a published
image gives it the identity of their own IPSW: `personalize --identity DeviceTree.bin` (the device tree
darwin/tools/unpack.sh leaves beside the IPSW, or a blob mkidentity.py --blob made), which the loader takes from the
payload (arch/arm64/boot/src/identity_given.c). A phone's UEFI build carries its vendor's drivers: boot.img goes into
a repository only with --with-uefi, which says the repository's owner may publish them.
"""
import argparse
import datetime
import gzip
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'arch/arm64/boot'))
import boards  # noqa: E402  (arch/arm64/boot/dts/NAME/board.dts)
import defconfig  # noqa: E402  (arch/arm64/configs)

PAYLOAD_MAGIC = 0x50554E58      # "XNUP" (arch/arm64/boot/mkpayload.py)
IDENTITY_MAGIC = b'IDNT'        # an identity blob (mkidentity.py --blob)
PLACEHOLDER = b'hakios-placeholder'     # what the placeholder identity puts in a loader, and a real one never does
DEFAULT_DEFCONFIG = 'phone_first'
UEFI_FILES = ('SILICIUM_UEFI.fd', 'BootShim.bin', 'device-tree.dtb')


def say(text):
    print(f'image.py: {text}')


def die(text):
    raise SystemExit(f'image.py: {text}')


# ---- the payload ----------------------------------------------------------------------------------------------------

def payload_pack(entries):
    """An XNUP container of [(name, bytes)], as mkpayload.py writes one."""
    body = b''
    for name, data in entries:
        if len(name.encode()) >= 16:
            die(f'an entry name of 16 bytes or more: {name}')
        body += struct.pack('<16sII', name.encode(), len(data), 0) + data + b'\0' * (-len(data) % 16)
    body += b'\0' * 24
    return struct.pack('<II', PAYLOAD_MAGIC, len(body)) + body


def payload_entries(data):
    """[(name, bytes)] of an XNUP container, as the loader reads it (arch/arm64/boot/src/payload.c)."""
    if len(data) < 8:
        die('not a payload: too short')
    magic, size = struct.unpack_from('<II', data)
    if magic != PAYLOAD_MAGIC or 8 + size > len(data):
        die('not a payload (no XNUP at its start, or a size past its end)')
    entries, at, end = [], 8, 8 + size
    while at + 24 <= end:
        name, length, _ = struct.unpack_from('<16sII', data, at)
        name = name.split(b'\0')[0].decode(errors='replace')
        if not name:
            break
        if at + 24 + length > end:
            die(f'the entry "{name}" goes past the payload\'s end')
        entries.append((name, data[at + 24:at + 24 + length]))
        at += 24 + length + (-length % 16)
    return entries


# ---- the boot image -------------------------------------------------------------------------------------------------

def boot_image_parts(data):
    """(header version, the kernel's bytes) of an Android boot image, version 0 to 2."""
    if data[:8] != b'ANDROID!':
        die('not an Android boot image')
    kernel_size, = struct.unpack_from('<I', data, 8)
    page, version = struct.unpack_from('<II', data, 36)
    if version not in (0, 1, 2) or page == 0:
        die(f'a boot image of header version {version}, page {page}: only 0 to 2 are read here')
    return version, data[page:page + kernel_size]


def boot_image_payload(data):
    """(shim + UEFI image, payload, device tree) of a boot image mkboot.py made: the payload is the XNUP container at
    the end of the gzip stream, which is where mkboot.py put it and the only place one ends the stream."""
    _, kernel = boot_image_parts(data)
    stream = zlib.decompressobj(16 + zlib.MAX_WBITS)
    blob = stream.decompress(kernel)
    dtb = stream.unused_data
    # (the shim is of any length: the payload starts where it and the UEFI image end, at no particular alignment; the
    # stream is padded to 16 after it)
    at = len(blob)
    while (at := blob.rfind(b'XNUP', 0, at)) >= 0:
        size, = struct.unpack_from('<I', blob, at + 4) if at + 8 <= len(blob) else (1 << 32,)
        if at + 8 + size <= len(blob) and len(blob) - (at + 8 + size) < 16:
            return blob[:at], blob[at:at + 8 + size], dtb
    die('no payload at the end of the boot image\'s kernel: not one mkboot.py made with -p')


def boot_image_with(data, payload):
    """The boot image DATA with its payload replaced by PAYLOAD (the UEFI, its shim and the device tree as they were)."""
    import mkboot
    version, _ = boot_image_parts(data)
    front, _, dtb = boot_image_payload(data)
    blob = front + payload
    blob += b'\0' * (-len(blob) % 16)
    return mkboot.boot_image(gzip.compress(blob, 6, mtime=0) + dtb, version)


# ---- what an image is made of -----------------------------------------------------------------------------------------

def git_says(*arguments):
    done = subprocess.run(['git', '-C', str(ROOT)] + list(arguments), capture_output=True, text=True)
    return done.stdout.strip() if done.returncode == 0 else ''


def board_of(name):
    """(name, its board) of the package named, or of the only one here with a uefi { } node."""
    found = boards.load()[1]
    phones = {n: b for n, b in found.items() if 'uefi' in b}
    if name is None:
        if len(phones) != 1:
            die('which phone? --board NAME (here: ' + (', '.join(sorted(phones)) or
                'no package with a uefi { } node; tools/device.py add NAME') + ')')
        name = next(iter(phones))
    if name not in found:
        die(f'no board "{name}" here (tools/device.py list)')
    return name, found[name]


def boot_arguments(board, preset):
    """The board's arguments and its cpus=, then the defconfig's, each replacing one of the same name (as ./bench)."""
    boot = board.get('boot', {})
    given = list(boot.get('bootargs', [])) + ([f'cpus={boot["cpus"]}'] if boot.get('cpus') else [])
    merged = {}
    for argument in given + list(defconfig.arguments(preset).values()):
        merged[defconfig.key(argument)] = argument
    return ' '.join(merged.values())


def identity_blob(path):
    """An identity for the payload: a blob as it is, or one made from an iPhone's device tree (mkidentity.py --blob)."""
    data = Path(path).read_bytes()
    if data[:4] == IDENTITY_MAGIC:
        return data
    with tempfile.TemporaryDirectory() as work:
        out = Path(work) / 'identity.bin'
        done = subprocess.run([sys.executable, str(ROOT / 'arch/arm64/boot/mkidentity.py'), '--blob', str(out), str(path)],
                              capture_output=True, text=True)
        if done.returncode != 0:
            die(f'{path}: no identity made of it: {(done.stderr or done.stdout).strip()}')
        return out.read_bytes()


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def loader_identity(loader):
    """What a loader was built with: 'placeholder', or 'apple' (identity.h made from an IPSW)."""
    return 'placeholder' if PLACEHOLDER in loader else 'apple'


# ---- build ----------------------------------------------------------------------------------------------------------

README = """# {name}: one image

{description}

Built from {code} with the device package {package}, {date}.

| File | What it is |
| --- | --- |
| `{name}.payload` | The loader, the kernel and the boot arguments in one XNUP container, as the phone's UEFI starts them. |
{boot_row}| `manifest.json` | What went in, with each file's SHA-256: `tools/image.py show` checks an image against it. |

Boot arguments ({defconfig}):

```
{cmdline}
```

## Booting it

Nothing is written to the phone: it runs from RAM, and a reboot brings Android back.

{boot_how}

Darwin needs to be told which iPhone it runs on, from your own IPSW: {identity_how}
The system image (`./bench ramdisk`) is never part of an image.
"""


def build(options):
    if getattr(options, 'every_run', False):
        if options.defconfig:
            die('--every-run builds one image for each phone_ defconfig: not with --defconfig')
        name, _ = board_of(options.board)
        top = Path(options.out or ROOT / 'build/images' / name)
        runs = sorted(path.name.removesuffix('_defconfig') for path in defconfig.CONFIGS.glob('phone_*_defconfig'))
        for run in runs:
            build(argparse.Namespace(**dict(vars(options), every_run=False, defconfig=run, out=str(top / run))))
        say(f'{top}: {len(runs)} images, one a run: {", ".join(runs)}')
        return 0
    name, board = board_of(options.board)
    kernel_path = Path(options.kernel or os.environ.get('BENCH_KERNEL') or
                       Path(os.environ.get('XNU_SRC') or ROOT.parent / 'xnu-12377-hakios') / 'BUILD/obj/mach.development.bcm2837')
    loader_path = Path(options.loader or ROOT / 'arch/arm64/boot/loader.efi')
    for what, path, how in (('kernel', kernel_path, 'kernel/build.sh (or make kernel)'),
                            ('loader', loader_path, 'arch/arm64/boot/build.sh')):
        if not path.is_file():
            die(f'no {what} {path}: {how}')
    kernel, loader = kernel_path.read_bytes(), loader_path.read_bytes()
    if kernel[:4] != b'\xcf\xfa\xed\xfe':
        die(f'{kernel_path} is not a 64-bit Mach-O kernel')
    if loader[:2] != b'MZ':
        die(f'{loader_path} is not a UEFI application')
    preset = options.defconfig or DEFAULT_DEFCONFIG
    cmdline = boot_arguments(board, preset)
    preset = Path(preset).name.removesuffix('_defconfig')     # (a file of one's own: its name, not where it is)
    entries = [('loader.efi', loader), ('kernel', kernel), ('cmdline', cmdline.encode() + b'\0')]
    identity = None
    if options.identity:
        identity = identity_blob(options.identity)
        entries.append(('identity', identity))
    payload = payload_pack(entries)

    out = Path(options.out or ROOT / 'build/images' / name)
    out.mkdir(parents=True, exist_ok=True)
    for stale in ('boot.img', f'{name}.payload', 'manifest.json', 'README.md'):
        (out / stale).unlink(missing_ok=True)
    files = {f'{name}.payload': payload}
    uefi = Path(options.uefi) if options.uefi else ROOT / 'firmware' / name / 'uefi'
    boot = None
    if 'uefi' in board and all((uefi / f).is_file() for f in UEFI_FILES):
        import mkboot
        image, shim, dtb = ((uefi / f).read_bytes() for f in UEFI_FILES)
        boot = mkboot.pack(shim, image, dtb, payload, board['uefi'])
        files['boot.img'] = boot
    for file, data in files.items():
        (out / file).write_bytes(data)

    package_note = boards.DEVICES / name / '.from'
    package = package_note.read_text().strip() if package_note.is_file() else f'arch/arm64/boot/dts/{name}'
    commit = git_says('rev-parse', 'HEAD')
    dirty = bool(git_says('status', '--porcelain', '--untracked-files=no'))
    stamp = git_says('log', '-1', '--format=%cI') or datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')
    manifest = {
        'board': name, 'soc': board.get('soc', ''), 'description': board.get('description', ''),
        'code': {'commit': commit, 'describe': git_says('describe', '--always', '--dirty'), 'changed': dirty},
        'device_package': package, 'date': stamp, 'defconfig': preset, 'cmdline': cmdline,
        'kernel': {'sha256': sha256(kernel), 'size': len(kernel)},
        'loader': {'sha256': sha256(loader), 'size': len(loader), 'identity': loader_identity(loader)},
        'identity_in_payload': identity is not None,
        'files': {file: {'sha256': sha256(data), 'size': len(data)} for file, data in sorted(files.items())},
    }
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    boot_how = ('```sh\nfastboot boot boot.img\n```\n\nfrom the phone\'s own bootloader (`adb reboot bootloader`).' if boot else
                f'This image has no `boot.img`: the phone\'s UEFI build was not there (firmware/{name}/uefi, or --uefi). With it, '
                f'`arch/arm64/boot/mkboot.py --board {name} -p {name}.payload -o boot.img` makes one; '
                f'or `./bench` sends the loader and the kernel over USB.')
    identity_how = ('this one carries an identity already: it is for its builder\'s phone, not for passing around.'
                    if identity is not None else
                    '`tools/image.py personalize {} --identity DeviceTree.bin` adds it.'.format('boot.img' if boot else f'{name}.payload'))
    (out / 'README.md').write_text(README.format(
        name=name, description=board.get('description', ''), code=manifest['code']['describe'] or 'this checkout',
        package=package, date=stamp, defconfig=preset, cmdline=cmdline, boot_how=boot_how, identity_how=identity_how,
        boot_row='| `boot.img` | The phone\'s UEFI with that payload: `fastboot boot boot.img`. |\n' if boot else ''))
    say(f'{out}: {", ".join(sorted(files))} for {name} ({preset})')
    if manifest['loader']['identity'] == 'apple' or identity is not None:
        say('it carries Apple\'s data (an identity from an IPSW): for your own phone; publish takes only images without')
    if dirty:
        say('the checkout has changes that are not committed: the manifest says so')
    return 0


# ---- publish --------------------------------------------------------------------------------------------------------

def check_against_manifest(directory):
    """manifest.json of an image directory, after every file it lists has been found to be what it says."""
    path = Path(directory) / 'manifest.json'
    if not path.is_file():
        die(f'{directory}: no manifest.json (tools/image.py build makes one)')
    manifest = json.loads(path.read_text())
    for file, said in manifest.get('files', {}).items():
        data = (Path(directory) / file).read_bytes() if (Path(directory) / file).is_file() else None
        if data is None or sha256(data) != said['sha256']:
            die(f'{directory}/{file} is not what manifest.json says it is')
    return manifest


def publishable(source, manifest):
    """What publish refuses in the image at SOURCE, as a sentence; None when nothing."""
    if manifest['loader']['identity'] != 'placeholder':
        return ('the loader was built with an identity from an IPSW (Apple\'s data): build the loader without one '
                '(arch/arm64/boot/build.sh with no DeviceTree.bin) for an image that is published')
    names = [entry for entry, _ in payload_entries((source / f'{manifest["board"]}.payload').read_bytes())]
    if 'identity' in names or manifest.get('identity_in_payload'):
        return 'the payload carries an identity (Apple\'s data): build it without --identity for an image that is published'
    if 'ramdisk' in names:
        return 'the payload carries a system image: never part of an image that is published'
    return None


def publish(options):
    source = Path(options.image)
    manifest = check_against_manifest(source)
    name = manifest['board']
    repository = Path(options.repository)
    if not repository.is_dir():
        die(f'{repository} is not a directory (a checkout of the phone\'s repository)')
    refused = publishable(source, manifest)
    if refused:
        die(refused)
    files = [f'{name}.payload', 'manifest.json', 'README.md']
    if (source / 'boot.img').is_file():
        if options.with_uefi:
            files.append('boot.img')
        else:
            say('boot.img left out: it carries the phone\'s UEFI and its vendor\'s drivers (--with-uefi to publish it)')
    version = options.version or '{}-{}'.format(manifest['date'][:10], (manifest['code']['commit'] or 'unknown')[:12])
    target = repository / 'images' / version
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    for file in files:
        shutil.copy2(source / file, target / file)
    index = repository / 'images' / 'README.md'
    rows = sorted((p for p in (repository / 'images').iterdir() if (p / 'manifest.json').is_file()), reverse=True)
    lines = ['# Images', '', 'Built with `tools/image.py build` and put here with `tools/image.py publish`; the newest first.',
             'Each directory\'s README.md says how to boot it; `tools/image.py show DIR/FILE` checks it against its manifest.', '',
             '| Version | Board | Code | Boot arguments |', '| --- | --- | --- | --- |']
    for row in rows:
        m = json.loads((row / 'manifest.json').read_text())
        lines.append(f'| [{row.name}]({row.name}/README.md) | {m["board"]} | {m["code"]["describe"] or "?"} | {m["defconfig"]} |')
    index.write_text('\n'.join(lines) + '\n')
    say(f'{target}: {", ".join(files)}; {index} lists it. In {repository}: git add images && git commit')
    return 0


# ---- personalize ----------------------------------------------------------------------------------------------------

def personalize(options):
    path = Path(options.image)
    data = path.read_bytes()
    identity = identity_blob(options.identity)
    is_boot = data[:8] == b'ANDROID!'
    payload = boot_image_payload(data)[1] if is_boot else data
    entries = [(n, d) for n, d in payload_entries(payload) if n != 'identity'] + [('identity', identity)]
    payload = payload_pack(entries)
    out = Path(options.output or path.with_name(path.stem + '-mine' + path.suffix))
    out.write_bytes(boot_image_with(data, payload) if is_boot else payload)
    say(f'{out}: {path.name} with the identity of {Path(options.identity).name} ({len(identity)} bytes): '
        f'Apple\'s data, for your own phone')
    return 0


# ---- show -----------------------------------------------------------------------------------------------------------

def show(options):
    path = Path(options.file)
    data = path.read_bytes()
    payload = boot_image_payload(data)[1] if data[:8] == b'ANDROID!' else data
    print(f'{path}: {"a boot image with a payload" if payload is not data else "a payload"}, {len(data)} bytes')
    for name, entry in payload_entries(payload):
        what = {'loader.efi': loader_identity(entry) + ' identity built in' if name == 'loader.efi' else '',
                'cmdline': entry.rstrip(b'\0').decode(errors='replace'),
                'identity': 'Apple\'s data: for its builder\'s own phone'}.get(name, '')
        print(f'  {name:12} {len(entry):10} bytes  {sha256(entry)[:16]}  {what}')
    manifest = path.parent / 'manifest.json'
    if manifest.is_file():
        said = json.loads(manifest.read_text()).get('files', {}).get(path.name)
        if said is not None:
            print(f'  manifest.json: {"the same" if said["sha256"] == sha256(data) else "NOT THE SAME FILE"}')
    return 0


def verify(options):
    root = Path(options.directory)
    found = sorted(path.parent for path in root.rglob('manifest.json'))
    if not found:
        die(f'{root}: no image (a directory with a manifest.json) under it')
    bad = 0
    for directory in found:
        try:
            manifest = check_against_manifest(directory)
            refused = publishable(directory, manifest)
        except SystemExit as stop:
            refused = str(stop).removeprefix('image.py: ')
        print(f'{directory}: ' + (f'NOT AS PUBLISHED: {refused}' if refused else
                                  f'{manifest["board"]}, {manifest["code"]["describe"] or "?"}, {manifest["defconfig"]}: as its manifest says'))
        bad += refused is not None
    return 1 if bad else 0


def extract(options):
    data = Path(options.file).read_bytes()
    payload = boot_image_payload(data)[1] if data[:8] == b'ANDROID!' else data
    entries = dict(payload_entries(payload))
    if options.payload:
        Path(options.payload).write_bytes(payload)
    if options.loader:
        if 'loader.efi' not in entries:
            die(f'{options.file}: its payload has no loader')
        Path(options.loader).write_bytes(entries['loader.efi'])
    say(f'{options.file}: payload {len(payload)} bytes' + (f', loader {len(entries.get("loader.efi", b""))} bytes' if options.loader else ''))
    return 0


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    command = commands.add_parser('build', help='the image of a phone, into build/images/NAME/')
    command.add_argument('--board', default=os.environ.get('BENCH_BOARD') or os.environ.get('HAKIOS_BOARD'))
    command.add_argument('--defconfig', help=f'the boot arguments of arch/arm64/configs/NAME_defconfig ({DEFAULT_DEFCONFIG})')
    command.add_argument('--kernel', help='the kernel (kernel/build.sh\'s)')
    command.add_argument('--loader', help='the loader (arch/arm64/boot/loader.efi)')
    command.add_argument('--uefi', help='the phone\'s UEFI build (firmware/NAME/uefi)')
    command.add_argument('--identity', help='an identity to carry: Apple\'s data, for your own phone')
    command.add_argument('--out', help='where (build/images/NAME)')
    command.add_argument('--every-run', action='store_true', help='one image for each phone_ defconfig, in OUT/DEFCONFIG/')
    command = commands.add_parser('publish', help='an image into a phone\'s repository')
    command.add_argument('image', metavar='IMAGE_DIR')
    command.add_argument('repository', metavar='DEVICE_REPO')
    command.add_argument('--version')
    command.add_argument('--with-uefi', action='store_true')
    command = commands.add_parser('personalize', help='an image given the identity of your own IPSW')
    command.add_argument('image', metavar='IMAGE')
    command.add_argument('--identity', required=True)
    command.add_argument('-o', '--output')
    command = commands.add_parser('show', help='what an image holds')
    command.add_argument('file', metavar='FILE')
    command = commands.add_parser('verify', help='every image under a directory, as published')
    command.add_argument('directory', metavar='DIR')
    command = commands.add_parser('extract', help='its payload and its loader, as files')
    command.add_argument('file', metavar='FILE')
    command.add_argument('--payload')
    command.add_argument('--loader')
    options = parser.parse_args(arguments)
    return {'build': build, 'publish': publish, 'personalize': personalize, 'show': show,
            'verify': verify, 'extract': extract}[options.command](options)


if __name__ == '__main__':
    sys.exit(main())
