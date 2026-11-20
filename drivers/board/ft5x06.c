/*
 * A touch screen on I2C: a FocalTech controller of the FT5x06 register family (FT5x06, FT5x16, FT5x46, FT6x06,
 * FT6x36 and their relatives; what they say and how sure this file is of it: board/ft5x06_report.h), as a source of
 * touches for /dev/hakios-touch (sys/board/input.h). Where the chip is, its address, its interrupt and reset, its
 * supplies, how many fingers it reports and the largest coordinates are the board's touch-i2c { } (board/board.h),
 * on a board that lists "touch-ft5x06".
 *
 * NOT RUN ON A PHONE: written from the family's register layout as remembered, and run against a model of the chip
 * only (workbench/touch_ft5x06_test.c). Begin with touchscreen=-1.
 *
 *   I2C        done by hand (board/i2c_gpio.h) on the board's two pins, taken for each transaction and given back as
 *              they were found: this kernel drives no serial engine as I2C
 *   interrupt  the chip's INT, low while it has a report: only looked at, 125 times a second
 *   power      up to two regulators asked of RPMh by name, and a pin that switches the analog supply, each only where
 *              the board names it: a supply that is always on is nobody's to switch
 *
 * Nothing is ever written into the chip: its working mode needs nothing set, and this file only reads -- the mode, the
 * identity, the firmware's version and the reports. NO FIRMWARE IS UPLOADED: the driver expects a controller that
 * already runs its firmware. The parts with their own flash (the FT5x06s, FT5x46s, FT6x36s) do after a reset. The
 * parts without (the display-and-touch chips, FT8006, FT8716, FT8719 and the like) are given theirs by Android's
 * driver every time they are powered, and are not this driver's: reset here they answer no identity, or one of their
 * boot ROM, and the driver says so and stops.
 *
 * Nothing here is done unless a boot-arg asks for it:
 *
 *   touchscreen=1   the chip powered, reset and identified, then its reports read and its touches posted
 *   touchscreen=-1  only looks: the pins as the firmware left them, and the identity read if the chip answers as it was
 *                   left (unpowered or held in reset it does not, which is said). Nothing powered, no pin configured
 *                   but the bus's two for the reads, given back as found; nothing written
 *   touch_flip=N    bit 0 mirrors the touches left to right, bit 1 top to bottom (as the other touch drivers)
 *
 * The bring-up is made in steps, each said in the log before it is made. Boot-arg ft5x06_steps=N stops after the
 * first N, and what was powered for them is powered off again:
 *   1  the pins as they were found (reads of the pin controller only)
 *   2  the chip held in reset and its supplies switched on (RPMh, the analog supply's pin)
 *   3  the reset let go, and the chip waited for
 *   4  its mode, identity and firmware version read (the first transfers on the bus)
 *   5  its reports read, from here on
 * At touchscreen=-1 steps 1 and 4 are made, and 4 with nothing switched on for it.
 *
 * What a report costs: the bus is driven by hand at 100 kHz at most with a core spun for it (i2c_gpio.h). The four
 * bytes of a look that finds no finger are some 36 bits, about 0.4 ms; a report with one finger down is read as its
 * status and two slots (the finger's, and one more in case it lifts: ft5x06_slots()), some 150 bits, about 1.6 ms.
 * Idle, the chip is looked at 12 times a second (0.5 % of a core); with a finger down or the interrupt line low 125
 * times: a fifth of a core with one finger, more with more (ten fingers: 61 bytes, 6 ms a report, so most of a
 * core). A serial engine as I2C would cost a hundredth of that, and is the first thing to write once this has been
 * seen to work.
 *
 * Fingers that fall silent: a finger is lifted once the chip has said nothing believable for a quarter of a second
 * (LIFT_US) -- reads that fail, reports that cannot be ones. After 125 looks in a row without one the chip is reset
 * and identified again, at most three times (forgotten after a minute without trouble); the fourth time the polling
 * ends, and the chip is powered off.
 *
 * When bring-up fails, when the polling gives up, when ft5x06_steps stops it, and at the run's end (and on a panic's
 * way out, hakios_ft5x06_quiesce()) the chip is powered off as far as this driver powered it: held in reset, the
 * votes for its analog and then its I/O supply withdrawn, the analog supply's pin and the interrupt's as they were
 * found. The bus's pins are as they were found after every transaction; a transaction the run's end cut off is ended
 * there, and a data line the chip then still holds is clocked free. The fingers still down are lifted when the thread
 * itself stops; at the run's end nothing is posted (the queue's lock cannot be taken there, and nobody reads it any
 * more).
 *
 * Timings (NOT SURE of any: remembered from data sheets that differ between parts, and taken long): the supplies are
 * given 10 ms before the reset is let go; the reset is held low 10 ms (the data sheets ask for 1 to 5); the chip is
 * first asked 200 ms after it (they say up to 300 ms until it reports), and then every 100 ms for a second more.
 * The order of the supplies, I/O before analog as board/fts.c has it, is not known to matter with the chip in reset.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/board/input.h>
#include <sys/systm.h>

#include "kernel.h"
#include "board.h"
#include "log.h"
#include "ft5x06_report.h"

static vm_offset_t tlmm, rsc, cmd_db;

/*
 * The thread and the run's end (hakios_ft5x06_quiesce()): the thread holds `busy' around everything it does with the
 * chip and begins nothing once `leaving' is set; `on_bus' is set from just before it takes the bus's pins until it has
 * given them back, so that the run's end knows a transaction was cut off in its middle.
 */
