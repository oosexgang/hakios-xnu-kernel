/*
 * The flashlight: the flash LED module of a PM8350C ("qcom,pm8350c-flash-led", four channels), the
 * rear flash LED on its channel 0. Where the module and the PMIC's temperature alarm are on SPMI is
 * the board's torch { }. The numbers below were found on the first phone, from Android's device tree, the vendor's
 * driver for it (drivers/leds/leds-qti-flash.c of the phone vendor's open kernel) and
 * what Android does when its own flashlight is switched on (its LED class devices read, nothing
 * written by us):
 *
 *   Android's torch    led:torch_0 at 125 mA through led:switch_0 (channel 0 alone), current
 *                      resolution 12.5 mA ("qcom,ires-ua" 12500): IRESOLUTION bit 0 clear, ITARGET
 *                      code round(125 / 12.5) - 1 = 9; the module enabled (0x46 = 0x80), then the
 *                      channel (0x4e bit 0). Off in the reverse order, ITARGET back to 0. The
 *                      torch has no safety timer (its bit is kept clear, as the driver keeps it)
 *   at Android's probe channel 0's strobe is the software's (0x4a: bits 6:4 and 2 clear) and its
 *                      safety timer off (0x3e bit 7 clear)
 *
 * Writing to a PMIC is not like writing anywhere else: the PMICs make the phone's supplies. So
 * every write goes through one function that takes only this module's registers above (the
 * peripheral the arbiter's channel is checked to be, the registers by a list), every write is
 * read back, and any surprise ends all writing for the run -- all but the light's going off, which
 * has a function of its own that takes only the two registers that put it out, and only 0: the
 * module's enable and then the channel's are written 0 whatever went before, with nothing read
 * first (off never waits on a read that may fail), then both read back, and written again, a few
 * times, until they read back off. Transfers through the arbiter are made one at a time with the
 * buttons' driver (hakios_spmi_lock()): two at once, and it drops one.
 *
 * What Darwin's flashlight is on this machine: the loader says the camera has a flash (with torch=N,
 * /product/camera "flash": MobileGestalt's camera-flash, Control Center's flashlight button), and
 * libhakios-media.dylib in mediaserverd hands FigFlashlight, which finds no camera here, a device
 * of ours whose torch level is written to /dev/hakios-torch (userland/src/inject_media.c).
 *
 * Nothing here is done unless a boot-arg asks for it:
 *
 *   torch=1    a look, reading only: the arbiter's channel for the module and who may write
 *              through it, the module's type, its registers, the PMIC's temperature alarm
 *   torch=2    ... and the light on once at Android's torch current for torch_secs (default 2),
 *              then off, every write read back
 *   torch=3    ... no light at the start; the light for Darwin until the run ends: /dev/hakios-torch
 *              takes a level of 0 to 1000 (a 32-bit number; thousandths of Android's current, 0
 *              off) and the module is set to it -- never above Android's 125 mA; a read gives
 *              what is lit now
 *
 * Limits, whatever asks: Android's current at most; the light on for torch_max_s at a time at most
 * (default 30, never more than 120), after which it stays off until it is asked for anew (0, then
 * more); while it is on, ten times a second, the module's fault and over-temperature flags, the
 * PMIC's temperature alarm and the battery's temperature (at most 45 C; from the audio DSP's battery
 * manager with adsp=1, or the firmware's with battery=1; with neither the other two guard; one that
 * was known and is no longer reported, its source gone quiet, is trouble too): any of
 * them, a read that fails or a write that does not read back, and the light goes off and stays off
 * for the rest of the run. At the run's end (and on a panic's way out) it goes off.
 *
 * Register layouts: Linux drivers/spmi/spmi-pmic-arb.c (the arbiter, versions 5 to 8, through
 * board/spmi.h, which knows where each version has a peripheral's write channel in "chnls": 0x10000
 * bytes a channel in version 5), Xiaomi's leds-qti-flash.c,
 * drivers/thermal/qcom/qcom-spmi-temp-alarm.c.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/board/audio.h>
#include <sys/board/input.h>
#include <sys/systm.h>
#include <sys/uio.h>

#include "kernel.h"
#include "board.h"
#include "log.h"

static uint32_t level;
static bool broken, leaving;
/*
 * The thread is between registers: set before it looks at `leaving' and writes, cleared after. The run's end sets
 * `leaving' and then waits for this to clear, so that it never meets a light half lit (written on, `lit' not yet
 * set) and the thread starts no writes after it (each looks at the other's after setting its own: one sees it).
 */
