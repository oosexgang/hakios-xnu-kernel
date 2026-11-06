#!/usr/bin/env python3
# usage: tools/dsc/aks-selectors.py /System/Library/PrivateFrameworks/AppleKeyStore.framework/AppleKeyStore (needs capstone)
"""For every call to IOConnectCall*Method in an image of the cache: the function it is in, the selector (w1) and
the scalar input count (w3), from the nearest preceding mov in the same function."""
import bisect, sys
sys.path.insert(0, sys.argv[0].rsplit('/', 1)[0])
import capstone
import dsc

c = dsc.Cache()
iokit = dsc.Image(c, '/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit')
iox = iokit.export_list()
targets = {iox[n]: n for n in ('_IOConnectCallMethod', '_IOConnectCallStructMethod', '_IOConnectCallScalarMethod',
                              '_IOConnectCallAsyncMethod', '_IOConnectCallAsyncStructMethod',
                              '_IOConnectCallAsyncScalarMethod') if n in iox}
im = dsc.Image(c, sys.argv[1])
ex = im.export_list()
starts = im.function_starts()
syms = sorted([(n, a) for n, a in ex.items()] + [(f'sub_{a:x}', a) for a in starts if a not in set(ex.values())], key=lambda t: t[1])
addrs = [a for _, a in syms]
text, size = im.sects[('__TEXT', '__text')]
code = c.read(text, size)
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
md.skipdata = True


def resolve_stub(a):
    """follow a branch island / stub (adrp/add/br or adrp/ldr/br) one level"""
    if a in targets:
        return targets[a]
    try:
        ins = list(md.disasm(c.read(a, 16), a))
    except KeyError:
        return None
    if len(ins) >= 3 and ins[0].mnemonic == 'adrp' and ins[2].mnemonic in ('br', 'braa', 'braaz'):
        page = int(ins[0].op_str.split('#')[1], 16)
        off = int(ins[1].op_str.split('#')[-1].rstrip(']!'), 16)
        if ins[1].mnemonic == 'add':
            return targets.get(page + off)
        if ins[1].mnemonic == 'ldr':
            return targets.get(c.u64(page + off) & 0xfffffffff)
    if len(ins) >= 1 and ins[0].mnemonic == 'b':
        return targets.get(int(ins[0].op_str[1:], 16))
    return None


regs = {}
cache = {}
results = []
calls = {}
fn_start = None
for ins in md.disasm(code, text):
    i = bisect.bisect_right(addrs, ins.address) - 1
    f = syms[i][0] if i >= 0 else '?'
    if f != fn_start:
        regs = {}
        fn_start = f
    m, o = ins.mnemonic, ins.op_str
    if m == 'mov' and o.startswith(('w1,', 'x1,', 'w3,', 'x3,')) and '#' in o:
        regs[o[1]] = int(o.split('#')[1], 16)
    elif m == 'mov' and o.startswith(('w1,', 'x1,', 'w3,', 'x3,')):
        regs[o[1]] = o.split(', ')[1]
    elif m == 'b' and o.startswith('#'):
        calls.setdefault(int(o[1:], 16), []).append(f)
    elif m == 'bl':
        t = int(o[1:], 16)
        if t not in cache:
            cache[t] = resolve_stub(t)
        name = cache[t]
        calls.setdefault(t, []).append(f)
        if name:
            results.append((f, ins.address, name, regs.get('1'), regs.get('3')))
        regs = {}
for f, a, n, s, k in results:
    sel = f'{s:#x}' if isinstance(s, int) else str(s)
    print(f'{f:50s} {a:#x} {n[1:]:28s} sel={sel} scalars_in={k}')

print('--- callers of sub_ functions that call IOConnect')
names = dict((a, n) for n, a in syms)
for f in sorted(set(r[0] for r in results if r[0].startswith('sub_'))):
    a = int(f[4:], 16)
    print(f, '<-', ' '.join(sorted(set(calls.get(a, [])))))