static bool leaving, busy, on_bus;
static bool started;                    /* a thread was started: there may be something to put away */

/* every line to the loader's log as well (and to the console when that may be written) */
HAKIOS_DEFINE_SAY(!__atomic_load_n(&leaving, __ATOMIC_SEQ_CST) && ml_get_interrupts_enabled())

#include "mmio.h"
#include "time.h"

#define wait_ms(ms)             hakios_wait_ms((ms), __atomic_load_n(&leaving, __ATOMIC_SEQ_CST))   /* (spun on the way out) */

/* On the way out every wait spins: interrupts are off and the other cores stopped (as in board/fts.c). */
static void __attribute__((unused))    /* (not where a test stands in for board/rpmh.h) */
ft5x06_wait_us(uint32_t us)
{
	if (__atomic_load_n(&leaving, __ATOMIC_SEQ_CST)) {
		spin_us(us);
	} else {
		sleep_us(us);
	}
}

#define POLL_MS                 8       /* the interrupt line is looked at 125 times a second */
#define IDLE_LOOKS              10      /* ... and the chip asked on every tenth look whatever the line says */
#define LIFT_US                 250000  /* nothing believable for so long: the fingers are lifted */
#define TROUBLE_LOOKS           125     /* so many looks in a row without a report: the chip is reset */
#define TROUBLE_RESETS          3       /* ... so many times, and the next time the polling ends */
#define TROUBLE_FORGOTTEN_US    (60 * 1000000ull)
#define SUPPLY_SETTLE_MS        10      /* (this file's head: Timings) */
#define RESET_LOW_MS            10
#define RESET_WAIT_MS           200
#define IDENTITY_TRIES          10
#define IDENTITY_GAP_MS         100
#define IO_MILLIVOLTS           1800    /* the I/O supply: the bus's level on these phones */

/* ---- pins (TLMM) ------------------------------------------------------------------------------------- */

#define TLMM_PHYS               (hakios_board()->pins.tlmm)     /* the SoC file's pins { } */
#define HAKIOS_TLMM             tlmm
#include "tlmm.h"                    /* a pin's two registers, and the four things done with them */

/* which pins, and where on the bus: the board's touch-i2c { } */
#define WIRED                   (&hakios_board()->touch_i2c)
#define PIN_SDA                 ((unsigned)WIRED->sda)
#define PIN_SCL                 ((unsigned)WIRED->scl)
#define PIN_INT                 ((unsigned)WIRED->interrupt)    /* low = the chip has a report */
#define PIN_RESET               ((unsigned)WIRED->reset)        /* low = in reset */
#define PIN_AVDD                ((unsigned)WIRED->analog_enable)        /* high = analog supply on (if the board has one) */
#define HAS_AVDD                (WIRED->analog_enable >= 0)
#define ADDRESS                 (WIRED->address)

/* a pin as it is, in words (as board/imu.c says its pins) */
static void
show_pin(unsigned pin, const char *what)
{
	uint32_t cfg = rd(GPIO_CFG(pin)), io = rd(GPIO_IO(pin));

	say("ft5x06: gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u\n", pin, what, (cfg >> 2) & 15, cfg & 3,
	    (((cfg >> 6) & 7) + 1) * 2, (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") : "input", io & 1);
}

/* step 1, at a look too: reads of the pin controller only */
static void
show_pins(void)
{
	say("ft5x06: step 1: the pins as they were found\n");
	show_pin(PIN_SDA, "I2C data");
	show_pin(PIN_SCL, "I2C clock");
	show_pin(PIN_INT, "interrupt");
	show_pin(PIN_RESET, "reset");
	if (HAS_AVDD) {
		show_pin(PIN_AVDD, "analog supply's switch");
	}
}

/* ---- the supplies (RPMh) ---------------------------------------------------------------------------------- */

#define CMD_DB_PHYS             (hakios_board()->rpmh.cmd_db)           /* the SoC file's rpmh { } */
#define CMD_DB_SIZE             (hakios_board()->rpmh.cmd_db_size)
#define RSC_PHYS                (hakios_board()->rpmh.rsc)      /* the application processor's "DRV 2" */
#define RSC_SIZE                0x10000UL
#define sleep_us ft5x06_wait_us
#include "rpmh.h"                    /* a slot set's registers, and one request through it */
#undef sleep_us

#define VRM_VOLTAGE             0               /* a regulator's registers, from its address: millivolts */
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7

#include "cmd_db.h"

/* The RPMh address of a regulator by its name in the command database the boot loader left; 0 = none. */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;

	if (db == NULL || rsc == 0) {
		say("ft5x06: RPMh or its command database is not mapped\n");
		return 0;
	}
	if (!hakios_cmd_db_valid(db)) {
		say("ft5x06: no command database (magic %02x %02x %02x %02x)\n", db->magic[0], db->magic[1], db->magic[2],
		    db->magic[3]);
		return 0;
	}
	return hakios_cmd_db_address(db, CMD_DB_SIZE, name);
}

/* (without the lock on the way out: the other cores are stopped, one of them perhaps holding it -- as board/fts.c) */
static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	bool locking = !__atomic_load_n(&leaving, __ATOMIC_SEQ_CST);
	const char *problem;

	if (locking) {
		hakios_rpmh_lock();
	}
	problem = hakios_rpmh_send_rsc(rsc, hakios_board()->rpmh.tcs_offset, address, data, count);
	if (locking) {
		hakios_rpmh_unlock();
	}
	if (problem != NULL) {
		say("ft5x06: RPMh: %s\n", problem);
	}
	return problem == NULL;
}

