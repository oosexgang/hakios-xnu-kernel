/*
 * The buttons of the POCO X5 Pro (redwood) -- power, volume up, volume down -- as a source of
 * button records for /dev/redwood-touch (sys/redwood_input.h). Only with boot-arg buttons=1:
 * every address below belongs to this phone.
 *
 * None of the three is a pin of the SoC. They are inputs of two of the power management chips:
 *   power        KPDPWR_N of the power-on block ("PON") of the PMK8350
 *   volume down  RESIN_N of the same block
 *   volume up    GPIO6 of the PM7325, pulled up; the key pulls it low
 * Both chips sit on the SPMI bus (slaves 0 and 1), and the way to that bus is the SoC's "PMIC
 * arbiter": it owns the bus and gives each of the systems on the chip (this one, the modem, the
 * always-on processor, TrustZone...) a set of registers per PMIC peripheral -- a "channel" -- to
 * ask for a transfer with. Reads go through the "observer" channels, which every system has for
 * every peripheral, whoever owns it.
 *
 * THIS DRIVER ONLY READS. Nothing is written to either chip, neither a register of a PMIC nor
 * the configuration of the pin: the PMICs make the phone's supplies, and a wrong write there
 * can switch one off. The firmware reads the same keys itself (volume down and volume up choose
 * between fastboot and recovery) and has left both inputs as they are needed; the driver checks
 * that before it believes them, and where it finds something else it says so and leaves that
 * key alone. The only thing written is the command word that asks the arbiter for a read.
 *
 * What a press is: the PON's "real time status" has a bit per key, set while the key is held
 * (after the block's own debouncing); the GPIO's has the level of the pin. A thread reads the
 * two registers 60 times a second, and a key has changed when two looks in a row say so. A key
 * that reads as held before it has ever been seen released is not believed (a pin that is not
 * what it was taken for must not become a volume key held down for the whole session).
 * No interrupt is used; what one would take is said at the end of this file.
 *
 * The mapping: the phone has what the iPhone X has, whose user space runs here -- a side button
 * and two volume keys, no home button. Power is the side button (REDWOOD_BUTTON_LOCK).
 *
 * The bring-up is made in steps, each said in the log before it is made. Boot-arg
 * buttons_steps=N stops after the first N:
 *   1  map the arbiter and read its version             (the first access to that block)
 *   2  find the channels of the two peripherals         (reads of the arbiter's tables)
 *   3  ask each peripheral what it is                   (the first transfers on the bus)
 *   4  read how the keys' inputs are set up, and what they say now
 *   5  watch them                                       (the default)
 *
 * Register layouts: Linux drivers/spmi/spmi-pmic-arb.c (arbiter version 5),
 * drivers/input/misc/pm8941-pwrkey.c (the "pmk8350" kind), drivers/pinctrl/qcom/pinctrl-spmi-gpio.c,
 * and the phone's own device tree (qcom,spmi@c440000, pon_hlos@1300, gpio_keys).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

/*
 * Every line goes to the loader's log directly as well: without -v the kernel's own messages
 * reach that log only at the end of a run (with the kernel's message buffer), and "xl log"
 * during a run is to show a press when it happens.
 */
extern void loader_log_putc(char c);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}
#define printf say

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

/* ---- the PMIC arbiter ---------------------------------------------------------------------------- */

#define ARB_CORE_PHYS           0x0c440000UL    /* "core": the version, and which peripheral each channel is */
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           0x0c40a000UL    /* "cnfg": which system owns each channel */
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       0x0e600000UL    /* "obsrvr": 0x10000 per system, 0x80 per channel in it */
#define ARB_OBSERVER_SIZE       0x10000UL       /* ours is the first */
#define ARB_SYSTEM              0               /* who we are to the arbiter: the device tree's "qcom,ee" */

