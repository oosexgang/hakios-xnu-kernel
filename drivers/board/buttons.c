/*
 * A phone's buttons -- power, volume up, volume down -- as a source of button records for
 * /dev/hakios-touch (sys/board/input.h). Only with boot-arg buttons=1, on a board that lists
 * "buttons-pmic": where the arbiter is comes from the SoC file (spmi { }), which peripherals the keys
 * are on from the board file (buttons { }).
 *
 * None of the three is a pin of the SoC. They are inputs of two of the power management chips; on
 * the two phones this was written on:
 *   power        KPDPWR_N of the power-on block ("PON") of the PMK8350
 *   volume down  RESIN_N of the same block
 *   volume up    a PMIC GPIO (GPIO6 of a PM7325 or a PM8350 on the first phones), pulled up; the key pulls it low
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
 * and two volume keys, no home button. Power is the side button (HAKIOS_BUTTON_LOCK).
 *
 * The bring-up is made in steps, each said in the log before it is made. Boot-arg
 * buttons_steps=N stops after the first N:
 *   1  map the arbiter and read its version             (the first access to that block)
 *   2  find the channels of the two peripherals         (reads of the arbiter's tables)
 *   3  ask each peripheral what it is                   (the first transfers on the bus)
 *   4  read how the keys' inputs are set up, and what they say now
 *   5  watch them                                       (the default)
 *
 * The power-on block is of either kind Linux's pm8941-pwrkey.c reads: the one split in two of the PMK8350 and
 * after ("generation 3": its half for the operating system, the keys in bits 7 and 6 of its status), or the one of
 * before (the PM8998's and the PM8150's, "generations" 1 and 2: the keys in bits 0 and 1). Which it is, its subtype
 * says at step 3.
 *
 * Register layouts: Linux drivers/spmi/spmi-pmic-arb.c (the arbiter, versions 5 to 8: through board/spmi.h),
 * drivers/input/misc/pm8941-pwrkey.c (every kind of it), drivers/pinctrl/qcom/pinctrl-spmi-gpio.c,
 * and the phone's own device tree (qcom,spmi@c440000, pon_hlos@1300, gpio_keys).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/board/input.h>
#include <sys/systm.h>

#include "kernel.h"
#include "board.h"
#include "log.h"

/*
 * Every line goes to the loader's log directly as well: without -v the kernel's own messages
 * reach that log only at the end of a run (with the kernel's message buffer), and "bench log"
 * during a run is to show a press when it happens.
 */

HAKIOS_DEFINE_SAY(true)
#define printf say

#include "mmio.h"

/* ---- the PMIC arbiter ---------------------------------------------------------------------------- */

/* Where its blocks are, its registers and one transfer: board/spmi.h, shared with the other drivers of PMICs. */
#include "spmi.h"

static struct hakios_spmi arb;

/*
 * One transfer at a time through the arbiter for this system, whoever asks (the flashlight's
 * driver, bsd/dev/board/torch.c, as well): with two at once the arbiter drops one ("the transfer
 * was dropped"), as Linux never lets happen (one lock for the bus).
 */
static LCK_GRP_DECLARE(spmi_lck_grp, "hakios-spmi");
static LCK_SPIN_DECLARE(spmi_lock, &spmi_lck_grp);

void
hakios_spmi_lock(void)
{
	lck_spin_lock(&spmi_lock);
}

void
hakios_spmi_unlock(void)
{
	lck_spin_unlock(&spmi_lock);
}

