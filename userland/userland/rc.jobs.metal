# Commands run by the launchd job com.redwood.demo (see com.redwood.demo.plist).
# What iOS here believes about Metal (RC_JOBS=rc.jobs.metal, a run of half a minute): the services Metal
# looks for and does not find, the GPU the registry does have under another name, whether a plug-in bundle
# of this image would load at all, the nil a program gets, what MobileGestalt still says about graphics,
# and whether CoreImage can run a filter kernel without Metal. See src/metal_test.m and docs/metal.md.
#
# The wait is so that the run is a realistic one: by then SpringBoard is up and, with gpu=1 on the
# phone, the window server already holds the GPU and draws with it through src/eagl.c. That is the
# state in which the question about CoreImage over an EAGLContext of ours is worth asking.
# Mind the length of the run: on the phone SpringBoard is up in about 20 s, but in QEMU it took 208
# ("sound_test: the ringer is on after 208 s"), so a QEMU session wants 480 s or more -- a shorter
# one ends before metal_test is reached at all.
/redwood/bin/sound_test springboard
/redwood/bin/rwtouch wait 25
/redwood/bin/metal_test
