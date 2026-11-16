#!/bin/bash
# A way in over Wi-Fi to a running iOS 26/27: an SSH server and a shell, arm64e, into $OBJ (darwin/userland/env.sh),
# for root26-darwin.sh to put into the small image as /hakios/bin/dropbear, /hakios/bin/scp and /hakios/bin/sh.
# rc.jobs.ssh starts the server once the phone has an address. With it a question to the running system is a
# command over the network instead of a new image and a boot.
#
#   dropbear   the server, with its scp (for `scp -O` from this machine), keys only: no passwords. Patched
#              (ssh/dropbear-ios.patch): iOS has no /bin/sh and no /etc/shells, so the shell is HAKIOS_SHELL
#              whatever /etc/passwd says, and root is iOS's own should the user lookup find nothing.
#   mksh       the shell (iOS has none): a POSIX shell with line editing and history.
#
# The keys are this machine's, never the repository's: ../ssh/ beside it (as for 16.5) -- authorized_keys, the
# keys let in as root, and ssh_host_ed25519_key, the phone's host key, which dropbearconvert (built here for this
# machine) turns into dropbear's format. root26-darwin.sh takes them from $OBJ/ssh-keys unless SHARE=1.
#
# The sources are downloaded once into $XNUDEPS/build/ssh and checked against the hashes below.
set -e
RN=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=env.sh
source "$RN/userland/env.sh"

DROPBEAR=dropbear-2026.94
DROPBEAR_SHA256=e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81
DROPBEAR_URL=https://matt.ucc.asn.au/dropbear/releases/$DROPBEAR.tar.bz2
MKSH=mksh-R59c
MKSH_SHA256=77ae1665a337f1c48c61d6b961db3e52119b38e58884d1c89684af31f87bc506
MKSH_URL=http://www.mirbsd.org/MirOS/dist/mir/mksh/$MKSH.tgz
KEYS=${SSH_KEYS:-$(cd "$IOS_REPO/.." && pwd)/ssh}

W=$XNUDEPS/build/ssh
mkdir -p "$W" "$OBJ"
fetch() { # fetch URL SHA256 FILE
	if [ ! -f "$W/$3" ]; then
		curl -sfL --max-time 300 -o "$W/$3.part" "$1" || { echo "ssh.sh: could not download $1" >&2; exit 1; }
		mv "$W/$3.part" "$W/$3"
	fi
	echo "$2  $W/$3" | sha256sum -c --quiet - || { echo "ssh.sh: $3 is not the file this script was written for" >&2; exit 1; }
}
fetch "$DROPBEAR_URL" "$DROPBEAR_SHA256" "$DROPBEAR.tar.bz2"
fetch "$MKSH_URL" "$MKSH_SHA256" "$MKSH.tgz"

# the cross compiler as configure wants it: one command, the flags with it
# (the libraries' compiler and linker, env.sh: arm64e through ld64, or arm64 named arm64e by pac_strip)
CROSS=$DYN

# ---- dropbear: the server and scp for the phone, dropbearconvert for this machine ----------------------
rm -rf "$W/dropbear"
mkdir -p "$W/dropbear"
tar -xjf "$W/$DROPBEAR.tar.bz2" -C "$W/dropbear" --strip-components=1
patch -s -d "$W/dropbear" -p1 < "$RN/userland/ssh/dropbear-ios.patch"
cat > "$W/dropbear/localoptions.h" <<'EOF'
/* (darwin/userland/ssh.sh) keys only; the small image's shell, paths and host key */
#define HAKIOS_SHELL "/hakios/bin/sh"
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_X11FWD 0
#define DROPBEAR_SFTPSERVER 0
#define DEFAULT_PATH "/hakios/bin:/usr/bin:/bin:/usr/sbin:/sbin"
/* (root's is a PATH of its own; scp -O from this machine runs the phone's scp by name, so it must find it) */
#define DEFAULT_ROOT_PATH "/hakios/bin:/usr/sbin:/usr/bin:/sbin:/bin"
#define ED25519_PRIV_FILENAME "/hakios/etc/ssh/dropbear_ed25519_host_key"
/* (Darwin has no setresgid: one user, root, which the server runs as) */
#define DROPBEAR_SVR_DROP_PRIVS 0
#define DROPBEAR_SVR_MULTIUSER 0
EOF
(
	cd "$W/dropbear"
	# (the Makefile takes localoptions.h from the directory it builds in)
	mkdir -p host phone
	cp localoptions.h phone/
	(cd host && ../configure --disable-zlib --disable-pam --disable-syslog > ../host.log 2>&1 &&
		make -s PROGRAMS=dropbearconvert -j"$(nproc)" >> ../host.log 2>&1) || { tail -20 host.log >&2; exit 1; }
	# (libtomcrypt is an archive: llvm-ar in Darwin's format, with a symbol table ld64 reads -- GNU ar's it does not)
	(cd phone && CC="$CROSS" CFLAGS=-O2 AR="llvm-ar --format=darwin" RANLIB="llvm-ranlib" ../configure --host=aarch64-apple-darwin --enable-bundled-libtom --disable-zlib \
		--disable-pam --disable-harden --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx \
		--disable-loginfunc --disable-pututline --disable-pututxline > ../phone.log 2>&1 &&
		make -s PROGRAMS="dropbear scp" -j"$(nproc)" >> ../phone.log 2>&1) || { tail -20 phone.log >&2; exit 1; }
)
for p in dropbear scp; do
	pac_strip "$W/dropbear/phone/$p" "$OBJ/$p"
done

# ---- mksh: /hakios/bin/sh -----------------------------------------------------------------------------
rm -rf "$W/mksh"
mkdir -p "$W/mksh"
tar -xzf "$W/$MKSH.tgz" -C "$W/mksh" --strip-components=1
(cd "$W/mksh" && CC="$CROSS" CFLAGS=-O2 TARGET_OS=Darwin sh Build.sh -r > build.log 2>&1) || { tail -20 "$W/mksh/build.log" >&2; exit 1; }
pac_strip "$W/mksh/mksh" "$OBJ/sh"

# ---- the keys, when this machine has them ----------------------------------------------------------------
rm -rf "$OBJ/ssh-keys"
if [ -f "$KEYS/authorized_keys" ] && [ -f "$KEYS/ssh_host_ed25519_key" ]; then
	mkdir -p "$OBJ/ssh-keys"
	cp "$KEYS/authorized_keys" "$OBJ/ssh-keys/authorized_keys"
	"$W/dropbear/host/dropbearconvert" openssh dropbear "$KEYS/ssh_host_ed25519_key" "$OBJ/ssh-keys/dropbear_ed25519_host_key" > /dev/null
	echo "  ssh: keys from $KEYS"
else
	echo "  ssh: no $KEYS/authorized_keys and ssh_host_ed25519_key: the server is built, and nobody can log in" >&2
fi
for p in dropbear scp sh; do
	echo "  $p: $(file -b "$OBJ/$p" | cut -c1-50), $(stat -c %s "$OBJ/$p") bytes"
done
