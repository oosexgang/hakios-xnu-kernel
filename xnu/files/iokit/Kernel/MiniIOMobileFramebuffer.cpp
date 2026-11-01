/*
 * MiniIOMobileFramebuffer: a stand-in for Apple's display driver family, as seen by
 * IOMobileFramebuffer.framework (and through it by CoreAnimation's window server).
 *
 * The display is the framebuffer the boot loader left us (the one the kernel's text console
 * draws on). There is no hardware behind it that we drive: a "swap" copies the pixels of the
 * surfaces of its layers into that memory, and "vsync" is a timer.
 *
 * The protocol (selectors and the layout of the swap request) was worked out from the
 * user-space side of Darwin userland 16.5. Everything not implemented is logged once.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/proc.h>

extern "C" {
#include <console/video_console.h>
extern struct vc_info vinfo;            /* the boot framebuffer, as the text console knows it */
}

#include <IOKit/IOBufferMemoryDescriptor.h>

#include "MiniIOSurface.h"

extern "C" void hakios_tl(const char * format, ...) __printflike(1, 2);

/* IOConnectCallMethod selectors of the framebuffer user client (Darwin userland 16.5) */
enum {
	kFBMethodGetLayerDefaultSurface = 3,
	kFBMethodSwapBegin              = 4,
	kFBMethodSwapEnd                = 5,
	kFBMethodSwapWait               = 6,
	kFBMethodGetID                  = 7,
	kFBMethodGetDisplaySize         = 8,
	kFBMethodRequestPowerChange     = 12,
	kFBMethodSwapSignal             = 20,
	kFBMethodGetDigitalOutState     = 25,
	kFBMethodGetDotPitch            = 28,
	kFBMethodGetDisplayArea         = 29,
	kFBMethodSwapCancel             = 52,
	kFBMethodGetColorRemapMode      = 57,
	kFBMethodNotifications          = 72,
	kFBMethodChangeFrameInfo        = 73,
	kFBMethodSupportedFrameInfo     = 74,
	kFBMethodGetCurrentAbsoluteTime = 77,
	kFBMethodGetBlock               = 79,
	kFBMethodSwapCancelAll          = 81,
	kFBMethodGetProtectionOptions   = 82,
};

/* The swap request (IOMobileFramebufferSwapEnd): 0x46c bytes, of which we read these. */
#define SWAP_SIZE               0x46c
#define SWAP_ID                 0x50
#define SWAP_SURFACE(layer)     (0x54 + 4 * (layer))    /* surface number, 0 = none */
#define SWAP_SOURCE(layer)      (0x64 + 16 * (layer))   /* x, y, width, height in the surface */
#define SWAP_ENABLED(layer)     (0xa4 + 4 * (layer))
#define SWAP_DEST(layer)        (0xc4 + 16 * (layer))   /* x, y, width, height on the display */
#define SWAP_LAYERS_SET         0x104                   /* bit per layer given in this swap */
#define SWAP_LAYERS_CLEARED     0x108
#define SWAP_LAYERS             4

#define kFBNotifyVSync          5
#define kFBNotifyTypes          8
#define kFBRefreshHz            60                      /* unless boot-arg fb_hz says otherwise */

#define FRAME_TIMES             64                      /* milliseconds told apart between two frames */
#define FRAME_PAUSE             90                      /* from so many on, nothing was moving: not a slow frame */
#define SWAP_GPU_MS             500                     /* a frame the GPU has not drawn by then is shown as it is */
#define SWAP_HELD_MS            700                     /* a swap that cannot be taken by then is not shown at all */

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static uint64_t gFBLogged[2];           /* selectors already reported */
static uint8_t  gFBCalls[128];          /* how many calls of each selector were traced */

/*
 * The display user space is told about. The system carries the identity of the iPhone X
 * (see the loader), and Darwin knows what screen that phone has: with any other size
 * MobileGestalt reports an unknown screen class and the user interface has no layout for it.
 * So the display is always the iPhone's, and a swap scales the picture to the panel that is
 * really there (in QEMU the loader makes the panel exactly this size and nothing is scaled).
 * Boot-args fbwidth= and fbheight= change the size reported.
 */
#define kFBDefaultWidth         1125
#define kFBDefaultHeight        2436
#define kFBDotsPerInch          458

static TUNABLE(uint32_t, gFBWidth, "fbwidth", kFBDefaultWidth);
static TUNABLE(uint32_t, gFBHeight, "fbheight", kFBDefaultHeight);
/*
 * Boot-arg fbscale=2: the same 375 x 812 points drawn at two pixels a point instead of three.
 * Everything that is drawn is drawn by the processor, and this is 2.25 times fewer pixels to
 * draw; the picture is then enlarged to the panel and is that much less sharp.
 */
#define kFBPointsWide           375
#define kFBPointsHigh           812
#define kFBDefaultScale         3
static TUNABLE(uint32_t, gFBScale, "fbscale", 0);
/* Boot-arg fbtrace=N: log the first N calls of every selector (default 3). */
static TUNABLE(uint32_t, gFBTrace, "fbtrace", 3);

/*
 * A recording of the screen, for a machine one cannot look at while it runs (the phone is
 * reached only through the boot loader, before and after the kernel).
 * Boot-args rec_secs=N [rec_start=S] [rec_mb=M]: from S seconds after the first frame, for N
 * seconds, every frame that is shown is also kept -- one pixel of every REC_STEP in each
 * direction, three bytes a pixel -- in a piece of memory of M megabytes (default 480, which
 * is 520 frames) that is left in place when the kernel returns to the loader. The loader
 * hands it to the host ("xl recording"), where tools/recording.py makes a video of it.
 *
 *   struct rec_header, then per frame: struct rec_frame, width * height pixels (R, G, B)
 *
 * Boot-arg rec_ring=1: when the memory is full the recording does not stop but goes on over its
 * oldest frames, so that what it holds at the end is the last of the run: for something that
 * happens once in a long session, the run is ended right after it (five fingers) and the frames
 * before it are there. The header ("RWREC003") says where the oldest frame is (frames are all
 * the same size, one after the other) and when the recording began, in the kernel's time, for the
 * frames to be matched with what the window server's per-frame lines (glframes=1,
 * userland/src/eagl.c) say. rec_step=N (2 to 8, default 3) keeps one pixel of every N instead of 3:
 * N = 4 holds 1.8 times as many frames, 6 four times. The memory is taken from the system's for the
 * whole run: 640 MB of it under a long session of applications had Darwin killing processes for
 * memory, SpringBoard and the window server among them (two respring in one run); 256 did not.
 */
static TUNABLE(uint32_t, gRecSeconds, "rec_secs", 0);
static TUNABLE(uint32_t, gRecStart, "rec_start", 0);
static TUNABLE(uint32_t, gRecMegabytes, "rec_mb", 480);
static TUNABLE(uint32_t, gRecRing, "rec_ring", 0);
static TUNABLE(uint32_t, gRecStep, "rec_step", 3);
static TUNABLE(uint32_t, gDiff, "fb_diff", 0);
/*
 * Boot-arg fb_filter=0: the picture is scaled to the panel nearest neighbour, as it once was. The
 * iPhone's 1125 x 2436 on this panel's 1080 x 2400 is 45 columns and 36 rows fewer: nearest
 * neighbour leaves out every 25th column and every 67th row, and where one of them goes through a
 * letter or a slanting line, that is a step in it or a stroke a pixel thinner -- text and lines a
 * little crooked, here and there and never in the same places twice. By default every pixel of the
 * panel is worked out from the four of the picture around where it falls (bilinear).
 */
static TUNABLE(uint32_t, gFilter, "fb_filter", 2);
/*
 * fb_filter=2 (the default): nearest neighbour while frames come, bilinear once they stop. Bilinear costs: a frame
 * took 3.6 ms to scale onto the panel as a rule and 11 ms for one in ten, at any priority (fb_pri), against 1.7 and
 * 4.3 by nearest neighbour (touch_lat=1, 30 October) -- and at 120 Hz a refresh is 8.3 ms. What nearest neighbour
 * gets wrong (a column in 25 and a row in 67 left out) does not show while the picture moves; so a frame drawn so is
 * drawn again, filtered, when no other has come for SETTLE_MS: what stands still on the screen is the bilinear one.
 */
#define SETTLE_MS       40
static TUNABLE(uint32_t, gRefreshHz, "fb_hz", kFBRefreshHz);
/*
 * Boot-arg fb_ca_hz=N: the refresh rate told to CoreAnimation (IOMFBDisplayRefresh) when it is not to be fb_hz's. On
 * this system CoreAnimation draws at half the rate it is told unless something asks it for more: told 120 it gave 60
 * (tools/frames.py: frames every 16.7 ms, with or without CADisableMinimumFrameDurationOnPhone, CA_FORCE_120HZ and
 * CA_ENABLE_HIGH_FRAME_RATE_RESTRICTION=0, 30 October). The refreshes themselves still come at fb_hz.
 */
static TUNABLE(uint32_t, gCARefreshHz, "fb_ca_hz", 0);
static TUNABLE(uint32_t, gOnTheBeat, "fb_beat", 1);
/*
 * Boot-arg fb_threads=N (1 to 8): so many processors scale a frame onto the panel together (see
 * drawRows()). Three by default: a frame is in the panel's memory 3 ms after its turn instead of 9
 * to 10 (the phone, 15 October; fb_threads=1 is how it was).
 */
static TUNABLE(uint32_t, gThreads, "fb_threads", 3);
/*
 * Boot-arg fb_pri=N: the scheduling priority of the threads that put frames on the panel (the presenter and its
 * helpers); 0 is the kernel's default for its threads. By default 92 (BASEPRI_PREEMPT): at the kernel's they lost the
 * processors to the window server's own real-time threads under load, and a frame took 3.6 ms to put on the panel
 * as a rule but 10.9 for one in ten (touch_lat=1, 30 October), more than the whole of a refresh at 120 Hz.
 */
static TUNABLE(uint32_t, gThreadPriority, "fb_pri", 92);
/*
 * Boot-arg fb_line=PA: a command-mode panel's line counter (a display interface's INTF_TEAR_LINE_COUNT), and fb_lines=N
 * how many lines it counts, fb_scan_us=N how long a frame's transfer takes. This driver writes straight into the memory
 * the controller sends the panel, so a frame written while one goes out is seen torn. With the counter a frame is
 * written in the gap after the transfer, and goes on being written ahead of the next (writing is the faster), in place
 * of a beat of its own that drifts against the panel's.
 */
static TUNABLE(uint64_t, gPanelLinePA, "fb_line", 0);
static TUNABLE(uint32_t, gPanelLines, "fb_lines", 2400);
static TUNABLE(uint32_t, gPanelScanUs, "fb_scan_us", 7000);
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
static vm_offset_t gPanelLine;
static inline uint32_t refreshHz(void);

/* Until the panel's transfer is over, or a refresh at most. */
static void
waitForPanelGap(void)
{
	uint64_t deadline, ns = 0;

	if (gPanelLinePA == 0) {
		return;
	}
	if (gPanelLine == 0) {
		gPanelLine = ml_io_map((vm_offset_t) gPanelLinePA, 0x1000);
	}
	clock_interval_to_deadline(NSEC_PER_SEC / refreshHz() / NSEC_PER_USEC, NSEC_PER_USEC, &deadline);
	for (;;) {
		uint32_t line = *(volatile uint32_t *) gPanelLine & 0xffff;
		uint64_t until;

		if (line >= gPanelLines - 4 || mach_absolute_time() >= deadline) {
			return;
		}
		ns = (uint64_t) (gPanelLines - line) * gPanelScanUs * NSEC_PER_USEC / gPanelLines;
		nanoseconds_to_absolutetime(ns > 50 * NSEC_PER_USEC ? ns - 50 * NSEC_PER_USEC : 10 * NSEC_PER_USEC, &until);
		clock_delay_until(mach_absolute_time() + until);
	}
}
/*
 * Boot-arg fb_flip=1: two framebuffers in place of one, for a panel the display controller fetches by itself (the
 * Xiaomi 12S: a command-mode panel the controller refreshes from memory on every tear signal). Without it every frame
 * is written into the memory being sent, and one written while a transfer goes out shows torn. With it a frame is
 * written into the other buffer, the controller's pipe is pointed at it (its source address, then the control path's
 * flush: the hardware takes it at the start of the next transfer), and the buffer before is written again only once
 * the flush is seen taken. fb_mdp=PA: the display subsystem (the Xiaomi 12S's mdss 0xae00000, Linux's dpu catalog
 * for sm8450: pipes at 0x5000.. and control paths at 0x16000.. from it); fb_flip_gap=N: where the second buffer is,
 * after the first (inside the memory the boot loader reserved for the display: 0x2b00000 on the Xiaomi 12S).
 * If the hardware does not take a flip three times running, the driver goes back to one buffer, the boot loader's.
 */
