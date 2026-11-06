# Metal on the X6826B: the plan (agreed 27 October 2026)

The Mali draws CoreAnimation fast enough (92–96% of frames within 18 ms opening apps) but not
right: mixed strips, empty widgets, a darkened half-screen. Fixing what the user's videos show,
one symptom at a time, has stopped converging. The AIR → NIR → panfrost translation stays; what
changes is how correctness is checked, and the layer that orders and synchronises the work.

Done when: the reference scenes match pixel for pixel; no frame on the panel is two frames;
95% of frames within 18 ms opening and closing apps.

Order: 0 → 4 → 1 → 2 → 3 → 6.

## 0. Fix the starting point
Scissor, viewport and `ca_dirty=0` committed (95db131). Run 222: full redraw does not remove the
strips, so they are not CoreAnimation's dirty regions.

## 4. Synchronisation and the display
Every command buffer has a fence; the display and `IOSurface` locks wait for exactly that, with
no timeouts and no fallbacks. Detector first: RedwoodMali counts surfaces that begin to be drawn
while the display still holds them ("drawn into while the display holds it"). Then a tearing test:
each frame filled with its number as a colour, and no frame on the panel made of two.
Suspects: eight surfaces per command buffer at most; a second buffer's mark over the first's;
`waitUntilScheduled` answering early; the queue of three swaps.

Found (runs 232-242, 27 October): the "breakcore" -- doubled icons, the widget stack drawn as an
"H", icons cut in half while paging -- is gone with the driver fully synchronous, `rwm_async=0
rwm_finish=1` (runs 234 and 242, seen by eye). OpenGL ES (`camtl=0`, run 233) is clean too, so
the kernel, the display and panfrost are not it; neither is `waitUntilScheduled` (run 240,
`rwm_sched_wait=1`, still broken) nor scratch in registers (run 237, `scratchregs=0`). The fault
is in the asynchronous path: something of a frame is reused while the worker thread or the GPU
still reads it. What is left is to find it and keep the speed, so the micro-stalls of the
synchronous mode go. The run that works:

    cpus=4 touchscreen=2 buttons=2 mali=1 mtk_cpufreq=3 mali_mhz=800 fb_queue=1 fb_flip=1
    fbscale=2 fbfit=3 metal=1 camtl=1 metal_profile=4 rwm_async=0 rwm_finish=1

(`fbscale=2 fbfit=3`, the picture cut off at the bottom: without it the kernel scales every frame,
7 ms each.) For telling the rest apart, the panfrost patch can now turn off its own changes one by
one: `scratchregs=0`, `widen16=0`, `pushubo=0`. The `fb_flicker=1` count does not tell the
"breakcore" from ordinary animation (1303 of 4994 frames on OpenGL ES, 1305 of 4890 on Metal).

## 1. Record and replay
Record on the phone (debug build only, boot_a has ~18 KB to spare) everything CoreAnimation
gives Metal in a frame: functions, buffers, textures, state, passes. Replay one frame three ways:
real Metal on the Mac (the reference), our driver on the Mac in software (translation and driver
logic), our driver on the Mali (what is the Mali's). Where they differ says which layer is wrong.
Settled 27 October: the phone already runs the AIR slice of macOS QuartzCore's `default.metallib`,
which this Mac's Metal loads as it is. Our side on the Mac: `tools/mesa/host-air.sh` builds
`air_metal.c` on softpipe with `air_metal_test`. Next obstacle: softpipe aborts on 16-bit
operations (`nir_to_tgsi`), which CoreAnimation's shaders use -- run 223, Metal through softpipe
on the phone (`caenv=RWA_SOFTWARE=1`), took backboardd down in `rwa_render_create`. The software
path needs the 16-bit lowered to 32 before it can be a reference.
A run's log is a 256 KB ring: what the driver says once, early, is gone by the end. Since
27 October the first of each NOT DONE / refusal sentence is also a kept kernel line (`!rwm`).

## 5. Merge `origin/air` -- nothing to merge
Checked 27 October: `origin/air` ends at b2e79ed, the very commit this branch copied the driver
from (d5061e6), and `friend/metal` is the same driver renamed for HackiOS (`hkmetal.m`, same
6 333 lines, `air_to_nir.c` identical). The 282 commits are history already in that copy.

## 2. Every CoreAnimation shader
QuartzCore's `default.metallib` is a finite set: each function translated, run on test inputs,
compared with the reference. A table of verdicts, brought to 100%.

## 3. The whole Metal API surface (556 methods)
Count which methods CoreAnimation calls and how often; sort them into implemented / empty and
harmless / empty and wrong. Empty today: cull mode, winding, fill mode, blend colour, depth bias,
store actions, memory and texture barriers, fences and events, visibility results; no MSAA.
Each one implemented gets a scene from stage 1.

## 6. Speed
After the picture is right: the kernel, drm and queue speedups kept, retuned to the new
synchronisation, 60 frames a second held.

## Alongside, optional
OpenGL ES (`camtl=0`) measured with today's kernel and drm, as a stable fallback while Metal is
rebuilt.