static bool writing;

HAKIOS_DEFINE_SAY(!leaving && ml_get_interrupts_enabled())

/*
 * The events that matter (lit, off, cut, trouble) are also kept, and said again at the run's end:
 * the log of a long run keeps its beginning and its end, and these happen in between.
 */
#define NOTES                   24
static char notes[NOTES][200];
static unsigned noted;

static void note(const char *format, ...) __printflike(1, 2);

static void
note(const char *format, ...)
{
	/* (a slot of its own for each caller: the light's thread and the caller of a write note at
	 * the same moment, and with a plain ++ both took one slot and one of the two lines was lost) */
	char *slot = notes[__atomic_fetch_add(&noted, 1, __ATOMIC_RELAXED) % NOTES];
	uint64_t ns;
	va_list list;
	int n;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	n = snprintf(slot, sizeof(notes[0]), "%llu.%03llu s: ", ns / NSEC_PER_SEC, ns / NSEC_PER_MSEC % 1000);
	va_start(list, format);
	vsnprintf(slot + n, sizeof(notes[0]) - (size_t)n, format, list);
	va_end(list);
	say("torch: %s\n", slot);
}

#include "mmio.h"
#include "time.h"

#define wait_ms(ms)             hakios_wait_ms((ms), leaving)   /* (spun on the way out) */

/* ---- the PMIC arbiter (versions 5 to 8: board/spmi.h) -------------------------------------------- */

/* Where its blocks are, its registers and one transfer: board/spmi.h, shared with the other drivers of PMICs. */
#include "spmi.h"

struct peripheral {
	const char *    name;
	uint16_t        id;             /* slave << 8 | peripheral */
	uint16_t        channel;
	bool            found, writable;
	vm_offset_t     write;          /* its write channel, mapped (only the flash module's) */
};
/* (which peripherals of which PMIC: the board's torch { }, at the start; 0x2ee and 0x20a where the PM8350C is slave 2) */
static struct peripheral flash = { .name = "the PM8350C's flash LED module" };
static struct peripheral alarm = { .name = "the PM8350C's temperature alarm" };

static struct hakios_spmi arb;
static uint8_t alarm_subtype;

/* (the arbiter one transfer at a time with the buttons' driver; at the run's end the others are stopped, perhaps holding it) */
static void
lock(void)
{
	if (!leaving) {
		hakios_spmi_lock();
	}
}

static void
unlock(void)
{
	if (!leaving) {
		hakios_spmi_unlock();
	}
}

/*
 * `count' (1 to 8) registers from `reg' on; NULL, or what went wrong. A transfer the arbiter drops
 * now and then (the buttons' driver sees one in a thousand or so, from whatever else is on the bus)
 * is asked for again, twice at most.
 */
static const char *spmi_read_once(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count);

static const char *
spmi_read(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	const char *problem = NULL;

	for (int tries = 0; tries < 3; tries++) {
		if ((problem = spmi_read_once(p, reg, bytes, count)) == NULL || strcmp(problem, SPMI_SAYS_DROPPED) != 0) {
			break;
		}
	}
	return problem;
}

static const char *
spmi_read_once(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	const char *problem;

	if (!p->found) {
		return "no channel";
	}
	lock();
	problem = hakios_spmi_read(hakios_spmi_observer(&arb, p->channel), reg, bytes, count);
	unlock();
	return problem;
}