static TUNABLE(uint32_t, gFlip, "fb_flip", 0);
static TUNABLE(uint64_t, gMdpPA, "fb_mdp", 0xae00000);
static TUNABLE(uint32_t, gFlipGap, "fb_flip_gap", 0xa00000);
static TUNABLE(uint32_t, gFlipExtra, "fb_flip_flush", 0);       /* more flush bits, if the pipe's alone is not taken */
extern "C" vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);

#define FLIP_PIPES              8
#define FLIP_CTLS               6
#define FLIP_SRC0_ADDR          0x14
#define FLIP_CTL_FLUSH          0x18
#define FLIP_TAKEN_MS           50

static const uint32_t kFlipPipe[FLIP_PIPES] = { 0x5000, 0x7000, 0x9000, 0xb000, 0x25000, 0x27000, 0x29000, 0x2b000 };
static const char * const kFlipPipeName[FLIP_PIPES] = { "vig0", "vig1", "vig2", "vig3", "dma0", "dma1", "dma2", "dma3" };
static const uint32_t kFlipPipeFlush[FLIP_PIPES] = { 1u << 0, 1u << 1, 1u << 2, 1u << 18, 1u << 11, 1u << 12, 1u << 24, 1u << 25 };
static const uint32_t kFlipCtl[FLIP_CTLS] = { 0x16000, 0x17000, 0x18000, 0x19000, 0x1a000, 0x1b000 };

static struct {
	bool            on;
	vm_offset_t     mdp;
	uint32_t        pipe;                   /* the pipe that fetches the framebuffer */
	uint32_t        ctls;                   /* the control paths that have layers: flushed */
	uint32_t        flush;
	uint64_t        pa[2];
	uint8_t *       va[2];
	unsigned        front;                  /* the buffer the controller was last pointed at */
	bool            pending;                /* ... and the flush not yet seen taken */
	uint32_t        flips, waited, misses, missesInRow;
	uint64_t        waitAbs;
} gFlipper;

static inline uint32_t
flip_rd(uint32_t offset)
{
	return *(volatile uint32_t *) (gFlipper.mdp + offset);
}

static inline void
flip_wr(uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *) (gFlipper.mdp + offset) = value;
	__builtin_arm_dsb(0xf);
}

/* the flush bits still pending on any of the control paths flushed */
static uint32_t
flip_pending(void)
{
	uint32_t pending = 0;

	for (unsigned c = 0; c < FLIP_CTLS; c++) {
		if (gFlipper.ctls & (1u << c)) {
			pending |= flip_rd(kFlipCtl[c] + FLIP_CTL_FLUSH) & gFlipper.flush;
		}
	}
	return pending;
}

static void
flip_point(unsigned buffer)
{
	flip_wr(kFlipPipe[gFlipper.pipe] + FLIP_SRC0_ADDR, (uint32_t) gFlipper.pa[buffer]);
	for (unsigned c = 0; c < FLIP_CTLS; c++) {
		if (gFlipper.ctls & (1u << c)) {
			flip_wr(kFlipCtl[c] + FLIP_CTL_FLUSH, gFlipper.flush);
		}
	}
	gFlipper.front = buffer;
	gFlipper.pending = true;
	gFlipper.flips++;
}

/*
 * Until the last flip is taken: then the buffer that is not the front one is free to write. False if it was not taken
 * in FLIP_TAKEN_MS (three such in a row: back to the boot loader's buffer alone).
 */
static bool
flip_wait_taken(void)
{
	uint64_t began = mach_absolute_time(), deadline;

	if (!gFlipper.pending) {
		return true;
	}
	clock_interval_to_deadline(FLIP_TAKEN_MS, kMillisecondScale, &deadline);
	while (flip_pending() != 0) {
		if (mach_absolute_time() >= deadline) {
			gFlipper.misses++;
			if (++gFlipper.missesInRow >= 3) {
				IOLog("MiniIOMobileFramebuffer: flip: three flips running not taken in %u ms (flush still %#x): "
				    "ONE BUFFER AGAIN, the boot loader's\n", FLIP_TAKEN_MS, flip_pending());
				flip_point(0);
				gFlipper.pending = false;
				gFlipper.on = false;
			}
			return false;
		}
		IODelay(100);
	}
	gFlipper.waitAbs += mach_absolute_time() - began;
	gFlipper.waited++;
	gFlipper.missesInRow = 0;
	gFlipper.pending = false;
	return true;
}

static void
flip_setup(uint8_t * base, uint32_t rowBytes, uint32_t height)
{
	uint64_t size = (uint64_t) rowBytes * height;

	if (!gFlip) {
		return;
	}
	if (vinfo.v_physaddr == 0 || size > gFlipGap) {
		IOLog("MiniIOMobileFramebuffer: flip: no (framebuffer at %#llx, %llu bytes, the gap %#x)\n", vinfo.v_physaddr, size,
		    gFlipGap);
		return;
	}
	gFlipper.mdp = ml_io_map((vm_offset_t) gMdpPA, 0x30000);
	gFlipper.pa[0] = vinfo.v_physaddr;
	gFlipper.pa[1] = vinfo.v_physaddr + gFlipGap;
	gFlipper.pipe = FLIP_PIPES;
	for (unsigned p = 0; p < FLIP_PIPES; p++) {
		uint32_t at = flip_rd(kFlipPipe[p] + FLIP_SRC0_ADDR);

		IOLog("MiniIOMobileFramebuffer: flip: pipe %s source %#x\n", kFlipPipeName[p], at);
		if (gFlipper.pipe == FLIP_PIPES && at == (uint32_t) gFlipper.pa[0]) {
			gFlipper.pipe = p;
		}
	}
	for (unsigned c = 0; c < FLIP_CTLS; c++) {
		vm_offset_t ctl = kFlipCtl[c];
		uint32_t layers = flip_rd(ctl + 0x00) | flip_rd(ctl + 0x04) | flip_rd(ctl + 0x08) | flip_rd(ctl + 0x0c) |
		    flip_rd(ctl + 0x10) | flip_rd(ctl + 0x24);

		IOLog("MiniIOMobileFramebuffer: flip: control path %u: layers %#x %#x %#x %#x %#x %#x, top %#x, flush %#x\n", c,
		    flip_rd(ctl + 0x00), flip_rd(ctl + 0x04), flip_rd(ctl + 0x08), flip_rd(ctl + 0x0c), flip_rd(ctl + 0x10),
		    flip_rd(ctl + 0x24), flip_rd(ctl + 0x14), flip_rd(ctl + FLIP_CTL_FLUSH));
		if (layers != 0) {
			gFlipper.ctls |= 1u << c;
		}
	}
	if (gFlipper.pipe == FLIP_PIPES || gFlipper.ctls == 0) {
		IOLog("MiniIOMobileFramebuffer: flip: no pipe fetches %#llx or no control path has layers (%#x): one buffer\n",
		    gFlipper.pa[0], gFlipper.ctls);
		return;
	}
	{
		uint32_t pipe = kFlipPipe[gFlipper.pipe];

		/* (for a later hardware scaling: the pipe as the boot loader set it up; Linux's dpu_hw_sspp.c offsets) */
		IOLog("MiniIOMobileFramebuffer: flip: pipe %s: source size %#x, image size %#x, at %#x, out size %#x at %#x, "
		    "stride %#x, format %#x, unpack %#x, op mode %#x\n", kFlipPipeName[gFlipper.pipe], flip_rd(pipe + 0x00),
		    flip_rd(pipe + 0x04), flip_rd(pipe + 0x08), flip_rd(pipe + 0x0c), flip_rd(pipe + 0x10), flip_rd(pipe + 0x24),
		    flip_rd(pipe + 0x30), flip_rd(pipe + 0x34), flip_rd(pipe + 0x38));
	}
	gFlipper.flush = kFlipPipeFlush[gFlipper.pipe] | gFlipExtra;
	gFlipper.va[0] = base;
	gFlipper.va[1] = (uint8_t *) ml_io_map_wcomb((vm_offset_t) gFlipper.pa[1], (vm_size_t) size);
	if (gFlipper.va[1] == NULL) {
		return;
	}
	memcpy(gFlipper.va[1], base, (size_t) size);      /* (the same picture, whichever is fetched) */
	gFlipper.front = 0;
	gFlipper.on = true;
	IOLog("MiniIOMobileFramebuffer: flip: TWO BUFFERS, %#llx and %#llx, on pipe %s, control paths %#x, flush %#x\n",
	    gFlipper.pa[0], gFlipper.pa[1], kFlipPipeName[gFlipper.pipe], gFlipper.ctls, gFlipper.flush);
}

/*
 * Boot-arg fb_hud=1: the frame rate, the hottest temperature sensor and the processors' clocks in a corner of every frame
 * (written into the frame after its layers, before it goes to the panel; the Xiaomi 12S gives the numbers, through
 * hakios_mayfly_hud_numbers() in hakios.adreno7.c, and without it only the frame rate is shown). In the kernel console's
 * font (iso_font, 8 x 16, the low bit the leftmost), drawn twice the size.
 */
static TUNABLE(uint32_t, gHud, "fb_hud", 0);
extern "C" unsigned char iso_font[256 * 16];
extern "C" void hakios_mayfly_hud_numbers(int * temperature, uint32_t mhz[3], uint32_t * ceiling) __attribute__((weak));
extern "C" uint32_t hakios_mayfly_gpu_mhz(void) __attribute__((weak));
extern "C" unsigned int vm_page_free_count;
#define HUD_SCALE       2
#define HUD_X           24
#define HUD_Y           150
#define HUD_HISTORY     60              /* seconds in the graph under the numbers */
#define HUD_BAR_W       6
#define HUD_GRAPH_H     72              /* pixels for 120 frames a second */

static struct {
	uint64_t        since;
	uint32_t        frames, fps;
	int             temperature;
	uint32_t        mhz[3], ceiling;
	char            text[96];
	uint32_t        colour;
	uint8_t         history[HUD_HISTORY];   /* the frame rate of each of the last seconds, the newest at `newest' */
	uint8_t         lateHistory[HUD_HISTORY]; /* ... and how many of its frames came a refresh and a half or more after the one before */
	uint32_t        late;                   /* (counted by show(): this second's) */
	uint32_t        newest, seconds;
} gHudState;

static void
hud_tick(void)
{
	uint64_t now = mach_absolute_time(), second;

	nanoseconds_to_absolutetime(NSEC_PER_SEC, &second);
	gHudState.frames++;
	if (gHudState.since == 0) {
		gHudState.since = now;
	}
	if (now - gHudState.since < second && gHudState.text[0] != 0) {
		return;
	}
	gHudState.fps = (uint32_t) ((uint64_t) gHudState.frames * second / (now - gHudState.since + 1));
	gHudState.newest = (gHudState.newest + 1) % HUD_HISTORY;
	gHudState.history[gHudState.newest] = (uint8_t) (gHudState.fps > 240 ? 240 : gHudState.fps);
	gHudState.lateHistory[gHudState.newest] = (uint8_t) (gHudState.late > 255 ? 255 : gHudState.late);
	gHudState.late = 0;
	gHudState.seconds++;
	gHudState.frames = 0;
	gHudState.since = now;
	gHudState.temperature = -1000;
	if (hakios_mayfly_hud_numbers != NULL) {
		hakios_mayfly_hud_numbers(&gHudState.temperature, gHudState.mhz, &gHudState.ceiling);
	}
	if (gHudState.temperature > -1000) {
		int t = gHudState.temperature;

		snprintf(gHudState.text, sizeof(gHudState.text), "%3u fps %d.%dC cpu %u/%u/%u free %uM", gHudState.fps, t / 10,
		    (t < 0 ? -t : t) % 10, gHudState.mhz[0], gHudState.mhz[1], gHudState.mhz[2],
		    (uint32_t) ((uint64_t) vm_page_free_count * PAGE_SIZE >> 20));
		gHudState.colour = t < 650 ? 0xff40ff40 : t < 800 ? 0xffffd040 : 0xffff4040;
	} else {
		snprintf(gHudState.text, sizeof(gHudState.text), "%3u fps", gHudState.fps);
		gHudState.colour = 0xff40ff40;
	}
}

