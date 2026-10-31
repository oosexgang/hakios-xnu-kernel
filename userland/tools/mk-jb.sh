#!/bin/bash
# Fetch what Sileo needs into jb/ (never in the repository, like fw-*): the Procursus bootstrap for
# iOS and the Sileo package, both for arm64 and both "rootless" -- everything under /var/jb.
#
# Rootless is what fits this port: / is an APFS image that cannot be written to, and /private/var
# is the only writable filesystem there is (a memory filesystem, made afresh at every boot), so the
# whole prefix has to live under /var. The binaries of the rootless distribution are built for
# iOS 15 and up (dpkg 1.22.6, apt 2.9.4) and Sileo for iOS 11 and up; the "1800" in the path is the
# name of the distribution, not a demand for iOS 18.
#
# What is left behind in jb/:
#   root/var/jb/...      the bootstrap with Sileo unpacked into it, as the phone will have it
#   debs/                the packages it was made of, for a look
# userland/build.sh puts root/var/jb into the system image (as /jb, read only) and init copies it
# to /private/var/jb at every boot.
set -e
SUITE=1800                                              # the rootless distribution of Procursus
SILEO=org.coolstar.sileo_2.5.1_iphoneos-arm64.deb
MIRROR=https://apt.procurs.us
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
OUT=$ROOT/jb
mkdir -p "$OUT/debs"

get() {                                                 # fetch into jb/, once
	[ -s "$OUT/debs/$2" ] || curl -sL --fail -o "$OUT/debs/$2" "$1"
}
get "$MIRROR/bootstraps/$SUITE/bootstrap-iphoneos-arm64.tar.zst" bootstrap.tar.zst
get "$MIRROR/pool/main/iphoneos-arm64-rootless/$SUITE/sileo/$SILEO" sileo.deb

rm -rf "$OUT/root"
mkdir -p "$OUT/root"
zstd -dc "$OUT/debs/bootstrap.tar.zst" | tar -C "$OUT/root" -xf -
[ -d "$OUT/root/var/jb" ] || { echo "mk-jb.sh: the bootstrap has no var/jb in it" >&2; exit 1; }
# Unpacking as an ordinary user drops the set-user-ID and set-group-ID bits; they are put back on what
# the bootstrap has them on: login above all (NewTerm logs the user in with "login -fp mobile", which
# without the bit ends at once -- "Session Ended"), su, sudo, passwd, chpass, newgrp, quota. In the
# image these files belong to root, so the bits mean what they say there.
zstd -dc "$OUT/debs/bootstrap.tar.zst" | tar -tvf - | awk '$1 ~ /^-/ && ($1 ~ /^-..[sS]/ || $1 ~ /^-.....[sS]/) {
	print substr($1, 4, 1) substr($1, 7, 1), $NF }' | while read -r bits path; do
	[ -f "$OUT/root/$path" ] || continue
	case $bits in [sS]?) chmod u+s "$OUT/root/$path" ;; esac
	case $bits in ?[sS]) chmod g+s "$OUT/root/$path" ;; esac
	echo "mk-jb.sh: set-ID bits back on ${path#./}"
done

# Sileo on top of it: a .deb is an ar archive of control and data, and the data is the tree itself
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
(cd "$TMP" && ar x "$OUT/debs/sileo.deb")
tar -C "$OUT/root" -xf "$TMP"/data.tar.* 2>/dev/null ||
	zstd -dc "$TMP/data.tar.zst" | tar -C "$OUT/root" -xf -
[ -d "$OUT/root/var/jb/Applications/Sileo.app" ] || { echo "mk-jb.sh: no Sileo.app in the package" >&2; exit 1; }

# The package ships Sileo's root helper as -rwsr-xr-x root/root: everything Sileo does to the system
# (apt, dpkg, making its own list directory) it does by spawning giveMeRoot, which is set-user-ID.
# Unpacking the archive as an ordinary user drops that bit, and without it Sileo can read the
# repositories and queue packages but install nothing. In the image the file belongs to root, so the
# bit means what it says there.
chmod 4755 "$OUT/root/var/jb/Applications/Sileo.app/giveMeRoot"