/* ---- the flash LED module -------------------------------------------------------------------------- */

#define FLASH_REVISION1         0x00
#define FLASH_TYPE              0x04            /* 0x18: a flash LED module */
#define FLASH_SUBTYPE           0x05            /* 0x07: four channels */
#define FLASH_STATUS1           0x06            /* a channel open or shorted */
#define FLASH_STATUS2           0x07
#define  STATUS2_OTST1          0x20            /* over-temperature, the first and the second threshold */
#define  STATUS2_OTST2          0x10
#define  STATUS2_VPH_LOW        0x01            /* the supply drooped */
#define FLASH_INT_RT_STS        0x10
#define  RT_STS_FAULT           0x01            /* open, short, or its supply not ready */
#define FLASH_SAFETY_TIMER0     0x3e            /* [7] on */
#define FLASH_ITARGET0          0x42            /* [6:0]: (code + 1) x the resolution */
#define FLASH_MODULE_EN         0x46            /* [7] */
#define FLASH_IRESOLUTION       0x49            /* a bit per channel: 0 12.5 mA, 1 5 mA */
#define FLASH_STROBE_CTRL0      0x4a            /* [6:4] hardware strobe's kind, [2] hardware strobe */
#define FLASH_EN_LED_CTRL       0x4e            /* a bit per channel */
#define FLASH_OTST2_CFG1        0x78
#define FLASH_OTST1_CFG1        0x7a

#define ALARM_SUBTYPE           0x05            /* 0x08: the first generation, 0x09 the second */
#define ALARM_STATUS            0x08            /* the over-temperature stage: [1:0] (first generation) or [6:4] */

#define ANDROID_TORCH_MA        125             /* led:torch_0 while Android's flashlight is on */
#define IRES_UA                 12500           /* its resolution ("qcom,ires-ua") */
#define ANDROID_ITARGET         9               /* round(125000 / 12500) - 1: the most ever written */
#define HOTTEST_BATTERY_C       45

static const uint8_t writable_regs[] = {
	FLASH_SAFETY_TIMER0, FLASH_ITARGET0, FLASH_MODULE_EN, FLASH_IRESOLUTION, FLASH_STROBE_CTRL0, FLASH_EN_LED_CTRL,
};

static void
give_up(const char *why)
{
	if (!broken) {
		broken = true;
		note("%s: no more writing to the flash module for the rest of the run", why);
	}
}

/* One byte to the module, made (a dropped write has not been: asked for again); NULL, or what went wrong. */
static const char *
send(uint8_t reg, uint8_t value)
{
	const char *problem = NULL;

	for (int tries = 0; tries < 3; tries++) {
		lock();
		problem = hakios_spmi_write(flash.write, reg, value);
		unlock();
		if (problem == NULL || strcmp(problem, SPMI_SAYS_DROPPED) != 0) {
			break;
		}
	}
	return problem;
}

/*
 * One register of the flash module (and of the six above only) written and read back: false, and
 * no more writing, if it cannot be or does not read back.
 */
static bool
put(uint8_t reg, uint8_t value)
{
	const char *problem;
	uint8_t back = 0xff;
	bool allowed = false;

	for (size_t i = 0; i < sizeof(writable_regs); i++) {
		allowed |= writable_regs[i] == reg;
	}
	if (broken || !allowed || !flash.writable || flash.write == 0 || flash.id == 0) {
		give_up(allowed ? "a write that may not be made" : "a register that is not to be written");
		return false;
	}
	/* (the channel is still the module's: its entry in the arbiter's table is looked at again) */
	if (SPMI_ID_OF(hakios_spmi_channel_map(&arb, flash.channel)) != flash.id) {
		give_up("the arbiter's channel is no longer the flash module's");
		return false;
	}
	if ((problem = send(reg, value)) != NULL || (problem = spmi_read(&flash, reg, &back, 1)) != NULL) {
		say("torch: 0x%02x <- 0x%02x: %s\n", reg, value, problem);
		give_up("a write that failed");
		return false;
	}
	if (back != value) {
		say("torch: 0x%02x <- 0x%02x, but it reads 0x%02x\n", reg, value, back);
		give_up("a register that did not read back as written");
		return false;
	}
	return true;
}