/* The text into the frame at base (rows of rowBytes, width x height pixels), on a black box. */
static void
hud_draw(uint8_t * base, uint32_t rowBytes, uint32_t width, uint32_t height)
{
	size_t length = strlen(gHudState.text);
	uint32_t boxW = (uint32_t) length * 8 * HUD_SCALE + 16, boxH = 16 * HUD_SCALE + 8;

	uint32_t graphW = HUD_HISTORY * HUD_BAR_W + 16;

	boxW = boxW > graphW ? boxW : graphW;
	boxH += HUD_GRAPH_H + 8;
	if (length == 0 || HUD_X + boxW > width || HUD_Y + boxH > height) {
		return;
	}
	for (uint32_t y = 0; y < boxH; y++) {
		uint32_t * row = (uint32_t *) (base + (size_t) (HUD_Y + y) * rowBytes) + HUD_X;

		for (uint32_t x = 0; x < boxW; x++) {
			row[x] = 0xff000000;
		}
	}
	for (size_t c = 0; c < length; c++) {
		const unsigned char * glyph = iso_font + (unsigned char) gHudState.text[c] * 16;

		for (uint32_t gy = 0; gy < 16; gy++) {
			for (uint32_t sy = 0; sy < HUD_SCALE; sy++) {
				uint32_t * row = (uint32_t *) (base + (size_t) (HUD_Y + 4 + gy * HUD_SCALE + sy) * rowBytes) + HUD_X + 8 +
				    c * 8 * HUD_SCALE;

				for (uint32_t gx = 0; gx < 8; gx++) {
					if (glyph[gy] & (1u << gx)) {
						for (uint32_t sx = 0; sx < HUD_SCALE; sx++) {
							row[gx * HUD_SCALE + sx] = gHudState.colour;
						}
					}
				}
			}
		}
	}
	/* the graph: a bar a second, the oldest on the left; a line at 60 and at 120 */
	uint32_t top = HUD_Y + 16 * HUD_SCALE + 12, bottom = top + HUD_GRAPH_H;

	for (uint32_t i = 0; i < HUD_HISTORY; i++) {
		uint32_t index = (gHudState.newest + 1 + i) % HUD_HISTORY;
		uint32_t fps = i + gHudState.seconds >= HUD_HISTORY ? gHudState.history[index] : 0;
		/*
		 * the height: frames that second; the colour: whether any came late -- a second with few frames because nothing moved
		 * is a short green bar, a second of animation with frames that missed their refresh is red
		 */
		uint32_t late = i + gHudState.seconds >= HUD_HISTORY ? gHudState.lateHistory[index] : 0;
		uint32_t bar = fps * HUD_GRAPH_H / 120, colour = late == 0 ? 0xff40ff40 : late <= 3 ? 0xffffd040 : 0xffff4040;

		bar = bar > HUD_GRAPH_H ? HUD_GRAPH_H : bar;
		for (uint32_t y = 0; y < bar; y++) {
			uint32_t * row = (uint32_t *) (base + (size_t) (bottom - 1 - y) * rowBytes) + HUD_X + 8 + i * HUD_BAR_W;

			for (uint32_t x = 0; x + 1 < HUD_BAR_W; x++) {
				row[x] = colour;
			}
		}
	}
	for (uint32_t x = 0; x < HUD_HISTORY * HUD_BAR_W; x += 2) {
		((uint32_t *) (base + (size_t) (bottom - HUD_GRAPH_H) * rowBytes))[HUD_X + 8 + x] = 0xff808080;
		((uint32_t *) (base + (size_t) (bottom - HUD_GRAPH_H / 2) * rowBytes))[HUD_X + 8 + x] = 0xff808080;
	}
}

extern "C" kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void * parameter, integer_t priority,
    thread_t * new_thread);

/* a thread of the panel's, at fb_pri */
static kern_return_t
panel_thread_start(thread_continue_t continuation, void * parameter, thread_t * thread)
{
	if (gThreadPriority >= 1 && gThreadPriority <= 95) {
		return kernel_thread_start_priority(continuation, parameter, (integer_t) gThreadPriority, thread);
	}
	return kernel_thread_start(continuation, parameter, thread);
}
#define SWAP_QUEUE              2                       /* frames that may wait for the panel at most (fb_queue) */
static TUNABLE(uint32_t, gQueue, "fb_queue", 2);
static inline unsigned
swapQueue(void)
{
	return gQueue >= 1 && gQueue <= SWAP_QUEUE ? gQueue : 1;
}
/*
 * Boot-arg touch_lat=1: a line for every swap handed over ("tl: s ID at US") and for every frame
 * put on the panel ("tl: v ID ready US shown US done US": the GPU done with it, its beat come, the
 * pixels in the panel's memory), in microseconds of mach_absolute_time, for measuring how long a
 * touch takes to show (tools/latency.py; the touch's own lines: drivers/input/hakios.input.c).
 */
static TUNABLE(uint32_t, gTouchLat, "touch_lat", 0);

static inline uint64_t
micros(uint64_t absolute)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(absolute, &ns);
	return ns / 1000;
}

/*
 * Boot-arg fb_measure=1: how often the panel is really refreshed. The boot loader leaves the display controller running
 * and this driver only writes into the memory it scans, so fb_hz says how often frames are made, not how often the
 * panel shows one. The frame counter of the controller's DSI interface (SM7325's DPU: INTF_1 at 0x0ae36000,
 * INTF_FRAME_COUNT at 0xac, as Linux's catalogue for sc7280 has them) is read twice, a second apart, and the
 * difference printed. Reads only, once, a minute after the driver starts (what the kernel says in its first minute
 * does not come back with the phone's log), in a thread of its own; off by default.
 */
static TUNABLE(uint32_t, gMeasure, "fb_measure", 0);
#define DPU_INTF1_PAGE          0x0ae34000UL            /* the 16 KB page INTF_1 is in */
#define DPU_INTF1_OFFSET        0x2000
#define INTF_TIMING_ENGINE_EN   0x000
#define INTF_FRAME_COUNT        0x0ac
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
static inline uint32_t refreshHz(void);

static void
measurePanel(__unused void * arg, __unused wait_result_t wr)
{
	vm_offset_t intf;
	uint32_t on, first, second;

	IOSleep(60000);
	intf = ml_io_map(DPU_INTF1_PAGE, 0x4000) + DPU_INTF1_OFFSET;
	on = *(volatile uint32_t *)(intf + INTF_TIMING_ENGINE_EN);
	first = *(volatile uint32_t *)(intf + INTF_FRAME_COUNT);
	IOSleep(1000);
	second = *(volatile uint32_t *)(intf + INTF_FRAME_COUNT);
	IOLog("MiniIOMobileFramebuffer: the panel: DSI interface's timing engine %s, %u refreshes in one second "
	    "(frames are made at %u a second: fb_hz)\n", (on & 1) ? "on" : "OFF", second - first, refreshHz());
}

static inline uint32_t
refreshHz(void)
{
	return gRefreshHz >= 1 && gRefreshHz <= 240 ? gRefreshHz : kFBRefreshHz;
}
static inline uint32_t
recStep(void)
{
	return gRecStep >= 2 && gRecStep <= 8 ? gRecStep : 3;
}
#define REC_STEP        recStep()

struct rec_header {
	char     magic[8];              /* "RWREC003" */
	uint32_t width, height;
	uint32_t frames;
	uint32_t oldest;                /* the place of the oldest frame (rec_ring; 0 until the memory has once been full) */
	uint64_t began;                 /* when the first frame was recorded, in nanoseconds of the kernel's time */
};
struct rec_frame {
	uint64_t nanoseconds;           /* since the first frame of the recording */
};

extern "C" uint64_t loader_recording[3];        /* { physical address, bytes, kernel address }: osfmk/arm64/loader_return.c */