/* each supply's RPMh address once it has been asked for, whether the request went through or not; 0: not asked */
static uint32_t io_supply_at, analog_supply_at;

static bool
supply_on(const char *name, uint32_t millivolts, uint32_t *asked)
{
	uint32_t base = rpmh_address(name);
	uint32_t address[3] = { base + VRM_VOLTAGE, base + VRM_ENABLE, base + VRM_MODE };
	uint32_t data[3] = { millivolts, 1, VRM_MODE_LDO_HPM };

	if (base == 0 || (base >> 16 & 0xf) != 4) {
		say("ft5x06: no RPMh regulator %s (address 0x%x)\n", name, base);
		return false;
	}
	say("ft5x06: regulator %s at 0x%x: %u mV\n", name, base, millivolts);
	*asked = base;
	return rpmh_write(address, data, 3);
}

/* this driver's vote for a supply withdrawn, if it voted (only the enable: another may want the regulator on) */
static void
supply_off(const char *name, uint32_t *asked)
{
	uint32_t address = *asked + VRM_ENABLE, off = 0;

	if (*asked == 0) {
		return;
	}
	*asked = 0;
	say("ft5x06: regulator %s: vote withdrawn\n", name);
	(void)rpmh_write(&address, &off, 1);
}

/* ---- I2C by hand (as in board/imu.c) ------------------------------------------------------------------- */

#include "i2c_gpio.h"                /* the two lines, a bit, a byte, the bus taken and given back */

static struct i2c_gpio i2c_bus;          /* (its two pins: the board's, at the start) */
static const char not_at_rest[] = "the I2C bus is not at rest", not_acknowledged[] = "nobody acknowledged";
static const char *failed = not_acknowledged;   /* why the last transfer did not go through: one of the two */
static unsigned refusals_said;

/*
 * The bus's two pins as the run's first transaction found them, which is what every transaction and the run's end give
 * back. Not what each take finds: a take after one that was cut off half way would find them as plain GPIOs.
 */
static uint32_t bus_found_cfg[2], bus_found_io[2];
static bool bus_found;

static void
bus_record(void)
{
	if (__atomic_load_n(&bus_found, __ATOMIC_SEQ_CST)) {
		return;
	}
	bus_found_cfg[0] = rd(GPIO_CFG(i2c_bus.sda));
	bus_found_cfg[1] = rd(GPIO_CFG(i2c_bus.scl));
	bus_found_io[0] = rd(GPIO_IO(i2c_bus.sda));
	bus_found_io[1] = rd(GPIO_IO(i2c_bus.scl));
	__atomic_store_n(&bus_found, true, __ATOMIC_SEQ_CST);
}

static void
bus_give_back(void)
{
	for (unsigned i = 0; i < 2; i++) {
		i2c_bus.found_cfg[i] = bus_found_cfg[i];
		i2c_bus.found_io[i] = bus_found_io[i];
	}
	i2c_gpio_give_back(&i2c_bus);
	__atomic_store_n(&on_bus, false, __ATOMIC_SEQ_CST);
}

static bool
bus_take(void)
{
	bus_record();
	/* (on the bus before the pins are touched: a thread stopped inside the take leaves them to the run's end to give back) */
	__atomic_store_n(&on_bus, true, __ATOMIC_SEQ_CST);
	if (i2c_gpio_take(&i2c_bus)) {
		return true;
	}
	bus_give_back();
	if (refusals_said++ < 4) {
		say("ft5x06: the I2C bus is not at rest (gpio%u reads %u, gpio%u reads %u): not touched\n", PIN_SDA,
		    i2c_gpio_level(PIN_SDA), PIN_SCL, i2c_gpio_level(PIN_SCL));
	}
	failed = not_at_rest;
	return false;
}