/*
 * One of the two registers that put the light out, written 0: the way off, and nothing else. Not refused
 * once writing has been given up on, and nothing read first, so that off never waits on what else went
 * wrong; read back by dark(), after both. Still only through the module's own channel.
 */
static bool
put_off(uint8_t reg)
{
	const char *problem;

	if ((reg != FLASH_MODULE_EN && reg != FLASH_EN_LED_CTRL) || !flash.writable || flash.write == 0 || flash.id == 0) {
		give_up("a write that may not be made");
		return false;
	}
	if (SPMI_ID_OF(hakios_spmi_channel_map(&arb, flash.channel)) != flash.id) {
		give_up("the arbiter's channel is no longer the flash module's");
		return false;
	}
	if ((problem = send(reg, 0x00)) != NULL) {
		say("torch: 0x%02x <- 0x00, to put the light out: %s\n", reg, problem);
		give_up("a write that failed");
		return false;
	}
	return true;
}

static bool
get(const struct peripheral *p, uint8_t reg, uint8_t *value)
{
	const char *problem = spmi_read(p, reg, value, 1);

	if (problem != NULL) {
		say("torch: reading 0x%02x of %s: %s\n", reg, p->name, problem);
		return false;
	}
	return true;
}

/* only the bits of `mask' changed, as the driver's masked writes do */
static bool
put_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
	uint8_t now;

	if (!get(&flash, reg, &now)) {
		give_up("a read that failed");
		return false;
	}
	return (now & mask) == (value & mask) || put(reg, (uint8_t)((now & ~mask) | (value & mask)));
}

static void
find_channel(struct peripheral *p)
{
	for (uint32_t n = 0; n < arb.channels; n++) {
		uint32_t map = hakios_spmi_channel_map(&arb, n), owner;

		if (map == 0 || SPMI_ID_OF(map) != p->id) {
			continue;
		}
		owner = hakios_spmi_channel_owner(&arb, n);
		say("torch: channel %u is %s (%#x); written by system %u%s\n", n, p->name, map, owner,
		    owner == arb.system ? ", which is us" : "");
		if (!p->found || owner == arb.system) {
			p->found = true;
			p->channel = (uint16_t)n;
			p->writable = owner == arb.system;
		}
	}
	if (!p->found) {
		say("torch: no channel of the arbiter is %s\n", p->name);
	}
}

static void
show_registers(void)
{
	uint8_t r[8];
	char line[200];

	for (unsigned base = 0; base < 0x100; base += 0x20) {
		int n = snprintf(line, sizeof(line), "torch: 0x%02x:", base);

		for (unsigned i = 0; i < 0x20; i += 8) {
			if (spmi_read(&flash, (uint8_t)(base + i), r, 8) != NULL) {
				n += snprintf(line + n, sizeof(line) - (size_t)n, " (unreadable)");
				continue;
			}
			for (unsigned j = 0; j < 8; j++) {
				n += snprintf(line + n, sizeof(line) - (size_t)n, " %02x", r[j]);
			}
		}
		say("%s\n", line);
	}
}