#define ARB_VERSION             0x000
#define ARB_VERSION_5           0x50000000u     /* what follows is version 5's; 7 moved things again */
#define ARB_VERSION_7           0x70000000u
#define ARB_FEATURES            0x004           /* [10:0] the number of channels */
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))       /* core: [19:8] slave and peripheral, [24] interrupts go to the owner */
#define ARB_CHANNEL_INTERRUPTS  (1u << 24)
#define ARB_OWNER(n)            (0x700 + 4 * (n))       /* cnfg: [2:0] the system that may write through channel n */
#define ARB_MAX_CHANNELS        512

#define OBSERVER_COMMAND        0x00
#define OBSERVER_STATUS         0x08
#define OBSERVER_DATA0          0x18            /* the bytes read, the first in the lowest byte */
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_READ            (1u << 27)      /* "extended register read long"; [11:4] register, [2:0] bytes - 1 */
#define READ_TIMEOUT_US         2000            /* a read takes some tens of microseconds */

static vm_offset_t arb_core, arb_cnfg, arb_observer;

/*
 * One transfer at a time through the arbiter for this system, whoever asks (the flashlight's
 * driver, bsd/dev/redwood_torch.c, as well): with two at once the arbiter drops one ("the transfer
 * was dropped"), as Linux never lets happen (one lock for the bus).
 */
static LCK_GRP_DECLARE(spmi_lck_grp, "redwood-spmi");
static LCK_SPIN_DECLARE(spmi_lock, &spmi_lck_grp);

void
redwood_spmi_lock(void)
{
	lck_spin_lock(&spmi_lock);
}

void
redwood_spmi_unlock(void)
{
	lck_spin_unlock(&spmi_lock);
}

struct peripheral {
	const char *    name;
	uint16_t        id;             /* slave << 8 | peripheral: the high byte of its register addresses */
	uint16_t        channel;
	bool            found;
};
static struct peripheral pon = { .name = "the PMK8350's power-on block", .id = 0x013 };          /* registers 0x1300 of slave 0 */
static struct peripheral pin = { .name = "GPIO6 of the PM7325", .id = 0x18d };                   /* registers 0x8d00 of slave 1 */

/* How the reads have gone: said once a minute. */
static struct {
	uint64_t reads, failed, total_ns, longest_ns, last_ns;
} bus;

/*
 * Read `count` (1 to 8) registers of a peripheral, from register `reg` on. Returns NULL, or what
 * went wrong.
 */
static const char *
spmi_read(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t channel = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * p->channel;
	uint64_t start = mach_absolute_time(), now, interval, taken;
	uint32_t status, data[2];
	const char *problem = NULL;

	nanoseconds_to_absolutetime(READ_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	redwood_spmi_lock();
	wr(channel + OBSERVER_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	do {
		status = rd(channel + OBSERVER_STATUS);
		now = mach_absolute_time();
	} while (!(status & STATUS_DONE) && now - start < interval);
	data[0] = rd(channel + OBSERVER_DATA0);
	data[1] = count > 4 ? rd(channel + OBSERVER_DATA1) : 0;
	redwood_spmi_unlock();

	if (!(status & STATUS_DONE)) {
		problem = "no answer from the arbiter";
	} else if (status & STATUS_DENIED) {
		problem = "the arbiter refuses";
	} else if (status & STATUS_FAILURE) {
		problem = "the transfer failed (nobody at that address?)";
	} else if (status & STATUS_DROPPED) {
		problem = "the transfer was dropped";
	} else {
		for (uint32_t i = 0; i < count; i++) {
			bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
		}
	}
	absolutetime_to_nanoseconds(now - start, &taken);
	bus.reads++;
	bus.total_ns += taken;
	bus.last_ns = taken;
	if (taken > bus.longest_ns) {
		bus.longest_ns = taken;
	}
	if (problem != NULL) {
		bus.failed++;
	}
	return problem;
}

/*
 * Which channel is that peripheral's. Several can be: one per system that may write to it. As
 * Linux chooses: the first, unless a later one is ours to write through.
 */
static void
find_channel(struct peripheral *p, uint32_t channels)
{
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n)), owner;

		if (map == 0 || ((map >> 8) & 0xfff) != p->id) {
			continue;
		}
		owner = rd(arb_cnfg + ARB_OWNER(n)) & 7;
		printf("buttons: channel %u is %s (%#x); written by system %u%s\n", n, p->name, map, owner,
		    (map & ARB_CHANNEL_INTERRUPTS) ? ", which also gets its interrupts" : "");
		if (!p->found || owner == ARB_SYSTEM) {
			p->found = true;
			p->channel = (uint16_t)n;
		}
	}
	if (!p->found) {
		printf("buttons: no channel of the arbiter is %s (slave %u, peripheral %#x)\n", p->name, p->id >> 8, p->id & 0xff);
	}
}

