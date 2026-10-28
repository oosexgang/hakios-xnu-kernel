# tools/compiler.mk: Mesa's compiler libraries linked into the driver, on the
# host (the tests) and for iOS (the phone compiles the image's own .metallib
# files where they are). Included by the main Makefile.
#
# SPDX-License-Identifier: MIT
#
#   make compiler-smoke       build out/host/compiler-smoke against the host Mesa build and run it on
#                             tools/kernels/fill_id.ll: device row, AIR -> NIR, NIR -> ir3, the listing
#   make ios-compiler-smoke   cross-compile the same program and link it with the iOS archives into
#                             out/ios/compiler-smoke (a Mach-O; not run: there is no phone here)
#
# What the main Makefile takes from here:
#   HAKIOS_METAL_MESA_CXXFLAGS      the -isystem includes (source + generated dirs, src/compiler/air among
#                                   them) and the -D set the host Mesa build compiled its own objects with
#   HAKIOS_METAL_MESA_LIBS          libair + the ir3/nir/util archives of that build, -lz -lm -pthread
#   HAKIOS_METAL_IOS_MESA_CXXFLAGS  the same for the iOS cross build (its own -D set: no glibc, has sysctl)
#   HAKIOS_METAL_IOS_MESA_LIBS      the same archives of the cross build, -lz -lm (ld64 needs no group)
#
# The host half reuses kernels.mk's variables (MESA_SRC, MESA_BUILD, MESA_DEFS,
# MESA_INC, MESA_ARCHIVE_PATHS, MESA_LIBS): that file is included here when the
# Makefile has not done so already, and nothing in it is redefined. The iOS half
# mirrors them with IOS_ names against the cross build.
ifeq ($(origin MESA_INC), undefined)
include tools/kernels.mk
endif

# --- host ---------------------------------------------------------------------
# The front end's own include dir: `#include "nir_air.h"` as the Mesa tools do.
HAKIOS_METAL_MESA_CXXFLAGS = $(MESA_DEFS) $(MESA_INC) -isystem $(MESA_SRC)/src/compiler/air
# libair first in the group: it needs nir and util, nothing needs it but the driver.
HAKIOS_METAL_MESA_LIBS = -Wl,--start-group $(MESA_BUILD)/src/compiler/air/libair.a $(MESA_ARCHIVES) -Wl,--end-group -lz -lm -pthread

# --- iOS ----------------------------------------------------------------------
# The cross build: Mesa built for arm64 iOS by hakios's tools/mk-mesa.sh (meson
# cross file xnu-deps/build/mesa/cross.ini, the same xnu-clang the Makefile
# uses), with the AIR front end from hakios's tools/mesa/air copied in. Its
# libair.a is not a target of the default build (nothing of libOSMesa links it):
# `ninja -C <build> src/compiler/air/libair.a` once after mk-mesa.sh. Override
# IOS_MESA_SRC and IOS_MESA_BUILD for a build elsewhere.
IOS_MESA_ROOT  ?= $(abspath $(HAKIOS))/xnu-deps/build/mesa
IOS_MESA_SRC   ?= $(IOS_MESA_ROOT)/mesa-25.0.7
IOS_MESA_BUILD ?= $(IOS_MESA_ROOT)/build-softpipe-freedreno

# Its -D set, read the same way as the host's (kernels.mk): off the ir3.c.o line
# of its ninja file. It differs from the host's where the libc does (no
# HAVE_SECURE_GETENV, HAVE_BSD_QSORT_R instead of GNU's, HAVE_SYS_SYSCTL_H,
# NDEBUG), and a header compiled with the host's set would declare things the
# iOS objects do not have.
ifeq ($(origin IOS_MESA_DEFS), undefined)
IOS_MESA_DEFS := $(shell grep -m1 -A4 '^build src/freedreno/ir3/libfreedreno_ir3.a.p/ir3.c.o:' $(IOS_MESA_BUILD)/build.ninja \
	| grep -m1 ' ARGS = ' | tr ' ' '\n' | grep '^-D' | tr '\n' ' ')
