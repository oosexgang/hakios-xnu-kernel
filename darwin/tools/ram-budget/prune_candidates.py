#!/usr/bin/env python3
import pickle, collections, re
import cachedir
big = pickle.load(open(cachedir.path('rootfs26.pkl'), 'rb'))   # (z bytes, raw, rel), from rootfs_select.py
M = 1 << 20
apps = collections.Counter(); appsr = collections.Counter()
for b, s, rel in big:
    if rel.startswith('Applications/'):
        apps[rel.split('/')[1]] += b; appsr[rel.split('/')[1]] += s
print('applications by z9 MiB (raw):')
for a, b in apps.most_common(45):
    print(f'  {b/M:6.1f} ({appsr[a]/M:6.1f})  {a}')
print('apps total', round(sum(apps.values()) / M, 1), 'count', len(apps))

groups = {
    'poster apps (*Poster*.app)': lambda r: re.match(r'Applications/[^/]*Poster[^/]*\.app/', r),
    'System/Library/ExtensionKit': lambda r: r.startswith('System/Library/ExtensionKit/'),
    'System/Library/Health': lambda r: r.startswith('System/Library/Health/'),
    'Fonts: AppFonts': lambda r: r.startswith('System/Library/Fonts/AppFonts/'),
    'emoji font (untrimmed)': lambda r: r.endswith('AppleColorEmoji-160px.ttc'),
    'SFSymbols.framework': lambda r: r.startswith('System/Library/PrivateFrameworks/SFSymbols.framework/'),
    '*.car asset catalogs (all)': lambda r: r.endswith('.car'),
    '*.loctable (all)': lambda r: r.endswith('.loctable'),
    'Mach-O-ish executables in Applications (no extension)': lambda r: r.startswith('Applications/') and '.' not in r.rsplit('/', 1)[1],
    'usr/libexec+usr/sbin': lambda r: r.startswith(('usr/libexec/', 'usr/sbin/')),
    '*.metallib anywhere chosen': lambda r: r.endswith('.metallib'),
    '*.bin/*.weight* anywhere chosen': lambda r: r.endswith(('.bin', '.weights', '.weight.bin')),
    'System/Library/DriverExtensions': lambda r: r.startswith('System/Library/DriverExtensions/'),
    'System/Library/CoreServices/*.app except SpringBoard': lambda r: r.startswith('System/Library/CoreServices/') and '.app/' in r and 'SpringBoard.app/' not in r,
    'PreferenceBundles': lambda r: r.startswith('System/Library/PreferenceBundles/'),
    'Library/Audio/Tunings': lambda r: r.startswith('Library/Audio/Tunings/'),
}
print('\ncandidate groups (z9 MiB / raw MiB):')
for name, f in groups.items():
    b = sum(x[0] for x in big if f(x[2])); s = sum(x[1] for x in big if f(x[2]))
    print(f'  {b/M:7.1f} {s/M:7.1f}  {name}')
dirs = collections.Counter()
for b, s, rel in big:
    if rel.startswith('System/Library/') and not rel.startswith(('System/Library/PrivateFrameworks', 'System/Library/Frameworks', 'System/Library/Fonts')):
        dirs['/'.join(rel.split('/')[:3])] += b
print('\nSystem/Library top-level dirs (non-framework):')
for d, b in dirs.most_common(25):
    print(f'  {b/M:7.1f}  {d}')
fw = collections.Counter()
for b, s, rel in big:
    if rel.startswith(('System/Library/PrivateFrameworks/', 'System/Library/Frameworks/')):
        fw['/'.join(rel.split('/')[:4])] += b
print('\nbiggest frameworks dirs:')
for d, b in fw.most_common(20):
    print(f'  {b/M:7.1f}  {d}')
