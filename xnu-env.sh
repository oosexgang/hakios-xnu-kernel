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