# dpkg is to know that Sileo is installed, or Sileo itself will offer to install it again
mkdir -p "$TMP/control" && tar -C "$TMP/control" -xf "$TMP"/control.tar.* 2>/dev/null ||
	zstd -dc "$TMP/control.tar.zst" | tar -C "$TMP/control" -xf -
{
	sed '/^$/d' "$TMP/control/control"
	echo "Status: install ok installed"
	echo
} >> "$OUT/root/var/jb/Library/dpkg/status"

# ElleKit: the tweak-injection framework (Cydia Substrate's modern replacement). It is what loads a
# tweak's .dylib into a process and does the function and method hooking; on a rootless jailbreak the
# jailbreak itself installs it, and it is not in the apt repository, so it is fetched here from its own
# distribution. Its libraries are fat (arm64 and arm64e): the arm64 slice runs on this phone's
# Qualcomm chip. mobilesubstrate and org.coolstar.libhooker are virtual packages it Provides, which is
# what preferenceloader and nearly every tweak depend on. The loader /var/jb/usr/lib/TweakLoader.dylib
# (ellekit/libinjector) reads /var/jb/usr/lib/TweakInject/*.plist, matches the process by the Filter
# and dlopens the tweak; src/init.c (etc/inject.conf) inserts it into SpringBoard, Preferences and the
# apps. Tweaks link /var/jb/usr/lib/lib{substrate,hooker}.dylib and CydiaSubstrate.framework, all
# ElleKit.
EK=ellekit_${ELLEKIT:-0.4.5}_iphoneos-arm64.deb
get "https://ellekit.space/pool/$EK" "$EK"
EKTMP=$(mktemp -d); trap 'rm -rf "$TMP" "$EKTMP"' EXIT
(cd "$EKTMP" && ar x "$OUT/debs/$EK")
mkdir -p "$EKTMP/data" && { tar -C "$EKTMP/data" -xf "$EKTMP"/data.tar.* 2>/dev/null ||
	zstd -dc "$EKTMP"/data.tar.zst | tar -C "$EKTMP/data" -xf -; }
cp -a "$EKTMP/data/var/jb/." "$OUT/root/var/jb/"
mkdir -p "$OUT/root/var/jb/usr/lib/TweakInject"
# the old tweak directory, a symlink to the new one, as ElleKit's own preinst makes it
mkdir -p "$OUT/root/var/jb/Library/MobileSubstrate"
rm -rf "$OUT/root/var/jb/Library/MobileSubstrate/DynamicLibraries"
ln -s /var/jb/usr/lib/TweakInject "$OUT/root/var/jb/Library/MobileSubstrate/DynamicLibraries"
# dpkg is to know ElleKit is installed, with what it Provides, so apt satisfies tweaks' "mobilesubstrate"
{
	mkdir -p "$EKTMP/control" && { tar -C "$EKTMP/control" -xf "$EKTMP"/control.tar.* 2>/dev/null ||
		zstd -dc "$EKTMP"/control.tar.zst | tar -C "$EKTMP/control" -xf -; }
	sed '/^$/d' "$EKTMP/control/control"
	echo "Status: install ok installed"
	echo
} >> "$OUT/root/var/jb/Library/dpkg/status"

# Newer versions of what the bootstrap carries, and anything else worth having before the first boot:
# the bootstrap is a snapshot, and some of it is older than the repository (debianutils, launchctl).
# apt on the phone would upgrade them, but only over the network and only after a boot, while what is
# in the image is what a first boot has. PREINSTALL names them; mobilesubstrate is not among them
# because there is no such package -- ElleKit above Provides it.
# OpenSSH among them: with the phone's radio up (wifi=1) a shell on the running system costs
# seconds instead of a rebuilt image and a six-minute boot, which changes how the rest of this
# port can be worked on. Nothing starts it -- a scenario does (userland/rc.jobs.ssh) -- and the
# key that lets anyone in is not in the repository. dropbear would have been smaller, but its
# package here depends on libtomcrypt1 and libtommath1, which are in no suite of this repository.
PREINSTALL=${PREINSTALL-"debianutils launchctl preferenceloader openssh-server openssh-client openssh-sftp-server libssl3 libmd0"}
if [ -n "$PREINSTALL" ]; then
	get "$MIRROR/dists/iphoneos-arm64-rootless/$SUITE/main/binary-iphoneos-arm64/Packages" Packages
	MIRROR="$MIRROR" python3 "$ROOT/tools/jb-preinstall.py" "$OUT/root" "$OUT/debs/Packages" "$OUT/debs" $PREINSTALL
