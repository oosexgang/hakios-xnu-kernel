# Commands init runs after its own checks: one per line, words separated by blanks.
#
# The first experiment with Metal's device discovery (docs/metal.md 10.14 and 10.31), and
# deliberately the smallest one: with this file as etc/rc (RC=rc.ios.metal) init runs these lines
# itself and halts, so launchd is never born and nothing in the system can ask Metal for a device.
# What is measured here is only whether the two properties can be put on the IOAcceleratorES
# service from user space -- nothing about what Metal then does with them.
#
# It needs the kernel booted with metal=1 (iokit/Kernel/MiniIOMobileFramebuffer.cpp); without it
# every "on"/"off" line below answers 0xe00002c7 and the properties stay (none).
#
# And one thing about that kernel, for whoever boots it next and not for this run: a kernel
# built in a working copy where tools/mk-ath11k.sh has not run has no Wi-Fi driver in it (weak
# stubs, bsd/dev/redwood_lx.c: "lx: THE DRIVER IS NOT BUILT IN"), which costs this experiment
# nothing and costs a boot with wifi=1 everything.
/redwood/bin/metal_props show
/redwood/bin/metal_props on
# the registry's own answer, from a program of Apple's that knows nothing about any of this
/usr/sbin/ioreg -l -c IOAcceleratorES
# and off again: the way out without a reboot, measured in the same run
#
# And then the whole chain, from a program that does NOT load our dylib by hand: metal_test
# asks Metal the way any process does. With the two properties on the service and the bundle
# at the path step 3 composes, MTLCreateSystemDefaultDevice() has to find us by itself --
# steps 1 and 2 enumerate the service and read the properties, 3 opens the bundle, 4 checks
# the class is a subclass of _MTLDevice, 5 initialises it. Every step before this has been
# passed by hand (docs/metal.md 10.14); this is the first time Metal walks the whole way.
#
# It is run here, under RC=rc.ios.metal, for the reason 10.14 gives: launchd is never born,
# so no daemon can ask Metal and meet a half-made device. The properties are removed again
# on the line after it, so nothing outlives this list.
/redwood/bin/metal_test
/redwood/bin/metal_props off
