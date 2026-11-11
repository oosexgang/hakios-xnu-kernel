/*
 * The panel's light, for any device: one driver; what differs between devices is the board's data
 * (hakios_board.h, from devices/NAME/board.toml), and the arithmetic is hakios_backlight_core.h
 * (tested on the host: tests/backlight_test.c, and the driver itself: tests/backlight_driver_test.c).
 *
 * What iOS sees: the sysctls hw.backlight.* (any user may write the first four; the injected library
 * ios/userland/inject_brightness.c is who does, for the slider of Control Center and Settings and
 * for the switch of automatic brightness):
 *
 *   level      0 to 1000: the slider (iOS's DisplayBrightness times 1000). Written while the brightness
 *              is automatic, it moves the automatic curve by the difference, as on an iPhone
 *   auto       0 or 1: automatic brightness (iOS's DisplayBrightnessAuto)
 *   power      the display: 1 on, 0 off (the panel dark), 2 dimmed (at most level 150); written by
 *              ios/userland/inject_backlight.c from iOS's display modes (the side button, the idle timer)
 *   lux        the ambient light in thousandths of a lux. Written: a reading of the "virtual" sensor
 *              (QEMU, tests, or user space with a sensor of its own); read: the light in use
 *   cap        the most it may show while the phone is hot (bsd/dev/hakios_thermal.c), 1000 otherwise
 *   shown      what is on the panel now, 0 to 1000 (it moves towards the level, see bl_step())
 *   raw        the last number the panel was given (backend dcs)
 *   offset     the user's move of the automatic curve
 *   backend    "dcs", "soft" or "none"; als: where the light readings come from
 *
 * A driver of a real light sensor reports with hakios_als_report(millilux, name).
 *
 * The backends:
 *
 *   dcs    MIPI DCS set_display_brightness (0x51) to a DSI panel, through a Qualcomm DSI controller
 *          (2.2 or later: SM7325, SM7350, SM8x50 ...) in its command FIFO -- the controller's "test
 *          pattern" store, which Linux's display driver uses where it may not use memory. The
 *          controller is the one the firmware set the panel going with; nothing of its setup is
 *          changed, only a command added. Every read and write is behind the same checks of clocks as
 *          the display scan (bsd/dev/redwood_adreno.c): the display block's power switch and the bus
 *          clock of its registers. A command the controller does not report sent within 20 ms stops
 *          this backend for the run.
 *   soft   no panel to tell: the display driver (MiniIOMobileFramebuffer.cpp) darkens every pixel on
 *          its way to the panel, from a copy in memory, so a new level is shown at once even when no
 *          new frame comes. Works anywhere (QEMU); costs a copy of each frame.
 *   none   nothing (the default where nothing is known and backlight=0).
 *
 * Boot-args (each overrides the profile of the board, so another device can be tried without a new
 * kernel):
 *
 *   backlight=0        off (default): nothing touched, no sysctls answer anything but "none"
 *   backlight=1        the profile's backend, read only: the DSI controller's version and state logged
 *   backlight=2        the profile's backend in use
 *   bl_backend=dcs|soft|none
 *   bl_dsi=ADDRESS     the DSI controller's registers (physical)
 *   bl_min= bl_max=    the panel's numbers for the lowest and the highest brightness
 *   bl_bytes=1|2       how many bytes the panel's 0x51 takes; bl_le=1: least significant first
 *   bl_level=N         the level at boot (default 600)
 *   bl_auto=1          automatic brightness from the start
 *   bl_floor=N         backend soft: what is kept of a pixel at level 0, of 256 (default 24)
 *
 * Adding a device: [backlight] in its devices/NAME/board.toml -- the panel's
 * qcom,mdss-dsi-bl-min-level / -max-level, qcom,mdss-dsi-bl-inverted-dbv (MSB first), the panel's mode --
 * and in its SoC's file the DSI controller (mdss_dsi0 "dsi_ctrl") and the display clock controller's
 * offsets (Linux dispcc-*.c). `tools/boards.py new --dts` reads most of it from the device tree. Until
 * then the boot-args above do the same, and a board not described at all gets backend soft.
 */
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include "hakios_backlight_core.h"
#include "hakios_board.h"
#include "hakios_log.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void thread_set_thread_name(thread_t thread, const char *name);

#define say hakios_say

/* ---- what differs between devices ----------------------------------------------------------------- */