static uint32_t
swapWord(const uint8_t * swap, unsigned offset)
{
	uint32_t value;

	memcpy(&value, swap + offset, sizeof(value));
	return value;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct RowJob;

class IOMobileFramebuffer : public IOService
{
	OSDeclareDefaultStructors(IOMobileFramebuffer);

public:
	IOLock *        fLock;
	uint8_t *       fBase;                  /* the boot framebuffer: the panel */
	uint32_t        fPanelWidth, fPanelHeight, fRowBytes;
	uint32_t        fWidth, fHeight;        /* the display as user space sees it */
	MiniIOSurface * fDefaultSurface;
	uint32_t        fSwapID;
	uint32_t        fSwapsShown;
	uint32_t        fLayersShown;           /* which layers the last swap had a surface in */
	bool            fGraphics;              /* the text console has been told to stop */
	/*
	 * A frame goes to the panel when the GPU is done with it: a thread of ours waits for that
	 * and copies it there, and whoever made the frame is back at the next one meanwhile. One
	 * frame waits at a time: the swap after it is held, in SwapEnd, until this one is being
	 * copied to the panel. Whoever draws is so at most two frames ahead of the panel, and
	 * needs a third surface to be that: CoreAnimation makes itself a new one for every frame.
	 * By default two frames may wait (boot-arg fb_queue=1: one, as before): CoreAnimation draws on a
	 * clock of its own, and a SwapEnd held past its next tick makes that tick late, the clock going
	 * on from there (rc.jobs.fps: 28% of the frames over 16.7 ms with two, 33% with one).
	 */
	IOLock *        fSwapLock;              /* around the next five */
	uint8_t         fSwap[SWAP_QUEUE][SWAP_SIZE];   /* the swaps on their way to the panel (fb_queue), from fSwapFirst on */
	MiniIOSurface * fSwapSurfaces[SWAP_QUEUE][SWAP_LAYERS]; /* ... their surfaces, held from SwapEnd on: their maker may let go of them */
	unsigned        fSwapFirst, fSwapsWaiting;
	/*
	 * ... and not whenever the GPU happens to be done, but on a beat of the refresh rate
	 * (boot-arg fb_beat=0: at once): frames then follow each other at an even pace whatever
	 * each took to draw, as long as there is one for every beat. The beat is this driver's own.
	 * (CoreAnimation does not go by the refresh notifications of this driver -- it never asks
	 * for them -- but by a clock of its own at the rate it reads here; and where the panel's
	 * scan is nobody can read: the display block's register clock is off.) It starts with a
	 * frame that comes later than a beat and a half after the one before -- the first after a
	 * pause, or every one of a renderer that does not keep up, the software one: such a frame
	 * is shown at once, a beat would only make it later still -- and gives way by an eighth of
	 * what a frame is early or late, so that frames are ready some six milliseconds before
	 * their beat: neither a slow one nor a quick one then lands on the wrong side of it.
	 */
	uint64_t        fBeat;                  /* the beat the last frame was shown on */
	uint64_t        fReady;                 /* when the last frame was ready to be shown */
	uint32_t        fStatOnBeat;            /* frames that waited for a refresh, and for how long together */
	uint32_t        fStatSettled;           /* frames drawn again, filtered, once they stood still (fb_filter=2) */
	uint64_t        fStatBeatWait;
	uint32_t        fStatSurfaces[8];       /* the first different surfaces the bottom layer was given */
	uint32_t        fStatHeld, fStatLost;   /* swaps that waited for the one before; that gave up waiting */
	uint64_t        fSwapTime;              /* when the last swap was handed over */
	uint32_t        fStatSwapsLate;         /* swaps that came more than a refresh and a quarter after the one before (and no pause) */
	uint64_t        fStatWaiting;           /* how long frames waited for the GPU here */
	/* how the display is doing: frames and time spent drawing them here since fStatSince */
	uint32_t        fStatFrames;
	uint64_t        fStatSince, fStatDrawing;
	/* ... and how evenly: the time from each frame to the next, counted by the millisecond (the last count: longer) */
	uint64_t        fLastFrame;
	uint32_t        fFrameTimes[FRAME_TIMES + 1], fFramePauses, fFrameLongest;
	/* boot-arg fb_diff=1: which pixels of the panel each frame changed (it costs: the panel's memory is slow to read) */
	bool            fDiff;
	uint64_t        fDiffPixels;
	uint32_t        fDiffLeft, fDiffTop, fDiffRight, fDiffBottom;
	/* the screen recording, if one was asked for */
	uint32_t *      fColumn;                /* drawLayer(): source column for every panel column */
	uint16_t *      fColumnWeight;          /* ... with fb_filter: how much of the column after it, of 256 */
	/*
	 * ... and, unfiltered, the same map as runs: panel columns whose source columns follow each other, copied as one
	 * piece (1125 onto 1080 is 45 runs of 24 or so a row: some 45 copies in place of 1080 lookups)
	 */
	uint32_t *      fRunFrom;
	uint32_t *      fRunTo;
	uint32_t *      fRunLength;
	uint32_t        fRuns;
	uint32_t        fStatReports;
	uint32_t *      fBlendRow;              /* ... and a row of the picture blended from two (fBlendRowSize pixels) */
	uint32_t        fBlendRowSize;
	uint8_t *       fRecBase;
	uint64_t        fRecSize, fRecUsed;
	uint64_t        fFirstFrame, fRecFirst; /* when the first frame was shown, and the first recorded */
	bool            fRecDone;
	bool            fRecWrapped;            /* (rec_ring) the memory has been full once: frames go over the oldest */
	uint64_t        fRecCost, fRecCostMax;  /* what copying the recorded frames took (mach_absolute_time), and the worst */
	uint32_t        fRecCosted;             /* ... of how many frames: a line every 120 */

	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	void publishModes(void);
	void publishRefresh(void);
	MiniIOSurface * defaultSurface(void);
	void present(const uint8_t * swap);
	void show(const uint8_t * swap, MiniIOSurface ** surfaces);
	void settle(const uint8_t * swap, MiniIOSurface ** surfaces);
	bool fSettling;                 /* drawing the last frame again, filtered (fb_filter=2) */
	void presenter(void);
	static void presenterMain(void * self, wait_result_t result);
	void reportFrameTimes(void);
	void drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst);
	void drawRows(const struct RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow);
	static void helperMain(void * self, wait_result_t result);
	/* the scaling shared out (fb_threads): fThreads drawers, the presenter and fThreads - 1 helpers */
	unsigned        fThreads;
	IOLock *        fHelperLock;
	const struct RowJob * fHelperJob;
	uint64_t        fHelperGeneration;
	unsigned        fHelpersBusy, fHelpersStarted;
	uint32_t *      fHelperBlend[7];
	void record(MiniIOSurface * s);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

static void
setNumber(OSDictionary * dict, const char * key, uint64_t value)
{
	OSNumber * n = OSNumber::withNumber(value, 32);

	if (n != NULL) {
		dict->setObject(key, n);
		n->release();
	}
}

/*
 * The modes of the display, the way CoreAnimation reads them (the properties TimingElements
 * and ColorElements; without a mode it draws nothing). One timing, the panel's size at
 * 60 Hz (boot-arg fb_hz=N: at N), with one colour mode, 8-bit RGB. The identifiers are what "the current mode"
 * (block 0x40) answers with.
 */
#define kFBTimingID     1
#define kFBColorID      1

/*
 * The refresh the window server clocks its frames by: the property IOMFBDisplayRefresh, a dictionary of the shortest
 * interval between refreshes, the longest, and the step between them (QuartzCore, windowserver-apple-display.cpp:
 * displayMinRefreshInterval, displayRefreshStepMachTime; CoreMedia's FPSupport: displayMaxRefreshInterval,
 * displayRefreshStep). Without it CoreAnimation takes a built-in panel for a 60 Hz one whatever its timing says: the
 * mode it makes is "internal_panel (fixed)" at 60, UIScreen's maximumFramesPerSecond is 60, and frames came at 60 on
 * the 120 Hz panel (userland/src/fps_info.m, 30 October).
 * The intervals are in units of 2^-32 of a second (32.32 fixed point), not in mach_absolute_time's: given 1/120 s in
 * the phone's ticks (160000 of 19.2 MHz) CoreAnimation answered a refresh rate of 26844 a second, which is
 * 2^32 / 160000. Only the step whose name says MachTime is in ticks. This panel does not vary its rate, so all of
 * them are the one interval, fb_hz's.
 */
void
IOMobileFramebuffer::publishRefresh(void)
{
	OSDictionary * refresh = OSDictionary::withCapacity(4);
	uint32_t told = gCARefreshHz >= 1 && gCARefreshHz <= 480 ? gCARefreshHz : refreshHz();
	uint64_t interval = (1ULL << 32) / told, ticks;

	if (refresh == NULL) {
		return;
	}
	nanoseconds_to_absolutetime(NSEC_PER_SEC / told, &ticks);
	setNumber(refresh, "displayMinRefreshInterval", interval);
	setNumber(refresh, "displayMaxRefreshInterval", interval);
	setNumber(refresh, "displayRefreshStep", interval);
	setNumber(refresh, "displayRefreshStepMachTime", ticks);
	setProperty("IOMFBDisplayRefresh", refresh);
	refresh->release();
	IOLog("MiniIOMobileFramebuffer: IOMFBDisplayRefresh: %u Hz (the panel: %u), an interval of %llu (2^-32 s), %llu ticks\n",
	    told, refreshHz(), interval, ticks);
}

void
IOMobileFramebuffer::publishModes(void)
{
	OSDictionary * timing = OSDictionary::withCapacity(8);
	OSDictionary * horizontal = OSDictionary::withCapacity(4);
	OSDictionary * vertical = OSDictionary::withCapacity(4);
	OSDictionary * color = OSDictionary::withCapacity(8);
	OSArray * timings = OSArray::withCapacity(1);
	OSArray * colors = OSArray::withCapacity(1);
	const uint32_t rate = refreshHz() << 16;                /* 16.16 fixed point */

	if (timing == NULL || horizontal == NULL || vertical == NULL || color == NULL || timings == NULL || colors == NULL) {
		return;
	}
	setNumber(color, "ID", kFBColorID);
	setNumber(color, "PixelEncoding", 0);                   /* RGB 4:4:4 */
	setNumber(color, "Depth", 8);
	setNumber(color, "Colorimetry", 1);
	setNumber(color, "EOTF", 1);
	setNumber(color, "DynamicRange", 0);
	setNumber(color, "Score", 1);
	color->setObject("IsVirtual", kOSBooleanFalse);
	colors->setObject(color);

	setNumber(horizontal, "Active", fWidth);
	setNumber(horizontal, "Total", fWidth);
	setNumber(horizontal, "SyncRate", rate);
	setNumber(horizontal, "PreciseSyncRate", rate);
	setNumber(vertical, "Active", fHeight);
	setNumber(vertical, "Total", fHeight);
	setNumber(vertical, "SyncRate", rate);
	setNumber(vertical, "PreciseSyncRate", rate);
	setNumber(timing, "ID", kFBTimingID);
	setNumber(timing, "Score", 1);
	timing->setObject("IsVirtual", kOSBooleanFalse);
	timing->setObject("IsOverscanned", kOSBooleanFalse);
	timing->setObject("HorizontalAttributes", horizontal);
	timing->setObject("VerticalAttributes", vertical);
	timing->setObject("ColorModes", colors);
	timings->setObject(timing);

	setProperty("TimingElements", timings);
	setProperty("ColorElements", colors);
	publishRefresh();
	OSSafeReleaseNULL(timings);
	OSSafeReleaseNULL(colors);
	OSSafeReleaseNULL(timing);
	OSSafeReleaseNULL(horizontal);
	OSSafeReleaseNULL(vertical);
	OSSafeReleaseNULL(color);
}

bool
IOMobileFramebuffer::start(IOService * provider)
{
	thread_t thread;

	if (!IOService::start(provider)) {
		return false;
	}
	if (vinfo.v_baseaddr == 0 || vinfo.v_width == 0 || vinfo.v_height == 0 || vinfo.v_depth != 32) {
		IOLog("MiniIOMobileFramebuffer: no usable boot framebuffer (%ux%u, depth %u)\n", vinfo.v_width, vinfo.v_height,
		    vinfo.v_depth);
		return false;
	}
	if (gMeasure && kernel_thread_start(measurePanel, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	fLock = IOLockAlloc();
	fSwapLock = IOLockAlloc();
	if (panel_thread_start(&IOMobileFramebuffer::presenterMain, this, &thread) != KERN_SUCCESS) {
		IOLog("MiniIOMobileFramebuffer: no thread to put frames on the panel with\n");
		return false;
	}
	thread_set_thread_name(thread, "frames to the panel");
	thread_deallocate(thread);
	fThreads = gThreads >= 1 && gThreads <= 8 ? gThreads : 1;
	fHelperLock = IOLockAlloc();
	for (unsigned i = 0; i + 1 < fThreads; i++) {
		if (panel_thread_start(&IOMobileFramebuffer::helperMain, this, &thread) != KERN_SUCCESS) {
			fThreads = i + 1;
			break;
		}
		thread_set_thread_name(thread, "frames to the panel: a helper");
		thread_deallocate(thread);
	}
	fBase = (uint8_t *) vinfo.v_baseaddr;
	fPanelWidth = vinfo.v_width;
	fPanelHeight = vinfo.v_height;
	fRowBytes = vinfo.v_rowbytes;
	flip_setup(fBase, fRowBytes, fPanelHeight);
	fWidth = gFBWidth ? gFBWidth : fPanelWidth;
	fHeight = gFBHeight ? gFBHeight : fPanelHeight;
	if (gFBScale != 0 && gFBScale != kFBDefaultScale) {
		static const char * const places[][2] = { { "/chosen", "display-scale" }, { "/product", "artwork-scale-factor" } };
		uint32_t scale = gFBScale;

		fWidth = kFBPointsWide * scale;
		fHeight = kFBPointsHigh * scale;
		/* what the loader's device tree says about the scale is where MobileGestalt has it from */
		for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
			IORegistryEntry * node = IORegistryEntry::fromPath(places[i][0], gIODTPlane);

			if (node != NULL) {
				node->setProperty(places[i][1], &scale, sizeof(scale));
				node->release();
			}
		}
	}
	fColumn = IONewZero(uint32_t, fPanelWidth);
	fRunFrom = IONewZero(uint32_t, fPanelWidth);
	fRunTo = IONewZero(uint32_t, fPanelWidth);
	fRunLength = IONewZero(uint32_t, fPanelWidth);
	fColumnWeight = IONewZero(uint16_t, fPanelWidth);
	fDiff = gDiff != 0;
	fDiffLeft = fDiffTop = ~0u;
	IOLog("MiniIOMobileFramebuffer: display %ux%u on a panel of %ux%u (%u bytes per row)\n", fWidth, fHeight,
	    fPanelWidth, fPanelHeight, fRowBytes);

	if (gRecSeconds != 0) {
		/* one piece of physical memory: the loader will send it over USB as it lies */
		IOBufferMemoryDescriptor * memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		    kIODirectionInOut | kIOMemoryPhysicallyContiguous, (mach_vm_size_t) gRecMegabytes << 20, ~0ULL);

		if (memory != NULL) {
			struct rec_header * header = (struct rec_header *) memory->getBytesNoCopy();

			fRecBase = (uint8_t *) header;
			fRecSize = (uint64_t) gRecMegabytes << 20;
			fRecUsed = sizeof(*header);
			bzero(header, sizeof(*header));
			memcpy(header->magic, "RWREC003", 8);
			header->width = fWidth / REC_STEP;
			header->height = fHeight / REC_STEP;
			loader_recording[0] = memory->getPhysicalSegment(0, NULL);
			loader_recording[1] = fRecUsed;
			loader_recording[2] = (uint64_t) (uintptr_t) fRecBase;
			IOLog("MiniIOMobileFramebuffer: recording %u s of the screen at %ux%u from second %u, into %u MB at %#llx%s\n",
			    gRecSeconds, header->width, header->height, gRecStart, gRecMegabytes, loader_recording[0],
			    gRecRing ? ", the last of it kept (rec_ring)" : "");
		} else {
			IOLog("MiniIOMobileFramebuffer: no memory for a recording of %u MB\n", gRecMegabytes);
		}
	}

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	/* for who draws with the GPU (userland/src/eagl.c): a frame need not be waited for before it is handed over */
	setProperty("HakiosWaitsForGPU", kOSBooleanTrue);
	publishModes();
	setName("disp0");
	registerService();
	return true;
}

/*
 * Are the first `rows` rows of a surface, `columns` 32-bit pixels of each, inside the memory it
 * has? Its stride, element height and size come from whoever made it, so they are checked here,
 * before anything reads the rows: in 64 bits, so that nothing wraps.
 */
static bool
surfaceHolds(const MiniIOSurface * s, uint64_t rows, uint64_t columns)
{
	if (rows == 0 || columns == 0) {
		return true;
	}
	return (uint64_t) s->info.offset + (rows - 1) * s->info.bytesPerRow + columns * 4 <= s->info.allocSize;
}

/* Keep a small copy of the frame that is about to be shown (the surface is the whole screen). */
void
IOMobileFramebuffer::record(MiniIOSurface * s)
{
	struct rec_header * header = (struct rec_header *) fRecBase;
	uint64_t now = mach_absolute_time(), since_first, since_start = 0, frame_size;
	struct rec_frame * frame;
	uint8_t * to;

	if (fFirstFrame == 0) {
		fFirstFrame = now;
	}
	if (fRecBase == NULL || fRecDone || s->info.width < fWidth || s->info.height < fHeight) {
		return;
	}
	/* a surface whose rows do not fit in its memory is not read */
	if (!surfaceHolds(s, fHeight, fWidth)) {
		return;
	}
	absolutetime_to_nanoseconds(now - fFirstFrame, &since_first);
	if (since_first < (uint64_t) gRecStart * NSEC_PER_SEC) {
		return;
	}
	if (fRecFirst == 0) {
		uint64_t began;

		fRecFirst = now;
		absolutetime_to_nanoseconds(now, &began);
		header->began = began;
		IOLog("MiniIOMobileFramebuffer: the recording begins at %llu.%03llu s of the kernel's time (mach_absolute_time)\n",
		    began / NSEC_PER_SEC, began / NSEC_PER_MSEC % 1000);
	}
	absolutetime_to_nanoseconds(now - fRecFirst, &since_start);
	frame_size = sizeof(*frame) + (uint64_t) header->width * header->height * 3;
	if (since_start > (uint64_t) gRecSeconds * NSEC_PER_SEC || (fRecUsed + frame_size > fRecSize && !gRecRing)) {
		fRecDone = true;
		IOLog("MiniIOMobileFramebuffer: recording finished: %u frames, %llu MB\n", header->frames, fRecUsed >> 20);
		return;
	}
	if (fRecUsed + frame_size > fRecSize) {
		/* (rec_ring) round again: over the oldest, which is the next one */
		fRecUsed = sizeof(*header);
		fRecWrapped = true;
	}
	frame = (struct rec_frame *) (fRecBase + fRecUsed);
	frame->nanoseconds = since_start;
	to = (uint8_t *) (frame + 1);
	for (uint32_t y = 0; y < header->height; y++) {
		const uint32_t * from = (const uint32_t *) (s->pixels() + (size_t) (y * REC_STEP + 1) * s->info.bytesPerRow);

		for (uint32_t x = 0; x < header->width; x++) {
			uint32_t pixel = from[x * REC_STEP + 1];        /* B G R A in memory */

			*to++ = (uint8_t) (pixel >> 16);
			*to++ = (uint8_t) (pixel >> 8);
			*to++ = (uint8_t) pixel;
		}
	}
	fRecUsed += frame_size;
	/* what the copy cost: it runs in the display's thread, between one frame and the next */
	now = mach_absolute_time() - now;
	fRecCost += now;
	fRecCostMax = now > fRecCostMax ? now : fRecCostMax;
	if (++fRecCosted == 120) {
		IOLog("MiniIOMobileFramebuffer: recording a frame took %llu us on average, %llu at worst (the last 120)\n",
		    micros(fRecCost) / 120, micros(fRecCostMax));
		fRecCost = fRecCostMax = 0;
		fRecCosted = 0;
	}
	if (!fRecWrapped) {
		header->frames++;
		loader_recording[1] = fRecUsed;
	} else {
		/* the memory full of frames: all of them are handed over, from the one after this */
		header->oldest = (uint32_t) ((fRecUsed - sizeof(*header)) / frame_size);
		if (header->oldest >= header->frames) {
			header->oldest = 0;
		}
		loader_recording[1] = sizeof(*header) + (uint64_t) header->frames * frame_size;
	}
}

MiniIOSurface *
IOMobileFramebuffer::defaultSurface(void)
{
	IOLockLock(fLock);
	if (fDefaultSurface == NULL) {
		fDefaultSurface = MiniIOSurfaceCreateSimple(fWidth, fHeight, FOURCC('B', 'G', 'R', 'A'));
	}
	IOLockUnlock(fLock);
	return fDefaultSurface;
}

/*
 * Two pixels mixed: `w` of 256 of the second. Two of the four channels at a time, in one word.
 */
static inline uint32_t
blend(uint32_t a, uint32_t b, uint32_t w)
{
	uint32_t rb = ((a & 0xff00ff) * (256 - w) + (b & 0xff00ff) * w) >> 8 & 0xff00ff;
	uint32_t ga = ((a >> 8 & 0xff00ff) * (256 - w) + (b >> 8 & 0xff00ff) * w) & 0xff00ff00;

	return rb | ga;
}

/*
 * Where the middle of the `i`th of `out` pixels falls among `in` of them, in 256ths of a pixel,
 * kept within the first and the last.
 */
static inline int64_t
sample_at(int64_t i, int64_t in, int64_t out)
{
	int64_t at = ((2 * i + 1) * in - out) * 128 / out;

	return at < 0 ? 0 : at > (in - 1) * 256 ? (in - 1) * 256 : at;
}

/*
 * The rows of one layer, from `y0` to `y1` (panel rows), as drawLayer() has worked them out:
 * scaling a whole frame takes one processor some 9 ms, so with boot-arg fb_threads=N it is shared
 * out between N of them, each with rows of its own and a row of blending of its own.
 */
/* A layer's rectangle on the panel, in pixels, further out than this is not drawn (the panel is a few thousand wide). */
#define DST_LIMIT       (1 << 24)

struct RowJob {
	const uint8_t * pixels;
	uint32_t        stride;
	int32_t         sx, sy, sw, sh, px0, py0, px1, py1, cx0, cx1;
	int32_t         ry0, ry1;       /* the rows of the rectangle that are on the panel: all that is looked at, whatever its size */
	bool            filter, unscaled;
};

void
IOMobileFramebuffer::drawRows(const RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow)
{
	const uint8_t * pixels = job->pixels;
	uint32_t stride = job->stride;
	int32_t sx = job->sx, sy = job->sy, sw = job->sw, sh = job->sh;
	int32_t px0 = job->px0, py0 = job->py0, px1 = job->px1, py1 = job->py1, cx0 = job->cx0, cx1 = job->cx1;
	bool filter = job->filter;

	for (int32_t y = y0; y < y1; y++) {
		const uint32_t * from;
		uint32_t * to;

		if (y < 0 || (uint32_t) y >= fPanelHeight) {
			continue;
		}
		to = (uint32_t *) (fBase + (size_t) y * fRowBytes);
		if (job->unscaled) {
			/* not scaled at all */
			memcpy(to + px0, pixels + (size_t) (sy + y - py0) * stride + (size_t) sx * 4, (size_t) sw * 4);
			continue;
		}
		if (filter) {
			int64_t at = sample_at(y - py0, sh, py1 - py0);
			uint32_t row = (uint32_t) (at >> 8), weight = (uint32_t) (at & 255);
			const uint32_t * upper = (const uint32_t *) (pixels + (size_t) (sy + row) * stride) + sx;

			if (weight == 0 || row + 1 >= (uint32_t) sh) {
				from = upper;
			} else {
				const uint32_t * lower = (const uint32_t *) (pixels + (size_t) (sy + row + 1) * stride) + sx;
				uint32_t first = fColumn[cx0], last = fColumn[cx1 - 1] + 1 < (uint32_t) sw ? fColumn[cx1 - 1] + 1 : fColumn[cx1 - 1];

				uint32_t * __restrict mixed = blendRow;

				for (uint32_t c = first; c <= last; c++) {
					uint32_t a = upper[c], b = lower[c];

					mixed[c] = a == b ? a : blend(a, b, weight);           /* (most of a screen is the same as next to it) */
				}
				from = blendRow;
			}
		} else {
			from = (const uint32_t *) (pixels + (size_t) (sy + (int64_t) (y - py0) * sh / (py1 - py0)) * stride);
		}
		if (fDiff) {
			for (int32_t x = cx0; x < cx1; x++) {
				uint32_t value = filter ? blend(from[fColumn[x]], from[fColumn[x] + (fColumnWeight[x] != 0)], fColumnWeight[x]) :
				    from[fColumn[x]];

				if (to[x] != value) {
					fDiffPixels++;
					fDiffLeft = (uint32_t) x < fDiffLeft ? (uint32_t) x : fDiffLeft;
					fDiffRight = (uint32_t) x > fDiffRight ? (uint32_t) x : fDiffRight;
					fDiffTop = (uint32_t) y < fDiffTop ? (uint32_t) y : fDiffTop;
					fDiffBottom = (uint32_t) y > fDiffBottom ? (uint32_t) y : fDiffBottom;
				}
			}
		}
		if (filter) {
			const uint32_t * __restrict column = fColumn + cx0;
			const uint16_t * __restrict weights = fColumnWeight + cx0;
			uint32_t * __restrict out = to + cx0;

			for (uint32_t i = 0, n = (uint32_t) (cx1 - cx0); i < n; i++) {
				uint32_t c = column[i], w = weights[i], a = from[c];

				out[i] = w == 0 || a == from[c + 1] ? a : blend(a, from[c + 1], w);
			}
		} else if (fRuns != 0) {
			const uint32_t * __restrict runFrom = fRunFrom, * __restrict runTo = fRunTo, * __restrict runLength = fRunLength;

			for (uint32_t r = 0, n = fRuns; r < n; r++) {
				memcpy(to + runTo[r], from + runFrom[r], (size_t) runLength[r] * 4);
			}
		} else {
			for (int32_t x = cx0; x < cx1; x++) {
				to[x] = from[fColumn[x]];
			}
		}
	}
}

/* A helper of the scaling (fb_threads): waits for rows to do, does them, says so. */
void
IOMobileFramebuffer::helperMain(void * self, wait_result_t result __unused)
{
	IOMobileFramebuffer * fb = (IOMobileFramebuffer *) self;
	unsigned index;
	uint64_t done = 0;

	IOLockLock(fb->fHelperLock);
	index = fb->fHelpersStarted++;
	IOLockUnlock(fb->fHelperLock);
	for (;;) {
		const RowJob * job;
		int32_t rows, y0, y1;
		uint32_t * blend;

		IOLockLock(fb->fHelperLock);
		while (fb->fHelperGeneration == done) {
			IOLockSleep(fb->fHelperLock, &fb->fHelperGeneration, THREAD_UNINT);
		}
		done = fb->fHelperGeneration;
		job = fb->fHelperJob;
		IOLockUnlock(fb->fHelperLock);
		/* share index + 1 of fThreads (the presenter takes share 0) */
		rows = job->ry1 - job->ry0;
		y0 = job->ry0 + (int32_t) ((int64_t) rows * (index + 1) / fb->fThreads);
		y1 = job->ry0 + (int32_t) ((int64_t) rows * (index + 2) / fb->fThreads);
		blend = fb->fHelperBlend[index];
		fb->drawRows(job, y0, y1, blend);
		IOLockLock(fb->fHelperLock);
		fb->fHelpersBusy--;
		if (fb->fHelpersBusy == 0) {
			IOLockWakeup(fb->fHelperLock, &fb->fHelpersBusy, true);
		}
		IOLockUnlock(fb->fHelperLock);
	}
}

/*
 * Put a rectangle of a 32-bit surface on the screen. `dst` is in display coordinates; the
 * panel may be of another size, and the layer itself may be scaled: both are done in one
 * step, bilinear (or nearest neighbour: fb_filter=0).
 */
void
IOMobileFramebuffer::drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst)
{
	int32_t sx = src[0], sy = src[1], sw = src[2], sh = src[3];
	int32_t dx = dst[0], dy = dst[1], dw = dst[2], dh = dst[3];
	int32_t px0, py0, px1, py1, cx0, cx1;
	bool filter = gFilter == 1 || (gFilter == 2 && fSettling);
	RowJob job;

	if (sw <= 0 || sh <= 0) {
		sx = sy = 0;
		sw = (int32_t) s->info.width;
		sh = (int32_t) s->info.height;
	}
	if (dw <= 0 || dh <= 0) {
		dx = dy = 0;
		dw = (int32_t) fWidth;
		dh = (int32_t) fHeight;
	}
	if (sx < 0 || sy < 0 || (uint32_t) (sx + sw) > s->info.width || (uint32_t) (sy + sh) > s->info.height) {
		return;
	}
	/* the rows and columns named must be in the surface's memory, whatever stride and size it was made with */
	if (!surfaceHolds(s, (uint32_t) (sy + sh), (uint32_t) (sx + sw))) {
		return;
	}
	/*
	 * the rectangle on the panel, worked out in 64 bits: the request's numbers are the caller's, and a
	 * rectangle of billions of rows is not drawn (nor looked at row by row)
	 */
	int64_t qx0 = (int64_t) dx * fPanelWidth / fWidth;
	int64_t qy0 = (int64_t) dy * fPanelHeight / fHeight;
	int64_t qx1 = ((int64_t) dx + dw) * fPanelWidth / fWidth;
	int64_t qy1 = ((int64_t) dy + dh) * fPanelHeight / fHeight;

	if (qx0 < -DST_LIMIT || qy0 < -DST_LIMIT || qx1 > DST_LIMIT || qy1 > DST_LIMIT) {
		return;
	}
	px0 = (int32_t) qx0;
	py0 = (int32_t) qy0;
	px1 = (int32_t) qx1;
	py1 = (int32_t) qy1;
	if (px1 <= px0 || py1 <= py0) {
		return;
	}
	/* nothing of it on the panel: nothing to draw (and the columns below are only worked out for ones that are) */
	if (px1 <= 0 || px0 >= (int32_t) fPanelWidth || py1 <= 0 || py0 >= (int32_t) fPanelHeight) {
		return;
	}
	/* (blending, a row of the picture mixed from two: room for it, for every one who draws rows) */
	if (filter && fBlendRowSize < (uint32_t) sw) {
		if (fBlendRow != NULL) {
			IODelete(fBlendRow, uint32_t, fBlendRowSize);
		}
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			if (fHelperBlend[i] != NULL) {
				IODelete(fHelperBlend[i], uint32_t, fBlendRowSize);
			}
		}
		fBlendRowSize = (uint32_t) sw;
		fBlendRow = IONew(uint32_t, fBlendRowSize);
		bool allocated = fBlendRow != NULL;

		for (unsigned i = 0; i + 1 < fThreads; i++) {
			fHelperBlend[i] = IONew(uint32_t, fBlendRowSize);
			allocated = allocated && fHelperBlend[i] != NULL;
		}
		if (!allocated) {
			/* every array that did get made is given back at the size it was made with, so that no later growth frees one with the wrong size */
			if (fBlendRow != NULL) {
				IODelete(fBlendRow, uint32_t, fBlendRowSize);
				fBlendRow = NULL;
			}
			for (unsigned i = 0; i + 1 < fThreads; i++) {
				if (fHelperBlend[i] != NULL) {
					IODelete(fHelperBlend[i], uint32_t, fBlendRowSize);
					fHelperBlend[i] = NULL;
				}
			}
			fBlendRowSize = 0;
		}
	}
	filter = filter && fBlendRow != NULL;
	/* which pixel of a source row each pixel of a panel row takes: worked out once, not 2.6 million times */
	cx0 = px0 < 0 ? 0 : px0;
	cx1 = (uint32_t) px1 > fPanelWidth ? (int32_t) fPanelWidth : px1;
	for (int32_t x = cx0; x < cx1; x++) {
		if (filter) {
			int64_t at = sample_at(x - px0, sw, px1 - px0);

			fColumn[x] = (uint32_t) (at >> 8);              /* (in the blended row: from sx on) */
			fColumnWeight[x] = (uint16_t) (fColumn[x] + 1 < (uint32_t) sw ? at & 255 : 0);
		} else {
			fColumn[x] = (uint32_t) (sx + (int64_t) (x - px0) * sw / (px1 - px0));
		}
	}
	fRuns = 0;
	if (!filter && !fDiff && fRunFrom != NULL && fRunTo != NULL && fRunLength != NULL) {
		for (int32_t x = cx0; x < cx1; x++) {
			if (fRuns != 0 && fColumn[x] == fColumn[x - 1] + 1 && (uint32_t) x == fRunTo[fRuns - 1] + fRunLength[fRuns - 1]) {
				fRunLength[fRuns - 1]++;
			} else {
				fRunFrom[fRuns] = fColumn[x];
				fRunTo[fRuns] = (uint32_t) x;
				fRunLength[fRuns] = 1;
				fRuns++;
			}
		}
	}
	job.pixels = s->pixels();
	job.stride = s->info.bytesPerRow;
	job.sx = sx;
	job.sy = sy;
	job.sw = sw;
	job.sh = sh;
	job.px0 = px0;
	job.py0 = py0;
	job.px1 = px1;
	job.py1 = py1;
	job.cx0 = cx0;
	job.cx1 = cx1;
	job.ry0 = py0 < 0 ? 0 : py0;
	job.ry1 = py1 > (int32_t) fPanelHeight ? (int32_t) fPanelHeight : py1;
	job.filter = filter;
	job.unscaled = px1 - px0 == sw && py1 - py0 == sh && px0 >= 0 && (uint32_t) px1 <= fPanelWidth;
	if (fThreads <= 1 || fDiff || fHelpersStarted + 1 < fThreads) {
		drawRows(&job, job.ry0, job.ry1, fBlendRow);
		return;
	}
	/* shared out: the helpers take their rows, this thread the first share, then waits for them */
	IOLockLock(fHelperLock);
	fHelperJob = &job;
	fHelpersBusy = fThreads - 1;
	fHelperGeneration++;
	IOLockWakeup(fHelperLock, &fHelperGeneration, false);
	IOLockUnlock(fHelperLock);
	drawRows(&job, job.ry0, job.ry0 + (int32_t) ((int64_t) (job.ry1 - job.ry0) / fThreads), fBlendRow);
	IOLockLock(fHelperLock);
	while (fHelpersBusy != 0) {
		IOLockSleep(fHelperLock, &fHelpersBusy, THREAD_UNINT);
	}
	IOLockUnlock(fHelperLock);
}

