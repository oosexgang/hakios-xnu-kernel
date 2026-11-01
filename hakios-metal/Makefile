# hakios-metal: the Metal driver's core, and its tests.
#
#   make test            build and run the tests on this machine (macOS or Linux, any C++17 compiler), against the
#                        fake GPU (include/hakios-metal/fake_gpu.hpp), which runs the command streams it is given
#   make tsan            the same under ThreadSanitizer (out/host-thread/)
#   make asan            the same under AddressSanitizer (out/host-address/)
#   make check           the tests, then the Metal plug-in (plugin/) on this Mac twice: as hakios-xnu-kernel builds it without
#                        hakios-metal, and with it (plugin/tests/metal-plugin-test.sh; macOS only)
#   make ios             the library and the probe for iOS 16.5 on the phone (needs the iPhoneOS 16.5 SDK; THEOS
#                        or SDK= says where), into out/ios/ (make darwin: the same)
#   make clean

CXX       ?= c++
CXXFLAGS  ?= -O2 -g
# `override`: a CXXFLAGS given on the command line replaces the variable, and the flags below used to go with it
# (make test CXXFLAGS=-O1 then found no headers)
override CXXFLAGS += -std=c++17 -Wall -Wextra -Werror -Iinclude
SAN       ?=
ifneq ($(SAN),)
override CXXFLAGS += -fsanitize=$(SAN) -fno-omit-frame-pointer
endif
HOSTOUT   = out/host$(if $(SAN),-$(SAN))
.DEFAULT_GOAL := test

# The host tools that make shader kernels into fixtures (tools/kernels.mk), and the link of Mesa's compiler libraries
# (tools/compiler.mk). `make test MESA=1` puts the compiler into the test binary (against MESA_SRC/MESA_BUILD, a Mesa
# host build with hakios's AIR front end in it) and compiler_test compares it with the fixture; without MESA=1 the
# tests build on any C++17 and compileAir() is a refusal by name. Included here, before the rules, so that HOSTOUT and
# the flags are settled when the rules are read.
# the hakios tree (HAKIOS=), named before the fragments: compiler.mk reads the iOS Mesa build's -D set from
# $(HAKIOS)/xnu-deps/build/mesa at parse time, which an unset HAKIOS would turn into a silent empty set
HAKIOS    ?= ../workspace
-include tools/kernels.mk
-include tools/compiler.mk
ifneq ($(strip $(MESA)),)
override CXXFLAGS += -DHAKIOS_METAL_MESA $(HAKIOS_METAL_MESA_CXXFLAGS)
HOSTLIBS  += $(HAKIOS_METAL_MESA_LIBS)
HOSTOUT   = out/host-mesa$(if $(SAN),-$(SAN))
endif
# the SDK as hakios lays it out: theos, or on macOS xnu-deps/theos-mac
SDK       ?= $(abspath $(firstword $(wildcard $(THEOS)/sdks/iPhoneOS16.5.sdk $(HAKIOS)/xnu-deps/theos-mac/sdks/iPhoneOS16.5.sdk $(HOME)/theos/sdks/iPhoneOS16.5.sdk)))
ifeq ($(shell uname),Darwin)
IOS_CXX   ?= xcrun clang++ -arch arm64 -isysroot $(SDK) -miphoneos-version-min=16.0
IOS_AR    ?= ar rcs
else
# On Linux the cross compiler is hakios's wrapper around clang and the cctools linker (xnu-deps/host/bin, laid out by
# xnu-deps/setup.sh), and the archive must be in the form Apple's ld reads: with GNU ar's symbol table the probe failed
# to link against every symbol of the library.
IOS_CXX   ?= $(abspath $(HAKIOS))/xnu-deps/host/bin/xnu-clang++ -arch arm64 -isysroot $(SDK) -miphoneos-version-min=16.0
IOS_AR    ?= llvm-ar --format=darwin rcs
endif

