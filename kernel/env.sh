# shellcheck shell=bash
# source this (from bash) before building the kernel by hand; kernel/build.sh does it itself.
#
# What is where (each can be set in the environment before sourcing):
#   XNU_SRC         the assembled kernel tree (kernel/assemble.sh makes it), next to the repository
#   XNUDEPS         what the build needs beside the tree: host tools, its own SDK copy, the kernel's two
#                   static libraries (built against this release's headers)
#   DARWIN_RENOIR   optional: a darwin-renoir checkout whose xnu-deps has the host tools (mig, tcsh,
#                   unifdef) and Apple's open sources already; without it kernel/deps/stage1.sh makes them
#   THEOS           theos: the cctools and iPhoneOS16.5.sdk
# Neither the repository nor darwin-renoir is written to.
HAKIOS_KERNEL=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HAKIOS=$(cd "$HAKIOS_KERNEL/.." && pwd)
export HAKIOS_KERNEL HAKIOS
# shellcheck source=UPSTREAM
source "$HAKIOS_KERNEL/UPSTREAM"
# the build's configuration: build.config.arm64 at the repository's top (with build.config.common), or the file
# there that BUILD_CONFIG names; what the environment sets already is kept
BUILD_CONFIG=${BUILD_CONFIG:-build.config.arm64}
# shellcheck source=../build.config.arm64
source "$HAKIOS/$BUILD_CONFIG"
export DARWIN_RENOIR=${DARWIN_RENOIR:-$(cd "$HAKIOS/.." && pwd)/darwin-renoir}
if [ -z "${THEOS:-}" ]; then
	THEOS=$HOME/theos
	[ -d "$THEOS/toolchain/linux/iphone" ] || THEOS=$DARWIN_RENOIR/build-tools/theos
fi
export THEOS
# (named after the release: xnu-12377.121.6 -> xnu-12377-hakios)
export XNU_SRC=${XNU_SRC:-$(cd "$HAKIOS/.." && pwd)/${UPSTREAM_TAG%%.*}-hakios}
export XNUDEPS=${XNUDEPS:-$XNU_SRC-deps}
export PATH=$XNUDEPS/host/bin:$PATH
export XNU_SDK_PATH=$XNUDEPS/sdk/iPhoneOS16.5.xnu.sdk
export XNU_SDK_VERSION
export XNU_PLATFORM_PATH=$XNUDEPS/sdk/Platforms/iPhoneOS.platform
# (XNU_ARCH: arm64, without pointer authentication, from the build's configuration)
export XNU_ARCH
