#!/usr/bin/env python3
"""ca-metallib.py OUTDIR [SYSIMAGE]: QuartzCore's default.metallib for CoreAnimation drawing through Metal.

This image is the iPhone X's, and its QuartzCore carries no default.metallib: only default.pipelinelib, whose
"functions" file holds the four uber shaders (fixed_vert_lph_gen/_spc, fixed_frag_lph_cpf/_cph) as 451 variants
already specialised for the pipelines pipelines.desc names. The window server's Metal renderer wants the library
itself, with the rest of CoreAnimation's functions (blurs, clears, paths, copies): those are taken from the air64
slice of THIS MAC's QuartzCore.framework/default.metallib (a later CoreAnimation, read where it lies and never put
in the repository), with one variant of each of the four uber shaders put in their place -- the plug-in compiles the
variant a pipeline's label names (userland/src/HakiosMetalCA.m, hm_bitcode_for()).

OUTDIR/QuartzCore-default.metallib is written; build the image with EXTRA_SHARE=OUTDIR and run with
ca_metallib=/redwood/share/<OUTDIR's name>/QuartzCore-default.metallib (userland/src/inject.c).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import merge_metallib  # noqa: E402

MAC_QC = '/System/Library/Frameworks/QuartzCore.framework/Versions/A/Resources/default.metallib'
AIR64 = 0x01000017


def air64_slice(path):
    with open(path, 'rb') as f:
        magic, n = struct.unpack('>II', f.read(8))
        assert magic == 0xcafebabe, '%s: not a universal file' % path
        for _ in range(n):
            cputype, _sub, off, size, _align = struct.unpack('>iiIII', f.read(20))
            if cputype == AIR64:
                f.seek(off)
                return f.read(size)
    raise SystemExit('%s: no air64 slice' % path)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    image = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), '..', 'sysimage', 'rootfs')
    functions = open(os.path.join(image, 'System/Library/Frameworks/QuartzCore.framework/default.pipelinelib/functions'),
                     'rb').read()
    os.makedirs(out, exist_ok=True)
    tmp = os.path.join(out, '.part.metallib')
    variants, at = {}, 0
    while (at := functions.find(b'MTLB', at)) >= 0:
        total = struct.unpack_from('<Q', functions, at + 0x10)[0]
        open(tmp, 'wb').write(functions[at:at + total])
        lib = merge_metallib.read(tmp)
        variants.setdefault(dict(lib['funcs'][0]['tags'])[b'NAME'], (lib['funcs'][0], lib['head']))
        at += total
    mac = os.path.join(out, '.air64.metallib')
    open(mac, 'wb').write(air64_slice(MAC_QC))
    lib = merge_metallib.read(mac)
    lib['funcs'] = [variants[n][0] if (n := dict(f['tags'])[b'NAME']) in variants else f for f in lib['funcs']]
    lib['head'] = next(iter(variants.values()))[1]
    merge_metallib.write([lib], os.path.join(out, 'QuartzCore-default.metallib'))
    os.remove(tmp)
    os.remove(mac)
    print('%s: %d functions, %d of them this image\'s uber shaders' %
          (os.path.join(out, 'QuartzCore-default.metallib'), len(lib['funcs']), len(variants)))


if __name__ == '__main__':
    main()