/* ---- the two peripherals ------------------------------------------------------------------------- */

/* every peripheral of these PMICs */
#define REG_TYPE                0x04
#define REG_SUBTYPE             0x05
#define REG_RT_STATUS           0x10            /* the inputs as they are now */
#define REG_INT_SET_TYPE        0x11            /* interrupts: a bit per input in each of these */
#define REG_INT_POLARITY_HIGH   0x12
#define REG_INT_POLARITY_LOW    0x13
#define REG_INT_ENABLED         0x15
#define REG_INT_LATCHED         0x18
#define REG_INT_PENDING         0x19

/* the power-on block, of the kind that is split in two ("generation 3"): this is its half for the operating system */
#define PON_TYPE                0x01
#define PON_SUBTYPE_HLOS        0x09
#define PON_KPDPWR              0x80            /* in the status and in the interrupt registers */
#define PON_RESIN               0x40

/* a GPIO of the low or medium voltage kind */
#define GPIO_TYPE               0x10
#define GPIO_SUBTYPE_LV         0x10
#define GPIO_SUBTYPE_MV_VIN3    0x13            /* 0x10 to 0x13: one layout */
#define GPIO_LEVEL              0x01            /* in the status */
#define GPIO_REG_MODE           0x40            /* [1:0]: 0 input, 1 output, 2 both, 3 analog */
#define GPIO_REG_SUPPLY         0x41
#define GPIO_REG_PULL           0x42            /* [2:0]: 0 to 3 up (30, 1.5, 31.5 uA, 1.5 with a boost), 4 down, 5 none */
#define GPIO_REG_ENABLE         0x46            /* [7] */

#define POLL_MS                 16
#define MINUTE_TICKS            (60 * 1000 / POLL_MS)

static struct button {
	const char *    name;
	uint32_t        record;         /* REDWOOD_BUTTON_* */
	bool            usable;         /* its input is what it was taken for */
	bool            live;           /* it has been seen released */
	bool            last;           /* the look before this one */
	bool            down;           /* as reported */
	uint64_t        since;          /* when it went down */
	uint32_t        presses;
} power = { .name = "power", .record = REDWOOD_BUTTON_LOCK },
    volume_up = { .name = "volume up", .record = REDWOOD_BUTTON_VOLUME_UP },
    volume_down = { .name = "volume down", .record = REDWOOD_BUTTON_VOLUME_DOWN };

/* "12.345 s (08:13:05.918 UTC)": since boot as in the log's own stamps, and as the daemons' lines have it */
static void
now_text(char *text, size_t size, uint64_t now)
{
	clock_sec_t seconds;
	clock_usec_t micro;
	uint64_t up;

	absolutetime_to_nanoseconds(now, &up);
	clock_get_calendar_microtime(&seconds, &micro);
	snprintf(text, size, "%llu.%03llu s (%02u:%02u:%02u.%03u UTC)", up / NSEC_PER_SEC, up / NSEC_PER_MSEC % 1000,
	    (unsigned)(seconds / 3600 % 24), (unsigned)(seconds / 60 % 60), (unsigned)(seconds % 60), (unsigned)(micro / 1000));
}