fi

# the repositories Sileo starts with: Procursus itself, where everything here came from
mkdir -p "$OUT/root/var/jb/etc/apt/sources.list.d"
cat > "$OUT/root/var/jb/etc/apt/sources.list.d/procursus.sources" <<EOF
Types: deb
URIs: $MIRROR
Suites: iphoneos-arm64-rootless/$SUITE
Components: main
EOF

# The certificates for apt. gnutls here is built to trust /var/jb/etc/ssl/certs/cacert.pem, which the
# package ca-certificates of the bootstrap brings along, and yet apt says "No system certificates
# available" and refuses the handshake with the repository: it asks the system trust store for them,
# and in this port trustd's own connections go unanswered (it talks to Apple and gets nothing back).
# So apt is pointed at that file directly, which needs no trust store at all.
mkdir -p "$OUT/root/var/jb/etc/apt/apt.conf.d"
cat > "$OUT/root/var/jb/etc/apt/apt.conf.d/10redwood.conf" <<'EOF'
Acquire::https::CaInfo "/var/jb/etc/ssl/certs/cacert.pem";
EOF

# Three packages of the bootstrap have no meaning on this port and only break apt: libkrw0 and
# libdimentio0 read and write the kernel of a jailbroken phone through a plug-in that a jailbreak
# supplies (libkrw0-plugin), and shshd saves its signing blobs with them. Here the kernel is ours;
# nothing provides the plug-in, and apt refuses to install anything at all while one dependency in
# the database is unmet. They go, files and all.
for package in shshd libkrw0 libdimentio0; do
	list=$OUT/root/var/jb/Library/dpkg/info/$package.list
	if [ -f "$list" ]; then
		while IFS= read -r path; do
			[ -f "$OUT/root/var/jb/$path" ] && rm -f "$OUT/root/var/jb/$path"
		done < "$list"
		rm -f "$OUT"/root/var/jb/Library/dpkg/info/$package.*
	fi
	awk -v want="Package: $package" 'BEGIN { RS = ""; FS = "\n" }
		{
			drop = 0
			for (i = 1; i <= NF; i++) {
				if ($i == want) drop = 1
			}
			if (!drop) printf "%s\n\n", $0
		}' "$OUT/root/var/jb/Library/dpkg/status" > "$OUT/root/var/jb/Library/dpkg/status.new"
	mv "$OUT/root/var/jb/Library/dpkg/status.new" "$OUT/root/var/jb/Library/dpkg/status"
done

# The dependency on "firmware" is taken out of the records of the packages that carry it. That
# package is not a package: usr/libexec/firmware writes it straight into dpkg's database (the iOS
# version, "firmware 16.5"), no repository has it and nothing provides it. apt on the command line
# is content with that -- but Sileo asks apt with an index of its own making (it writes a reduced
# Packages file into var/lib/apt/sileolists and runs "apt-get -sqf ... -oDir::State::lists=
# sileolists/"), and against that index apt calls the same firmware "not installable" and reports
# org.coolstar.sileo and snaputil as broken. Sileo then shows those as errors and refuses to install
# anything at all. What the dependency says -- "this iOS is new enough" -- is true here by
# construction: the system is iOS 16.5 and cannot be anything else.
python3 - "$OUT/root/var/jb/Library/dpkg/status" <<'EOF'
import re, sys