struct peripheral {
	const char *    name;
	uint16_t        id;             /* slave << 8 | peripheral: the high byte of its register addresses */
	uint16_t        channel;
	bool            found;
	unsigned        unread;         /* the looks in a row whose read of it has failed (up to LET_GO_LOOKS) */
};
/* The board's buttons { }; on both phones so far 0x013 (registers 0x1300 of slave 0) and 0x18d (0x8d00 of slave 1). */
static struct peripheral pon = { .name = "the power-on block" };
static struct peripheral pin = { .name = "volume up's GPIO" };
#define REGISTERS(p)            (((p)->id & 0xffu) << 8)        /* where its registers begin, on its slave */

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
	uint64_t start = mach_absolute_time(), taken;
	const char *problem;

	hakios_spmi_lock();
	problem = hakios_spmi_read(hakios_spmi_observer(&arb, p->channel), reg, bytes, count);
	hakios_spmi_unlock();
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &taken);
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
find_channel(struct peripheral *p)
{
	for (uint32_t n = 0; n < arb.channels; n++) {
		uint32_t map = hakios_spmi_channel_map(&arb, n), owner;

		if (map == 0 || SPMI_ID_OF(map) != p->id) {
			continue;
		}
		owner = hakios_spmi_channel_owner(&arb, n);
		printf("buttons: channel %u is %s (%#x); written by system %u%s\n", n, p->name, map, owner,
		    (map & ARB_CHANNEL_INTERRUPTS) ? ", which also gets its interrupts" : "");
		if (!p->found || owner == arb.system) {
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

/*
 * the power-on block: of the kind that is split in two ("generation 3"), its half for the operating system; or the
 * primary one of before (generations 1 and 2). Its subtype says which, and where the keys are in its status.
 */
#define PON_TYPE                0x01
#define PON_SUBTYPE_PRIMARY     0x01
#define PON_SUBTYPE_GEN2_PRIMARY 0x04
#define PON_SUBTYPE_GEN3_HLOS   0x09
#define PON_GEN3_KPDPWR         0x80            /* in the status and in the interrupt registers */
#define PON_GEN3_RESIN          0x40
#define PON_KPDPWR_BEFORE       0x01            /* generations 1 and 2 */
#define PON_RESIN_BEFORE        0x02
static uint8_t pon_kpdpwr = PON_GEN3_KPDPWR, pon_resin = PON_GEN3_RESIN;   /* (the block's, from step 3) */

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
#define LET_GO_LOOKS            (1000 / POLL_MS)        /* a second of looks without a read: its held keys let go */

static struct button {
	const char *    name;
	uint32_t        record;         /* HAKIOS_BUTTON_* */
	bool            usable;         /* its input is what it was taken for */
	bool            live;           /* it has been seen released */
	bool            last;           /* the look before this one */
	bool            down;           /* as reported */
	uint64_t        since;          /* when it went down */
	uint32_t        presses;
} power = { .name = "power", .record = HAKIOS_BUTTON_LOCK },
    volume_up = { .name = "volume up", .record = HAKIOS_BUTTON_VOLUME_UP },
    volume_down = { .name = "volume down", .record = HAKIOS_BUTTON_VOLUME_DOWN };

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
	hakios_touch_post(pressed ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP, b->record, 0, 0);
}

/*
 * A look at a peripheral whose read has failed: once its reads have failed for a second in a row, a key of it that
 * was reported held is let go. Nobody can see that key released while its peripheral does not answer, and the
 * driver gives up on the bus only when more than half of all its reads have failed: never while the other
 * peripheral still answers (volume up's GPIO silent, the power-on block read well), and after an hour of good
 * reads only after another hour of failed ones. Darwin kept the side or a volume button held all that time -- a
 * volume run to its end, a side button held into Siri. A key still held when the reads come back is pressed anew
 * (two looks in a row that say so, as any press: what it was is forgotten here).
 */
static void
button_unread(const struct peripheral *p, struct button *b)
{
	if (p->unread < LET_GO_LOOKS || !b->down) {
		return;
	}
	b->down = false;
	b->last = false;
	printf("buttons: %s let go: %s has not been read for %u looks in a row\n", b->name, p->name, p->unread);
	hakios_touch_post(HAKIOS_BUTTON_UP, b->record, 0, 0);
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
			pon.unread = 0;
			button_look(&power, status & pon_kpdpwr);
			button_look(&volume_down, status & pon_resin);
		} else {
			pon.unread += pon.unread < LET_GO_LOOKS;
			button_unread(&pon, &power);
			button_unread(&pon, &volume_down);
			if (said < 8) {
				said++;
				printf("buttons: reading %s: %s\n", pon.name, problem);
			}
		}
	}
	if (volume_up.usable) {
		problem = spmi_read(&pin, REG_RT_STATUS, &status, 1);
		if (problem == NULL) {
			pin.unread = 0;
			button_look(&volume_up, !(status & GPIO_LEVEL));
		} else {
			pin.unread += pin.unread < LET_GO_LOOKS;
			button_unread(&pin, &volume_up);
			if (said < 8) {
				said++;
				printf("buttons: reading %s: %s\n", pin.name, problem);
			}
		}
	}
}