/* the chip addressed for reading from `reg' on (it counts the register's number up across a read) */
static bool
read_from(uint8_t reg)
{
	i2c_gpio_start(&i2c_bus);
	if (!i2c_gpio_write(&i2c_bus, (uint8_t)(ADDRESS << 1)) || !i2c_gpio_write(&i2c_bus, reg)) {
		return false;
	}
	i2c_gpio_start(&i2c_bus);
	return i2c_gpio_write(&i2c_bus, (uint8_t)(ADDRESS << 1 | 1));
}

/* `count' registers from `reg' on */
static bool
regs_read(uint8_t reg, uint8_t *values, unsigned count)
{
	bool answered;

	if (!bus_take()) {
		return false;
	}
	answered = read_from(reg);
	for (unsigned i = 0; answered && i < count; i++) {
		values[i] = i2c_gpio_read(&i2c_bus, i + 1 < count);
	}
	i2c_gpio_stop(&i2c_bus);
	bus_give_back();
	if (!answered) {
		failed = not_acknowledged;
	}
	return answered;
}

static struct { bool down; uint32_t x, y; } contact[FT5X06_CONTACTS];
static unsigned contacts = FT5X06_CONTACTS;     /* how many fingers the board's chip reports */

static unsigned
fingers_down(void)
{
	unsigned down = 0;

	for (unsigned id = 0; id < FT5X06_CONTACTS; id++) {
		down += contact[id].down;
	}
	return down;
}

/*
 * A report in one transaction: the status byte, and then as many slots as it gives a reason to read (ft5x06_slots())
 * -- how many more bytes are wanted is said to the chip byte by byte, in the acknowledgement of each, so the status is
 * looked at before its own is given.
 */
static bool
report_read(uint8_t *status, uint8_t *points, unsigned *slots)
{
	bool answered;

	*slots = 0;
	if (!bus_take()) {
		return false;
	}
	answered = read_from(FT5X06_REG_STATUS);
	if (answered) {
		unsigned bytes;
		uint8_t byte = 0;

		for (int bit = 7; bit >= 0; bit--) {
			byte = (uint8_t)(byte << 1 | i2c_gpio_bit(&i2c_bus, true));
		}
		*status = byte;
		*slots = ft5x06_slots(byte, fingers_down(), contacts);
		bytes = *slots * FT5X06_SLOT_BYTES;
		i2c_gpio_bit(&i2c_bus, bytes == 0);     /* acknowledged if slots are to follow */
		for (unsigned i = 0; i < bytes; i++) {
			points[i] = i2c_gpio_read(&i2c_bus, i + 1 < bytes);
		}
	}
	i2c_gpio_stop(&i2c_bus);
	bus_give_back();
	if (!answered) {
		failed = not_acknowledged;
	}
	return answered;
}

/* ---- touches ------------------------------------------------------------------------------------------- */

static uint32_t flip, trace;
static uint32_t panel_x, panel_y;       /* the largest coordinates: the board's */
static struct { uint32_t looks, reports, bad, reads_failed, positions; } seen;

static void
report(const struct ft5x06_frame *frame)
{
	for (uint32_t id = 0; id < FT5X06_CONTACTS; id++) {
		uint32_t x, y;

		if (!(frame->active & (1u << id))) {
			if (contact[id].down) {
				hakios_touch_post(HAKIOS_TOUCH_UP, id, contact[id].x, contact[id].y);
				contact[id].down = false;
			}
			continue;
		}
		x = (uint32_t)frame->x[id] * 65535 / panel_x;
		y = (uint32_t)frame->y[id] * 65535 / panel_y;
		if (flip & 1) {
			x = 65535 - x;
		}
		if (flip & 2) {
			y = 65535 - y;
		}
		if (!contact[id].down) {
			if (trace && seen.positions < 40) {
				say("ft5x06: finger %u down at %u, %u (of %u x %u)\n", id, frame->x[id], frame->y[id], panel_x, panel_y);
			}
			hakios_touch_post(HAKIOS_TOUCH_DOWN, id, x, y);
		} else if (contact[id].x != x || contact[id].y != y) {
			hakios_touch_post(HAKIOS_TOUCH_MOVE, id, x, y);
		}
		contact[id].down = true;
		contact[id].x = x;
		contact[id].y = y;
		seen.positions++;
	}
}

/* every finger that is down, lifted */
static void
release(void)
{
	const struct ft5x06_frame nobody = { 0 };

	report(&nobody);
}

/* ---- the chip: its power, its reset, its identity ------------------------------------------------------- */

