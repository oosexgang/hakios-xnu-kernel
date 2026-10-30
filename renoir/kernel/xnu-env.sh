# source this (from bash) before building the renoir xnu-12377 kernel on Linux.
# The toolchain is darwin-renoir's (theos cctools, mig, tcsh, unifdef and Apple's open sources it
# fetched); the dependency directory with its own SDK copy and the kernel's two static libraries
# (built against xnu-12377's headers, not xnu-8796's) lives next to the kernel tree, outside the
# repository, so neither darwin-renoir nor base/ is written to.
RENOIR_KERNEL=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HAKIOS=$(cd "$RENOIR_KERNEL/../.." && pwd)
export RENOIR_KERNEL HAKIOS
export DARWIN_RENOIR=${DARWIN_RENOIR:-$(cd "$HAKIOS/.." && pwd)/darwin-renoir}
if [ -z "${THEOS:-}" ]; then
	THEOS=$HOME/theos
	[ -d "$THEOS/toolchain/linux/iphone" ] || THEOS=$DARWIN_RENOIR/build-tools/theos
fi
export THEOS
export XNU_SRC=${XNU_SRC:-$(cd "$HAKIOS/.." && pwd)/xnu-12377-renoir}
export XNUDEPS=${XNUDEPS:-$XNU_SRC-deps}
export PATH=$XNUDEPS/host/bin:$PATH
export XNU_SDK_PATH=$XNUDEPS/sdk/iPhoneOS16.5.xnu.sdk
export XNU_SDK_VERSION=16.5
export XNU_PLATFORM_PATH=$XNUDEPS/sdk/Platforms/iPhoneOS.platform
# renoir's SM7350 (Cortex-A78/A55, ARMv8.2) has no pointer authentication: plain arm64 only
export XNU_ARCH=arm64