/* Step 3: is each peripheral what the registers above are those of? */
static bool
identify(const struct peripheral *p, uint8_t type, uint8_t subtype_from, uint8_t subtype_to, uint8_t *subtype)
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
	if (subtype != NULL) {
		*subtype = is[1];
	}
	return true;
}

/* Step 3 for the power-on block: one of the kinds above, and where its keys are, by its subtype */
static bool
pon_identify(void)
{
	uint8_t subtype;

	if (!identify(&pon, PON_TYPE, PON_SUBTYPE_PRIMARY, PON_SUBTYPE_GEN3_HLOS, &subtype)) {
		return false;
	}
	if (subtype == PON_SUBTYPE_GEN3_HLOS) {
		pon_kpdpwr = PON_GEN3_KPDPWR, pon_resin = PON_GEN3_RESIN;
	} else if (subtype == PON_SUBTYPE_PRIMARY || subtype == PON_SUBTYPE_GEN2_PRIMARY) {
		pon_kpdpwr = PON_KPDPWR_BEFORE, pon_resin = PON_RESIN_BEFORE;
	} else {
		/*
		 * (a secondary block, or the half that is not the operating system's: no keys there. Subtype 3, a block of
		 * one register Linux's driver also reads, has not been seen on a phone with these PMICs: left alone too.)
		 */
		printf("buttons: ... a power-on block of subtype %#x, which has no keys this driver reads: left alone\n", subtype);
		return false;
	}
	printf("buttons: ... a power-on block of %s: power in bit %u of its status, volume down in bit %u\n",
	    subtype == PON_SUBTYPE_GEN3_HLOS ? "generation 3" : "before generation 3", (unsigned)__builtin_ctz(pon_kpdpwr),
	    (unsigned)__builtin_ctz(pon_resin));
	return true;
}

/* Step 4 for the power-on block: nothing to check but what it says now; its interrupt setup goes to the log. */
static void
pon_inputs(void)
{
	const char *problem;
	uint8_t r[8], more[2];

	printf("buttons: next: read the status and the interrupt setup of %s (registers %#x to %#x)\n", pon.name,
	    REGISTERS(&pon) + 0x10, REGISTERS(&pon) + 0x19);
	problem = spmi_read(&pon, REG_RT_STATUS, r, 8);
	if (problem == NULL) {
		problem = spmi_read(&pon, REG_INT_LATCHED, more, 2);
	}
	if (problem != NULL) {
		printf("buttons: %s: %s\n", pon.name, problem);
		return;
	}
	printf("buttons: power-on block: status %#x (power %s, volume down %s); interrupts: edge %#x, high %#x, low %#x, "
	    "enabled %#x, latched %#x, pending %#x\n", r[0], (r[0] & pon_kpdpwr) ? "HELD" : "released",
	    (r[0] & pon_resin) ? "HELD" : "released", r[REG_INT_SET_TYPE - REG_RT_STATUS], r[REG_INT_POLARITY_HIGH - REG_RT_STATUS],
	    r[REG_INT_POLARITY_LOW - REG_RT_STATUS], r[REG_INT_ENABLED - REG_RT_STATUS], more[0], more[1]);
	power.usable = volume_down.usable = true;
	power.last = r[0] & pon_kpdpwr;
	volume_down.last = r[0] & pon_resin;
}

