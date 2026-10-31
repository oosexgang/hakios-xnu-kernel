#!/usr/bin/env python3
"""Put packages of the repository into the prefix as installed, newest version first.

usage: jb-preinstall.py PREFIX PACKAGES-INDEX DEBS-DIR NAME...
  PREFIX          the prefix tree, the directory that holds var/jb (jb/root of tools/mk-jb.sh)
  PACKAGES-INDEX  the repository's Packages file, already fetched
  DEBS-DIR        where the .deb files are kept (they are fetched into it when missing)
  NAME...         the packages to put in

The bootstrap is a snapshot, and some of what it carries is older than the repository (debianutils,
launchctl). apt on the phone would upgrade them, but only over the network and only after a boot,
and what is in the image is what a first boot has; so the newest version of each is unpacked into
the prefix here and written into dpkg's database, replacing an older entry if there is one.

Nothing of the package's own scripts is run: these are plain file packages, and /var/jb/prep.sh does
the little that the bootstrap's postinst scripts are needed for at every boot.
"""
import os
import re
import lzma
import subprocess
import sys
import tarfile


def version_parts(version):
    """A Debian version as (epoch, upstream, revision), each ready for compare_part."""
    epoch, _, rest = version.partition(':') if ':' in version else ('0', '', version)
    if not rest:
        epoch, rest = '0', version
    upstream, _, revision = rest.rpartition('-')
    if not upstream:
        upstream, revision = rest, ''
    return int(epoch or 0), upstream, revision


def compare_part(a, b):
    """Debian's own order for one part: digit runs numerically, and "~" before everything else."""
    def order(c):
        if c == '~':
            return -1
        if c.isalpha():
            return ord(c)
        return ord(c) + 256

    i = j = 0
    while i < len(a) or j < len(b):
        # the stretch that is not digits, character by character
        while (i < len(a) and not a[i].isdigit()) or (j < len(b) and not b[j].isdigit()):
            x = order(a[i]) if i < len(a) and not a[i].isdigit() else 0
            y = order(b[j]) if j < len(b) and not b[j].isdigit() else 0
            if x != y:
                return -1 if x < y else 1
            if i < len(a) and not a[i].isdigit():
                i += 1
            if j < len(b) and not b[j].isdigit():
                j += 1
        # and the stretch that is, as a number
        first, second = i, j
        while i < len(a) and a[i].isdigit():
            i += 1
        while j < len(b) and b[j].isdigit():
            j += 1
        x = int(a[first:i] or 0)
        y = int(b[second:j] or 0)
        if x != y:
            return -1 if x < y else 1
    return 0


def newer(a, b):
    """Is version a newer than version b?"""
    ea, ua, ra = version_parts(a)
    eb, ub, rb = version_parts(b)
    if ea != eb:
        return ea > eb
    for x, y in ((ua, ub), (ra, rb)):
        order = compare_part(x, y)
        if order:
            return order > 0
    return False


def stanzas(text):
    for block in text.split('\n\n'):
        if block.strip():
            yield block.strip('\n')


