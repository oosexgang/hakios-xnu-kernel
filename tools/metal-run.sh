#!/bin/bash
# One Metal run on the phone, measured: the window server drawing through Metal (an image built with METAL_PLUGIN=ca),
# the frame-rate bench driving it, and what came of it in one summary. Git Bash on Windows, macOS and Linux alike.
#
#   tools/metal-run.sh                  the run (the phone may be in Android, in its bootloader or in the loader)
#   SECS=180 tools/metal-run.sh         its length in seconds (default 180: the bench's four rounds; see rc.jobs.bench)
#   MORE="metal_gmem=1" tools/...       boot-args beside the ones below; one of the same name takes a default's place
#   PERSIST=0 tools/metal-run.sh        without var_store: /private/var starts empty and nothing is written to darwin
#   UPLOAD=0 tools/metal-run.sh         keep the system image already in the phone's RAM (default: upload this copy's)
#   (bench at this screen: RC_JOBS=rc.jobs.bench.xr, Settings lies lower; UPLOAD=0 keeps the image in the phone's RAM)
#   SHOTS=N                             a screenshot every N seconds during the run into logs/<stamp>-shots/ (default 0)
#   VERBOSE=1 tools/metal-run.sh        with -v: the kernel's text console drawn over the picture (off by default: the log has it all)
#
# What it needs, built in WSL (docs/windows.md) and copied here: userland/root.apfs and userland/system.apfs from
#   METAL_PLUGIN=ca RC_JOBS=rc.jobs.bench EXTRA_SHARE=<dir of QuartzCore-default.metallib> REST=1 SYSTEM=1 userland/build.sh
# (tools/ca-metallib.py writes that file; off a Mac with CA_QC_METALLIB=...). The kernel: hakios-kernel, else prebuilt/.
#
# Boot-args, and why each:
#   gpu=1 metal_fps=1 touch_lat=1   the GPU on, Metal's frame and pass counters, the "tl:" lines tools/frames.py reads
#   metal=1                         the accelerator names the Metal plug-in (MiniIOMobileFramebuffer.cpp): without it
#                                   the window server finds no device and stays on OpenGL ES, saying nothing
#   touchscreen=1                   the bench taps and swipes through the touch screen's driver
#   ca_metallib=...                 QuartzCore's library for the window server (tools/ca-metallib.py)
#   gpu_recover=1                   a GPU hang is reset and counted, not the end of the run
#   anim_max=1                      SpringBoard's animations ask for 80..120 frames a second
#   bootcpu=7 cpus=8 cpufreq=1      the kernel on a big core with all eight and their clocks (docs/u-boot.md): xl's own
#                                   default is one small core, on which the first boot is still compiling CoreAnimation's
#                                   shaders when the bench ends (2 November: first frame at 159 s on one core, 86 s on eight)
#   fb_hz=120 fb_measure=1          the framebuffer's beat at the panel's 120 Hz, not its default 60, and the panel's own
#                                   rate measured once (MiniIOMobileFramebuffer.cpp)
#   fb_threads=6                    six processors scale a frame onto the panel, not three: Settings' scroll 75 -> 84 frames
#   touch_poll_us=4000              the touch screen looked at every 4 ms, not 8: a sample in every refresh at 120 Hz
#   wallpaper_freeze=1              the wallpaper one picture once at the home screen (the icons blink once): home button 28 -> 92 frames
#   metal_defer=1                   a command buffer's draws recorded when it is committed, not on the way
#   fbscale=2                       the iPhone X's 375 x 812 points at 2 pixels a point (750 x 1624, 2.25 times fewer pixels), stretched to the panel:
#                                   110 frames a second over the bench's gestures, 1.1% over 16.7 ms (4 runs, 4 November, tools/frames.py). Its status
#                                   bar comes from SpringBoard (inject_sb.c). The iPhone XR's class (fbwidth=828 fbheight=1792 fbsubtype=1792) drew
#                                   a little more and at it the bottom edge opened Control Center under real fingers
#   ufs=2 ufs_write=1 var_store=1   (PERSIST=1, the default) /private/var kept in the partition darwin between boots: the
#                                   compiled shader stages in /var/db/hakios-metal-cache survive, so only the first boot
#                                   ever compiles them (tools/metal-cache-seed.sh then puts them in the image)
# The summary: logs/<stamp>-metal.txt -- tools/frames.py's frames per gesture, the HakiosMetal fps lines (the slowest
# passes, each sysmem one with its reason), refusals counted by kind, and var_store's save.
set -u
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1
case "$(uname -s)" in
MINGW*|MSYS*|CYGWIN*)
	# Git Bash: tools/win-bin's flock, and Google's platform-tools where docs/windows.md puts them
	export PATH="$PWD/tools/win-bin:/c/platform-tools:$PATH"
	PY=$(command -v python || command -v python3) ;;
*)
	PY=$(command -v python3 || command -v python) ;;
esac
[ -n "$PY" ] || { echo "metal-run: no python on PATH"; exit 1; }
for f in userland/root.apfs userland/system.apfs; do
	[ -f "$f" ] || { echo "metal-run: $f is missing (build it in WSL: docs/windows.md, METAL_PLUGIN=ca)"; exit 1; }
done
command -v fastboot >/dev/null && command -v adb >/dev/null || { echo "metal-run: adb and fastboot are not on PATH (C:\\platform-tools)"; exit 1; }