CORE      = src/pm4.cpp src/gpu.cpp src/command.cpp src/compute.cpp src/texture.cpp src/render.cpp src/link.cpp src/compiler.cpp src/metallib.cpp src/c_api.cpp
HOST      = $(CORE) src/fake_gpu.cpp
TESTS     = $(wildcard tests/*_test.cpp) tests/main.cpp

.PHONY: test tsan asan check ios darwin clean sweep

# (and the front end's archive: a test binary linked before the front end changed passed or failed for the old one)
$(HOSTOUT)/tests: $(HOST) $(TESTS) $(wildcard include/hakios-metal/*.hpp) $(wildcard tests/fixtures/*.hpp) tests/test.hpp \
		$(if $(strip $(MESA)),$(MESA_BUILD)/src/compiler/air/libair.a)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(HOST) $(TESTS) $(HOSTLIBS)

# the metallib sweep (tools/sweep.cpp): every function of the libraries given through the compiler, the refusals counted;
# `make sweep MESA=1` builds $(HOSTOUT)/sweep, then run it on the libraries where they lie
$(HOSTOUT)/sweep: tools/sweep.cpp $(HOST) $(wildcard include/hakios-metal/*.hpp)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -pthread -o $@ tools/sweep.cpp $(HOST) $(HOSTLIBS)

sweep: $(HOSTOUT)/sweep

# (with the compiler in, compiler_test compiles every tools/kernels/*.ll against its fixture: their bitcode must be there)
test: $(HOSTOUT)/tests $(if $(strip $(MESA)),$(KERNEL_BC))
	$(HOSTOUT)/tests

tsan:
	$(MAKE) test SAN=thread

asan:
	$(MAKE) test SAN=address

check: test
	plugin/tests/metal-plugin-test.sh
	GPU=1 plugin/tests/metal-plugin-test.sh

ios: out/ios/hakios-metal-probe out/ios/libhakios-metal.a

darwin: ios

# with MESA=1 the compiler goes to the phone too, against the iOS build of Mesa's libraries (IOS_MESA_BUILD, tools/compiler.mk)
IOS_CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Werror
IOS_LIBS     =
ifneq ($(strip $(MESA)),)
IOS_CXXFLAGS += -DHAKIOS_METAL_MESA $(HAKIOS_METAL_IOS_MESA_CXXFLAGS)
IOS_LIBS     += $(HAKIOS_METAL_IOS_MESA_LIBS)
endif

out/ios/libhakios-metal.a: $(CORE) src/fake_gpu.cpp src/iokit_transport.cpp $(wildcard include/hakios-metal/*.hpp)
	@mkdir -p out/ios/obj
	cd out/ios/obj && for f in $(CORE) src/fake_gpu.cpp src/iokit_transport.cpp; do \
		$(IOS_CXX) $(IOS_CXXFLAGS) -I../../../include -c ../../../$$f || exit 1; done
	rm -f $@ && $(IOS_AR) $@ out/ios/obj/*.o

# (and the front end's archive, as the host tests: a probe linked before the front end changed tests the old one)
out/ios/hakios-metal-probe: tools/probe.cpp out/ios/libhakios-metal.a $(wildcard tests/fixtures/*.hpp) \
		$(if $(strip $(MESA)),$(IOS_MESA_BUILD)/src/compiler/air/libair.a)
	$(IOS_CXX) $(IOS_CXXFLAGS) -Iinclude -o $@ tools/probe.cpp out/ios/libhakios-metal.a $(IOS_LIBS) \
		-framework IOKit -framework CoreFoundation -framework IOSurface


# a kernel's bitcode as a header, for a probe that compiles it on the phone (our own kernels only: tools/kernels/*.ll)
tests/fixtures/%_bc.hpp: out/host/kernels/%.bc tools/bc-header.py
	python3 -I tools/bc-header.py $< $* > $@

clean:
	rm -rf out