/*
 * The identities this file has a name for, by the high byte (0xa3). Fairly sure of 0x55, 0x54, 0x06 and 0x36; of the
 * others in the family (an FT5x16 is remembered as 0x0a, an FT5606 as 0x08, an FT5336 as 0x14) not sure enough to
 * name them. A chip without a name here is read all the same and said to be unnamed: nothing is written to it either
 * way. 0x00 and 0xff are what a chip that is not there, or not running, reads as.
 */
static const struct {
	uint8_t high;
	const char *name;
} families[] = {
	{ 0x55, "FT5x06" }, { 0x54, "FT5x46" }, { 0x06, "FT6x06" }, { 0x36, "FT6x36" },
};

static const char *
family_of(uint8_t high)
{
	for (unsigned i = 0; i < sizeof(families) / sizeof(families[0]); i++) {
		if (families[i].high == high) {
			return families[i].name;
		}
	}
	return NULL;
}

/* The pins bring-up configures, as it found them: what powering off configures them back to. */
#define KEPT_PINS               3
static unsigned pin_number[KEPT_PINS];
static uint32_t pin_found_cfg[KEPT_PINS], pin_found_io[KEPT_PINS];
static unsigned pins_kept;
static bool powered;                    /* bring-up has begun to change things: there is something to put back */

static void
pins_record(void)
{
	pins_kept = 0;
	pin_number[pins_kept++] = PIN_RESET;
	pin_number[pins_kept++] = PIN_INT;
	if (HAS_AVDD) {
		pin_number[pins_kept++] = PIN_AVDD;
	}
	for (unsigned i = 0; i < pins_kept; i++) {
		pin_found_cfg[i] = rd(GPIO_CFG(pin_number[i]));
		pin_found_io[i] = rd(GPIO_IO(pin_number[i]));
	}
	__atomic_store_n(&powered, true, __ATOMIC_SEQ_CST);
}

/*
 * The chip powered off, as far as this driver powered it (this file's head): bounded, and without sleeping on the way
 * out. The reset pin keeps its level low under the configuration it is given back.
 */
static void
power_off(void)
{
	if (!__atomic_load_n(&powered, __ATOMIC_SEQ_CST)) {
		return;                         /* (nothing was changed, or it has been put back already) */
	}
	gpio_output(PIN_RESET, false, PULL_NONE, 2);
	if (WIRED->analog_supply != NULL) {
		supply_off(WIRED->analog_supply, &analog_supply_at);
	}
	if (WIRED->io_supply != NULL) {
		supply_off(WIRED->io_supply, &io_supply_at);
	}
	for (unsigned i = 0; i < pins_kept; i++) {
		if (pin_number[i] != PIN_RESET) {
			wr(GPIO_IO(pin_number[i]), pin_found_io[i] & 2);        /* (the level first, as gpio_output()) */
		}
		wr(GPIO_CFG(pin_number[i]), pin_found_cfg[i]);
	}
	__atomic_store_n(&powered, false, __ATOMIC_SEQ_CST);
	say("ft5x06: powered off: in reset, what was switched on for it off again, the pins as found\n");
}

/* The reset held low and let go, and the chip given its time (this file's head: Timings). */
static void
reset_pulse(void)
{
	gpio_set(PIN_RESET, false);
	wait_ms(RESET_LOW_MS);
	gpio_set(PIN_RESET, true);
	wait_ms(RESET_WAIT_MS);
}

/*
 * The chip asked what it is, `tries' times a tenth of a second apart: its identity, its mode, its firmware's version.
 * True if a chip that works answered; said either way.
 */
static bool
identify(unsigned tries)
{
	uint8_t high = 0, low = 0, mode = 0xff, firmware = 0;
	const char *name;
	bool acknowledged = false, identity = false, answered = false;

	for (unsigned attempt = 0; attempt < tries && !answered && !__atomic_load_n(&leaving, __ATOMIC_SEQ_CST); attempt++) {
		if (attempt != 0) {
			wait_ms(IDENTITY_GAP_MS);
		}
		acknowledged = regs_read(FT5X06_REG_ID_HIGH, &high, 1);
		identity = acknowledged && high != 0x00 && high != 0xff;
		answered = identity && regs_read(FT5X06_REG_MODE, &mode, 1);
	}
	if (!answered) {
		if (identity) {
			say("ft5x06: the chip at 0x%02x says identity 0x%02x and then nothing more: %s\n", ADDRESS, high, failed);
		} else if (acknowledged) {
			say("ft5x06: the chip at 0x%02x reads identity 0x%02x: one that runs no firmware (none is uploaded here)\n", ADDRESS,
			    high);
		} else if (failed == not_at_rest) {
			say("ft5x06: nothing read at 0x%02x: %s\n", ADDRESS, failed);
		} else {
			say("ft5x06: nobody answers at 0x%02x: a chip that is not there, not powered or held in reset\n", ADDRESS);
		}
		return false;
	}
	/* (the low byte and the firmware's version are said, nothing depends on them: a part older than the FT5x46 may have no 0x9f) */
	(void)regs_read(FT5X06_REG_ID_LOW, &low, 1);
	(void)regs_read(FT5X06_REG_FIRMWARE, &firmware, 1);
	name = family_of(high);
	say("ft5x06: the chip at 0x%02x answers identity 0x%02x%02x, %s%s; firmware 0x%02x, mode 0x%02x\n", ADDRESS, high, low,
	    name != NULL ? "an " : "NOT ONE THIS DRIVER HAS A NAME FOR: read as the family's all the same", name != NULL ? name : "",
	    firmware, mode);
	if ((mode & FT5X06_MODE_MASK) != 0) {
		say("ft5x06: the chip is not in its working mode (0x%02x): its reports are not read\n", mode);
		return false;
	}
	return true;
}