/* What would make the light go off now, or NULL. */
static const char *
trouble(void)
{
	static char why[180];
	uint8_t rt = 0, status1 = 0, status2 = 0, alarm_status = 0, stage;

	if (!get(&flash, FLASH_INT_RT_STS, &rt) || !get(&flash, FLASH_STATUS1, &status1) ||
	    !get(&flash, FLASH_STATUS2, &status2) || (alarm.found && !get(&alarm, ALARM_STATUS, &alarm_status))) {
		return "a read of the flags that failed";
	}
	stage = alarm_subtype == 0x08 ? alarm_status & 0x03 : (alarm_status >> 4) & 0x07;
	if ((rt & RT_STS_FAULT) || status1 != 0 || (status2 & (STATUS2_OTST1 | STATUS2_OTST2 | STATUS2_VPH_LOW)) || stage != 0) {
		uint8_t more[8] = { 0 }, latched[2] = { 0 };

		(void)spmi_read(&flash, 0x06, more, 8);
		(void)spmi_read(&flash, 0x18, latched, 2);
		snprintf(why, sizeof(why), "the module says fault %#x, channels %#x, temperature and supply %#x (0x06-0x0d: %02x %02x %02x "
		    "%02x %02x %02x %02x %02x, latched %#x); the PMIC's temperature stage %u", rt, status1, status2, more[0], more[1],
		    more[2], more[3], more[4], more[5], more[6], more[7], latched[0], stage);
		return why;
	}
	/* (a battery never heard of guards nothing, as before; one heard of and gone quiet is not known to be cool) */
	if (hakios_battery_went_quiet()) {
		return "the battery's temperature, known earlier, is no longer reported (its source went quiet)";
	}
	if (hakios_battery_temperature() >= HOTTEST_BATTERY_C) {
		snprintf(why, sizeof(why), "the battery is at %d C", hakios_battery_temperature());
		return why;
	}
	return NULL;
}

static bool configured, lit;

/* Android's probe, once: channel 0 strobed by software, its safety timer off. */
static bool
configure(void)
{
	if (!configured && put_bits(FLASH_STROBE_CTRL0, 0x74, 0x00) && put_bits(FLASH_SAFETY_TIMER0, 0x80, 0x00)) {
		configured = true;
	}
	return configured;
}

/* The light at `permille' of Android's current: its writes in its order. */
static bool
light(uint32_t permille)
{
	uint32_t ua = ANDROID_TORCH_MA * 1000 * permille / 1000, code;
	const char *why;

	if (ua < IRES_UA) {
		ua = IRES_UA;           /* (the driver's least: one step) */
	}
	code = (ua + IRES_UA / 2) / IRES_UA - 1;
	if (code > ANDROID_ITARGET) {
		code = ANDROID_ITARGET;
	}
	if (!lit && (why = trouble()) != NULL) {
		note("not lit: %s", why);
		give_up("trouble before the light");
		return false;
	}
	if (!configure() || !put_bits(FLASH_IRESOLUTION, 0x01, 0x00) || !put_bits(FLASH_ITARGET0, 0x7f, (uint8_t)code)) {
		return false;
	}
	if (!lit) {
		/*
		 * Taken for lit from the first write that may light it: a write that lands and then does not read back leaves
		 * it lit all the same, and whatever ends this (the service, the run's end) switches it off only if lit is set.
		 */
		lit = true;
		if (!put(FLASH_MODULE_EN, 0x80) || !put_bits(FLASH_EN_LED_CTRL, 0x01, 0x01)) {
			return false;
		}
	}
	return true;
}

#define DARK_TRIES              5

/*
 * Off: the module (its enable alone puts the light out, so it goes first), then the channel, each written 0 with
 * nothing read first and whatever the other did, then both read back; all of it again, DARK_TRIES times at most,
 * until they read back off. Then the target current and the timer's bit back as they were found (these read
 * first: the light is out without them). True if all of it was done and read back as written; `lit' cleared as
 * soon as the light reads back off.
 */
