#!/usr/bin/env python3
"""
qc-metallib.py MACOS_AIR.metallib OUT.metallib QUARTZCORE_STRINGS.txt

QuartzCore's shaders in AIR for CoreAnimation's Metal renderer on the phone (boot-arg camtl=1,
userland/build.sh QC_METALLIB). The iPhone X firmware has none as a library (only
default.pipelinelib), so this takes the AIR slice of a macOS QuartzCore's default.metallib (the
first slice of the fat file, cputype 0x1000017) and makes iOS 16.5's Metal take it:

  - the header says iOS 16.5 and every function AIR 2.5 (Metal refuses a macOS library);
  - the functions iOS 16.5 asks for and macOS has not got (p3_clamp, harmony, luma_boost,
    display_overdrive_*, path_stroke_*, alpha_linear_premultiply) are ALIASES: a second
    function-list entry over another function's module (src/redwoodmetal.m passes no entry
    name, so the compiler takes the module's one entry point);
  - the modules of functions QuartzCore 16.5 never names are zeroed, which is what makes the
    file fit boot_a (QUARTZCORE_STRINGS: QuartzCore's __cstring, one string a line).

Apple's file, so not kept in the repository.
"""
import sys,struct,re
src,dst,cstr=sys.argv[1:4]
b=bytearray(open(src,'rb').read())
fo,fl=struct.unpack_from('<QQ',b,0x18)
cnt=struct.unpack_from('<I',b,fo)[0]
entries=[]; q=fo+4
for _ in range(cnt):
    size=struct.unpack_from('<I',b,q)[0]; entries.append(bytearray(b[q:q+size])); q+=size
def tags(e):
    r=4; out={}
    while r<len(e):
        t=bytes(e[r:r+4])
        if t==b'ENDT': break
        ln=struct.unpack_from('<H',e,r+4)[0]; out[t]=(r+6,ln); r+=6+ln
    return out
def name(e):
    o,l=tags(e)[b'NAME']; return bytes(e[o:o+l]).rstrip(b'\0').decode()
byname={name(e):e for e in entries}
ALIAS={'p3_clamp':'edr_scale','display_overdrive_lut_gen':'attachment_clear_frag','display_overdrive_linearize':'edr_scale',
 'path_stroke_line_frag':'path_stroke_curve_frag','path_stroke_round_join_frag':'path_stroke_curve_frag','path_stroke_miter_join_frag':'path_stroke_curve_frag',
 'path_stroke_line_vert':'path_stroke_curve_vert','path_stroke_curve_vert_0':'path_stroke_curve_vert','path_stroke_curve_vert_1':'path_stroke_curve_vert',
 'path_stroke_miter_join_vert':'path_stroke_curve_vert','path_stroke_round_join_vert':'path_stroke_curve_vert','alpha_linear_premultiply':'alpha_threshold','harmony':'edr_scale','display_overdrive_srgb_sampler':'edr_scale','luma_boost':'edr_scale'}
new=[]; donors=set()
for want,donor in ALIAS.items():
    for suf in ('','_lph','_lpf'):
        if want+suf in byname: continue
        if donor+suf not in byname: continue
        d=byname[donor+suf]; donors.add(donor+suf)
        o,l=tags(d)[b'NAME']; nm=(want+suf).encode()+b'\0'
        e=bytearray(d[:o-2])+struct.pack('<H',len(nm))+nm+d[o+l:]
        struct.pack_into('<I',e,0,len(e)); new.append(e); print('alias',want+suf,'->',donor+suf)
# iOS header and versions
for e in entries+new:
    t=tags(e)
    if b'VERS' in t: o,l=t[b'VERS']; e[o:o+8]=bytes.fromhex('0200050003000000')
qc=set(l.strip() for l in open(cstr,encoding='latin-1'))
bo=struct.unpack_from('<Q',b,0x48)[0]
for e in entries:
    n=name(e); base=re.sub(r'_(lpf|lph)$','',n)
    if n in qc or base in qc or n in donors: continue
    t=tags(e); off=struct.unpack_from('<QQQ',e,t[b'OFFT'][0])[2]; sz=struct.unpack_from('<Q',e,t[b'MDSZ'][0])[0]
    b[bo+off:bo+off+sz]=bytes(sz)
table=struct.pack('<I',cnt+len(new))+b''.join(bytes(e) for e in entries+new)
delta=len(table)-fl
out=bytearray(b[:fo])+table+b[fo+fl:]
out[0:16]=bytes.fromhex('4d544c42010002000700008210000500')
struct.pack_into('<Q',out,0x20,len(table))
for f in (0x28,0x38,0x48):
    struct.pack_into('<Q',out,f,struct.unpack_from('<Q',out,f)[0]+delta)
struct.pack_into('<Q',out,0x10,len(out))
# extended header: HDYN / RLST hold absolute offsets
r=fo+len(table)
assert out[r:r+4]==b'ENDT'; r+=4
while out[r:r+4]!=b'ENDT':
    t=bytes(out[r:r+4]); ln=struct.unpack_from('<H',out,r+4)[0]
    if t in (b'HDYN',b'RLST'):
        v=struct.unpack_from('<Q',out,r+6)[0]; struct.pack_into('<Q',out,r+6,v+delta); print('shift',t,v,'->',v+delta)
    r+=6+ln
open(dst,'wb').write(out); print(len(new),'aliases, delta',delta)