static uint32_t steps = 5;              /* ft5x06_steps=N */

/* Steps 1 to 4 (this file's head): the chip ready to be read, not there (for a reason said), or the steps asked for made. */
enum bring_up { READY, NO_CHIP, STEPS_MADE };

static enum bring_up
bring_up(void)
{
	show_pins();
	if (steps < 2) {
		return STEPS_MADE;
	}
	say("ft5x06: step 2: the chip held in reset, its supplies switched on (I/O %s, analog %s%s)\n",
	    WIRED->io_supply != NULL ? WIRED->io_supply : "not ours to switch",
	    WIRED->analog_supply != NULL ? WIRED->analog_supply : HAS_AVDD ? "behind its pin" : "not ours to switch",
	    WIRED->analog_supply != NULL && HAS_AVDD ? " and its pin" : "");
	pins_record();
	gpio_output(PIN_RESET, false, PULL_NONE, 2);
	gpio_configure(PIN_INT, 0, PULL_UP, 2, false);
	wait_ms(1);
	if (WIRED->io_supply != NULL && !supply_on(WIRED->io_supply, IO_MILLIVOLTS, &io_supply_at)) {
		return NO_CHIP;
	}
	if (WIRED->analog_supply != NULL && !supply_on(WIRED->analog_supply, WIRED->analog_mv, &analog_supply_at)) {
		return NO_CHIP;
	}
	if (HAS_AVDD) {
		gpio_output(PIN_AVDD, true, PULL_NONE, 2);
	}
	wait_ms(SUPPLY_SETTLE_MS);
	if (steps < 3) {
		return STEPS_MADE;
	}
	say("ft5x06: step 3: the reset let go after %u ms, the chip given %u ms\n", RESET_LOW_MS, RESET_WAIT_MS);
	reset_pulse();
	if (steps < 4) {
		return STEPS_MADE;
	}
	say("ft5x06: step 4: the first transfers on the I2C bus (gpio%u, gpio%u): the chip's identity\n", PIN_SDA, PIN_SCL);
	if (!identify(IDENTITY_TRIES)) {
		return NO_CHIP;
	}
	return steps < 5 ? STEPS_MADE : READY;
}

/*
 * touchscreen=-1: steps 1 and 4 alone. The chip is asked as the firmware left it -- once: nothing was switched on that
 * it could be waited for -- and nothing but the bus's two pins is touched, those given back as found.
 */
static void
look(void)
{
	uint32_t reset_cfg = rd(GPIO_CFG(PIN_RESET)), reset_io = rd(GPIO_IO(PIN_RESET));

	say("ft5x06: touchscreen=-1: looking only\n");
	show_pins();
	if ((reset_cfg & CFG_OUTPUT) && !(reset_io & 2)) {
		say("ft5x06: the chip is held in reset as found (gpio%u drives low): it is not expected to answer\n", PIN_RESET);
	}
	say("ft5x06: step 4: the chip's identity, as it was left (steps 2 and 3, its power and its reset, are not made)\n");
	(void)identify(1);
	say("ft5x06: touchscreen=-1: nothing powered, configured or written\n");
}

/* ---- the thread ------------------------------------------------------------------------------------------- */

static bool looking_only;              /* touchscreen=-1 */

/* The chip is the thread's from here until it calls done(); false once the run's end has it. */
static bool
begin(void)
{
	__atomic_store_n(&busy, true, __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&leaving, __ATOMIC_SEQ_CST)) {
		__atomic_store_n(&busy, false, __ATOMIC_SEQ_CST);
		return false;
	}
	return true;
}

static void
done(void)
{
	__atomic_store_n(&busy, false, __ATOMIC_SEQ_CST);
}

/* looks in a row without a report, resets made for that, and when the chip last said something believable and last did not */
static struct { unsigned failures, resets; uint64_t last_good_us, last_failure_us; } trouble;

static void
heard(uint64_t now)
{
	trouble.failures = 0;
	trouble.last_good_us = now;
	if (trouble.resets != 0 && now - trouble.last_failure_us >= TROUBLE_FORGOTTEN_US) {
		trouble.resets = 0;
	}
}

