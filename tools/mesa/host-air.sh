#!/bin/bash
# tools/mesa/host-air.sh: the Metal driver's library (tools/mesa/air/air_metal.c) built for THIS Mac, on
# softpipe, with its test program -- the phone's translation of Metal, run where real Metal is beside
# it (docs/metal-x6826b-plan.md, stage 1). Out of xnu-deps/build/mesa/build-host, whose static
# libraries it builds first; nothing of the phone's builds is touched. Prints the test's verdict.
#   tools/mesa/host-air.sh            build, then run air_metal_test
#   tools/mesa/host-air.sh build      build only
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
M=$ROOT/xnu-deps/build/mesa
B=$M/build-host
S=$M/mesa-25.0.7
OUT=$B/host-air
mkdir -p $OUT
# (meson regenerates with the python that has mako: the phone build's)
PATH=$M/venv/bin:$PATH ninja -C $B src/gallium/auxiliary/libgallium.a src/gallium/auxiliary/libgalliumvl_stub.a \
	src/gallium/winsys/sw/null/libws_null.a src/gallium/drivers/softpipe/libsoftpipe.a \
	src/compiler/air/libair.a src/compiler/nir/libnir.a src/compiler/libcompiler.a src/util/libmesa_util.a \
	src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a > $OUT/ninja.log
INC="-I$B/src/gallium/auxiliary -I$S/src/gallium/auxiliary -I$S/src/gallium/include -I$B/include -I$S/include
	-I$B/src -I$S/src -I$B/src/compiler/nir -I$S/src/compiler/nir -I$B/src/compiler -I$S/src/compiler
	-I$B/src/util/format -I$S/src/mapi -I$S/src/mesa -I$S/src/gallium/drivers -I$S/src/gallium/winsys
	-I$S/src/gallium/winsys/sw -I$S/src/compiler/air -I$B/src/compiler/air"
DEFS="-DGALLIUM_SOFTPIPE -DHAVE_SOFTPIPE -DHAVE_SWRAST -DHAVE_PTHREAD -DUSE_GCC_ATOMIC_BUILTINS -DHAVE_DLADDR
	-DHAVE_STRUCT_TIMESPEC -DHAVE_FUNC_ATTRIBUTE_PACKED -DHAVE_FUNC_ATTRIBUTE_VISIBILITY -DMESA_DEBUG=0 -DNDEBUG"
cc -std=c11 -O1 -g -fPIC $INC $DEFS -Wno-everything -include drm-uapi/drm_fourcc.h -include frontend/winsys_handle.h -c $ROOT/tools/mesa/air/air_metal.c -o $OUT/air_metal.o
cc -std=c11 -O1 -g $INC $DEFS -Wno-everything -c $ROOT/tools/mesa/air/air_metal_test.c -o $OUT/air_metal_test.o
LIBS="$B/src/compiler/air/libair.a $B/src/gallium/drivers/softpipe/libsoftpipe.a $B/src/gallium/auxiliary/libgallium.a
	$B/src/gallium/auxiliary/libgalliumvl_stub.a $B/src/gallium/winsys/sw/null/libws_null.a
	$B/src/compiler/nir/libnir.a $B/src/compiler/libcompiler.a $B/src/util/libmesa_util.a
	$B/src/util/blake3/libblake3.a $B/src/c11/impl/libmesa_util_c11.a"
[ -f $B/src/util/libmesa_util_sse41.a ] && LIBS="$LIBS $B/src/util/libmesa_util_sse41.a"
# what panfrost and redwood-drm give the library on the phone, as nothing here (air_metal.c's weak
# imports, of which the variables are only weak in the declaration that is not the first)
printf '%s\n' '#include <stdint.h>' 'uint64_t redwood_pan_submit_ns, redwood_pan_submits, redwood_pan_blend_compiles;' \
	'uint64_t redwood_pan_variant_compiles, redwood_pan_variant_ns;' > $OUT/stubs.c
cc -c $OUT/stubs.c -o $OUT/stubs.o
c++ -o $OUT/air_metal_test $OUT/air_metal_test.o $OUT/air_metal.o $OUT/stubs.o $LIBS -lz -lpthread -lm -Wl,-undefined,dynamic_lookup
echo "host-air.sh: $OUT/air_metal_test"
[ "$1" = build ] && exit 0
cd $ROOT/tools/mesa/air && RWA_SOFTWARE=1 $OUT/air_metal_test 2>&1 | tail -5
