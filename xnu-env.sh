# source this (from bash) before building XNU on Linux
# Everything is found relative to this file. THEOS (default ~/theos) provides the cctools and the
# Darwin SDK; XNU_SRC is the kernel tree (xnu/apply.sh creates it).
DARWIN_REDWOOD=$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)
export DARWIN_REDWOOD
export XNU_HOST_OS=$(uname)
# On macOS there is no theos: xnu-deps/setup.sh lays out xnu-deps/theos-mac like one (links to the
# cctools of Xcode and to the iPhoneOS 16.5 SDK of github.com/theos/sdks).
if [ "$XNU_HOST_OS" = Darwin ]; then
	export THEOS=${THEOS:-$DARWIN_REDWOOD/xnu-deps/theos-mac}
else
	export THEOS=${THEOS:-$HOME/theos}
fi
export XNU_SRC=${XNU_SRC:-$DARWIN_REDWOOD/xnu-src/xnu-8796.121.2}
export XNUDEPS=$DARWIN_REDWOOD/xnu-deps
export PATH=$XNUDEPS/host/bin:$PATH
export XNU_SDK_PATH=$XNUDEPS/sdk/iPhoneOS16.5.xnu.sdk
export XNU_SDK_VERSION=16.5
export XNU_PLATFORM_PATH=$XNUDEPS/sdk/Platforms/iPhoneOS.platform
export PERL5LIB="/home/ankit/.local/usr/share/autoconf:/home/ankit/.local/usr/share/automake-1.18${PERL5LIB:+:$PERL5LIB}"
export M4="/home/ankit/.local/bin/m4"
export AUTOM4TE_CFG="/home/ankit/.local/usr/share/autoconf/autom4te.cfg"
export BISON_PKGDATADIR="/home/ankit/.local/usr/share/bison"
export AC_MACRODIR="/home/ankit/.local/usr/share/autoconf"
export autom4te_perllibdir="/home/ankit/.local/usr/share/autoconf"
export trailer_m4="/home/ankit/.local/usr/share/autoconf/autoconf/trailer.m4"
export PATH="/home/ankit/.local/bin:$PATH"
export C_INCLUDE_PATH="/home/ankit/.local/usr/include${C_INCLUDE_PATH:+:$C_INCLUDE_PATH}"
export CPLUS_INCLUDE_PATH="/home/ankit/.local/usr/include${CPLUS_INCLUDE_PATH:+:$CPLUS_INCLUDE_PATH}"
export LIBRARY_PATH="/home/ankit/.local/usr/lib/x86_64-linux-gnu:/home/ankit/.local/usr/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"
export LD_LIBRARY_PATH="/home/ankit/theos/toolchain/linux/iphone/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