/* One look at the chip; false: it is given up. */
static bool
poll_once(unsigned tick)
{
	uint8_t status = 0, points[FT5X06_CONTACTS * FT5X06_SLOT_BYTES];
	struct ft5x06_frame frame;
	unsigned slots = 0;
	/* (the line may only pulse for a report, and a board may have it wrong: asked now and then whatever it says) */
	bool pending = !gpio_get(PIN_INT), down = fingers_down() != 0, got;
	uint64_t now;

	if (!pending && !down && tick % IDLE_LOOKS != 0) {
		return true;
	}
	seen.looks++;
	got = report_read(&status, points, &slots);
	now = now_us();
	if (got && ft5x06_decode(status, points, slots, contacts, panel_x, panel_y, &frame)) {
		heard(now);
		seen.reports++;
		report(&frame);
	} else {
		seen.bad++;
		seen.reads_failed += !got;
		trouble.last_failure_us = now;
		if (seen.bad <= 4) {
			if (got) {
				say("ft5x06: a report that cannot be one: status 0x%02x, %u slots, the first %02x %02x %02x %02x\n", status,
				    slots, slots ? points[0] : 0, slots ? points[1] : 0, slots ? points[2] : 0, slots ? points[3] : 0);
			} else {
				say("ft5x06: a report not read: %s\n", failed);
			}
		}
		if (down && now - trouble.last_good_us >= LIFT_US) {
			say("ft5x06: nothing believable for %u ms with a finger down: lifted\n",
			    (unsigned)((now - trouble.last_good_us) / 1000));
			release();
		}
		if (++trouble.failures >= TROUBLE_LOOKS) {
			release();
			say("ft5x06: %u looks in a row without a report (%u reads failed in all); resets made for that before: %u of %u\n",
			    trouble.failures, seen.reads_failed, trouble.resets, TROUBLE_RESETS);
			if (trouble.resets++ >= TROUBLE_RESETS) {
				return false;
			}
			reset_pulse();
			if (!identify(IDENTITY_TRIES)) {
				return false;
			}
			trouble.failures = 0;
		}
	}
	if (tick % (60 * 1000 / POLL_MS) == 0) {
		say("ft5x06: so far %u reports in %u looks (%u without one, %u reads failed), %u finger positions; the interrupt "
		    "line reads %s\n", seen.reports, seen.looks, seen.bad, seen.reads_failed, seen.positions,
		    gpio_get(PIN_INT) ? "high" : "low");
	}
	return true;
}

static void
ft5x06_thread(__unused void *parameter, __unused wait_result_t result)
{
	bool supplied = WIRED->io_supply != NULL || WIRED->analog_supply != NULL;
	enum bring_up made;

	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	if (tlmm == 0) {
		say("ft5x06: the pins could not be mapped\n");
		thread_terminate(current_thread());
		return;
	}
	if (looking_only) {
		if (begin()) {
			look();
			done();
		}
		thread_terminate(current_thread());
		return;
	}
	if (supplied) {
		rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
		cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
	}
	if (!begin()) {
		thread_terminate(current_thread());
		return;
	}
	if (supplied && (rsc == 0 || cmd_db == 0)) {
		say("ft5x06: RPMh or its command database could not be mapped\n");
		made = NO_CHIP;
	} else {
		made = bring_up();
	}
	if (made != READY) {
		if (made == STEPS_MADE) {
			say("ft5x06: stopping here (ft5x06_steps=%u)\n", steps);
		} else {
			say("ft5x06: touch screen unavailable\n");
		}
		power_off();
		done();
		thread_terminate(current_thread());
		return;
	}
	done();
	say("ft5x06: step 5: touch screen ready, %u x %u, %u fingers; the interrupt line (gpio%u) is looked at every %u ms\n", panel_x,
	    panel_y, contacts, PIN_INT, POLL_MS);
	trouble.last_good_us = now_us();
	for (unsigned tick = 1; begin(); tick++) {
		if (!poll_once(tick)) {
			release();
			say("ft5x06: stopped: the chip does not come back\n");
			power_off();
			done();
			break;
		}
		done();
		wait_ms(POLL_MS);
	}
	/* (or the run's end has the chip: hakios_ft5x06_quiesce() powers it off) */
	thread_terminate(current_thread());
}