static bool
dark(void)
{
	/*
	 * Off is tried whatever went wrong before: a light left on would stay on for the rest of the run with its safety
	 * timer off. (broken stops every other write; it is put back as it was, or set if this failed too.)
	 */
	bool was_broken = broken, written = true, off = false, ok;
	uint8_t module = 0xff, channels = 0xff;

	for (int tries = 0; tries < DARK_TRIES && !off; tries++) {
		if (tries > 0) {
			wait_ms(5);
		}
		written = put_off(FLASH_MODULE_EN) & written;
		written = put_off(FLASH_EN_LED_CTRL) & written;
		module = channels = 0xff;
		off = get(&flash, FLASH_MODULE_EN, &module) && get(&flash, FLASH_EN_LED_CTRL, &channels) && (module & 0x80) == 0 &&
		    (channels & 0x01) == 0;
	}
	if (!off) {
		note("THE LIGHT IS NOT SEEN TO GO OUT after %d tries: enable 0x%02x, channels 0x%02x", DARK_TRIES, module, channels);
	} else if (module != 0 || channels != 0) {
		say("torch: put out, but the module reads: enable 0x%02x, channels 0x%02x\n", module, channels);
		give_up("a register that did not read back as written");
		written = false;
	}
	broken = false;
	ok = put_bits(FLASH_ITARGET0, 0x7f, 0x00) && put_bits(FLASH_SAFETY_TIMER0, 0x80, 0x00) && off && written;
	broken = was_broken || !ok;
	lit = lit && !off;
	return ok;
}

/* ---- torch=3: the light for Darwin --------------------------------------------------------------------- */

static volatile uint32_t wanted;                /* the level asked for last, 0 to 1000 */
static volatile uint32_t asked;                 /* how many times one was */
static uint32_t max_secs = 30;

static int
torch_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t value;
	int error;

	if (broken) {
		return ENXIO;           /* (given up on: Darwin is not to show a light that nothing lights) */
	}
	if (uio_resid(uio) < (user_ssize_t)sizeof(value)) {
		return EINVAL;          /* (a level is 4 bytes: less is not taken, and not to be asked again for ever) */
	}
	while (uio_resid(uio) >= (user_ssize_t)sizeof(value)) {
		if ((error = uiomove((caddr_t)&value, sizeof(value), uio)) != 0) {
			return error;
		}
		wanted = value > 1000 ? 1000 : value;
		asked++;
	}
	return 0;
}

/* what is lit now: the level, 0 if nothing */
static uint32_t shown;

static int
torch_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t value = shown;

	if (uio_offset(uio) != 0) {
		return 0;               /* (the level once a read: the end after it, so that a reader to the end ends) */
	}
	return uiomove((caddr_t)&value, (int)MIN(sizeof(value), (size_t)uio_resid(uio)), uio);
}