def field(stanza, name):
    m = re.search(r'(?m)^%s: (.*)$' % name, stanza)
    return m.group(1).strip() if m else None


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    prefix, index_path, debs, names = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
    jb = os.path.join(prefix, 'var/jb')
    status_path = os.path.join(jb, 'Library/dpkg/status')
    index = open(index_path, encoding='utf-8', errors='replace').read()

    best = {}
    for stanza in stanzas(index):
        name = field(stanza, 'Package')
        if name in names:
            version = field(stanza, 'Version') or '0'
            if name not in best or newer(version, field(best[name], 'Version') or '0'):
                best[name] = stanza

    status = open(status_path, encoding='utf-8', errors='replace').read()
    installed = {field(s, 'Package'): field(s, 'Version') for s in stanzas(status)}
    done = []
    for name in names:
        if name not in best:
            print('jb-preinstall.py: %s: not in the repository' % name)
            continue
        stanza = best[name]
        version = field(stanza, 'Version')
        have = installed.get(name)
        if have is not None and not newer(version, have):
            print('jb-preinstall.py: %s %s is already there' % (name, have))
            continue
        filename = field(stanza, 'Filename')
        deb = os.path.join(debs, os.path.basename(filename))
        if not os.path.exists(deb):
            url = os.environ.get('MIRROR', 'https://apt.procurs.us').rstrip('/') + '/' + filename
            # curl, as the rest of tools/mk-jb.sh fetches: the repository turns away urllib
            if subprocess.run(['curl', '-sL', '--fail', '-o', deb, url]).returncode != 0:
                sys.exit('jb-preinstall.py: cannot fetch %s' % url)
        # a .deb is an ar archive; its data holds the tree, under ./var/jb for a rootless package.
        # The member is found in the listing: "ar p" says nothing and still succeeds for a name that
        # is not in the archive, so asking for one that may not be there tells us nothing.
        listing = subprocess.run(['ar', 't', deb], capture_output=True, text=True)
        member = next((line.strip() for line in listing.stdout.split('\n') if line.startswith('data.tar')), None)
        if member is None:
            sys.exit('jb-preinstall.py: %s: no data member in %s' % (name, deb))
        data = subprocess.run(['ar', 'p', deb, member], capture_output=True).stdout
        if not data:
            sys.exit('jb-preinstall.py: %s: %s is empty in %s' % (name, member, deb))
        if member.endswith('.zst'):
            data = subprocess.run(['zstd', '-dc'], input=data, capture_output=True).stdout
        elif member.endswith('.lzma'):
            data = lzma.decompress(data, format=lzma.FORMAT_ALONE)
        # .gz, .bz2 and .xz tarfile knows by itself
        tmp = deb + '.tar'
        with open(tmp, 'wb') as f:
            f.write(data)
        with tarfile.open(tmp) as tar:
            for member in tar.getmembers():
                path = os.path.normpath(member.name.lstrip('./'))
                # only into the prefix, and never climbing out of it
                if not path.startswith('var/jb/') or '..' in path.split('/'):
                    continue
                member.name = './' + path
                target = os.path.join(prefix, path)
                if os.path.islink(target) or (os.path.exists(target) and not os.path.isdir(target)):
                    os.unlink(target)                   # the older version's file makes way
                # "fully_trusted": the prefix is full of links to absolute paths of its own
                # (/var/jb/bin/run-parts -> /var/jb/usr/bin/run-parts), which the careful filter
                # refuses; where the files land is checked above instead.
                tar.extract(member, prefix, set_attrs=True, filter='fully_trusted')
        os.unlink(tmp)
        # dpkg's database: the new stanza in place of the old one, or added
        entry = '\n'.join(line for line in stanza.split('\n')
                          if not line.startswith(('Filename:', 'Size:', 'MD5sum:', 'SHA1:', 'SHA256:', 'SHA512:',
                                                  'Description-md5:'))) + '\nStatus: install ok installed\n'
        if have is not None:
            status = '\n\n'.join(entry if field(s, 'Package') == name else s for s in stanzas(status)) + '\n'
        else:
            status = status.rstrip('\n') + '\n\n' + entry + '\n'
        done.append('%s %s%s' % (name, version, ' (was %s)' % have if have else ''))
    # A package that still writes to Cydia Substrate's old directory turns the link ElleKit put there
    # into a real one when it is unpacked here; put the tweaks where they belong and the link back.
    old_dir = os.path.join(jb, 'Library/MobileSubstrate/DynamicLibraries')
    new_dir = os.path.join(jb, 'usr/lib/TweakInject')
    if os.path.isdir(old_dir) and not os.path.islink(old_dir):
        os.makedirs(new_dir, exist_ok=True)
        for entry in sorted(os.listdir(old_dir)):
            os.replace(os.path.join(old_dir, entry), os.path.join(new_dir, entry))
        os.rmdir(old_dir)
        os.symlink('/var/jb/usr/lib/TweakInject', old_dir)
        print('jb-preinstall.py: the tweaks moved to usr/lib/TweakInject, the old directory a link again')
    open(status_path, 'w', encoding='utf-8').write(status)
    if done:
        print('jb-preinstall.py: ' + ', '.join(done))


if __name__ == '__main__':
    main()