/*
 * How evenly the frames of the last seconds came: how many followed the one before within the
 * time of one refresh at 120, 60, 40 and 30 a second, and the times half and 95 in 100 of them
 * stayed under. With the lock held.
 */
void
IOMobileFramebuffer::reportFrameTimes(void)
{
	static const uint32_t limits[] = { 10, 18, 26, 35, FRAME_TIMES + 1 };
	uint32_t within[sizeof(limits) / sizeof(limits[0])] = { 0 }, total = 0, sum = 0, half = 0, most = 0;

	for (uint32_t ms = 0; ms <= FRAME_TIMES; ms++) {
		for (uint32_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
			if (ms < limits[i]) {
				within[i] += fFrameTimes[ms];
				break;
			}
		}
		total += fFrameTimes[ms];
	}
	for (uint32_t ms = 0; ms <= FRAME_TIMES && total != 0; ms++) {
		sum += fFrameTimes[ms];
		if (half == 0 && 2 * sum >= total) {
			half = ms + 1;
		}
		if (most == 0 && 100 * sum >= 95 * total) {
			most = ms + 1;
		}
	}
	if (total != 0) {
		IOLog("MiniIOMobileFramebuffer: from one frame to the next: %u times under 10 ms, %u under 18, %u under 26, %u under 35, "
		    "%u longer (the longest %u ms); half under %u ms, 95 in 100 under %u ms; %u pauses\n", within[0], within[1],
		    within[2], within[3], within[4], fFrameLongest, half, most, fFramePauses);
	}
	bzero(fFrameTimes, sizeof(fFrameTimes));
	fFramePauses = fFrameLongest = 0;
}

