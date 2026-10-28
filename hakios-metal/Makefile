# hakios-metal: the Metal driver's core, and its tests.
#
#   make test            build and run the tests on this machine (macOS or Linux, any C++17 compiler), against the
#                        fake GPU (include/hakios-metal/fake_gpu.hpp), which runs the command streams it is given
#   make ios             the library and the probe for iOS 16.5 on the phone (needs the iPhoneOS 16.5 SDK; THEOS
#                        or SDK= says where), into out/ios/
#   make clean

CXX       ?= c++
CXXFLAGS  ?= -O2 -g
CXXFLAGS  += -std=c++17 -Wall -Wextra -Werror -Iinclude
# the SDK as hakios lays it out: theos, or on macOS xnu-deps/theos-mac (HAKIOS= the hakios tree)
HAKIOS    ?= ../workspace
SDK       ?= $(abspath $(firstword $(wildcard $(THEOS)/sdks/iPhoneOS16.5.sdk $(HAKIOS)/xnu-deps/theos-mac/sdks/iPhoneOS16.5.sdk $(HOME)/theos/sdks/iPhoneOS16.5.sdk)))
IOS_CXX   ?= xcrun clang++ -arch arm64 -isysroot $(SDK) -miphoneos-version-min=16.0

CORE      = src/pm4.cpp src/gpu.cpp src/command.cpp src/metallib.cpp src/c_api.cpp
HOST      = $(CORE) src/fake_gpu.cpp
TESTS     = $(wildcard tests/*_test.cpp) tests/main.cpp

.PHONY: test ios clean

out/host/tests: $(HOST) $(TESTS) $(wildcard include/hakios-metal/*.hpp) tests/test.hpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(HOST) $(TESTS)

test: out/host/tests
	out/host/tests

ios: out/ios/hakios-metal-probe out/ios/libhakios-metal.a

out/ios/libhakios-metal.a: $(CORE) src/fake_gpu.cpp src/iokit_transport.cpp $(wildcard include/hakios-metal/*.hpp)
	@mkdir -p out/ios/obj
	cd out/ios/obj && for f in $(CORE) src/fake_gpu.cpp src/iokit_transport.cpp; do \
		$(IOS_CXX) -std=c++17 -O2 -Wall -Wextra -Werror -I../../../include -c ../../../$$f || exit 1; done
	rm -f $@ && ar rcs $@ out/ios/obj/*.o

out/ios/hakios-metal-probe: tools/probe.cpp out/ios/libhakios-metal.a
	$(IOS_CXX) -std=c++17 -O2 -Wall -Wextra -Werror -Iinclude -o $@ tools/probe.cpp out/ios/libhakios-metal.a \
		-framework IOKit -framework CoreFoundation

clean:
	rm -rf out