path = sys.argv[1]
stanzas = open(path, encoding='utf-8').read().split('\n\n')
changed = 0
for i, stanza in enumerate(stanzas):
    lines = stanza.split('\n')
    for k, line in enumerate(lines):
        if not line.startswith('Depends: '):
            continue
        groups = [g for g in line[len('Depends: '):].split(', ')
                  if not re.match(r'^\s*firmware\s*[( ]|^\s*firmware$', g.split(' | ')[0])]
        if len(groups) != len(line[len('Depends: '):].split(', ')):
            lines[k] = 'Depends: ' + ', '.join(groups) if groups else None
            changed += 1
    lines = [line for line in lines if line is not None]
    stanzas[i] = '\n'.join(lines)
open(path, 'w', encoding='utf-8').write('\n\n'.join(stanzas))
print(f"mk-jb.sh: the dependency on \"firmware\" taken out of {changed} records")
EOF

# What the bootstrap's own prep_bootstrap.sh does, less what this port has no use for: it asks for a
# terminal password through a window of its own, and sets login shells in an /etc that is made anew
# at every boot anyway. What is left matters: usr/libexec/firmware puts a package named "firmware"
# in dpkg's database with the iOS version in it, which is what every other package depends on
# (Sileo: "firmware (>= 11.0)"), and the few postinst scripts settle the shells and apt's own state.
cat > "$OUT/root/var/jb/prep.sh" <<'PREP'
#!/var/jb/bin/sh
# Run once per boot, after the prefix has been grafted onto /var/jb (rc.jobs.sileo does it).
/var/jb/usr/libexec/firmware
# The prefix's applications: in the system image they are in /Applications already (userland/build.sh),
# where LaunchServices finds them by itself; with the iOS volume on the flash as the root they are not,
# and are registered from the prefix as a rootless jailbreak does it (uikittools' uicache) -- by where the
# bundle really is, as there: in /var/jb, grafted, its files are links into the boot volume
# (/private/preboot/jb), and LaunchServices refuses a bundle made of links ("Unable to register").
# Asked again every five seconds, two minutes at most, aside: right after SpringBoard it may be busy.
for app in /var/jb/Applications/*.app; do
	[ -d "/Applications/${app##*/}" ] && continue
	# by its own path in the prefix first: that is the one a later uicache -- the trigger of any package
	# that brings an application -- finds again when it rebuilds the database, and an application
	# registered under some other path is "uninstalled" by that rebuild, which makes SpringBoard kill it
	# (a running Sileo died the moment a tweak was installed). The real path, with the grafted prefix a
	# directory of the boot volume, is only the fallback.
	real=$(/var/jb/usr/bin/readlink "$app/Info.plist" 2>/dev/null)
	real=${real%/Info.plist}
	(
		for try in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24; do
			for path in "$app" ${real:+"$real"}; do
				case "$(/var/jb/usr/bin/uicache -p "$path" 2>&1)" in
				*Error*) ;;
				*) echo "prep.sh: ${app##*/} registered from $path (try $try)"; exit 0 ;;
				esac
			done
			sleep 5
		done
		echo "prep.sh: ${app##*/} could not be registered"
	) &
done
/var/jb/usr/sbin/pwd_mkdb -p /var/jb/etc/master.passwd 2>/dev/null
for package in debianutils apt dash bash zsh vi; do
	[ -x "/var/jb/Library/dpkg/info/$package.postinst" ] &&
		"/var/jb/Library/dpkg/info/$package.postinst" configure 99999 >/dev/null 2>&1
done
exit 0
PREP
chmod 755 "$OUT/root/var/jb/prep.sh"
rm -f "$OUT/root/var/jb/prep_bootstrap.sh"

echo "mk-jb.sh: $OUT/root/var/jb ($(du -sh "$OUT/root/var/jb" | cut -f1), $(find "$OUT/root/var/jb" | wc -l) entries)"
echo "          Sileo $(grep -m1 '^Version:' "$TMP/control/control" | cut -d' ' -f2), $(grep -c '^Package:' "$OUT/root/var/jb/Library/dpkg/status") packages in dpkg's database"