/* SwapEnd: the swap is handed to the thread that puts frames on the panel, as soon as that has no other waiting. */
void
IOMobileFramebuffer::present(const uint8_t * swap)
{
	AbsoluteTime deadline;

	uint64_t now = mach_absolute_time(), period, pause;

	/* (a frame that is late on the panel was late here already, or it was not: for telling whose it is) */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz() * 5 / 4, &period);
	nanoseconds_to_absolutetime(FRAME_PAUSE * NSEC_PER_MSEC, &pause);
	clock_interval_to_deadline(SWAP_HELD_MS, kMillisecondScale, &deadline);
	if (gTouchLat) {
		hakios_tl("tl: s %u at %llu\n", swapWord(swap, SWAP_ID), micros(now));
	}
	IOLockLock(fSwapLock);
	fStatSwapsLate += fSwapTime != 0 && now - fSwapTime > period && now - fSwapTime < pause;
	fSwapTime = now;
	fStatHeld += fSwapsWaiting >= swapQueue();
	while (fSwapsWaiting >= swapQueue()) {
		if (IOLockSleepDeadline(fSwapLock, &fSwapsWaiting, deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
		    fSwapsWaiting >= swapQueue()) {
			fStatLost++;                    /* (the GPU has stopped answering: the frame before is waiting for it) */
			IOLockUnlock(fSwapLock);
			return;
		}
	}
	unsigned slot = (fSwapFirst + fSwapsWaiting) % SWAP_QUEUE;

	memcpy(fSwap[slot], swap, SWAP_SIZE);
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));

		fSwapSurfaces[slot][layer] = sid != 0 ? MiniIOSurfaceCopy(sid) : NULL;
	}
	fSwapsWaiting++;
	IOLockWakeup(fSwapLock, fSwap, true);
	IOLockUnlock(fSwapLock);
}

void
IOMobileFramebuffer::presenterMain(void * self, wait_result_t result __unused)
{
	((IOMobileFramebuffer *) self)->presenter();
}

void
IOMobileFramebuffer::presenter(void)
{
	static uint8_t swap[SWAP_SIZE];         /* (one thread) */
	MiniIOSurface * surfaces[SWAP_LAYERS] = { NULL };
	bool unsettled = false;                 /* the frame on the panel was drawn unfiltered, and its surfaces are held */

	for (;;) {
		IOLockLock(fSwapLock);
		while (fSwapsWaiting == 0) {
			uint64_t deadline;

			if (!unsettled) {
				IOLockSleep(fSwapLock, fSwap, THREAD_UNINT);
				continue;
			}
			clock_interval_to_deadline(SETTLE_MS, NSEC_PER_MSEC, &deadline);
			if (IOLockSleepDeadline(fSwapLock, fSwap, deadline, THREAD_UNINT) == THREAD_TIMED_OUT && fSwapsWaiting == 0) {
				IOLockUnlock(fSwapLock);
				settle(swap, surfaces);
				unsettled = false;
				IOLockLock(fSwapLock);
			}
		}
		IOLockUnlock(fSwapLock);
		/* the frame before is done with now, settled or not */
		for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
			if (surfaces[layer] != NULL) {
				MiniIOSurfaceDrop(surfaces[layer]);
				surfaces[layer] = NULL;
			}
		}
		IOLockLock(fSwapLock);
		memcpy(swap, fSwap[fSwapFirst], SWAP_SIZE);
		memcpy(surfaces, fSwapSurfaces[fSwapFirst], sizeof(surfaces));
		IOLockUnlock(fSwapLock);
		show(swap, surfaces);
		unsettled = gFilter == 2;
		if (!unsettled) {
			for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
				if (surfaces[layer] != NULL) {
					MiniIOSurfaceDrop(surfaces[layer]);
					surfaces[layer] = NULL;
				}
			}
		}
		MiniIOSurfaceClearKept();               /* (the next frame is some milliseconds away) */
	}
}

/*
 * The frame on the panel once more, filtered (fb_filter=2): the same layers from the same surfaces where show() put
 * them, nothing else (no turn to wait for, no swap to hand on, nothing counted). Its surfaces were held since.
 */
void
IOMobileFramebuffer::settle(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	int32_t src[4], dst[4];

	if (!fGraphics) {
		return;
	}
	fSettling = true;
	if (gFlipper.on) {
		flip_wait_taken();
		fBase = gFlipper.on ? gFlipper.va[1 - gFlipper.front] : gFlipper.va[0];
	}
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		MiniIOSurface * s = surfaces[layer];

		if (s == NULL || s->info.bytesPerElement != 4 || s->info.planeCount != 0) {
			continue;
		}
		memcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));
		memcpy(dst, swap + SWAP_DEST(layer), sizeof(dst));
		drawLayer(s, src, dst);
	}
	if (gHud) {
		hud_draw(fBase, fRowBytes, fPanelWidth, fPanelHeight);
	}
	if (gFlipper.on) {
		flip_point(1 - gFlipper.front);
	}
	fSettling = false;
	fStatSettled++;
}