SECS=${SECS:-180}
PERSIST=${PERSIST:-1}
UPLOAD=${UPLOAD:-1}
SHOTS=${SHOTS:-0}
CA_METALLIB=${CA_METALLIB:-/redwood/share/ca-share/QuartzCore-default.metallib}
# what is the phone's own: its big core and cores, its panel's rate, how many threads copy frames, how often the touch screen is read.
# DEVICE_ARGS= names them, or devices/<CONFIG_DEVICE>/metal.args (tools/hakios-device.py new makes one). Without either, on a phone that
# is not redwood, what assumes nothing: a wrong bootcpu= does not boot and a wrong fb_hz= shows every frame's beat wrong.
if [ -z "${DEVICE_ARGS:-}" ]; then
	device=$(sed -n 's/^CONFIG_DEVICE="\(.*\)"/\1/p' .config 2>/dev/null)
	if [ -n "$device" ] && [ -f "devices/$device/metal.args" ]; then
		DEVICE_ARGS=$(grep -v '^#' "devices/$device/metal.args" | tr '\n' ' ')
	elif [ -z "$device" ] || [ "$device" = redwood ]; then
		DEVICE_ARGS="bootcpu=7 cpus=8 cpufreq=1 fb_hz=120 fb_threads=6 touch_poll_us=4000"
	else
		DEVICE_ARGS="fb_hz=60 fb_threads=3"
		echo "metal-run: no devices/$device/metal.args: fb_hz=60 and one boot core; docs/porting/index.md" >&2
	fi
fi
ARGS="gpu=1 metal=1 metal_fps=1 touch_lat=1 touchscreen=1 ca_metallib=$CA_METALLIB gpu_recover=1 anim_max=1 $DEVICE_ARGS fb_measure=1 wallpaper_freeze=1 metal_defer=1 fbscale=2"
[ "$PERSIST" = 1 ] && ARGS="$ARGS ufs=2 ufs_write=1 var_store=1"
[ "${VERBOSE:-0}" = 1 ] && ARGS="-v $ARGS"
# (a default of the same name goes: the kernel takes the last of two, the plug-in and SpringBoard the first)
for more in ${MORE:-}; do
	kept=
	for word in $ARGS; do
		[ "${word%%=*}" = "${more%%=*}" ] || kept="$kept $word"
	done
	ARGS=$kept
done
ARGS="$ARGS ${MORE:-}"
stamp=$(date +%H%M%S)
mkdir -p logs
out=logs/$stamp-metal.txt

echo "metal-run: the phone is $(./xl state)"
./xl enter || { echo "metal-run: the phone did not reach the loader (./xl state; docs/windows.md, When something goes wrong)"; exit 1; }
if [ "$UPLOAD" = 1 ]; then
	./xl ramdisk userland/system.apfs || { echo "metal-run: the system image did not upload"; exit 1; }
fi

shots_pid=
if [ "$SHOTS" -gt 0 ] 2>/dev/null; then
	mkdir -p logs/$stamp-shots
	( sleep 30; i=0; while [ $i -lt $(( SECS / SHOTS )) ]; do ./xl screenshot logs/$stamp-shots/$i.png >/dev/null 2>&1; i=$((i + 1)); sleep "$SHOTS"; done ) &
	shots_pid=$!
fi
echo "metal-run: $SECS s with: $ARGS"
XL_CMDLINE="$ARGS" XL_SECS=$SECS XL_FOLLOW=${XL_FOLLOW:-10} XL_FOLLOW_KEEP=1 ./xl run
status=$?
[ -n "$shots_pid" ] && kill "$shots_pid" 2>/dev/null
log=logs/$(readlink logs/last.log 2>/dev/null || ls -t logs/*-run.log 2>/dev/null | head -1 | xargs -n1 basename)
[ -f "$log" ] || { echo "metal-run: no run log (xl run said $status)"; exit 1; }

{
	echo "metal-run $stamp: $log, $SECS s, xl run $status"
	echo "boot-args: $ARGS"
	echo
	echo "== frames per gesture (tools/frames.py)"
	"$PY" -I tools/frames.py "$log" $(ls "$log".live.* 2>/dev/null) 2>&1 | head -60
	echo
	echo "== HakiosMetal"
	grep -a -E 'HakiosMetal(\[[^]]*\])?: fps:' "$log" | tail -40
	grep -a -E 'HakiosMetal(\[[^]]*\])?: (inplace_copy|CoreAnimation.s .*compiled as)' "$log" | sort | uniq -c | head
	echo
	echo "== refused (count, kind)"
	grep -a -E 'HakiosMetal(\[[^]]*\])?: refused:' "$log" | sed -E 's/.*refused: //; s/[0-9]+ vertices.*: /...: /' | sort | uniq -c | sort -rn | head -15
	echo
	echo "== frames on the panel (MiniIOMobileFramebuffer, every 5 s: what the window server drew through Metal)"
	grep -a -E 'MiniIOMobileFramebuffer: the panel:' "$log" | sed 's/^\[[^]]*\]: //'
	grep -a -oE 'MiniIOMobileFramebuffer: [0-9]+ frames in [0-9]+ ms \([0-9]+ a second\), [0-9]+ us each in the driver, after [0-9]+ us waiting for the GPU' "$log" |
		sed -E 's/MiniIOMobileFramebuffer: //' | tail -40
	echo
	echo "== shader cache and var_store"
	grep -a -E 'var_store|hakios-metal-cache' "$log" | tail -12
	echo
	echo "== crashes"
	grep -a -E 'crashed|Assertion failed|abort|panic' "$log" | head -10
} > "$out" 2>&1
cat "$out"
echo
echo "metal-run: the summary is $out; the whole log $log"
[ "$PERSIST" = 1 ] && echo "metal-run: next, with the phone back in Android: tools/metal-cache-seed.sh (the stages into the next image)"
exit $status