void
hakios_ft5x06_init(void)
{
	const struct hakios_board *b = hakios_board();
	int32_t enabled = 0;
	thread_t thread;

	/* boot-arg touchscreen=1: the chip started, its touches posted to /dev/hakios-touch; touchscreen=-1: only looked at */
	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	looking_only = enabled == -1;
	/* (called only on a board that lists "touch-ft5x06": bsd/dev/board/drivers.c) */
	if (b == NULL || b->pins.tlmm == 0) {
		say("ft5x06: disabled: the SoC file does not say where the pins are (pins { })\n");
		return;
	}
	if (WIRED->sda == WIRED->scl || WIRED->address == 0 || WIRED->interrupt == WIRED->reset) {
		say("ft5x06: disabled: the board file does not say how the controller is wired (touch-i2c { })\n");
		return;
	}
	if ((WIRED->io_supply != NULL || WIRED->analog_supply != NULL) && (b->rpmh.cmd_db == 0 || b->rpmh.rsc == 0)) {
		say("ft5x06: disabled: touch-i2c { } names a supply, and the SoC file does not say where RPMh is (rpmh { })\n");
		return;
	}
	if (WIRED->analog_supply != NULL && WIRED->analog_mv == 0) {
		say("ft5x06: disabled: touch-i2c { } names an analog supply without its voltage (analog-millivolts)\n");
		return;
	}
	/* (a report carries 12 bits of a coordinate and a chip of the family at most ten fingers: more is a board file that is wrong) */
	contacts = WIRED->contacts;
	panel_x = WIRED->max_x;
	panel_y = WIRED->max_y;
	if (contacts == 0 || contacts > FT5X06_CONTACTS || panel_x == 0 || panel_y == 0 || panel_x > FT5X06_COORDINATE_MAX ||
	    panel_y > FT5X06_COORDINATE_MAX) {
		say("ft5x06: disabled: touch-i2c { } contacts %u and coordinates %u x %u: 1 to %u fingers, and coordinates of 1 to %u, "
		    "are what a report can say\n", contacts, panel_x, panel_y, FT5X06_CONTACTS, FT5X06_COORDINATE_MAX);
		return;
	}
	i2c_bus.sda = PIN_SDA;
	i2c_bus.scl = PIN_SCL;
	/*
	 * touch_flip=N: bit 0 mirrors the touches left to right, bit 1 top to bottom.
	 * ft5x06_trace=1: the first 40 touches' positions as the controller gives them, in the log.
	 * ft5x06_steps=N: the bring-up stops after its first N steps (this file's head), and powers off what it powered.
	 */
	PE_parse_boot_argn("touch_flip", &flip, sizeof(flip));
	PE_parse_boot_argn("ft5x06_trace", &trace, sizeof(trace));
	PE_parse_boot_argn("ft5x06_steps", &steps, sizeof(steps));
	if (steps == 0) {
		say("ft5x06: ft5x06_steps=0: nothing is done\n");
		return;
	}
	__atomic_store_n(&started, true, __ATOMIC_SEQ_CST);
	if (kernel_thread_start(ft5x06_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		__atomic_store_n(&started, false, __ATOMIC_SEQ_CST);
		say("ft5x06: cannot start the touch screen's thread\n");
	}
}

/*
 * At the run's end and on a panic's way out (osfmk/arm64/loader_return.c): no more polling, a transaction cut off in
 * its middle ended and the bus's pins given back, and the chip powered off (this file's head). Spins, with interrupts
 * off, a bounded time.
 */
void
hakios_ft5x06_quiesce(void)
{
	unsigned clocks = 0;

	if (!__atomic_load_n(&started, __ATOMIC_SEQ_CST)) {
		return;
	}
	__atomic_store_n(&leaving, true, __ATOMIC_SEQ_CST);
	for (unsigned i = 0; i < 100 && __atomic_load_n(&busy, __ATOMIC_SEQ_CST); i++) {
		spin_us(1000);                  /* (the thread finishing a transaction: 100 ms at most, a stopped core's never ends) */
	}
	if (tlmm == 0) {
		return;                         /* (the thread never came as far as the pins) */
	}
	if (__atomic_load_n(&on_bus, __ATOMIC_SEQ_CST)) {
		/* the thread stopped in the middle of a transaction, or of taking the pins: ended, the pins as the run found them */
		i2c_gpio_stop(&i2c_bus);
		bus_give_back();
		if (!i2c_gpio_level(PIN_SDA) || !i2c_gpio_level(PIN_SCL)) {
			/* a chip cut off in a byte it was sending holds the data line down until it has sent it: clocked free */
			bool came_back = i2c_gpio_unwedge(&i2c_bus, &clocks);

			bus_give_back();
			if (came_back) {
				say("ft5x06: at the run's end: a transfer cut off: the bus came back after %u clocks\n", clocks);
			} else {
				say("ft5x06: at the run's end: a transfer cut off: THE I2C BUS IS NOT AT REST (gpio%u reads %u, gpio%u reads %u)\n",
				    PIN_SDA, i2c_gpio_level(PIN_SDA), PIN_SCL, i2c_gpio_level(PIN_SCL));
			}
		} else {
			say("ft5x06: at the run's end: a transfer cut off: ended, the bus at rest\n");
		}
	}
	if (__atomic_load_n(&powered, __ATOMIC_SEQ_CST)) {
		say("ft5x06: at the run's end: %u reports in %u looks\n", seen.reports, seen.looks);
	}
	power_off();
}