enum bl_backend { BL_NONE, BL_DCS, BL_SOFT };
static const char *const bl_backend_names[] = { "none", "dcs", "soft" };

/*
 * What is used: the board's values (hakios_board.h: devices/NAME/board.toml [backlight], and its SoC's
 * [clocks] and [display]) with the boot-args over them.
 */
struct bl_profile {
	const char *board;              /* /chosen darwin-board */
	enum bl_backend backend;
	/* backend dcs */
	uint32_t dsi;                   /* the DSI controller's registers (dt: mdss_dsi0 "dsi_ctrl") */
	uint32_t raw_min, raw_max;      /* the panel's numbers: qcom,mdss-dsi-bl-min-level, -max-level (without HBM) */
	uint32_t raw_restore;           /* what the panel is left at for the loader (qcom,mdss-brightness-init-level) */
	uint8_t bytes;                  /* 0x51 takes 1 or 2 bytes */
	bool little_endian;             /* (2 bytes) least significant first: no qcom,mdss-dsi-bl-inverted-dbv */
	bool video_mode;                /* a video-mode panel: commands go between frames */
	/* the clocks the DSI controller's registers need (the same chain as the display scan) */
	uint32_t gcc, gcc_disp_ahb_cbcr;
	uint32_t dispcc, dispcc_core_gdscr, dispcc_ahb_cbcr;
};

/* the profile in use: the board's, with the boot-args over it */
static struct bl_profile profile;
static uint32_t mode;                   /* backlight=: 0 off, 1 look only, 2 in use */
static bool chosen;

static void
bl_choose(void)
{
	const struct hakios_board *board = hakios_board();
	char backend[8];
	uint32_t value;

	if (chosen) {
		return;
	}
	chosen = true;
	bzero(&profile, sizeof(profile));
	if (board == NULL) {
		profile.backend = BL_SOFT;      /* a board the table has not got: only what is safe anywhere */
		profile.board = "unknown";
	} else {
		profile.board = board->name;
		profile.backend = board->backlight.type == HAKIOS_BACKLIGHT_DCS ? BL_DCS :
		    board->backlight.type == HAKIOS_BACKLIGHT_SOFT ? BL_SOFT : BL_NONE;
		profile.dsi = board->backlight.dsi;
		profile.raw_min = board->backlight.raw_min;
		profile.raw_max = board->backlight.raw_max;
		profile.raw_restore = board->backlight.raw_restore;
		profile.bytes = board->backlight.bytes;
		profile.little_endian = board->backlight.lsb_first != 0;
		profile.video_mode = board->backlight.video_mode != 0;
		profile.gcc = board->clocks.gcc;
		profile.gcc_disp_ahb_cbcr = board->clocks.gcc_disp_ahb_cbcr;
		profile.dispcc = board->clocks.dispcc;
		profile.dispcc_core_gdscr = board->clocks.dispcc_mdss_gdscr;
		profile.dispcc_ahb_cbcr = board->clocks.dispcc_mdss_ahb_cbcr;
	}
	if (!PE_parse_boot_argn("backlight", &mode, sizeof(mode))) {
		mode = 0;
	}
	mode = mode > 2 ? 2 : mode;
	if (PE_parse_boot_argn("bl_backend", backend, sizeof(backend))) {
		profile.backend = strcmp(backend, "dcs") == 0 ? BL_DCS : strcmp(backend, "soft") == 0 ? BL_SOFT : BL_NONE;
	}
	if (PE_parse_boot_argn("bl_dsi", &value, sizeof(value))) {
		profile.dsi = value;
	}
	if (PE_parse_boot_argn("bl_min", &value, sizeof(value))) {
		profile.raw_min = value;
	}
	if (PE_parse_boot_argn("bl_max", &value, sizeof(value))) {
		profile.raw_max = value;
	}
	if (PE_parse_boot_argn("bl_bytes", &value, sizeof(value))) {
		profile.bytes = (uint8_t)value;
	}
	if (PE_parse_boot_argn("bl_le", &value, sizeof(value))) {
		profile.little_endian = value != 0;
	}
	if (mode == 0) {
		profile.backend = BL_NONE;
	}
	if (profile.backend == BL_DCS && (profile.dsi == 0 || profile.dispcc == 0 || profile.gcc == 0 ||
	    profile.raw_max <= profile.raw_min || (profile.bytes != 1 && profile.bytes != 2))) {
		say("backlight: %s: no complete DCS profile (DSI %#x, %u to %u, %u bytes): nothing done\n", profile.board,
		    profile.dsi, profile.raw_min, profile.raw_max, profile.bytes);
		profile.backend = BL_NONE;
	}
}

