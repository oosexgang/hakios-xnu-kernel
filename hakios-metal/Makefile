# hakios-metal: the Metal driver's core, and its tests.
#
#   make test            build and run the tests on this machine (macOS or Linux, any C++17 compiler), against the
#                        fake GPU (include/hakios-metal/fake_gpu.hpp), which runs the command streams it is given
#   make tsan            the same under ThreadSanitizer (out/host-thread/)
#   make asan            the same under AddressSanitizer (out/host-address/)
#   make ios             the library and the probe for iOS 16.5 on the phone (needs the iPhoneOS 16.5 SDK; THEOS
#                        or SDK= says where), into out/ios/
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
# the SDK as hakios lays it out: theos, or on macOS xnu-deps/theos-mac (HAKIOS= the hakios tree)
HAKIOS    ?= ../workspace
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

CORE      = src/pm4.cpp src/gpu.cpp src/command.cpp src/compute.cpp src/metallib.cpp src/c_api.cpp
HOST      = $(CORE) src/fake_gpu.cpp
TESTS     = $(wildcard tests/*_test.cpp) tests/main.cpp

.PHONY: test tsan asan ios clean

$(HOSTOUT)/tests: $(HOST) $(TESTS) $(wildcard include/hakios-metal/*.hpp) $(wildcard tests/fixtures/*.hpp) tests/test.hpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(HOST) $(TESTS)

test: $(HOSTOUT)/tests
	$(HOSTOUT)/tests

tsan:
	$(MAKE) test SAN=thread

asan:
	$(MAKE) test SAN=address

ios: out/ios/hakios-metal-probe out/ios/libhakios-metal.a

out/ios/libhakios-metal.a: $(CORE) src/fake_gpu.cpp src/iokit_transport.cpp $(wildcard include/hakios-metal/*.hpp)
	@mkdir -p out/ios/obj
	cd out/ios/obj && for f in $(CORE) src/fake_gpu.cpp src/iokit_transport.cpp; do \
		$(IOS_CXX) -std=c++17 -O2 -Wall -Wextra -Werror -I../../../include -c ../../../$$f || exit 1; done
	rm -f $@ && $(IOS_AR) $@ out/ios/obj/*.o

out/ios/hakios-metal-probe: tools/probe.cpp out/ios/libhakios-metal.a $(wildcard tests/fixtures/*.hpp)
	$(IOS_CXX) -std=c++17 -O2 -Wall -Wextra -Werror -Iinclude -o $@ tools/probe.cpp out/ios/libhakios-metal.a \
		-framework IOKit -framework CoreFoundation

# the host tools that make shader kernels into fixtures (tools/kernels.mk, when it is there)
-include tools/kernels.mk

clean:
	rm -rf out