static void
button_look(struct button *b, bool pressed)
{
	uint64_t now = mach_absolute_time(), held;
	bool steady = pressed == b->last;
	char when[64];

	b->last = pressed;
	if (!b->usable || !steady) {
		return;
	}
	if (!b->live) {
		if (!pressed) {
			b->live = true;
			printf("buttons: %s is released now: believed from here on\n", b->name);
		}
		return;
	}
	if (pressed == b->down) {
		return;
	}
	b->down = pressed;
	now_text(when, sizeof(when), now);
	if (pressed) {
		b->since = now;
		b->presses++;
		printf("buttons: %s PRESSED at %s\n", b->name, when);
	} else {
		absolutetime_to_nanoseconds(now - b->since, &held);
		printf("buttons: %s released at %s, after %llu ms\n", b->name, when, held / NSEC_PER_MSEC);
	}
	redwood_touch_post(pressed ? REDWOOD_BUTTON_DOWN : REDWOOD_BUTTON_UP, b->record, 0, 0);
}

/* One look at both peripherals. */
static void
buttons_poll(void)
{
	static unsigned said;
	const char *problem;
	uint8_t status;

	if (power.usable || volume_down.usable) {
		problem = spmi_read(&pon, REG_RT_STATUS, &status, 1);
		if (problem == NULL) {
			button_look(&power, status & PON_KPDPWR);
			button_look(&volume_down, status & PON_RESIN);
		} else if (said < 8) {
			said++;
			printf("buttons: reading %s: %s\n", pon.name, problem);
		}
	}
	if (volume_up.usable) {
		problem = spmi_read(&pin, REG_RT_STATUS, &status, 1);
		if (problem == NULL) {
			button_look(&volume_up, !(status & GPIO_LEVEL));
		} else if (said < 8) {
			said++;
			printf("buttons: reading %s: %s\n", pin.name, problem);
		}
	}
}

/* Step 3: is each peripheral what the registers above are those of? */
static bool
identify(const struct peripheral *p, uint8_t type, uint8_t subtype_from, uint8_t subtype_to)
{
	const char *problem;
	uint8_t is[2];

	if (!p->found) {
		return false;
	}
	printf("buttons: next: read the type of %s through channel %u (2 registers, 0x%02x04)\n", p->name, p->channel, p->id & 0xff);
	problem = spmi_read(p, REG_TYPE, is, 2);
	if (problem != NULL) {
		printf("buttons: %s: %s\n", p->name, problem);
		return false;
	}
	printf("buttons: %s says type %#x, subtype %#x (the read took %llu us)\n", p->name, is[0], is[1], bus.last_ns / 1000);
	if (is[0] != type || is[1] < subtype_from || is[1] > subtype_to) {
		printf("buttons: ... which is not what this driver knows (type %#x, subtype %#x to %#x): left alone\n", type,
		    subtype_from, subtype_to);
		return false;
	}
	return true;
}

/* Step 4 for the power-on block: nothing to check but what it says now; its interrupt setup goes to the log. */
static void
pon_inputs(void)
{
	const char *problem;
	uint8_t r[8], more[2];

	printf("buttons: next: read the status and the interrupt setup of %s (registers 0x1310 to 0x1319)\n", pon.name);
	problem = spmi_read(&pon, REG_RT_STATUS, r, 8);
	if (problem == NULL) {
		problem = spmi_read(&pon, REG_INT_LATCHED, more, 2);
	}
	if (problem != NULL) {
		printf("buttons: %s: %s\n", pon.name, problem);
		return;
	}
	printf("buttons: power-on block: status %#x (power %s, volume down %s); interrupts: edge %#x, high %#x, low %#x, "
	    "enabled %#x, latched %#x, pending %#x\n", r[0], (r[0] & PON_KPDPWR) ? "HELD" : "released",
	    (r[0] & PON_RESIN) ? "HELD" : "released", r[REG_INT_SET_TYPE - REG_RT_STATUS], r[REG_INT_POLARITY_HIGH - REG_RT_STATUS],
	    r[REG_INT_POLARITY_LOW - REG_RT_STATUS], r[REG_INT_ENABLED - REG_RT_STATUS], more[0], more[1]);
	power.usable = volume_down.usable = true;
	power.last = r[0] & PON_KPDPWR;
	volume_down.last = r[0] & PON_RESIN;
}