/* ---- state ------------------------------------------------------------------------------------- */

static volatile uint32_t user_level = 600;      /* the slider */
static volatile uint32_t automatic;             /* automatic brightness */
static volatile uint32_t power = 1;             /* the display: 0 off, 1 on, 2 dimmed (hw.backlight.power) */
static volatile uint32_t thermal_cap = BL_LEVEL_MAX;    /* the most the panel may show while the phone is hot */
static volatile int32_t auto_offset;            /* the user's move of the curve */
static volatile uint32_t ambient_mlx = 300000;  /* the light around, thousandths of a lux (an office until told) */
static const char *ambient_source = "virtual";
static volatile uint32_t shown_level;           /* on the panel now, 0 to 1000 */
static volatile uint32_t shown_raw;
static volatile uint32_t soft_dim = 256;        /* backend soft: what the display driver keeps of a pixel */
static uint32_t soft_floor = 24;
static bool broken;                             /* backend dcs: something went wrong, nothing more is written */
static int poke;                                /* the thread's wait channel */
static void (*soft_redraw)(void);

static void
bl_poke(void)
{
	thread_wakeup(&poke);
}

/* ---- backend dcs: a Qualcomm DSI controller (dsi_ctrl_reg.h, dsi_ctrl_hw_cmn.c of Linux's SDE driver) --- */

#define DSI_HW_VERSION                  0x000
#define DSI_CTRL                        0x004
#define   DSI_CTRL_ENABLE               (1u << 0)
#define   DSI_CTRL_VIDEO_MODE           (1u << 1)
#define   DSI_CTRL_CMD_MODE             (1u << 2)
#define DSI_STATUS                      0x008
#define   DSI_STATUS_CMD_MDP_BUSY       (1u << 2)
#define DSI_COMMAND_MODE_DMA_CTRL       0x03c
#define   DMA_CTRL_EMBEDDED             (1u << 28)
#define   DMA_CTRL_LOW_POWER            (1u << 26)
#define   DMA_CTRL_BROADCAST            (3u << 30)
#define DSI_DMA_CMD_LENGTH              0x04c
#define DSI_CMD_MODE_DMA_SW_TRIGGER     0x090
#define DSI_INT_CTRL                    0x110
#define   INT_CMD_DMA_DONE              (1u << 0)
#define   INT_VIDEO_DONE                (1u << 16)
#define   INT_ERROR                     (1u << 24)
/* the status bits of INT_CTRL (written 1: cleared); the others enable interrupts and are kept as they are */
#define   INT_STATUS_BITS               (0x1u | 0x100u | 0x400u | 0x1000u | 0x4000u | 0x10000u | 0x100000u | \
	                                 0x1000000u | 0x10000000u | 0x40000000u)
#define DSI_TEST_PATTERN_GEN_CTRL       0x15c
#define   TPG_CMD_DMA_FIFO              ((1u << 1) | (1u << 2) | (3u << 16))   /* TPG on, its DMA FIFO, our pattern */
#define DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL 0x17c
#define DSI_TPG_DMA_FIFO_RESET          0x1ec
#define DSI_SIZE                        0x400

#define CBCR_ENABLE                     (1u << 0)
#define CBCR_OFF                        (1u << 31)
#define GDSC_POWER_ON                   (1u << 31)

static vm_offset_t dsi, gcc, dispcc;

static inline void
barrier(void)
{
#if defined(__arm64__) || defined(__aarch64__)
	__asm__ volatile ("dsb sy" ::: "memory");
#else
	__asm__ volatile ("" ::: "memory");     /* (the host tests on another processor) */
#endif
}

#ifndef BL_TEST_MMIO
static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
	barrier();
}
#else
/* tests/backlight_driver_test.c: the registers are a model of the hardware's */
extern uint32_t bl_test_rd(vm_offset_t address);
extern void bl_test_wr(vm_offset_t address, uint32_t value);
#define rd(a)           bl_test_rd(a)
#define wr(a, v)        (bl_test_wr((a), (v)), barrier())
#endif

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_USEC;
}