# an empty set compiles Mesa's headers for the wrong libc (c11/threads.h: "Not supported on this platform"): say so
ifeq ($(strip $(IOS_MESA_DEFS)),)
$(warning tools/compiler.mk: no -D set for the iOS Mesa build in $(IOS_MESA_BUILD)/build.ninja (not built, or HAKIOS unset?); make ios MESA=1 will not compile)
endif
endif

IOS_MESA_INC = \
	-isystem $(IOS_MESA_SRC)/src -isystem $(IOS_MESA_SRC)/include \
	-isystem $(IOS_MESA_SRC)/src/freedreno -isystem $(IOS_MESA_SRC)/src/freedreno/common \
	-isystem $(IOS_MESA_SRC)/src/compiler -isystem $(IOS_MESA_SRC)/src/compiler/nir \
	-isystem $(IOS_MESA_SRC)/src/compiler/air \
	-isystem $(IOS_MESA_SRC)/src/gallium/include \
	-isystem $(IOS_MESA_BUILD)/src -isystem $(IOS_MESA_BUILD)/src/compiler -isystem $(IOS_MESA_BUILD)/src/compiler/nir \
	-isystem $(IOS_MESA_BUILD)/src/freedreno -isystem $(IOS_MESA_BUILD)/src/freedreno/common \
	-isystem $(IOS_MESA_BUILD)/src/freedreno/isa

HAKIOS_METAL_IOS_MESA_CXXFLAGS = $(IOS_MESA_DEFS) $(IOS_MESA_INC)
# The same archive list as the host's, under the cross build (libmesa_util_sse41.a
# exists there too, empty on arm64, and is harmless). ld64 resolves across all
# archives whatever their order, so there is no group to open. zlib and libm are
# in the iOS SDK (libz.tbd, libm as part of libSystem); nothing in this set
# needs a framework -- those come with libfreedreno_drm, which the driver does
# not link.
HAKIOS_METAL_IOS_MESA_LIBS = $(IOS_MESA_BUILD)/src/compiler/air/libair.a $(addprefix $(IOS_MESA_BUILD)/,$(MESA_ARCHIVE_PATHS)) -lz -lm

# The cross compiler as the main Makefile names it; these are no-ops when it is
# included from there, and let `make -f tools/compiler.mk` work alone.
HAKIOS    ?= ../workspace
SDK       ?= $(abspath $(firstword $(wildcard $(THEOS)/sdks/iPhoneOS16.5.sdk $(HAKIOS)/xnu-deps/theos-mac/sdks/iPhoneOS16.5.sdk $(HOME)/theos/sdks/iPhoneOS16.5.sdk)))
IOS_CXX   ?= $(abspath $(HAKIOS))/xnu-deps/host/bin/xnu-clang++ -arch arm64 -isysroot $(SDK) -miphoneos-version-min=16.0

# --- the smoke program ----------------------------------------------------------
.PHONY: compiler-smoke ios-compiler-smoke

out/host/compiler-smoke: tools/compiler-smoke.cc tools/compiler.mk tools/kernels.mk
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O1 -g -Iinclude $(HAKIOS_METAL_MESA_CXXFLAGS) -o $@ tools/compiler-smoke.cc $(HAKIOS_METAL_MESA_LIBS)

compiler-smoke: out/host/compiler-smoke out/host/kernels/fill_id.bc
	out/host/compiler-smoke --chip-id $(CHIP_ID) out/host/kernels/fill_id.bc

out/ios/compiler-smoke: tools/compiler-smoke.cc tools/compiler.mk tools/kernels.mk
	@mkdir -p $(dir $@)
	$(IOS_CXX) -std=c++17 -Wall -Wextra -Werror -O2 -Iinclude $(HAKIOS_METAL_IOS_MESA_CXXFLAGS) -o $@ tools/compiler-smoke.cc $(HAKIOS_METAL_IOS_MESA_LIBS)

ios-compiler-smoke: out/ios/compiler-smoke
	file $<
