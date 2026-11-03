#!/bin/bash
# Build UniXNU's Metal on Vulkan for the iOS 16.5 userland: AdrenoMetal's frontend (the Metal objects, MIT) over its
# bridge to steelbrain's metal2vulkan runtime (Rust; LGPL-3.0+/GPL-2.0+), whose Vulkan is Turnip (tools/mk-turnip.sh).
# Both sources at the revisions UniXNU qualified, metal2vulkan with AdrenoMetal's 30 patches; on top, what iOS 16.5
# needs (tools/m2v/: two names of the iPadOS 17 SDK, and the size check of Metal's _MTLDevice).
#
# What comes out, in xnu-deps/build/m2v/out/: frontend.o and libadreno_metal_bridge.a, linked into a program or the
# Metal plug-in (with -framework Foundation Metal IOSurface CoreFoundation -lc++), whose native_bridge_metal_open()
# gives the MTLDevice.
#
# Needs on this machine: rustup's cargo with Rust 1.98 and its aarch64-apple-ios target, llvm-as of LLVM 21
# (apt: llvm-21), clang, and the theos iPhoneOS16.5 SDK.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=$ROOT/xnu-deps/build/m2v
SDK=${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk
M2V_PIN=624bab3918de5b978e1101517abca2b4fa426565            # steelbrain/metal2vulkan, branch development
AM_PIN=243d63cf43305a85c8119b66b49cb7df4b30b019             # UniXNU/AdrenoMetal
export PATH=$HOME/.cargo/bin:$PATH
command -v cargo >/dev/null || { echo "mk-m2v.sh: no cargo (rustup, Rust 1.98, target aarch64-apple-ios)" >&2; exit 1; }
LLVM_AS=$(command -v llvm-as-21 || command -v llvm-as) || { echo "mk-m2v.sh: no llvm-as (apt: llvm-21)" >&2; exit 1; }
mkdir -p $B/out $B/obj
cd $B
fetch() {       # repository, revision, directory, patches...
	local url=$1 rev=$2 dir=$3; shift 3
	if [ ! -d $dir ]; then
		git clone -q --filter=blob:none $url $dir
		git -C $dir checkout -q $rev
		for p in "$@"; do git -C $dir apply "$p"; done
	fi
	[ "$(git -C $dir rev-parse HEAD)" = $rev ] || { echo "mk-m2v.sh: $dir is not at $rev" >&2; exit 1; }
}
fetch https://github.com/UniXNU/AdrenoMetal.git $AM_PIN AdrenoMetal $ROOT/tools/m2v/adrenometal-ios16.patch
A=$B/AdrenoMetal
fetch https://github.com/steelbrain/metal2vulkan.git $M2V_PIN metal2vulkan $A/bridge/patches/00*.patch $ROOT/tools/m2v/metal2vulkan-hakios.patch
S=$B/metal2vulkan

# the bridge: the runtime behind a C interface, as UniXNU's scripts/build-adreno-metal-bridge.py builds it
$LLVM_AS -o obj/runtime_examples.air $S/crates/metal2vulkan-validation/fixtures/public/examples/runtime_examples.ll
$LLVM_AS -o obj/function_constant.air $S/crates/metal2vulkan-validation/fixtures/public/kernel_function_constant_gate_fold.ll
$LLVM_AS -o obj/scanout_convert.air $A/shaders/scanout_convert.ll
# (the manifest in a directory of its own: one in $B would take the trees below it into its workspace)
mkdir -p bridge
cat > bridge/Cargo.toml <<EOF
[package]
name = "adreno-metal-bridge"
version = "0.1.0"
edition = "2024"
rust-version = "1.98"
[workspace]
[lib]
crate-type = ["staticlib"]
path = "$A/bridge/NativeMetalBridge.rs"
[dependencies]
metal2vulkan = { path = "$S/crates/metal2vulkan", features = ["ash"] }
metal2vulkan-runtime = { path = "$S/crates/metal2vulkan-runtime" }
libc = "0.2"
sha2 = { version = "0.10", features = ["asm"] }
[profile.release]
panic = "unwind"
debug = 1
lto = "fat"
codegen-units = 1
EOF
CC_aarch64_apple_ios=clang CFLAGS_aarch64_apple_ios="-target arm64-apple-ios16.0 -isysroot $SDK" \
AR_aarch64_apple_ios=$(command -v llvm-ar-21 || command -v llvm-ar) IPHONEOS_DEPLOYMENT_TARGET=16.0 SDKROOT=$SDK \
NMB_RUNTIME_ID=$(echo $M2V_PIN | cut -c1-32) NMB_FIXTURE_AIR=$B/obj/runtime_examples.air \
NMB_FC_AIR=$B/obj/function_constant.air NMB_SCANOUT_AIR=$B/obj/scanout_convert.air \
	cargo build --release --manifest-path bridge/Cargo.toml --target aarch64-apple-ios --target-dir target > cargo.log 2>&1 ||
	{ tail -30 cargo.log >&2; exit 1; }
cp target/aarch64-apple-ios/release/libadreno_metal_bridge.a out/

# the frontend: the Metal objects
clang++ -target arm64-apple-ios16.0 -isysroot $SDK -O2 -std=c++17 -fobjc-arc -Wno-deprecated-declarations \
	-include $ROOT/tools/m2v/ios16-compat.h -I $A -I $ROOT/xnu-deps/build/mesa/mesa-25.0.7/include \
	-x objective-c++ -c $A/NativeBridgeMetalFrontend.mm -o out/frontend.o
echo "mk-m2v.sh: $B/out: frontend.o ($(stat -c %s out/frontend.o) bytes), libadreno_metal_bridge.a ($(stat -c %s out/libadreno_metal_bridge.a) bytes)"