/*
 * May the DSI controller's registers be touched now? The display block powered, the bus clock of its
 * registers running -- turned on if the firmware left it off, as the display scan does (the same two
 * writes, and only those). Every command looks again: whoever else might take the clock away.
 */
static bool
dsi_reachable(void)
{
	uint32_t clock;

	if (gcc == 0 || dispcc == 0 || dsi == 0) {
		return false;
	}
	clock = rd(gcc + profile.gcc_disp_ahb_cbcr);
	if (!(clock & CBCR_ENABLE)) {
		wr(gcc + profile.gcc_disp_ahb_cbcr, clock | CBCR_ENABLE);
		delay(10000);
		if (!(rd(gcc + profile.gcc_disp_ahb_cbcr) & CBCR_ENABLE)) {
			return false;
		}
	}
	if (!(rd(dispcc + profile.dispcc_core_gdscr) & GDSC_POWER_ON)) {
		return false;
	}
	clock = rd(dispcc + profile.dispcc_ahb_cbcr);
	if (clock & CBCR_OFF) {
		wr(dispcc + profile.dispcc_ahb_cbcr, clock | CBCR_ENABLE);
		delay(10000);
		if (rd(dispcc + profile.dispcc_ahb_cbcr) & CBCR_OFF) {
			return false;
		}
	}
	return true;
}

static bool
dcs_start(void)
{
	uint32_t version, control;

	gcc = ml_io_map(profile.gcc, 0x80000);
	dispcc = ml_io_map(profile.dispcc, 0x4000);
	dsi = ml_io_map(profile.dsi, DSI_SIZE);
	if (!dsi_reachable()) {
		say("backlight: dcs: the display block is off or its registers have no clock: nothing done\n");
		return false;
	}
	version = rd(dsi + DSI_HW_VERSION);
	control = rd(dsi + DSI_CTRL);
	say("backlight: dcs: DSI controller at %#x: version %u.%u (%#x), ctrl %#x (%s%s%s), status %#x, dma ctrl %#x, int %#x\n",
	    profile.dsi, version >> 28, version >> 16 & 0xfff, version, control, control & DSI_CTRL_ENABLE ? "on" : "OFF",
	    control & DSI_CTRL_VIDEO_MODE ? ", video" : "", control & DSI_CTRL_CMD_MODE ? ", command" : "", rd(dsi + DSI_STATUS),
	    rd(dsi + DSI_COMMAND_MODE_DMA_CTRL), rd(dsi + DSI_INT_CTRL));
	if (version >> 28 != 2 || (version >> 16 & 0xfff) < 2) {
		say("backlight: dcs: not a DSI controller of 2.2 or later (no command FIFO): nothing done\n");
		return false;
	}
	if (!(control & DSI_CTRL_ENABLE)) {
		say("backlight: dcs: the controller is off (the firmware did not leave the panel lit through it): nothing done\n");
		return false;
	}
	if (((control & DSI_CTRL_VIDEO_MODE) != 0) != profile.video_mode) {
		say("backlight: dcs: the controller runs a %s-mode panel, the profile says %s mode: going by the controller\n",
		    control & DSI_CTRL_VIDEO_MODE ? "video" : "command", profile.video_mode ? "video" : "command");
		profile.video_mode = (control & DSI_CTRL_VIDEO_MODE) != 0;
	}
	return true;
}

/* wait until `bit` of INT_CTRL is set, at most `us` microseconds */
static bool
int_wait(uint32_t bit, uint64_t us)
{
	uint64_t until = now_us() + us;

	while (!(rd(dsi + DSI_INT_CTRL) & bit)) {
		if (now_us() > until) {
			return false;
		}
		delay(20);
	}
	return true;
}

static void
int_clear(uint32_t bits)
{
	wr(dsi + DSI_INT_CTRL, (rd(dsi + DSI_INT_CTRL) & ~INT_STATUS_BITS) | bits);
}

