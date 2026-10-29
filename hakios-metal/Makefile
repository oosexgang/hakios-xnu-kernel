# hakios-metal: the Metal driver's core, and its tests.
#
#   make test            build and run the tests on this machine (macOS or Linux, any C++17 compiler), against the
#                        fake GPU (include/hakios-metal/fake_gpu.hpp), which runs the command streams it is given
#   make darwin             the library and the probe for Darwin userland 16.5 on the phone (needs the iPhoneOS 16.5 SDK; THEOS
#                        or SDK= says where), into out/darwin/
#   make clean

CXX       ?= c++
CXXFLAGS  ?= -O2 -g
CXXFLAGS  += -std=c++17 -Wall -Wextra -Werror -Iinclude
# the SDK as hakios lays it out: theos, or on macOS xnu-deps/theos-mac (HAKIOS= the hakios tree)
HAKIOS    ?= ../workspace
SDK       ?= $(abspath $(firstword $(wildcard $(THEOS)/sdks/iPhoneOS16.5.sdk $(HAKIOS)/xnu-deps/theos-mac/sdks/iPhoneOS16.5.sdk $(HOME)/theos/sdks/iPhoneOS16.5.sdk)))
DARWIN_CXX   ?= xcrun clang++ -arch arm64 -isysroot $(SDK) -miphoneos-version-min=16.0

CORE      = src/pm4.cpp src/gpu.cpp src/command.cpp src/metallib.cpp src/c_api.cpp
HOST      = $(CORE) src/fake_gpu.cpp
TESTS     = $(wildcard tests/*_test.cpp) tests/main.cpp

.PHONY: test darwin clean

out/host/tests: $(HOST) $(TESTS) $(wildcard include/hakios-metal/*.hpp) tests/test.hpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(HOST) $(TESTS)

test: out/host/tests
	out/host/tests

darwin: out/darwin/hakios-metal-probe out/darwin/libhakios-metal.a

out/darwin/libhakios-metal.a: $(CORE) src/fake_gpu.cpp src/iokit_transport.cpp $(wildcard include/hakios-metal/*.hpp)
	@mkdir -p out/darwin/obj
	cd out/darwin/obj && for f in $(CORE) src/fake_gpu.cpp src/iokit_transport.cpp; do \
		$(DARWIN_CXX) -std=c++17 -O2 -Wall -Wextra -Werror -I../../../include -c ../../../$$f || exit 1; done
	rm -f $@ && ar rcs $@ out/darwin/obj/*.o

out/darwin/hakios-metal-probe: tools/probe.cpp out/darwin/libhakios-metal.a
	$(DARWIN_CXX) -std=c++17 -O2 -Wall -Wextra -Werror -Iinclude -o $@ tools/probe.cpp out/darwin/libhakios-metal.a \
		-framework IOKit -framework CoreFoundation

clean:
	rm -rf out
