# etc/rc for METAL=1: put our Metal plug-in's name on IOAcceleratorES (needs boot-arg metal=1), then
# hand process 1 to the iOS launchd. The path is relative to /System/Library/Extensions, so the bundle
# stays in /redwood/lib and the system image is not touched.
/redwood/bin/tolog /redwood/bin/metal_props on ../../../redwood/lib/RedwoodMetal RedwoodMetalDevice
exec /sbin/launchd.ios