static bool
dcs_send(uint32_t raw)
{
	uint32_t words[4], length = 0, control, dma_control;
	unsigned n = bl_dcs_brightness_words(raw, profile.bytes, profile.little_endian, 0, words, &length);
	bool sent;
	uint64_t began = now_us();

	if (broken || n == 0 || !dsi_reachable()) {
		return false;
	}
	control = rd(dsi + DSI_CTRL);
	if (!(control & DSI_CTRL_ENABLE)) {
		say("backlight: dcs: the controller has been switched off: no more commands this run\n");
		broken = true;
		return false;
	}
	if (profile.video_mode) {
		/* between two frames: the end of one, then the command in the blanking after it */
		int_clear(INT_VIDEO_DONE);
		(void)int_wait(INT_VIDEO_DONE, 40000);
	} else {
		/* not while a frame is on its way to the panel (8.6 ms of every 16.7 on renoir) */
		uint64_t until = now_us() + 20000;

		while ((rd(dsi + DSI_STATUS) & DSI_STATUS_CMD_MDP_BUSY) && now_us() < until) {
			delay(50);
		}
	}
	if (!(control & DSI_CTRL_CMD_MODE)) {
		wr(dsi + DSI_CTRL, control | DSI_CTRL_CMD_MODE);       /* (Linux's driver does the same for a video panel) */
	}
	int_clear(INT_CMD_DMA_DONE);
	/* the command into the FIFO, then the length, embedded mode, high speed, this controller alone, and go */
	wr(dsi + DSI_TEST_PATTERN_GEN_CTRL, TPG_CMD_DMA_FIFO);
	for (unsigned i = 0; i < n; i++) {
		wr(dsi + DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, words[i]);
	}
	dma_control = rd(dsi + DSI_COMMAND_MODE_DMA_CTRL);
	wr(dsi + DSI_COMMAND_MODE_DMA_CTRL, (dma_control & ~(DMA_CTRL_BROADCAST | DMA_CTRL_LOW_POWER)) | DMA_CTRL_EMBEDDED);
	wr(dsi + DSI_DMA_CMD_LENGTH, length);
	wr(dsi + DSI_CMD_MODE_DMA_SW_TRIGGER, 1);
	sent = int_wait(INT_CMD_DMA_DONE, 20000);
	/* the FIFO emptied and the pattern generator off, as after every command of Linux's */
	wr(dsi + DSI_TEST_PATTERN_GEN_CTRL, 0);
	wr(dsi + DSI_TPG_DMA_FIFO_RESET, 1);
	delay(1);
	wr(dsi + DSI_TPG_DMA_FIFO_RESET, 0);
	wr(dsi + DSI_COMMAND_MODE_DMA_CTRL, dma_control);
	if (!(control & DSI_CTRL_CMD_MODE)) {
		wr(dsi + DSI_CTRL, rd(dsi + DSI_CTRL) & ~DSI_CTRL_CMD_MODE);
	}
	int_clear(INT_CMD_DMA_DONE);
	if (!sent) {
		say("backlight: dcs: brightness %u NOT SENT in 20 ms (int %#x, status %#x): no more commands this run\n", raw,
		    rd(dsi + DSI_INT_CTRL), rd(dsi + DSI_STATUS));
		broken = true;
		return false;
	}
	if (rd(dsi + DSI_INT_CTRL) & INT_ERROR) {
		say("backlight: dcs: the controller reports an error after brightness %u (int %#x)\n", raw, rd(dsi + DSI_INT_CTRL));
	}
	(void)began;
	return true;
}

/* ---- the light, followed ------------------------------------------------------------------------- */

static void
apply(uint32_t level)
{
	shown_level = level;
	switch (profile.backend) {
	case BL_DCS: {
		uint32_t raw = bl_level_to_raw(level, profile.raw_min, profile.raw_max);

		if (raw != shown_raw && dcs_send(raw)) {
			shown_raw = raw;
		}
		break;
	}
	case BL_SOFT: {
		uint32_t dim = bl_level_to_dim(level, soft_floor);

		if (dim != soft_dim) {
			soft_dim = dim;
			if (soft_redraw != NULL) {
				soft_redraw();
			}
		}
		break;
	}
	default:
		break;
	}
}

/*
 * The display off: the panel dark (a DSI panel at brightness 0: black, and back at once; the software light:
 * every pixel black). Not the panel's display-off command, which would need its power-on sequence again.
 */
static void
apply_dark(void)
{
	shown_level = 0;
	switch (profile.backend) {
	case BL_DCS:
		if (shown_raw != 0 && dcs_send(0)) {
			shown_raw = 0;
		}
		break;
	case BL_SOFT:
		if (soft_dim != 0) {
			soft_dim = 0;
			if (soft_redraw != NULL) {
				soft_redraw();
			}
		}
		break;
	default:
		break;
	}
}

