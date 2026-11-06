#!/usr/bin/env python3
"""
rwm-recipes.py LOG... : the pipeline recipes processes said into a run's log ("rwr PROCESS N K/M text",
src/redwoodmetal.m, rw_recipes_say, on notification com.redwood.rwm_recipes) put back together and
merged into userland/share/rwm-recipes.txt -- a "[PROCESS]" section each, a recipe a line: the label and
the shape, written as a table ("=K words" once, then "LABEL @K": most pipelines share a shape, and the
boot partition has kilobytes, not tens of them, to spare). Recipes already in the file are kept.
"""
import os, re, sys

OUT = os.path.join(os.path.dirname(__file__), '..', 'userland', 'share', 'rwm-recipes.txt')
line_re = re.compile(r'rwr (\S+) (\d+) (\d+)/(\d+) (.*)$')

known = {}          # process -> [recipe, ...]
if os.path.exists(OUT):
    table, proc = {}, 'backboardd'
    for l in open(OUT):
        l = l.rstrip('\n')
        if not l or l.startswith('#'):
            continue
        if l.startswith('['):
            proc = l[1:].split(']')[0]
        elif l.startswith('='):
            k, words = l[1:].split(' ', 1)
            table[k] = words
        else:
            label, ref = l.split(' ', 1)
            if ref.startswith('@'):
                k, _, rest = ref[1:].partition(' ')
                known.setdefault(proc, []).append(f'{label} {table[k]}' + (f' {rest}' if rest else ''))
            else:
                known.setdefault(proc, []).append(l)
added = 0
for log in sys.argv[1:]:
    pieces = {}
    for raw in open(log, 'rb'):
        m = line_re.search(raw.decode('utf-8', 'replace').rstrip('\r\n'))
        if m:
            pieces.setdefault((m.group(1), int(m.group(2))), {})[int(m.group(3))] = (int(m.group(4)), m.group(5))
    for (proc, n), parts in sorted(pieces.items()):
        total = next(iter(parts.values()))[0]
        if sorted(parts) != list(range(1, total + 1)):
            print(f'{log}: {proc} recipe {n}: {len(parts)} of {total} pieces, left out', file=sys.stderr)
            continue
        recipe = ''.join(parts[k][1] for k in range(1, total + 1))
        if ' ' not in recipe or not recipe.startswith('com.apple.coreanimation.draw.'):
            continue
        if recipe not in known.setdefault(proc, []):
            known[proc].append(recipe)
            added += 1
shapes = []
def split(r):
    """label, shape words, and the functions (" |V|F|vc|fc") of one not in the pipeline library"""
    label, rest = r.split(' ', 1)
    words, _, funcs = rest.partition(' ')
    return label, words, funcs

for recipes in known.values():
    for r in recipes:
        words = split(r)[1]
        if words not in shapes:
            shapes.append(words)
if len(shapes) > 32:
    sys.exit(f'{len(shapes)} shapes: the driver reads 32 at most')
os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, 'w') as f:
    f.write('# CoreAnimation pipelines each process builds at start (tools/rwm-recipes.py, src/redwoodmetal.m)\n')
    for k, words in enumerate(shapes):
        f.write(f'={k} {words}\n')
    for proc, recipes in known.items():
        f.write(f'[{proc}]\n')
        for r in recipes:
            label, words, funcs = split(r)
            f.write(f'{label} @{shapes.index(words)}' + (f' {funcs}' if funcs else '') + '\n')
print(f'{OUT}: ' + ', '.join(f'{p} {len(r)}' for p, r in known.items()) + f' ({added} new)')