/* Step 4 for the pin: it is volume up only if it is an input with a pull-up, and switched on. */
static void
pin_input(void)
{
	const char *problem;
	uint8_t c[8], status = 0;
	unsigned mode, pull;

	printf("buttons: next: read the setup and the level of %s (registers 0x8d40 to 0x8d47, 0x8d10)\n", pin.name);
	problem = spmi_read(&pin, GPIO_REG_MODE, c, 8);
	if (problem == NULL) {
		problem = spmi_read(&pin, REG_RT_STATUS, &status, 1);
	}
	if (problem != NULL) {
		printf("buttons: %s: %s\n", pin.name, problem);
		return;
	}
	mode = c[0] & 3;
	pull = c[GPIO_REG_PULL - GPIO_REG_MODE] & 7;
	printf("buttons: volume up's pin: mode %u (%s), supply %u, pull %u (%s), %s; registers 0x40.. = %02x %02x %02x %02x %02x %02x "
	    "%02x %02x; level %s\n", mode, mode == 0 ? "input" : mode == 1 ? "output" : mode == 2 ? "input and output" : "analog",
	    c[GPIO_REG_SUPPLY - GPIO_REG_MODE] & 7, pull, pull < 4 ? "up" : pull == 4 ? "down" : "none",
	    (c[GPIO_REG_ENABLE - GPIO_REG_MODE] & 0x80) ? "enabled" : "DISABLED", c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7],
	    (status & GPIO_LEVEL) ? "high (released)" : "LOW (held)");
	if (mode != 0 || pull >= 4 || !(c[GPIO_REG_ENABLE - GPIO_REG_MODE] & 0x80)) {
		printf("buttons: volume up's pin is not an enabled input with a pull-up. Making it one is a write to the PMIC, "
		    "which this driver does not do: volume up is left alone\n");
		return;
	}
	volume_up.usable = true;
	volume_up.last = !(status & GPIO_LEVEL);
}

static void
say_summary(void)
{
	printf("buttons: so far %u presses of power, %u of volume up, %u of volume down; now%s%s%s%s; %llu reads of the bus, "
	    "%llu us each (the longest %llu), %llu failed\n", power.presses, volume_up.presses, volume_down.presses,
	    power.down ? " power" : "", volume_up.down ? " volume-up" : "", volume_down.down ? " volume-down" : "",
	    (power.down || volume_up.down || volume_down.down) ? " held" : " none held", bus.reads,
	    bus.reads ? bus.total_ns / bus.reads / 1000 : 0, bus.longest_ns / 1000, bus.failed);
}

