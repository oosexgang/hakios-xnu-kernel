# tools/kernels.mk: the host-side kernel tooling. Included by the main Makefile.
#
# SPDX-License-Identifier: MIT
#
#   make ir3-kernel      build out/host/ir3-kernel against a Mesa host build
#   make fixtures        regenerate tests/fixtures/*.hpp from tools/kernels/*.asm
#
# The tool links Mesa's ir3 assembler, so it needs a Mesa source tree and a
# meson build of it (the static libraries, and the generated headers). Point
# MESA_SRC and MESA_BUILD at them. The default is hakios's own host build in
# the frontend-port worktree (tools/air/mk-host.sh there), recognised by its
# libair.a; when that is not built, the Darwin-on-redwood host build stands in.
# Nothing of either tree is copied, only linked against.
MESA_PUBLIC_HOST  ?= /home/vcvk/hakios/workspace-air/build/mesa-host
MESA_PRIVATE_HOST ?= /home/vcvk/darwin-redwood-air/build/mesa-host
ifneq ($(wildcard $(MESA_PUBLIC_HOST)/build/src/compiler/air/libair.a),)
MESA_SRC   ?= $(MESA_PUBLIC_HOST)/mesa-25.0.7
MESA_BUILD ?= $(MESA_PUBLIC_HOST)/build
else
MESA_SRC   ?= $(MESA_PRIVATE_HOST)/mesa-25.0.7
MESA_BUILD ?= $(MESA_PRIVATE_HOST)/build
endif

# The chip id the fixtures are assembled for: the phone's Adreno 7c+ Gen 3
# (A635). Mesa's table matches it through its 0xffff06030500 wildcard row.
CHIP_ID    ?= 0x06030500

# Mesa's headers want the same -D flags its own objects were compiled with
# (HAVE_* for what the host has, _GNU_SOURCE, and so on): a header compiled
# with a different set declares things differently. They are read from the
# build's ninja file, off one ir3 object, rather than copied here, so that a
# build on another machine or of another Mesa gets its own.
ifeq ($(origin MESA_DEFS), undefined)
MESA_DEFS := $(shell grep -m1 -A4 '^build src/freedreno/ir3/libfreedreno_ir3.a.p/ir3.c.o:' $(MESA_BUILD)/build.ninja \
	| grep -m1 ' ARGS = ' | tr ' ' '\n' | grep '^-D' | tr '\n' ' ')
endif

# -isystem, not -I: Mesa's headers are not clean under -Wextra, and the tool is
# built -Werror. Both the source and the build tree are needed: the build tree
# has the generated headers (ir3-isa.h, nir_opcodes.h, freedreno_devices.h).
MESA_INC = \
	-isystem $(MESA_SRC)/src -isystem $(MESA_SRC)/include \
	-isystem $(MESA_SRC)/src/freedreno -isystem $(MESA_SRC)/src/freedreno/common \
	-isystem $(MESA_SRC)/src/compiler -isystem $(MESA_SRC)/src/compiler/nir \
	-isystem $(MESA_SRC)/src/gallium/include \
	-isystem $(MESA_BUILD)/src -isystem $(MESA_BUILD)/src/compiler -isystem $(MESA_BUILD)/src/compiler/nir \
	-isystem $(MESA_BUILD)/src/freedreno -isystem $(MESA_BUILD)/src/freedreno/common \
	-isystem $(MESA_BUILD)/src/freedreno/isa

# The link line is ir3_disasm's (src/freedreno/ir3/meson.build, the smallest
# ir3 consumer Mesa builds), read off the build's ninja file: ir3 and its
# encoder/decoder, nir (ir3 pulls its headers and a few symbols), the util
# library and its parts, and the device table. No gallium, no drm.
# The archives as paths under a build directory, so that compiler.mk can take
# the same list from the iOS cross build (MESA_ARCHIVE_PATHS) and add libair.
MESA_ARCHIVE_PATHS = \
	src/freedreno/ir3/libfreedreno_ir3.a \
	src/freedreno/isa/libir3encode.a \
	src/freedreno/isa/libir3decode.a \
	src/compiler/isaspec/libisaspec.a \
	src/compiler/nir/libnir.a \
	src/compiler/libcompiler.a \
	src/freedreno/common/libfreedreno_common.a \
	src/util/libmesa_util.a \
	src/util/libmesa_util_sse41.a \
	src/util/blake3/libblake3.a \
	src/c11/impl/libmesa_util_c11.a
MESA_ARCHIVES = $(addprefix $(MESA_BUILD)/,$(MESA_ARCHIVE_PATHS))
MESA_LIBS = -Wl,--start-group $(MESA_ARCHIVES) -Wl,--end-group -lz -lm -pthread

KERNEL_ASM      = $(wildcard tools/kernels/*.asm)
KERNEL_FIXTURES = $(patsubst tools/kernels/%.asm,tests/fixtures/%.hpp,$(KERNEL_ASM))

.PHONY: ir3-kernel fixtures

ir3-kernel: out/host/ir3-kernel

out/host/ir3-kernel: tools/ir3-kernel.cc include/hakios-metal/kernel.hpp tools/kernels.mk
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O1 -g -Iinclude $(MESA_DEFS) $(MESA_INC) -o $@ tools/ir3-kernel.cc $(MESA_LIBS)

tests/fixtures/%.hpp: tools/kernels/%.asm out/host/ir3-kernel
	out/host/ir3-kernel --chip-id $(CHIP_ID) -o $@ $<

fixtures: $(KERNEL_FIXTURES)

# The AIR path. A Metal kernel written as LLVM IR (tools/kernels/*.ll) goes
# through llvm-as, then through Mesa's ir3_compiler with the AIR front end --
# built with tools/mesa/ir3-dump.patch applied, which gives it -o (the fixture)
# and -n (its name): the stock ir3_compiler, and the one in the default
# MESA_BUILD today, prints a listing and nothing else. So this is a target of
# its own, not part of `fixtures`, until MESA_BUILD carries the patch.
LLVM_AS      ?= llvm-as
IR3_COMPILER ?= $(MESA_BUILD)/src/gallium/drivers/freedreno/ir3_compiler
KERNEL_LL        = $(wildcard tools/kernels/*.ll)
KERNEL_AIR_FIXTURES = $(patsubst tools/kernels/%.ll,tests/fixtures/%_air.hpp,$(KERNEL_LL))

.PHONY: air-fixtures
# keep the .bc: make would otherwise delete it as an intermediate and re-run llvm-as each time
.SECONDARY:

out/host/kernels/%.bc: tools/kernels/%.ll
	@mkdir -p $(dir $@)
	$(LLVM_AS) -o $@ $<

tests/fixtures/%_air.hpp: out/host/kernels/%.bc tools/kernels/%.ll
	$(IR3_COMPILER) --chip-id $(CHIP_ID) -o $@ -s tools/kernels/$*.ll -n $*_air $< > out/host/kernels/$*.listing

air-fixtures: $(KERNEL_AIR_FIXTURES)
