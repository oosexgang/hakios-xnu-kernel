#!/usr/bin/env python3
"""What an application asks of Metal, and which of it this driver answers.

owes.py counts the supply: what our classes still owe the protocols Apple's
daemons send messages through. This counts the DEMAND, from the other end -- a
real application's binary -- because the two lists are not the same list. The
window server sends a few hundred selectors and a game sends a different few
hundred, and the port is finished for a given application when the second list
is covered, not when the first one is.

The reason this cannot be done with __objc_selrefs, which would be the obvious
way, is worth stating because it is the whole method:

  Resident Evil 4 sends 862 selectors through __objc_selrefs and only six of
  them belong to Metal. It reaches Metal the metal-cpp way instead -- classes
  by objc_lookUpClass, selectors by sel_registerName on a string literal,
  messages by objc_msgSend -- so a selector it sends is a C STRING in the
  binary and appears in no selector-reference section at all. Both halves of
  that are checked here: --imports prints the runtime functions that make it
  possible, and the demand list is built by scanning strings.

So the method is: take every selector Apple's own Metal images DECLARE (the
protocols and classes of Metal.framework, the GPU bundle, IOGPU and QuartzCore,
read out of the device's own shared cache with `ipsw dyld macho --objc`), keep
those that appear as a whole string in the application, and ask our source
whether we implement each one. A string that happens to equal a selector is
weak evidence on its own -- "size", "texture", "arguments" are ordinary English
-- so the output separates the selectors that only Metal could have (those with
a colon, and those declared by a Metal protocol and nothing else) from the bare
words, and counts them apart.

usage:
  C=ios165/20F66__iPhone10,3_6/dyld_shared_cache_arm64
  for i in /System/Library/Frameworks/Metal.framework/Metal \
           /System/Library/Extensions/AGXMetalA11.bundle/AGXMetalA11 \
           /System/Library/PrivateFrameworks/IOGPU.framework/IOGPU \
           /System/Library/Frameworks/QuartzCore.framework/QuartzCore; do
          ipsw dyld macho $C $i --objc > $(basename $i).objc.txt
  done
  appasks.py APP_BINARY --decl Metal.objc.txt [--decl ...] [--src userland/src/redwoodmetal.m]
             [--imports] [--missing] [--protocol MTLDevice]
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# A selector of one word, no colon, that is also an ordinary English word or a
# common Foundation accessor: its presence in a binary says nothing by itself,
# because any library could have put that string there. Counted, never claimed.
BARE_WORDS = {
    'size', 'texture', 'arguments', 'label', 'name', 'device', 'type', 'width',
    'height', 'depth', 'length', 'offset', 'index', 'count', 'status', 'error',
    'format', 'options', 'contents', 'data', 'buffer', 'present', 'commit',
    'reset', 'retain', 'release', 'copy', 'description', 'hash', 'class',
}


def declared(paths):
    """selector -> set of (kind, owner), kind being 'protocol' or 'class'.

    The distinction decides whether a selector is ours to answer at all. A
    selector a PROTOCOL declares is one our object must answer, because our
    object stands where Apple's would. A selector only a CLASS declares --
    MTLTextureDescriptorInternal, AGXA11FamilyBlitContext -- is answered by
    Apple's own object inside Metal.framework, which runs unchanged on this
    phone; counting those as work would inflate the number by hundreds.
    """
    out = {}
    meth = re.compile(r'^\s*[-+]\[(\S+)\s+([^\]]+)\];')
    head = re.compile(r'^@(protocol|interface)\s+([A-Za-z_][A-Za-z0-9_]*)')
    for p in paths:
        kind = {}
        with open(p, encoding='utf-8', errors='replace') as f:
            for line in f:
                h = head.match(line)
                if h:
                    kind[h.group(2)] = h.group(1)
                    continue
                m = meth.match(line)
                if m:
                    owner = m.group(1).strip()
                    k = kind.get(owner, 'class' if owner not in kind else kind[owner])
                    out.setdefault(m.group(2).strip(), set()).add((k, owner))
    return out


def strings(path, least=3):
    """Every NUL-terminated printable string in the file, as a set."""
    out = set()
    with open(path, 'rb') as f:
        blob = f.read()
    for m in re.finditer(rb'[\x20-\x7e]{%d,}' % least, blob):
        out.add(m.group().decode('ascii'))
    return out


def implemented(src):
    """Selectors our driver's source defines, by stitching each method's parts.

    A declaration is read across lines, because 442 of this driver's 462 of them
    continue onto the next one -- a one-line match finds 18 and reads as if the
    driver implemented almost nothing.
    """
    out = set()
    pending = None
    with open(src, encoding='utf-8', errors='replace') as f:
        for line in f:
            t = line.strip()
            if pending is None:
                if not re.match(r'^[-+]\s*\(', t):
                    continue
                pending = t
            elif re.match(r'^[-+]\s*\(', t):
                pending = t                              # the one before it was not a declaration
            else:
                pending += ' ' + t
            # A one-line method ends with '}', not with '{' or ';', so terminate on the
            # first brace or semicolon ANYWHERE in what has been stitched. Ending on
            # endswith() instead leaves the declaration open, and it then swallows the
            # next ones and the comments between them into one selector:
            # 'addPurgedResource:addPurgedHeap:setPurgeableState:' was three methods.
            if '{' not in pending and ';' not in pending:
                if len(pending) > 2000:                  # not a declaration after all
                    pending = None
                continue
            m = re.match(r'^[-+]\s*\(([^;{]*?)\)\s*([^{;]+)', pending)
            pending = None
            if not m:
                continue
            body = m.group(2).strip()               # ' commit ' is not an identifier
            if ':' not in body:
                if re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', body):
                    out.add(body)
                continue
            # keep only the keywords that introduce an argument: a name immediately
            # before a colon, skipping the types and names inside the parentheses
            body = re.sub(r'\([^()]*(?:\([^()]*\)[^()]*)*\)', ' ', body)
            sel = ''.join(k + ':' for k in re.findall(r'([A-Za-z_][A-Za-z0-9_]*)\s*:', body))
            if sel:
                out.add(sel)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('binary')
    ap.add_argument('--decl', action='append', default=[], required=True)
    ap.add_argument('--src', default=os.path.join(ROOT, 'userland/src/redwoodmetal.m'))
    ap.add_argument('--imports', action='store_true', help='also how it reaches the runtime')
    ap.add_argument('--missing', action='store_true', help='list what we do not implement')
    ap.add_argument('--ours', action='store_true',
                    help='only selectors a PROTOCOL declares: the ones our objects must answer')
    ap.add_argument('--protocol', help='only selectors this protocol or class declares')
    a = ap.parse_args()

    decl = declared(a.decl)
    if not decl:
        sys.exit('appasks: no methods in the declaration dumps')
    have = implemented(a.src) if os.path.exists(a.src) else set()
    present = strings(a.binary)

    asked = {s: o for s, o in decl.items() if s in present}
    if a.protocol:
        asked = {s: o for s, o in asked.items() if any(n == a.protocol for _, n in o)}
    if a.ours:
        asked = {s: o for s, o in asked.items() if any(k == 'protocol' for k, _ in o)}

    # A selector with a colon is one: no ordinary string has that shape. A colonless
    # one is a word, and a 282 MB binary full of C++ names, asset paths and shader
    # names contains a great many words -- so those are an UPPER BOUND on the demand
    # and are never counted as demand. Keeping them apart is the only thing that makes
    # the first number worth quoting.
    strong = {s: o for s, o in asked.items() if ':' in s}
    words = {s: o for s, o in asked.items() if ':' not in s}
    plain = {s for s in words if s in BARE_WORDS}

    print('%s' % os.path.basename(a.binary))
    print('  %d selectors are declared by Apple\'s Metal images; %d appear in this binary.'
          % (len(decl), len(asked)))
    print('  %d of those have a colon, so they are selectors and nothing else: THE DEMAND.'
          % len(strong))
    print('  %d are a single word -- an upper bound, not demand (%d of them are ordinary'
          % (len(words), len(plain)))
    print('  English like "size" or "texture", which any library could have put there).')
    print()

    answered = {s for s in strong if s in have}
    print('  of the %d: %d answered in %s, %d NOT' %
          (len(strong), len(answered), os.path.basename(a.src), len(strong) - len(answered)))
    print('  (%d selectors are defined in that source; what Apple\'s base classes below us'
          % len(have))
    print('  answer is owes.py\'s question, not this one, so a NOT here is not yet an abort.)')
    print()
    single = {}

    if a.missing:
        print('--- asked for and not implemented, by the protocol or class that declares it ---')
        by = {}
        for s, owners in list(strong.items()) + list(single.items()):
            if s in have:
                continue
            for kind, owner in owners:
                by.setdefault('%s %s' % (kind, owner), []).append(s)
        for owner in sorted(by, key=lambda o: (-len(by[o]), o)):
            print('%s (%d):' % (owner, len(by[owner])))
            for s in sorted(by[owner]):
                print('    ' + s)

    if a.imports:
        print('--- how it reaches the Objective-C runtime at all ---')
        # the import table spells these with a leading underscore
        for s in sorted(present):
            if s.lstrip('_') in ('sel_registerName', 'objc_lookUpClass', 'objc_getClass',
                                 'objc_msgSend', 'dlopen', 'dlsym',
                                 'MTLCreateSystemDefaultDevice', 'objc_getProtocol'):
                print('    ' + s)


main()
