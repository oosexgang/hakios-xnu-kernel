# source this (from bash) before building XNU on Linux
# Everything is found relative to this file. THEOS (default ~/theos) provides the cctools and the
# iOS SDK; XNU_SRC is the kernel tree (xnu/apply.sh creates it).
DARWIN_REDWOOD=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export DARWIN_REDWOOD
export THEOS=${THEOS:-$HOME/theos}
export XNU_SRC=${XNU_SRC:-$DARWIN_REDWOOD/xnu-src/xnu-8796.121.2}
export XNUDEPS=$DARWIN_REDWOOD/xnu-deps
export PATH=$XNUDEPS/host/bin:$PATH
export XNU_SDK_PATH=$XNUDEPS/sdk/iPhoneOS16.5.xnu.sdk
export XNU_SDK_VERSION=16.5
export XNU_PLATFORM_PATH=$XNUDEPS/sdk/Platforms/iPhoneOS.platform
# The architecture of the kernel and of everything linked into it: arm64, or arm64e for the pointer
# authentication work (docs/ARM64E.md). One board configuration serves both.
export XNU_ARCH=${XNU_ARCH:-arm64}
# On macOS the cctools, mig, unifdef and csh come from Xcode and the system instead of theos and
# sources built here; the scripts look at this to choose.
[ "$(uname -s)" = Darwin ] && export XNU_HOST_DARWIN=1