/* dimmed (the idle timer's first step, or iOS's dimmed display mode): at most this */
#define BL_DIMMED_MAX   150

static uint32_t
wanted_level(void)
{
	return automatic ? bl_auto_target(ambient_mlx, auto_offset) : user_level;
}

#define TICK_MS         50

static void
bl_thread(__unused void *argument, __unused wait_result_t result)
{
	uint32_t now = user_level << 10;        /* in 1024ths of a level */
	uint32_t reference_mlx = ambient_mlx, target = wanted_level();

	bool dark = false;

	apply(user_level);
	for (;;) {
		uint32_t want = wanted_level(), goal;

		if (power == 0) {
			if (!dark) {
				apply_dark();
				dark = true;
			}
			assert_wait(&poke, THREAD_UNINT);
			thread_block(THREAD_CONTINUE_NULL);
			continue;
		}
		if (dark) {
			dark = false;
			apply((now + 512) >> 10);       /* lit again at once, where it was */
		}

		if (!automatic) {
			target = want;
		} else if (bl_lux_moved(ambient_mlx, reference_mlx) || (want > target ? want - target : target - want) > 20) {
			/* a real change of the light (not a flicker), or the user's own move of the curve */
			reference_mlx = ambient_mlx;
			target = want;
		}
		goal = power == 2 && target > BL_DIMMED_MAX ? BL_DIMMED_MAX : target;
		goal = goal > thermal_cap ? thermal_cap : goal;
		if (now != goal << 10) {
			/* by hand: there in a fifth of a second; automatic: bl_step()'s pace */
			now = automatic ? bl_step(now, goal << 10) : (now < goal << 10 ?
			    now + ((goal << 10) - now + 3) / 4 : now - (now - (goal << 10) + 3) / 4);
			apply((now + 512) >> 10);
		}
		if (now == goal << 10 && !automatic) {
			assert_wait(&poke, THREAD_UNINT);
		} else {
			assert_wait_timeout(&poke, THREAD_UNINT, TICK_MS, NSEC_PER_MSEC);
		}
		thread_block(THREAD_CONTINUE_NULL);
	}
}

/* ---- for other parts of the kernel -------------------------------------------------------------- */

/* the display driver: is the light made in software (then it keeps a copy of the frame to darken) */
bool
hakios_backlight_soft(void)
{
	bl_choose();
	return profile.backend == BL_SOFT;
}

/* ... how much of a pixel to keep (256: all), and whom to tell when that changes */
uint32_t
hakios_backlight_dim(void)
{
	return soft_dim;
}

void
hakios_backlight_set_redraw(void (*redraw)(void))
{
	soft_redraw = redraw;
}

/* the thermal limiter (bsd/dev/hakios_thermal.c): the most the panel may show, 0 to 1000 */
void
hakios_backlight_set_cap(uint32_t level)
{
	level = level > BL_LEVEL_MAX ? BL_LEVEL_MAX : level;
	if (level != thermal_cap) {
		thermal_cap = level;
		bl_poke();
	}
}

/* a light sensor's reading */
void
hakios_als_report(uint32_t millilux, const char *source)
{
	ambient_mlx = millilux;
	ambient_source = source;
	if (automatic) {
		bl_poke();
	}
}

/* ---- sysctl hw.backlight ------------------------------------------------------------------------ */

SYSCTL_NODE(_hw, OID_AUTO, backlight, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "the panel's light");

static int
sysctl_level SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg2)
	uint32_t value = *(volatile uint32_t *)arg1;
	int changed = 0;
	int error = sysctl_io_number(req, value, sizeof(value), &value, &changed);

	if (error != 0 || !changed) {
		return error;
	}
	if (arg1 == &user_level) {
		value = value > BL_LEVEL_MAX ? BL_LEVEL_MAX : value;
		user_level = value;
		if (automatic) {
			/* by hand while automatic: the curve moves by the difference */
			auto_offset = (int32_t)value - (int32_t)bl_auto_level(ambient_mlx);
		}
	} else if (arg1 == &automatic) {
		value = value != 0;
		if (value != automatic) {
			auto_offset = 0;
		}
		automatic = value;
	} else if (arg1 == &power) {
		power = value > 2 ? 1 : value;
	} else if (arg1 == &ambient_mlx) {
		ambient_mlx = value;
		ambient_source = "virtual";
	}
	bl_poke();
	return 0;
}