/* Step 4 for the pin: it is volume up only if it is an input with a pull-up, and switched on. */
static void
pin_input(void)
{
	const char *problem;
	uint8_t c[8], status = 0;
	unsigned mode, pull;

	printf("buttons: next: read the setup and the level of %s (registers %#x to %#x, %#x)\n", pin.name,
	    REGISTERS(&pin) + 0x40, REGISTERS(&pin) + 0x47, REGISTERS(&pin) + 0x10);
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
	const struct hakios_board *board = hakios_board();
	uint32_t steps = (uint32_t)(uintptr_t)parameter;
	bool known, pon_known, pin_known;

	printf("buttons: step 1: map the PMIC arbiter (%#x, %#x, %#x) and read its version\n", board->spmi.core, board->spmi.cnfg,
	    board->spmi.observer);
	known = hakios_spmi_map(&arb);
	printf("buttons: PMIC arbiter version %#x, %u channels\n", arb.version, arb.channels);
	if (!known) {
		printf("buttons: this driver knows versions 5, 7 and 8 of the arbiter only: no buttons\n");
		goto out;
	}
	if (steps < 2) {
		goto out;
	}

	printf("buttons: step 2: look through the arbiter's table for the channels of the two peripherals\n");
	find_channel(&pon);
	find_channel(&pin);
	if (steps < 3) {
		goto out;
	}

	printf("buttons: step 3: the first transfers on the SPMI bus\n");
	pon_known = pon_identify();
	pin_known = identify(&pin, GPIO_TYPE, GPIO_SUBTYPE_LV, GPIO_SUBTYPE_MV_VIN3, NULL);
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
			/* a key that was held when the reads stopped is let go: nobody will see it released any more, and Darwin
			 * would keep the side or volume button held for the rest of the run */
			struct button *keys[] = { &power, &volume_down, &volume_up };
			for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
				if (keys[k]->down) {
					keys[k]->down = false;
					printf("buttons: %s let go: it was held when the reads stopped\n", keys[k]->name);
					hakios_touch_post(HAKIOS_BUTTON_UP, keys[k]->record, 0, 0);
				}
			}
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
hakios_buttons_init(void)
{
	const struct hakios_board *board = hakios_board();
	uint32_t enabled = 0, steps = 5;
	thread_t thread;

	/*
	 * On with the touch screen (the phone, somebody at it) unless buttons=0 says otherwise: a hand
	 * has pressed all three keys and each press and release was read as it was made.
	 * touchscreen=1, with no buttons= given: the keys too.
	 */
	if (!PE_parse_boot_argn("buttons", &enabled, sizeof(enabled))) {
		uint32_t touch_screen = 0;

		enabled = PE_parse_boot_argn("touchscreen", &touch_screen, sizeof(touch_screen)) && touch_screen != 0;
	}
	if (!enabled) {
		return;
	}
	if (!hakios_board_has(HAKIOS_HAS_BUTTONS_PMIC)) {
		printf("buttons: disabled: this board's board.dts lists no \"buttons-pmic\"\n");
		return;
	}
	if (board->spmi.core == 0 || board->buttons.pon == 0 || board->buttons.volume_up == 0) {
		printf("buttons: disabled: no spmi { } in the SoC file or no buttons { } in the board file\n");
		return;
	}
	/* buttons_thread still checks the arbiter's version and that each peripheral is of the kind taken for. */
	pon.id = board->buttons.pon;
	pin.id = board->buttons.volume_up;
	if (board->buttons.pon_name != NULL) {
		pon.name = board->buttons.pon_name;
	}
	if (board->buttons.volume_up_name != NULL) {
		pin.name = board->buttons.volume_up_name;
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
