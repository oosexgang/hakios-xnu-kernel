# Metal measurements

What the phone did, in numbers. How to get the same numbers: [Metal on the Adreno](metal.md#measure). **120 frames a second has not
been reached**: not on hakios-metal, not on metal2vulkan, and not on OpenGL ES.

## The setup

| | |
|---|---|
| Phone | POCO X5 Pro 5G (`redwood`), SM7325 |
| GPU | Adreno 642L at 550 MHz, the most this phone's GPU is rated for (`adreno_speed=3`, the default) |
| User space | the Darwin 16.5 user space, laid out from your own IPSW |
| Panel | 1080 x 2400 at 120 Hz |
| Rendered | 1125 x 2436, scaled onto the panel by the driver |
| The run | `tools/metal-run.sh`: eight cores, `fb_hz=120 fb_threads=6 touch_poll_us=4000 wallpaper_freeze=1 metal_defer=1`, the round of `rc.jobs.bench` four times |

## The results

Frames a second, 3 and 4 November 2026. hakios-metal as the last run of 4 November had it: `wallpaper_freeze=1 metal_defer=1 fb_beam=1`.
metal2vulkan as measured on 3 November, before those.



| Motion | hakios-metal | metal2vulkan |
|---|---|---|
| Page swipe | 101 (100 to 104 over the runs) | 50 to 57 |
| Settings' scroll | 89 to 91 (109 in one run of six, not repeated) | 41 |
| Opening an app | about 90 | not recorded |
| Home button | 92 | 41 to 57 |
| Frames over 16.7 ms | 4% | not recorded |
| GPU time a frame | about 10 ms at the home screen, 3 November | 14 to 21 ms |

Both draw the system correctly. Not one of these is 120.

**hakios-metal, inside a swipe.** Once the finger has lifted, 185 of the animation's 205 frames take 8.3 ms, one refresh of the panel.
While the finger drags, frames come at 75 to 80 a second, and the window server waits 8 to 14 ms on GPU fences. The 100 to 104 in the
table covers both: `tools/frames.py` counts a gesture from the touch going down.

**metal2vulkan.** `tu_debug=gmem` and `tu_debug=sysmem` gave the same rates. Each frame renders to a tiled image and merges back into
the IOSurface with a compute pass. The linear attachment path (`VK_NV_linear_color_attachment`) stays unused: the pass has a second
attachment that is not linear, and the extension wants every colour attachment linear. With `tu_debug=perf`, no barrier turned GMEM
off and no texture was shadowed.


### Under real fingers (5 November)

Frames per second do not show what the eye calls jerking: the touch screen was read only when its interrupt line was seen low, and a report
that line was missed for waited for the look of every 80 ms.

| | Before | After (`drivers/input/hakios.goodix.c`) |
|---|---|---|
| A finger's lift after its last move | 88 to 169 ms | 5 ms, at most 6 |
| Between moves, over 13 ms apart | some | none of 164, at most 6 ms |

12 strokes kept in that log; `touch: N strokes` in the kernel's log now sums them up for a whole session. What is left of the jerking
at the start of a gesture is SpringBoard's main thread, before its commit: turns of 100 to 300 ms, 40 to 100 ms after the finger comes down
(`tl: t` in the log has its stack, with `touch_lat=1`). A share of a frame's rows late for its scan is counted too: 19 to 115 in a session.

### The iPhone X's screen at 2 pixels a point (4 November)

`fbscale=2`, the default of `tools/metal-run.sh`: 750x1624, stretched to the panel by the CPU, the layout of the iPhone X's 375 x 812 points with 2.25
times fewer pixels for the GPU. The picture is softer than at 1125x2436, nearest neighbour while something moves and bilinear once it is still. A main
pass is 5 ms in 21 tiles. UIKit has no status bar for this screen: SpringBoard (`inject_sb.c`) makes its fallback the iPhone X's.

| Gesture (`rc.jobs.bench`, 4 runs) | Frames a second |
|---|---|
| Settings' scroll | 118 to 120 |
| Page swipe | 107 to 118 |
| Opening Settings | 107 to 108 |
| Home button | 95 to 111 |
| Frames over 16.7 ms | 1.1 to 1.2% |

The scroll is at the panel's rate. The home button and the opening of an application are held by the GPU: two passes of 7 to 8 ms for a frame, more than the
8.33 ms of a refresh, so those frames come a refresh late. The hitches at the start of a gesture are SpringBoard's main thread: 10 of its 4745 turns in a run
were over 30 ms, the longest 101. These are synthetic gestures (`rwtouch`); fingers cost more.

The framebuffer logs, for each thread's share of the rows, its time and how often it was late for the scan: 1 to 9 torn of about 49 000 shares in a run, all in the
first two shares, whose rows the scan comes to first. The last shares take 2 to 3 ms and were never late. This counts against the scan as the interface's
counters report it; what the panel shows has not been measured independently, and a phone's camera sees lines of its own on a panel refreshing at 120.

### The screen as the iPhone XR's (4 November)

`fbwidth=828 fbheight=1792 fbscale=2 fbsubtype=1792`, the default until the iPhone X's at 2 replaced it (under real fingers the bottom edge opened a sheet here), stretched to the panel by the CPU. 1.85 times fewer
pixels for the GPU; the picture is softer than at 1125x2436. The poster draws nothing on this screen: `wallpaper_freeze=1` shows the picture a
boot at the iPhone X's screen kept in `/var/mobile/Library/hakios-wallpaper.raw`.

| Gesture (`rc.jobs.bench.xr`) | Frames a second | At 1125x2436 |
|---|---|---|
| Settings' scroll | 118 | 89 to 91 |
| Page swipe | 110 to 115 | 101 to 103 |
| Home button | 106 | 89 to 92 |
| Opening Settings | 86 | about 95 |
| Frames over 16.7 ms | 1% | 4 to 6% |

While something moves, half the frames are 8.33 ms apart and most of the rest one refresh more. What keeps the averages under 120: SpringBoard's main
thread at the start of a gesture (36 to 57 ms) and of an application (104 ms), frames the GPU delivers a refresh late now and then, and the
measured stretch ending in the slow ticks of a still screen. These are synthetic gestures (`rwtouch`).

## Tried, and it did not help

| What | Arguments | Result |
|---|---|---|
| The threads that copy frames real-time, with the scan as their deadline | a build with a time-constraint policy, since removed | 684 shares late in a session, against 115 |
| Bilinear on every frame, so that nothing jumps when motion starts or stops | `fb_filter=1` | under real fingers it jerks as much, and 125 shares late against 19 to 43 |
| The stretch written by runs of a source column, a row the same as the one above it copied from there | in `MiniIOMobileFramebuffer` | worse: 8.7 ms in the driver a frame against 7 to 8, every swap waiting for the frame before, 102 frames a second against 110 |
| Touch samples at 250 a second, not 125 | `touch_poll_us=4000`, `rwtouch` every 4 ms | swipes 100 to 102: unchanged |
| Surfaces let go of as soon as their pixels are on the panel, not when the next frame comes | in `MiniIOMobileFramebuffer` | unchanged |
| The iPhone XR's screen class, 1.85 times fewer pixels | `fbwidth=828 fbheight=1792 fbscale=2 fbsubtype=1792` | idle GPU time 2.2 ms, not 6.8; gestures not faster; the wallpaper black |
| The iPhone 12 mini's screen class | `fbwidth=`, `fbheight=`, `fbscale=`, `fbsubtype=` | 103 to 107, with the red rdar://45025538 status bar |
| The wallpaper hidden, for swipes | `wallpaper_freeze=2` | 101 to 103, as with it: the wallpaper is not what holds swipes under 120. It is what held the home button: 78 hidden, against 28 with `wallpaper_cache=1` |
| The wallpaper's picture through `-drawViewHierarchyInRect:` | `wallpaper_freeze=1`, first version | one colour; with all of SpringBoard's windows hidden the screen itself was black. It now reads the IOSurface on the panel |

## What did help

On hakios-metal, each as the commit that made the change measured it, newest first.

| Change | Before | After |
|---|---|---|
| `wallpaper_freeze=1 metal_defer=1` | home button 50 (28 with `wallpaper_cache=1`); Settings' scroll 90; frames over 16.7 ms 9% | home button 89 to 92; scroll 91 (109 once); 4 to 6%; swipes 101 to 103 |
| Eight cores, not one, for the first boot | first frame at 159 s | 86 s |
| `fb_threads=6`, not 3 | Settings' scroll 75 | 84 |
| `wallpaper_cache=1` | swipes 85 to 91; an app's close 40 | swipes 97 to 103; an app's close 22 to 30, slower: the poster changes under its zoom |
| `ui_lite=1` | an app's close 40 | 55 to 58, with Reduce Transparency and Reduce Motion on |
| `luma_boost` left out, where QuartzCore's library has its own | 83% of the GPU's timed passes | gone; `metal_luma_boost=1` draws it |

## metal2vulkan, on the way

| When (3 November) | What | Result |
|---|---|---|
| `vk_test` | Turnip alone | a submission and its fence wait 0.5 ms at best, 2.0 on average |
| `m2v_test` | the Metal device alone | a full-screen clear committed and waited for in 1.08 ms |
| the window server, first | **the picture was wrong**: grey shapes, the variants not chosen yet | 3456 command buffers, 0 failed, 226k draws in 150 s; swipes 71 to 80, scroll 47, an app's open 25, GPU 17 to 36 ms a frame |
| the window server, right | the variants chosen by label, own pages as textures | swipes 50 to 57, scroll 41 |

The first window-server numbers are higher than the later ones because less was being drawn. They are here so that nobody takes them
for a result.

## OpenGL ES, for comparison

The default path, which is not Metal: Settings scrolled at 107 to 114 a second on the four big cores (`cpus=4`, `fb_hz=120 ca_vbl=1`,
31 October, [hakios-sm7325](https://github.com/oosexgang/hakios-sm7325) `a8e7a87`). The conditions differ from the table above in the core count and in the day's kernel, so that is a reference point and not
a comparison run.

## Tearing

The panel reads the very buffer the driver writes. It takes the scan 8.3 ms to read it and the driver about 3 ms to write it, so a frame
written across the line being read came out torn: a piece of the last frame and a piece of this one, moving bands with doubled icons and
text wherever something moved. Seen on the phone.

`fb_beam=1`, the default, writes each thread's rows behind the scan line, read from the interface's own `INTF_LINE_COUNT`, so that the next
scan reads all of the new frame. The scan measured 2433 lines, 8338 us. It is looked at a minute after the driver starts; if the counter
does not count as a line counter should, frames stay on their beat, and the log says so. 2 to 16 frames in 5 s miss their scan.

The bands were gone to the eye. That is the only evidence: `xl screenshot` cannot tell, because it sends live memory in pieces of 1 MiB, and
the bands in its pictures are 243 rows apart.

It costs no frames. The same run otherwise, 4 November:



| | `fb_beam=1` | `fb_beam=0` |
|---|---|---|
| Page swipe | 101 | 99 |
| Settings' scroll | 91 | 89 |
| Home button | 92 | 81 |
| Frames over 16.7 ms | 4% | 6% |