static void
buttons_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)parameter, version, channels;
	bool pon_known, pin_known;

	printf("buttons: step 1: map the PMIC arbiter (%#lx, %#lx, %#lx) and read its version\n", ARB_CORE_PHYS, ARB_CNFG_PHYS,
	    ARB_OBSERVER_PHYS);
	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	version = rd(arb_core + ARB_VERSION);
	channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	printf("buttons: PMIC arbiter version %#x, %u channels\n", version, channels);
	if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
		printf("buttons: this driver knows version 5 of the arbiter only: no buttons\n");
		goto out;
	}
	if (channels == 0 || channels > ARB_MAX_CHANNELS) {
		channels = ARB_MAX_CHANNELS;
	}
	if (steps < 2) {
		goto out;
	}

	printf("buttons: step 2: look through the arbiter's table for the channels of the two peripherals\n");
	find_channel(&pon, channels);
	find_channel(&pin, channels);
	if (steps < 3) {
		goto out;
	}

	printf("buttons: step 3: the first transfers on the SPMI bus\n");
	pon_known = identify(&pon, PON_TYPE, PON_SUBTYPE_HLOS, PON_SUBTYPE_HLOS);
	pin_known = identify(&pin, GPIO_TYPE, GPIO_SUBTYPE_LV, GPIO_SUBTYPE_MV_VIN3);
	if (steps < 4) {
		goto out;
	}

	printf("buttons: step 4: how the keys' inputs are set up\n");
	if (pon_known) {
		pon_inputs();
	}
	if (pin_known) {
		pin_input();
	}
	if (!power.usable && !volume_up.usable) {
		printf("buttons: nothing that can be read: no buttons\n");
		goto out;
	}
	if (power.last || volume_down.last || volume_up.last) {
		printf("buttons: a key reads as held before anything was watched:%s%s%s. It is not reported until it has been "
		    "seen released\n", power.last ? " power" : "", volume_up.last ? " volume-up" : "",
		    volume_down.last ? " volume-down" : "");
	}
	/* (a key that is released now is believed at once) */
	power.live = power.usable && !power.last;
	volume_down.live = volume_down.usable && !volume_down.last;
	volume_up.live = volume_up.usable && !volume_up.last;
	if (steps < 5) {
		goto out;
	}

	printf("buttons: step 5: watching%s%s%s, every %u ms: power is the side button\n", power.usable ? " power" : "",
	    volume_up.usable ? " volume-up" : "", volume_down.usable ? " volume-down" : "", POLL_MS);
	for (unsigned tick = 1;; tick++) {
		buttons_poll();
		/* once early on as well: a short run is to say what the reads cost too */
		if (tick % MINUTE_TICKS == 0 || tick == 10 * 1000 / POLL_MS) {
			say_summary();
		}
		if (bus.failed >= 1000 && bus.failed * 2 > bus.reads) {
			printf("buttons: more than half of %llu reads have failed: giving up\n", bus.reads);
			break;
		}
		delay_for_interval(POLL_MS, NSEC_PER_MSEC);
	}
out:
	if (steps < 5) {
		printf("buttons: stopping here (buttons_steps=%u)\n", steps);
	}
	thread_terminate(current_thread());
}

void
redwood_buttons_init(void)
{
	uint32_t enabled = 0, steps = 5;
	thread_t thread;

	/*
	 * On with the touch screen (the phone, somebody at it) unless buttons=0 says otherwise: a hand
	 * has pressed all three keys and each press and release was read as it was made.
	 */
	if (!PE_parse_boot_argn("buttons", &enabled, sizeof(enabled))) {
		uint32_t touch_screen = 0;

		enabled = PE_parse_boot_argn("touchscreen", &touch_screen, sizeof(touch_screen)) && touch_screen != 0;
	}
	if (!enabled) {
		return;
	}
	PE_parse_boot_argn("buttons_steps", &steps, sizeof(steps));
	if (steps == 0) {
		return;
	}
	if (kernel_thread_start(buttons_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/*
 * What interrupts would take, instead of looking 60 times a second:
 *
 *   - The arbiter has one interrupt line for all the PMICs' peripherals ("periph_irq": through the
 *     power domain controller's pin 1 to the interrupt controller's SPI 481). This kernel has no
 *     driver that takes a hardware interrupt other than the timer's so far; that is the first
 *     thing to add (a handler registered for one more interrupt of the GICv3 the kernel already
 *     drives, and that line unmasked and set to level, high).
 *   - Per channel, the arbiter has an enable bit ("ACC_ENABLE"), a status and a clear register
 *     (in the "chnls" region: 0x100, 0x104, 0x108 of 0x10000 per channel), and per system a
 *     summary of which channels have something. Those are writes to the arbiter, not to a PMIC.
 *   - But the peripheral must be told to raise them as well: for each key a bit in "interrupt
 *     type" (edge), "polarity high" and "polarity low" (both edges), and "enable set". Those are
 *     WRITES TO THE PMIC -- to the power-on block for power and volume down, to the GPIO for
 *     volume up -- unless the firmware has left them set; step 4 prints what it finds there.
 *     After an interrupt the peripheral's latched status is cleared by another write to it.
 *   - Only the system that owns a peripheral's channel gets its interrupts (step 2 prints the
 *     owners).
 * Looking costs two reads of some tens of microseconds each, 60 times a second (the minute's
 * summary line says what they really take), so the reason to want interrupts would not be
 * processor time; it would be a system that sleeps and is to be woken by a key, and this one
 * does not sleep.
 */