SYSCTL_PROC(_hw_backlight, OID_AUTO, level, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
    __DEVOLATILE(void *, &user_level), 0, sysctl_level, "IU", "the slider, 0 to 1000");
SYSCTL_PROC(_hw_backlight, OID_AUTO, auto, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
    __DEVOLATILE(void *, &automatic), 0, sysctl_level, "IU", "automatic brightness");
SYSCTL_PROC(_hw_backlight, OID_AUTO, power, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
    __DEVOLATILE(void *, &power), 0, sysctl_level, "IU", "the display: 0 off, 1 on, 2 dimmed");
SYSCTL_PROC(_hw_backlight, OID_AUTO, lux, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
    __DEVOLATILE(void *, &ambient_mlx), 0, sysctl_level, "IU", "ambient light, thousandths of a lux");
SYSCTL_UINT(_hw_backlight, OID_AUTO, shown, CTLFLAG_RD | CTLFLAG_LOCKED, __DEVOLATILE(uint32_t *, &shown_level), 0,
    "on the panel now, 0 to 1000");
SYSCTL_UINT(_hw_backlight, OID_AUTO, raw, CTLFLAG_RD | CTLFLAG_LOCKED, __DEVOLATILE(uint32_t *, &shown_raw), 0,
    "the panel's number");
SYSCTL_UINT(_hw_backlight, OID_AUTO, cap, CTLFLAG_RD | CTLFLAG_LOCKED, __DEVOLATILE(uint32_t *, &thermal_cap), 0,
    "the most the panel may show while the phone is hot");
SYSCTL_INT(_hw_backlight, OID_AUTO, offset, CTLFLAG_RD | CTLFLAG_LOCKED, __DEVOLATILE(int32_t *, &auto_offset), 0,
    "the user's move of the automatic curve");

static int
sysctl_names SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	const char *name = arg2 == 0 ? bl_backend_names[profile.backend] : ambient_source;

	return SYSCTL_OUT(req, name, strlen(name) + 1);
}

SYSCTL_PROC(_hw_backlight, OID_AUTO, backend, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_names, "A",
    "dcs, soft or none");
SYSCTL_PROC(_hw_backlight, OID_AUTO, als, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 1, sysctl_names, "A",
    "where the light readings come from");

/* ---- start and end ------------------------------------------------------------------------------- */

void
hakios_backlight_init(void)
{
	thread_t thread;
	uint32_t value;

	bl_choose();
	if (PE_parse_boot_argn("bl_level", &value, sizeof(value))) {
		user_level = value > BL_LEVEL_MAX ? BL_LEVEL_MAX : value;
	}
	if (PE_parse_boot_argn("bl_auto", &value, sizeof(value))) {
		automatic = value != 0;
	}
	if (PE_parse_boot_argn("bl_floor", &value, sizeof(value))) {
		soft_floor = value > 256 ? 256 : value;
	}
	say("backlight: board %s, backend %s%s, level %u%s\n", profile.board, bl_backend_names[profile.backend],
	    mode == 1 ? " (backlight=1: looking only)" : "", user_level, automatic ? ", automatic" : "");
	if (profile.backend == BL_DCS) {
		if (!dcs_start() || mode < 2) {
			profile.backend = BL_NONE;
			return;
		}
		say("backlight: dcs: the panel's brightness is ours: %u to %u, %u byte%s%s, %s mode\n", profile.raw_min,
		    profile.raw_max, profile.bytes, profile.bytes == 1 ? "" : "s", profile.bytes == 2 ?
		    (profile.little_endian ? " LSB first" : " MSB first") : "", profile.video_mode ? "video" : "command");
	}
	if (profile.backend == BL_NONE) {
		return;
	}
	if (kernel_thread_start(bl_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_set_thread_name(thread, "backlight");
		thread_deallocate(thread);
	}
}

/* At the run's end (and on a panic's way out): the panel left lit for the loader and whatever comes next (also when it was dark). */
void
hakios_backlight_quiesce(void)
{
	if (profile.backend != BL_DCS || broken || profile.raw_restore == 0) {
		return;
	}
	if (dcs_send(profile.raw_restore)) {
		say("backlight: at the run's end: the panel back at %u\n", profile.raw_restore);
	}
}