static int
torch_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw torch_cdevsw = {
	.d_open = torch_open, .d_close = torch_open, .d_read = torch_read, .d_write = torch_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static uint64_t
now_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

static void
serve(void)
{
	uint32_t applied = 0, times = 0, cut = 0;
	uint64_t since = 0, last_look = 0;
	bool waiting_for_off = false;           /* cut at the limit: dark until Darwin asks for 0 */
	int major = cdevsw_add(-1, &torch_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-torch") == NULL) {
		say("torch: /dev/hakios-torch cannot be made\n");
		return;
	}
	say("torch: THE LIGHT IS DARWIN'S (/dev/hakios-torch) from here on, until the run ends: at most Android's %u mA, at most %u s "
	    "at a time\n", ANDROID_TORCH_MA, max_secs);
	while (!broken) {
		uint32_t want = wanted;
		uint64_t now = now_ms();

		__atomic_store_n(&writing, true, __ATOMIC_SEQ_CST);
		if (__atomic_load_n(&leaving, __ATOMIC_SEQ_CST)) {
			break;                  /* (the run's end has the light now: hakios_torch_quiesce()) */
		}

		if (want == 0) {
			waiting_for_off = false;
		}
		if (waiting_for_off) {
			want = 0;
		}
		if (want != applied) {
			if (want == 0 ? dark() : light(want)) {
				if (applied == 0) {
					since = now;
					times++;
				}
				if (want == 0) {
					note("off (Darwin asked for 0)");
				} else {
					note("ON (Darwin asked for %u of 1000; the battery %d C)", wanted, hakios_battery_temperature());
				}
				applied = want;
				shown = want;
			} else {
				break;
			}
		}
		if (applied != 0 && now - since >= max_secs * 1000ull) {
			cut++;
			note("on for %u s, the limit: off until Darwin asks again", max_secs);
			waiting_for_off = true;
			if (!dark()) {
				break;
			}
			applied = 0;
			shown = 0;
			__atomic_store_n(&writing, false, __ATOMIC_SEQ_CST);
			continue;
		}
		if (applied != 0 && now - last_look >= 100) {
			const char *why;

			last_look = now;
			if ((why = trouble()) != NULL) {
				note("OFF: %s", why);
				(void)dark();
				give_up("trouble while lit");
				break;
			}
		}
		__atomic_store_n(&writing, false, __ATOMIC_SEQ_CST);
		wait_ms(20);
	}
	if (lit && !__atomic_load_n(&leaving, __ATOMIC_SEQ_CST)) {
		(void)dark();
	}
	__atomic_store_n(&writing, false, __ATOMIC_SEQ_CST);
	shown = 0;
	say("torch: the light is given up on (%u times lit, %u cut at %u s)\n", times, cut, max_secs);
}

/* ---- the start ------------------------------------------------------------------------------------- */

static void
torch_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint8_t id[2] = { 0, 0 }, revision = 0, alarm_status = 0;

	say("torch: level %u: %s\n", level, level == 1 ? "a look, reading only" : level == 2 ? "the light on once" :
	    "the light for Darwin");
	if (!hakios_spmi_map(&arb)) {
		say("torch: PMIC arbiter version %#x, not 5, 7 or 8: nothing done\n", arb.version);
		goto out;
	}
	find_channel(&flash);
	find_channel(&alarm);
	if (!flash.found || spmi_read(&flash, FLASH_TYPE, id, 2) != NULL || spmi_read(&flash, FLASH_REVISION1, &revision, 1) != NULL) {
		say("torch: the flash module cannot be read\n");
		goto out;
	}
	say("torch: the module says type %#x, subtype %#x, revision %#x%s\n", id[0], id[1], revision,
	    id[0] == 0x18 && id[1] == 0x07 ? ": a flash LED module of four channels, as Android's driver expects" : ": NOT WHAT WAS EXPECTED");
	show_registers();
	if (alarm.found && get(&alarm, ALARM_SUBTYPE, &alarm_subtype) && get(&alarm, ALARM_STATUS, &alarm_status)) {
		uint8_t stage = alarm_subtype == 0x08 ? alarm_status & 0x03 : (alarm_status >> 4) & 0x07;

		say("torch: the PMIC's temperature alarm (subtype %#x) says %#x: stage %u%s\n", alarm_subtype, alarm_status, stage,
		    stage == 0 ? " (all is well)" : "");
	} else {
		alarm.found = false;
	}
	if (id[0] != 0x18 || id[1] != 0x07 || level < 2) {
		goto out;
	}
	if (!flash.writable) {
		say("torch: the channel is not ours to write through: nothing written\n");
		goto out;
	}
	flash.write = hakios_spmi_map_write(&arb, flash.channel);
	PE_parse_boot_argn("torch_max_s", &max_secs, sizeof(max_secs));
	max_secs = max_secs < 1 ? 1 : max_secs > 120 ? 120 : max_secs;
	if (level == 2) {
		uint32_t secs = 2;
		uint64_t until;
		const char *why = NULL;

		PE_parse_boot_argn("torch_secs", &secs, sizeof(secs));
		secs = secs < 1 ? 1 : secs > max_secs ? max_secs : secs;
		say("torch: the light ON at Android's %u mA (ITARGET %u, 12.5 mA steps) for %u s\n", ANDROID_TORCH_MA, ANDROID_ITARGET,
		    secs);
		if (light(1000)) {
			uint8_t rt = 0, s1 = 0, s2 = 0;

			wait_ms(50);
			(void)get(&flash, FLASH_INT_RT_STS, &rt);
			(void)get(&flash, FLASH_STATUS1, &s1);
			(void)get(&flash, FLASH_STATUS2, &s2);
			note("lit at Android's %u mA; 50 ms on: interrupt status %#x (%s), channels %#x, temperature and supply %#x",
			    ANDROID_TORCH_MA, rt, (rt & 0x10) ? "ramped up" : "NOT ramped up", s1, s2);
			show_registers();
			for (until = now_ms() + secs * 1000ull; now_ms() < until && (why = trouble()) == NULL;) {
				wait_ms(100);
			}
			if (why != NULL) {
				note("OFF early: %s", why);
			}
			if (dark()) {
				note("off again after %u s", secs);
			}
			show_registers();
		} else if (lit) {
			/* (lit by a write that then did not read back as written: put out all the same, not left on with no limit) */
			(void)dark();
		}
	} else {
		serve();
	}
out:
	say("torch: done\n");
	thread_terminate(current_thread());
}