/* A swap's surfaces onto the panel, once the GPU is done with them. */
void
IOMobileFramebuffer::show(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	uint32_t set = swapWord(swap, SWAP_LAYERS_SET);
	uint64_t began = mach_absolute_time(), nanos, ready, shown, drawn;
	uint32_t layers = 0;
	MiniIOSurface * recorded = NULL;
	bool report;

	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		if (surfaces[layer] != NULL && !MiniIOSurfaceDrawn(surfaces[layer], SWAP_GPU_MS)) {
			IOLog("MiniIOMobileFramebuffer: swap %u: the GPU is not done with surface %u after %u ms; shown as it is\n",
			    swapWord(swap, SWAP_ID), swapWord(swap, SWAP_SURFACE(layer)), SWAP_GPU_MS);
		}
	}
	fStatWaiting += mach_absolute_time() - began;
	began = ready = mach_absolute_time();
	if (gPanelLinePA != 0) {
		waitForPanelGap();
		fStatOnBeat++;
		fStatBeatWait += mach_absolute_time() - began;
		began = mach_absolute_time();
	} else if (gOnTheBeat && !gFlipper.on) {
		uint64_t period, aim, next;

		nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
		aim = period * 3 / 8;
		next = fReady;
		fReady = began;
		if (fBeat == 0 || began - next > period * 3 / 2) {
			fBeat = began;
		} else {
			for (next = fBeat + period; next < began; next += period) {
			}
			if (next - began > aim) {
				next -= (next - began - aim) / 8;
			} else {
				next += (aim - (next - began)) / 8;
			}
			clock_delay_until(next);
			fBeat = next;
			fStatOnBeat++;
			fStatBeatWait += mach_absolute_time() - began;
			began = mach_absolute_time();
		}
	}
	if (gFlipper.on) {
		/* (fb_flip) the buffer not being fetched: free once the last flip is taken -- the wait is the refresh's beat */
		flip_wait_taken();
		fStatOnBeat++;
		fStatBeatWait += mach_absolute_time() - began;
		began = mach_absolute_time();
		if (gFlipper.on) {
			fBase = gFlipper.va[1 - gFlipper.front];
		} else {
			fBase = gFlipper.va[0];
		}
	}
	shown = began;
	/* its turn has come: the swap after it may be handed over */
	IOLockLock(fSwapLock);
	fSwapFirst = (fSwapFirst + 1) % SWAP_QUEUE;
	fSwapsWaiting--;
	IOLockWakeup(fSwapLock, &fSwapsWaiting, true);
	IOLockUnlock(fSwapLock);
	IOLockLock(fLock);
	if (fLastFrame != 0) {
		absolutetime_to_nanoseconds(began - fLastFrame, &nanos);
		nanos /= NSEC_PER_MSEC;
		if (nanos >= FRAME_PAUSE) {
			fFramePauses++;
		} else {
			fFrameTimes[nanos < FRAME_TIMES ? nanos : FRAME_TIMES]++;
			gHudState.late += nanos * 2 >= 3000 / refreshHz();     /* (a refresh and a half: 12.5 ms at 120 Hz) */
			fFrameLongest = nanos > fFrameLongest ? (uint32_t) nanos : fFrameLongest;
		}
	}
	fLastFrame = began;
	report = fSwapsShown < 8;
	if (report) {
		IOLog("MiniIOMobileFramebuffer: swap %u (#%u): surfaces %u %u %u %u, layers set 0x%x cleared 0x%x\n",
		    swapWord(swap, SWAP_ID), fSwapsShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)),
		    swapWord(swap, SWAP_SURFACE(2)), swapWord(swap, SWAP_SURFACE(3)), set, swapWord(swap, SWAP_LAYERS_CLEARED));
	}
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));
		int32_t src[4], dst[4];
		MiniIOSurface * s;

		layers |= sid != 0 ? 1u << layer : 0;
		/* the frames' surfaces: is it a few, in turn? */
		for (unsigned i = 0; layer == 0 && sid != 0 && i < 8; i++) {
			if (fStatSurfaces[i] == 0 || fStatSurfaces[i] == sid) {
				fStatSurfaces[i] = sid;
				break;
			}
		}
		if (sid == 0) {
			/* the bottom layer taken away: nothing is shown, the screen is black */
			if (layer == 0 && (set & 1) && fGraphics) {
				for (uint32_t y = 0; y < fPanelHeight; y++) {
					bzero(fBase + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4);
				}
			}
			continue;
		}
		memcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));
		memcpy(dst, swap + SWAP_DEST(layer), sizeof(dst));
		s = surfaces[layer];
		if (report) {
			IOLog("MiniIOMobileFramebuffer: swap %u layer %u: surface %u%s (%ux%u '%c%c%c%c') "
			    "from %d,%d %dx%d to %d,%d %dx%d, layers set 0x%x\n", swapWord(swap, SWAP_ID), layer, sid,
			    s ? "" : " MISSING", s ? s->info.width : 0, s ? s->info.height : 0,
			    s ? (int) (s->info.pixelFormat >> 24 & 0xff) : '?', s ? (int) (s->info.pixelFormat >> 16 & 0xff) : '?',
			    s ? (int) (s->info.pixelFormat >> 8 & 0xff) : '?', s ? (int) (s->info.pixelFormat & 0xff) : '?',
			    src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], set);
		}
		if (s == NULL) {
			continue;
		}
		if (s->info.bytesPerElement == 4 && s->info.planeCount == 0) {
			if (!fGraphics) {
				/* the first picture: the kernel's text console must stop drawing over it */
				fGraphics = true;
				PE_initialize_console(NULL, kPEGraphicsMode);
			}
			drawLayer(s, src, dst);
			if (layer == 0) {
				recorded = s;   /* (recorded once the frame is in the panel's memory: see below) */
			}
		}
	}
	if (gHud && fGraphics) {
		hud_tick();
		hud_draw(fBase, fRowBytes, fPanelWidth, fPanelHeight);
	}
	if (gFlipper.on) {
		flip_point(1 - gFlipper.front);
	}
	drawn = mach_absolute_time();
	if (recorded != NULL) {
		record(recorded);
	}
	fSwapsShown++;
	if (gTouchLat) {
		hakios_tl("tl: v %u ready %llu shown %llu done %llu\n", swapWord(swap, SWAP_ID), micros(ready), micros(shown),
		    micros(drawn));
	}
	if (layers != fLayersShown) {
		IOLog("MiniIOMobileFramebuffer: swap #%u: surfaces in layers 0x%x now (0x%x before): %u %u %u %u\n", fSwapsShown, layers,
		    fLayersShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)), swapWord(swap, SWAP_SURFACE(2)),
		    swapWord(swap, SWAP_SURFACE(3)));
		fLayersShown = layers;
	}

	/* every five seconds in which something was drawn: how many frames, and what each cost here */
	fStatFrames++;
	fStatDrawing += mach_absolute_time() - began;
	if (fStatSince == 0) {
		fStatSince = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - fStatSince, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t drawing, waiting, beat;
		unsigned different;

		absolutetime_to_nanoseconds(fStatDrawing, &drawing);
		absolutetime_to_nanoseconds(fStatWaiting, &waiting);
		absolutetime_to_nanoseconds(fStatBeatWait, &beat);
		IOLog("MiniIOMobileFramebuffer: %u frames in %llu ms (%llu a second), %llu us each in the driver, after %llu us "
		    "waiting for the GPU and, %u of them, %llu us for the next refresh; %u swaps waited for the frame before, "
		    "%u gave up; %u swaps came a refresh late themselves; %u frames drawn again filtered\n", fStatFrames, nanos / NSEC_PER_MSEC,
		    fStatFrames * NSEC_PER_SEC / nanos, drawing / fStatFrames / NSEC_PER_USEC,
		    waiting / fStatFrames / NSEC_PER_USEC, fStatOnBeat, fStatOnBeat ? beat / fStatOnBeat / NSEC_PER_USEC : 0,
		    fStatHeld, fStatLost, fStatSwapsLate, fStatSettled);
		fStatSwapsLate = 0;
		if (gFlip) {
			uint64_t flipWait;

			absolutetime_to_nanoseconds(gFlipper.waitAbs, &flipWait);
			IOLog("MiniIOMobileFramebuffer: flip: %s; %u flips, %u seen taken (%llu us waited each), %u not taken in %u ms\n",
			    gFlipper.on ? "two buffers" : "one buffer", gFlipper.flips, gFlipper.waited,
			    gFlipper.waited ? flipWait / gFlipper.waited / NSEC_PER_USEC : 0, gFlipper.misses, FLIP_TAKEN_MS);
			if (gFlipper.on && (fStatReports++ % 6) == 0) {
				uint32_t pipe = kFlipPipe[gFlipper.pipe];

				/* (again now and then: the boot's lines are long gone from the log when it is fetched) */
				IOLog("MiniIOMobileFramebuffer: flip: pipe %s, control paths %#x: source size %#x, image size %#x, at %#x, "
				    "out size %#x at %#x, stride %#x, format %#x, unpack %#x, op mode %#x\n", kFlipPipeName[gFlipper.pipe],
				    gFlipper.ctls, flip_rd(pipe + 0x00), flip_rd(pipe + 0x04), flip_rd(pipe + 0x08), flip_rd(pipe + 0x0c),
				    flip_rd(pipe + 0x10), flip_rd(pipe + 0x24), flip_rd(pipe + 0x30), flip_rd(pipe + 0x34), flip_rd(pipe + 0x38));
			}
			gFlipper.flips = gFlipper.waited = gFlipper.misses = 0;
			gFlipper.waitAbs = 0;
		}
		for (different = 0; different < 8 && fStatSurfaces[different] != 0; different++) {
		}
		if (different == 8 && fStatFrames > 8) {
			uint32_t made, freed, again;
			uint64_t making, freeing;

			/* (CoreAnimation has made itself a new surface for every frame: see "the frames' surfaces" above) */
			MiniIOSurfaceCost(&made, &making, &freed, &freeing, &again);
			IOLog("MiniIOMobileFramebuffer: the frames came in surfaces %u, %u, %u, %u, %u and on, none twice; in that time "
			    "%u surfaces of all kinds were made, in %llu us each (%u of them of memory kept from one before), and %u "
			    "freed, in %llu us each\n", fStatSurfaces[0], fStatSurfaces[1], fStatSurfaces[2], fStatSurfaces[3],
			    fStatSurfaces[4], made, making, again, freed, freeing);
		}
		bzero(fStatSurfaces, sizeof(fStatSurfaces));
		fStatHeld = fStatLost = fStatOnBeat = fStatSettled = 0;
		fStatWaiting = fStatBeatWait = 0;
		reportFrameTimes();
		if (fDiff) {
			IOLog("MiniIOMobileFramebuffer: %llu pixels of the panel changed a frame, all within %u,%u to %u,%u\n",
			    fDiffPixels / fStatFrames, fDiffLeft, fDiffTop, fDiffRight, fDiffBottom);
			fDiffPixels = 0;
			fDiffLeft = fDiffTop = ~0u;
			fDiffRight = fDiffBottom = 0;
		}
		fStatFrames = 0;
		fStatDrawing = 0;
		fStatSince = 0;
	}
	IOLockUnlock(fLock);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class IOMobileFramebufferUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

	task_t                  fTask;
	IOMobileFramebuffer *   fOwner;
	IOLock *                fLock;
	mach_port_t             fPorts[kFBNotifyTypes];
	mach_vm_address_t       fCallbacks[kFBNotifyTypes];
	io_user_reference_t     fRefcons[kFBNotifyTypes];
	thread_call_t           fVSyncCall;
	uint64_t                fVSyncCount;
	uint64_t                fVSyncTime;             /* when the refresh being waited for is due */
	bool                    fVSyncRunning;
	bool                    fClosed;

	static void vsyncFired(thread_call_param_t self, thread_call_param_t unused);
	void vsync(void);
	void armVSync(void);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService * provider)
{
	fOwner = OSDynamicCast(IOMobileFramebuffer, provider);
	if (fOwner == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fLock = IOLockAlloc();
	fVSyncCall = thread_call_allocate(&IOMobileFramebufferUserClient::vsyncFired, this);
	return fLock != NULL && fVSyncCall != NULL;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fLock != NULL) {
		IOLockLock(fLock);
		fClosed = true;
		IOLockUnlock(fLock);
	}
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::free(void)
{
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
		thread_call_free(fVSyncCall);
		fVSyncCall = NULL;
	}
	for (unsigned i = 0; i < kFBNotifyTypes; i++) {
		if (fPorts[i] != NULL) {
			releaseNotificationPort(fPorts[i]);
			fPorts[i] = NULL;
		}
	}
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	mach_port_t old;

	IOLog("MiniIOMobileFramebuffer: notification port for type %u\n", (unsigned) type);
	if (type >= kFBNotifyTypes) {
		return kIOReturnBadArgument;
	}
	IOLockLock(fLock);
	old = fPorts[type];
	fPorts[type] = port;
	IOLockUnlock(fLock);
	if (old != NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::vsyncFired(thread_call_param_t self, thread_call_param_t unused)
{
	((IOMobileFramebufferUserClient *) self)->vsync();
}

/* With the lock held. */
void
IOMobileFramebufferUserClient::armVSync(void)
{
	uint64_t period, now = mach_absolute_time();

	/* one refresh after the last, not after whenever we got round to this: the rate does not drift */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
	fVSyncTime += period;
	if (fVSyncTime <= now || fVSyncTime > now + 2 * period) {
		fVSyncTime = now + period;
	}
	thread_call_enter_delayed(fVSyncCall, fVSyncTime);
}

/*
 * One refresh: tell the client, which gets three 64-bit numbers as six 32-bit halves --
 * the time of this refresh, the same again (the time it was presented), and a counter.
 */
void
IOMobileFramebufferUserClient::vsync(void)
{
	OSAsyncReference64 ref;
	io_user_reference_t args[6];
	uint64_t now;

	IOLockLock(fLock);
	now = fVSyncTime;
	if (fClosed || fCallbacks[kFBNotifyVSync] == 0 || fPorts[kFBNotifyVSync] == NULL) {
		fVSyncRunning = false;
	} else {
		fVSyncCount++;
		setAsyncReference64(ref, fPorts[kFBNotifyVSync], fCallbacks[kFBNotifyVSync], fRefcons[kFBNotifyVSync], fTask);
		args[0] = (uint32_t) now;
		args[1] = now >> 32;
		args[2] = (uint32_t) now;
		args[3] = now >> 32;
		args[4] = (uint32_t) fVSyncCount;
		args[5] = fVSyncCount >> 32;
		armVSync();
		/*
		 * sent with the lock held: registerNotificationPort() swaps the port under this lock and
		 * only then lets go of the old one, so the port in `ref` cannot be released while it is sent to
		 * (the send does not wait: a full queue drops the message)
		 */
		IOReturn sent = sendAsyncResult64WithOptions(ref, kIOReturnSuccess, args, 6, kIOUserNotifyOptionCanDrop);
		if (gTouchLat) {
			/* "tl: y COUNT due US at US SENT": the refresh told to the window server, when it was due, when it went, and
			 * whether it went (0) or was dropped on a full queue -- for the window server's idle gaps (tools/frames.py) */
			hakios_tl("tl: y %llu due %llu at %llu %#x\n", fVSyncCount, micros(now), micros(mach_absolute_time()), sent);
		}
	}
	IOLockUnlock(fLock);
}

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	const uint64_t * in = args->scalarInput;
	uint64_t * out = args->scalarOutput;
	MiniIOSurface * s;

	if (selector < 128 && gFBCalls[selector] < gFBTrace) {
		char procname[32] = "?";

		gFBCalls[selector]++;
		proc_selfname(procname, sizeof(procname));
		IOLog("MiniIOMobileFramebuffer: %s[%d] selector %u (scalars %u in [0x%llx 0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", procname, proc_selfpid(), selector, args->scalarInputCount,
		    args->scalarInputCount > 0 ? in[0] : 0, args->scalarInputCount > 1 ? in[1] : 0,
		    args->scalarInputCount > 2 ? in[2] : 0, args->scalarInputCount > 3 ? in[3] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}

	switch (selector) {
	case kFBMethodGetDisplaySize:                   /* out: width, height */
		if (args->scalarOutputCount < 2) {
			return kIOReturnBadArgument;
		}
		out[0] = fOwner->fWidth;
		out[1] = fOwner->fHeight;
		args->scalarOutputCount = 2;
		return kIOReturnSuccess;

	case kFBMethodGetID:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetDotPitch:                      /* out: pixels per inch */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = kFBDotsPerInch * fOwner->fWidth / kFBDefaultWidth;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetLayerDefaultSurface:           /* in: size wanted (or 0, 0); out: a send right for the surface */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		s = fOwner->defaultSurface();
		if (s == NULL) {
			return kIOReturnNoMemory;
		}
		out[0] = MiniIOSurfaceMakeSendRight(fTask, s);
		args->scalarOutputCount = 1;
		return out[0] != 0 ? kIOReturnSuccess : kIOReturnNoMemory;

	case kFBMethodSwapBegin:                        /* out: the number of the new swap */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = OSIncrementAtomic(&fOwner->fSwapID) + 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodSwapEnd:
		if (args->structureInput == NULL || args->structureInputSize < SWAP_SIZE) {
			IOLog("MiniIOMobileFramebuffer: swap request of %u bytes, expected %u\n", args->structureInputSize, SWAP_SIZE);
			return kIOReturnBadArgument;
		}
		fOwner->present((const uint8_t *) args->structureInput);
		return kIOReturnSuccess;

	case kFBMethodSwapWait:                         /* a swap is on the screen as soon as it is made */
		if (gTouchLat) {
			hakios_tl("tl: w %llu %#llx %#llx at %llu\n", args->scalarInputCount > 0 ? in[0] : 0,
			    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, micros(mach_absolute_time()));
		}
		return kIOReturnSuccess;
	case kFBMethodSwapSignal:
	case kFBMethodSwapCancel:
	case kFBMethodSwapCancelAll:
	case kFBMethodRequestPowerChange:
	case kFBMethodChangeFrameInfo:
		return kIOReturnSuccess;

	case kFBMethodNotifications:                    /* in: callback (0 = off), its argument, type, 0 */
		if (args->scalarInputCount < 3 || in[2] >= kFBNotifyTypes) {
			return kIOReturnBadArgument;
		}
		IOLockLock(fLock);
		fCallbacks[in[2]] = in[0];
		fRefcons[in[2]] = in[1];
		if (in[2] == kFBNotifyVSync && in[0] != 0 && !fVSyncRunning && !fClosed) {
			fVSyncRunning = true;
			armVSync();
		}
		IOLockUnlock(fLock);
		return kIOReturnSuccess;

	case kFBMethodSupportedFrameInfo:               /* in: type; out: how many, and their tags: none */
		if (args->scalarOutputCount >= 1) {
			out[0] = 0;
			args->scalarOutputCount = 1;
		}
		if (args->structureOutput != NULL) {
			bzero(args->structureOutput, args->structureOutputSize);
		}
		return kIOReturnSuccess;

	case kFBMethodGetBlock:                         /* in: which block; out: its bytes */
		if (args->structureOutput == NULL) {
			return kIOReturnBadArgument;
		}
		bzero(args->structureOutput, args->structureOutputSize);
		if (args->scalarInputCount >= 1 && in[0] == 0x40 && args->structureOutputSize >= 12) {
			/* the current mode: its colour and timing identifiers */
			uint32_t ids[2] = { kFBColorID, kFBTimingID };

			memcpy((uint8_t *) args->structureOutput + 4, ids, sizeof(ids));
		}
		return kIOReturnSuccess;

	case kFBMethodGetCurrentAbsoluteTime:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = mach_absolute_time();
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	default:
		break;
	}

	if (selector < 128 && !(gFBLogged[selector / 64] & (1ULL << (selector % 64)))) {
		gFBLogged[selector / 64] |= 1ULL << (selector % 64);
		IOLog("MiniIOMobileFramebuffer: selector %u not implemented (scalars %u in [0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", selector, args->scalarInputCount, args->scalarInputCount > 0 ? in[0] : 0,
		    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}
	/* a request that only sets something is accepted; one that wants an answer gets zeros */
	if (args->scalarOutputCount != 0) {
		bzero(out, args->scalarOutputCount * sizeof(out[0]));
	}
	if (args->structureOutput != NULL && args->structureOutputSize != 0) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	return kIOReturnSuccess;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * IOMobileFramebuffer.framework counts the displays it should wait for by looking for
 * children of /arm-io in the device tree whose device_type is "display-subsystem".
 */
static void
publishDisplayNode(void)
{
	IORegistryEntry * armio = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
	IORegistryEntry * node;
	OSDictionary * props;
	OSData * data;

	if (armio == NULL) {
		IOLog("MiniIOMobileFramebuffer: no /arm-io in the device tree\n");
		return;
	}
	node = OSTypeAlloc(IORegistryEntry);
	props = OSDictionary::withCapacity(4);
	if (node != NULL && props != NULL) {
		data = OSData::withBytes("display-subsystem", sizeof("display-subsystem"));
		props->setObject("device_type", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("redwood,framebuffer", sizeof("redwood,framebuffer"));
		props->setObject("compatible", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("disp0", sizeof("disp0"));
		props->setObject("name", data);
		OSSafeReleaseNULL(data);
		if (node->init(props)) {
			node->setName("disp0");
			node->attachToParent(armio, gIODTPlane);
		}
	}
	OSSafeReleaseNULL(props);
	OSSafeReleaseNULL(node);
	armio->release();
}

/*
 * The graphics accelerator. There is none, but CoreAnimation waits half a minute for a
 * service of this class to appear before it looks for a GPU at all (once per process). An
 * accelerator without the properties that name its Metal and OpenGL ES drivers is passed
 * over by both, which leaves CoreAnimation with its software renderer, as before.
 */
class IOAcceleratorES : public IOService
{
	OSDeclareDefaultStructors(IOAcceleratorES);
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

static void
publishAccelerator(IOService * provider)
{
	IOAcceleratorES * accelerator = OSTypeAlloc(IOAcceleratorES);

	if (accelerator != NULL && accelerator->init() && accelerator->attach(provider)) {
		uint32_t metal = 0;

		accelerator->setName("sgx");
		/*
		 * Boot-arg metal=1: say where Metal's device is (docs/metal-plan.md, step 1). Metal reads these two
		 * properties, opens /System/Library/Extensions/<name>.bundle and takes the class (userland/src/HakiosMetal.m).
		 * Never by default: with them a program that asks for Metal gets a device, and CoreAnimation would choose
		 * Metal for the screen; until that device can draw, a screen drawn without it is the working one.
		 */
		if (PE_parse_boot_argn("metal", &metal, sizeof(metal)) && metal != 0) {
			accelerator->setProperty("MetalPluginName", "HakiosMetal");
			accelerator->setProperty("MetalPluginClassName", "HakiosMetalDevice");
			IOLog("MiniIOMobileFramebuffer: metal=1: the accelerator names HakiosMetal as its Metal plug-in\n");
		}
		accelerator->registerService();
	}
	OSSafeReleaseNULL(accelerator);
}

/*
 * A picture from inside the kernel (the GPU driver's first ones): rows of 32-bit pixels, put at
 * the top left corner of the panel as they are and cut to its size.
 */
extern "C" void
MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	uint32_t columns = width < vinfo.v_width ? width : (uint32_t) vinfo.v_width;
	uint32_t rows = height < vinfo.v_height ? height : (uint32_t) vinfo.v_height;

	if (vinfo.v_baseaddr == 0 || vinfo.v_depth != 32) {
		return;
	}
	for (uint32_t y = 0; y < rows; y++) {
		memcpy((void *) (vinfo.v_baseaddr + (size_t) y * vinfo.v_rowbytes), pixels + (size_t) y * rowBytes,
		    (size_t) columns * 4);
	}
}

void
MiniIOMobileFramebufferStart(IOService * provider)
{
	IOMobileFramebuffer * fb = OSTypeAlloc(IOMobileFramebuffer);

	if (fb == NULL || !fb->init() || !fb->attach(provider)) {
		OSSafeReleaseNULL(fb);
		return;
	}
	if (fb->start(provider)) {
		publishDisplayNode();
		publishAccelerator(provider);
	} else {
		fb->detach(provider);
	}
	fb->release();
}