void
hakios_torch_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("torch", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (!hakios_board_has(HAKIOS_HAS_TORCH_PM8350C)) {
		/* every address below is one phone's: not on a board that does not say it is wired the same */
		say("torch: disabled: this board's board.dts lists no \"torch-pm8350c\"\n");
		level = 0;
		return;
	}
	if (hakios_board()->spmi.core == 0 || hakios_board()->spmi.channels == 0) {
		say("torch: disabled: the SoC file's spmi { } does not say where the arbiter and its channels are\n");
		level = 0;
		return;
	}
	if (hakios_board()->torch.flash == 0 || hakios_board()->torch.temp_alarm == 0) {
		say("torch: disabled: the board file does not say which peripherals of the PMIC the light is (torch { })\n");
		level = 0;
		return;
	}
	flash.id = hakios_board()->torch.flash;
	alarm.id = hakios_board()->torch.temp_alarm;
	level = level > 3 ? 3 : level;
	if (kernel_thread_start(torch_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end and on a panic's way out: the light off, if it was ever lit. */
void
hakios_torch_quiesce(void)
{
	uint8_t module = 0xff, channels = 0xff, target = 0xff;

	if (level < 2 || flash.write == 0) {
		return;
	}
	__atomic_store_n(&leaving, true, __ATOMIC_SEQ_CST);
	for (unsigned i = 0; i < 100 && __atomic_load_n(&writing, __ATOMIC_SEQ_CST); i++) {
		wait_ms(1);             /* (the thread finishing what it writes: 100 ms at most, a stopped CPU's never ends) */
	}
	if (lit) {
		broken = false;         /* (one last time, whatever went before) */
		say("torch: at the run's end: the light %s\n", dark() ? "switched off" : "COULD NOT BE SWITCHED OFF");
	}
	say("torch: what happened in this run (%u events):\n", noted);
	for (unsigned i = noted > NOTES ? noted - NOTES : 0; i < noted; i++) {
		say("torch:   %s\n", notes[i % NOTES]);
	}
	(void)spmi_read(&flash, FLASH_MODULE_EN, &module, 1);
	(void)spmi_read(&flash, FLASH_EN_LED_CTRL, &channels, 1);
	(void)spmi_read(&flash, FLASH_ITARGET0, &target, 1);
	say("torch: at the run's end the module reads: enable 0x%02x, channels 0x%02x, target 0x%02x%s\n", module, channels, target,
	    module == 0 && (channels & 0x01) == 0 && target == 0 ? " (dark)" : " (NOT DARK)");
}
