/*
 * The driver of a FocalTech FT5x06-family touch controller on I2C (drivers/board/ft5x06.c), run as it is against a
 * model of the SoC's pins with the chip on two of them: an I2C device (i2c_device_model.h) with the chip's mode,
 * identity and firmware version, and a report it puts out whole when its status register is read; it answers only
 * while it has its supplies and its reset is let go, and not for a while after either; RPMh and its command database
 * are stood in for, each request noted. The model is the family's registers as ft5x06_report.h has them, as sure and
 * as unsure as that file: it holds the driver to what the driver was written for, not to a chip.
 *
 * What is tested: nothing without the boot argument or the board's wiring; a look (touchscreen=-1) reads the identity
 * and powers, configures and writes nothing; the bring-up's steps in their order, the supplies and the reset as the
 * file's head says, and stopped where ft5x06_steps says; a chip without firmware, in its factory mode, of an unnamed
 * identity, or whose supply RPMh refuses; one finger and several with their exact positions, numbers and the events
 * down, move and up, mirrored where touch_flip says; a second phone with the part wired otherwise; reports that cannot
 * be ones; a finger that falls silent lifted after a quarter of a second and not by one bad read; a chip that stops
 * answering, and one that a reset brings back; a bus that is not at rest; and the run's end (hakios_ft5x06_quiesce())
 * -- no more reads, nothing posted, the chip powered off and the pins as found, also when the thread was stopped in
 * the middle of a transfer.
 *
 *   touch_ft5x06_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <unistd.h>

#include "fakekernel.h"

/* which pins the chip is on is the board's: the model is put where the board the driver reads says */
static unsigned sda_pin = 42, scl_pin = 43, int_pin = 59, reset_pin = 58;
static int avdd_pin = -1;
static unsigned chip_address = 0x38;
#define I2C_DEVICE_SDA          sda_pin
#define I2C_DEVICE_SCL          scl_pin
#define I2C_DEVICE_ADDRESS      chip_address
#include "i2c_device_model.h"

extern char log_text[];

static uint64_t real_ns(void) { return mach_absolute_time(); }

/* ---- what the driver posts ----------------------------------------------------------------------------------------- */

static struct post { uint32_t type, finger, x, y; uint64_t t; } posted[512];
static volatile unsigned posts;
static pthread_mutex_t post_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool in_quiesce;
static volatile unsigned posts_in_quiesce;

void
hakios_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	pthread_mutex_lock(&post_lock);
	if (posts < 512) {
		posted[posts] = (struct post){ type, finger, x, y, real_ns() };
	}
	posts++;
	posts_in_quiesce += in_quiesce;
	pthread_mutex_unlock(&post_lock);
}

/* ---- RPMh and its command database, in place of board/cmd_db.h and board/rpmh.h ------------------------------------- */

static volatile unsigned locks, locks_in_quiesce;
void hakios_rpmh_lock(void) { locks++; locks_in_quiesce += in_quiesce; }
void hakios_rpmh_unlock(void) { }

#define HAKIOS_CMD_DB_H
struct cmd_db_header { uint8_t magic[4]; };
static bool hakios_cmd_db_valid(const struct cmd_db_header *db __unused) { return true; }
/* a regulator's address by its name (the test phone's two); RPMh's regulators are at 0x4xxxx */
static uint32_t
supply_base(const char *name)
{
	return strcmp(name, "ldoa8") == 0 ? 0x40800 : strcmp(name, "ldoa17") == 0 ? 0x41100 : 0x30f00;
}
static uint32_t hakios_cmd_db_address(const struct cmd_db_header *db __unused, uint32_t s __unused, const char *n) { return supply_base(n); }

#define IO_SUPPLY       0x40800
#define ANALOG_SUPPLY   0x41100
static struct { uint32_t address, value; } asked[64];   /* every write asked of RPMh, in order */
static unsigned asks;
static uint32_t refused_supply;                 /* a supply whose switching on RPMh does not answer */
static bool io_on, analog_on;                   /* the two supplies, as RPMh has them */
static void chip_state(void);

#define HAKIOS_RPMH_H
static const char *
hakios_rpmh_send_rsc(vm_offset_t r __unused, uint32_t o __unused, const uint32_t *a, const uint32_t *d, unsigned c)
{
	for (unsigned i = 0; i < c; i++) {
		if (asks < 64) {
			asked[asks].address = a[i];
			asked[asks].value = d[i];
		}
		asks++;
	}
	if (refused_supply != 0 && c == 3 && a[0] == refused_supply) {
		return "no answer to the request";
	}
	for (unsigned i = 0; i < c; i++) {
		if (a[i] == IO_SUPPLY + 4) {
			io_on = d[i] != 0;
		} else if (a[i] == ANALOG_SUPPLY + 4) {
			analog_on = d[i] != 0;
		}
	}
	chip_state();
	return NULL;
}

/* the first (or, from the end, the last) time that register was asked to hold that value; -1: never */
static int
ask_of(uint32_t address, uint32_t value, bool last)
{
	int found = -1;

	for (unsigned i = 0; i < asks && i < 64; i++) {
		if (asked[i].address == address && asked[i].value == value && (last || found < 0)) {
			found = (int)i;
		}
	}
	return found;
}

/* ---- the chip -------------------------------------------------------------------------------------------------------- */

#define REG_MODE        0x00
#define REG_STATUS      0x02
#define REG_ID_LOW      0x9f
#define REG_ID_HIGH     0xa3
#define REG_FIRMWARE    0xa6
#define BOOT_NS         (20 * 1000000ull)       /* how long the chip takes after its reset or its power (the driver's waits
                                                 * are five times shorter here: this is 100 ms of its 200) */

static unsigned chip_contacts = 10;             /* how many slots this chip has; after them come other registers (zeros) */
static bool was_powered, was_released;
static bool running;                            /* it has its supplies and its reset is let go */
static uint64_t ready_at, reset_low_at, shortest_reset_ns = ~0ull;
static unsigned boots;                          /* times it started: its power came, or its reset was let go */
static unsigned starts_seen, early_starts, starts_while_off;    /* transfers begun: before it was ready, while it was off */
static volatile bool silent;                    /* it does not acknowledge its address */
static bool hung;                               /* ... until it is reset */
static unsigned hang_after;                     /* it hangs after so many reports read since it started (0: never) */
static unsigned reports_since_boot;
static volatile unsigned reports_read;          /* reads of its status register */
static bool int_stuck_high, int_stuck_low;

/* the report it puts out next, set by the scenario, and taken whole when the status register is read */
static uint8_t staged[1 + 6 * 10];
static pthread_mutex_t stage_lock = PTHREAD_MUTEX_INITIALIZER;

static bool
pin_high(unsigned pin)
{
	if (model.cfg[pin] & 1u << 9) {
		return (model.io[pin] & 2) != 0;
	}
	return (model.cfg[pin] & 3) == 3;       /* (let go: where its pull takes it) */
}

static void
chip_state(void)
{
	bool power = io_on && analog_on && (avdd_pin < 0 || pin_high((unsigned)avdd_pin)), released = pin_high(reset_pin);
	uint64_t now = real_ns();

	if (was_released && !released) {
		reset_low_at = now;
	}
	if (power && released && !(was_powered && was_released)) {
		if (was_powered && now - reset_low_at < shortest_reset_ns) {
			shortest_reset_ns = now - reset_low_at;
		}
		boots++;
		hung = false;
		reports_since_boot = 0;
		ready_at = now + BOOT_NS;
	}
	was_powered = power;
	was_released = released;
	running = power && released;
}

static void
wires_changed(void)
{
	unsigned before = device.starts;

	chip_state();
	device.absent = !running || real_ns() < ready_at || silent || hung;
	changed();
	if (device.starts != before) {
		starts_seen++;
		starts_while_off += !running;
		early_starts += running && real_ns() < ready_at;
	}
}

static uint8_t
chip_read(uint8_t reg)
{
	if (reg == REG_STATUS) {
		pthread_mutex_lock(&stage_lock);
		memcpy(&device.reg[REG_STATUS], staged, 1 + 6 * chip_contacts);
		pthread_mutex_unlock(&stage_lock);
		reports_read++;
		if (hang_after != 0 && ++reports_since_boot >= hang_after) {
			hung = true;            /* (from the next transfer on) */
		}
	}
	return device.reg[reg];
}

struct slot { unsigned id, event, x, y; };
#define LANDED  0
#define LIFTED  1
#define STAYS   2

/* what the chip says from its next report on: the count of its status register, and `n' slots; the others nobody's */
static void
chip_says(unsigned count, const struct slot *slots, unsigned n)
{
	pthread_mutex_lock(&stage_lock);
	memset(staged, 0xff, sizeof(staged));
	staged[0] = (uint8_t)count;
	for (unsigned i = 0; i < n; i++) {
		uint8_t *s = &staged[1 + 6 * i];

		s[0] = (uint8_t)(slots[i].event << 6 | slots[i].x >> 8);
		s[1] = (uint8_t)slots[i].x;
		s[2] = (uint8_t)(slots[i].id << 4 | slots[i].y >> 8);
		s[3] = (uint8_t)slots[i].y;
		s[4] = 0x20;
		s[5] = 0x10;
	}
	pthread_mutex_unlock(&stage_lock);
	/* its interrupt line: low while it has a finger to report */
	model.input[int_pin] = int_stuck_high || (!int_stuck_low && count == 0);
}

static void
no_finger(void)
{
	chip_says(0, NULL, 0);
}

static void
one_finger(unsigned id, unsigned event, unsigned x, unsigned y)
{
	const struct slot s = { id, event, x, y };

	chip_says(1, &s, 1);
}

/* ---- the registers: the pins' model, with the chip on them ----------------------------------------------------------- */

static unsigned pin_writes[PINS];

uint32_t
hakios_test_rd(vm_offset_t address)
{
	i2c_device_looked_at(address);
	return tlmm_model_rd(address);
}

/*
 * The driver's thread stopped where a scenario says, as the run's end stops the other cores: after each of its
 * writes to the pins `stop_after' is asked, and once it says so the thread does nothing more.
 */
static pthread_t scenario_thread;
static bool (*volatile stop_after)(vm_offset_t address);
static volatile bool stopped;

void
hakios_test_wr(vm_offset_t address, uint32_t value)
{
	uint32_t pin = (uint32_t)(address - model.base) / 0x1000;

	if (address >= model.base && pin < PINS) {
		pin_writes[pin]++;
	}
	tlmm_model_wr(address, value);
	if (stop_after != NULL && !pthread_equal(pthread_self(), scenario_thread) && stop_after(address)) {
		stopped = true;
		for (;;) {
			usleep(100000);
		}
	}
}

static unsigned mappings;
vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { mappings++; return phys; }
vm_offset_t ml_io_map_wcomb(vm_offset_t phys, vm_size_t size __unused) { mappings++; return phys; }

void hakios_ft5x06_init(void);
void hakios_ft5x06_quiesce(void) __attribute__((weak));        /* (a driver without one is shown, not a build that fails) */

#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#include "board/board.c"
#include "board/ft5x06.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited_ = 0; !(cond) && waited_ < (ms); waited_ += 2) usleep(2000); } while (0)

/* ---- a phone, its pins as the firmware left them ------------------------------------------------------------------ */

#define BUS_AS_FOUND    (1u << 2 | 3u)          /* the bus's pins: the serial engine's function, pulled up */
#define INT_AS_FOUND    (1u)                    /* the interrupt's: an input, pulled down */
#define RESET_AS_FOUND  (1u << 9 | 2u << 6)     /* the reset's: an output of 6 mA, driving low (the chip in reset) */
#define AVDD_AS_FOUND   (1u)                    /* the analog supply's switch: an input, pulled down (off) */

static uint32_t cfg_found[PINS], io_found[PINS];

/* the phone as the firmware leaves it: the chip unpowered and in reset, or (`left_running') powered and let go long ago */
static void
phone(const char *name, bool left_running)
{
	const struct hakios_board *b;

	board = name;
	fake_delay_divisor = 5;
	b = hakios_board();
	tlmm_model_at(b->pins.tlmm);
	if (b->touch_i2c.sda != b->touch_i2c.scl) {
		sda_pin = b->touch_i2c.sda;
		scl_pin = b->touch_i2c.scl;
		int_pin = b->touch_i2c.interrupt;
		reset_pin = b->touch_i2c.reset;
		avdd_pin = b->touch_i2c.analog_enable;
		chip_address = b->touch_i2c.address;
		chip_contacts = b->touch_i2c.contacts;
	}
	model.cfg[sda_pin] = model.cfg[scl_pin] = BUS_AS_FOUND;
	model.cfg[int_pin] = INT_AS_FOUND;
	model.cfg[reset_pin] = RESET_AS_FOUND;
	model.io[reset_pin] = left_running ? 2 : 0;
	model.wanted[reset_pin] = left_running;
	if (avdd_pin >= 0) {
		model.cfg[avdd_pin] = left_running ? (1u << 9) : AVDD_AS_FOUND;
		model.io[avdd_pin] = left_running ? 2 : 0;
		model.wanted[avdd_pin] = left_running;
	}
	/* (a board that names no regulator has them always on) */
	io_on = left_running || b->touch_i2c.io_supply == NULL;
	analog_on = left_running || b->touch_i2c.analog_supply == NULL;
	memcpy(cfg_found, model.cfg, sizeof(cfg_found));
	memcpy(io_found, model.io, sizeof(io_found));
	memset(device.reg, 0, sizeof(device.reg));
	device.reg[REG_MODE] = 0x00;
	device.reg[REG_ID_HIGH] = 0x54;
	device.reg[REG_ID_LOW] = 0x22;
	device.reg[REG_FIRMWARE] = 0x1e;
	device.read = chip_read;
	no_finger();
	chip_state();
	ready_at = 0;                           /* (as found it started long ago, if it runs) */
	boots = 0;
	model.after_write = wires_changed;
	device.absent = !running;
	lines();
	device.was_sda = device.was_scl = true;
}

/* every pin as the firmware left it; the reset's (`in_reset') with its level low under the configuration it had */
static void
pins_as_found(const char *when, bool in_reset)
{
	CHECK(device.driven_high == 0, "%s: a line of the bus was driven high %u times", when, device.driven_high);
	CHECK(model.elsewhere == 0, "%s: %u accesses outside the pins' registers", when, model.elsewhere);
	CHECK(model.drove_wrong == 0, "%s: a pin was made an output before its level was said, %u times", when, model.drove_wrong);
	for (unsigned i = 0; i < PINS; i++) {
		uint32_t io = i == reset_pin && in_reset ? 0 : io_found[i];

		if (model.cfg[i] != cfg_found[i] || (model.io[i] & 2) != (io & 2)) {
			CHECK(false, "%s: gpio%u is %#x/%#x, was found %#x/%#x", when, i, model.cfg[i], model.io[i], cfg_found[i], io_found[i]);
		}
	}
}

/* nothing but the bus's two pins was written to */
static void
only_the_bus_touched(const char *when)
{
	for (unsigned i = 0; i < PINS; i++) {
		CHECK(pin_writes[i] == 0 || i == sda_pin || i == scl_pin, "%s: gpio%u was written %u times", when, i, pin_writes[i]);
	}
}

/* Powered off: in reset, the analog supply's vote withdrawn and then the I/O supply's, without RPMh's lock if at the run's end */
static void
powered_off(const char *when)
{
	int io_off = ask_of(IO_SUPPLY + 4, 0, true), io_was_on = ask_of(IO_SUPPLY + 4, 1, true);
	int analog_off = ask_of(ANALOG_SUPPLY + 4, 0, true), analog_was_on = ask_of(ANALOG_SUPPLY + 4, 1, true);

	if (hakios_board()->touch_i2c.io_supply != NULL) {
		CHECK(io_off > io_was_on && (analog_was_on < 0 || analog_off > analog_was_on), "%s: the supplies' votes withdrawn "
		    "(I/O on %d off %d, analog on %d off %d)", when, io_was_on, io_off, analog_was_on, analog_off);
		CHECK(analog_was_on < 0 || analog_off < io_off, "%s: the analog supply's first (%d, %d)", when, analog_off, io_off);
		CHECK(!io_on && !analog_on, "%s: a supply is still on", when);
	} else {
		CHECK(asks == 0, "%s: RPMh was asked %u times on a board that names no regulator", when, asks);
	}
	CHECK(!pin_high(reset_pin), "%s: the chip is not held in reset", when);
	CHECK(logged("ft5x06: powered off"), "%s: said: %s", when, log_text);
	pins_as_found(when, true);
}

static void
start(const char *arguments)
{
	boot_args = arguments;
	hakios_ft5x06_init();
}

/* touchscreen=1 on the phone with the chip on I2C, until its reports are read */
static void
ready_on(const char *name, const char *arguments)
{
	phone(name, false);
	start(arguments);
	WAIT_FOR(logged("touch screen ready") || logged("touch screen unavailable"), 10000);
	CHECK(logged("touch screen ready"), "the chip brought up: %s", log_text);
}

static void
ready(void)
{
	ready_on("phone-i2c", "touchscreen=1");
}

/* until the driver has read the chip's report as it is now, twice */
static void
taken(void)
{
	unsigned before = reports_read;

	WAIT_FOR(reports_read >= before + 2, 3000);
	CHECK(reports_read >= before + 2, "the driver goes on reading reports: %u since: %s", reports_read - before, log_text);
	usleep(2000);                           /* (and has made of the last what it makes of it) */
}

static unsigned
scaled(unsigned raw, unsigned most)
{
	return raw * 65535 / most;
}

/* the post numbered n (from 0) is that one */
static void
post_is(unsigned n, uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	struct post p = { 0 };
	unsigned count;

	pthread_mutex_lock(&post_lock);
	count = posts;
	if (n < count && n < 512) {
		p = posted[n];
	}
	pthread_mutex_unlock(&post_lock);
	CHECK(n < count, "post %u (type %u finger %u at %u, %u) was not made: %u posts", n, type, finger, x, y, count);
	if (n < count) {
		CHECK(p.type == type && p.finger == finger && p.x == x && p.y == y, "post %u is type %u finger %u at %u, %u, not type %u "
		    "finger %u at %u, %u", n, p.type, p.finger, p.x, p.y, type, finger, x, y);
	}
}

/* the first post of that type and finger at or after `from'; -1: none */
static int
post_of(uint32_t type, uint32_t finger, unsigned from)
{
	int found = -1;

	pthread_mutex_lock(&post_lock);
	for (unsigned i = from; i < posts && i < 512 && found < 0; i++) {
		if (posted[i].type == type && posted[i].finger == finger) {
			found = (int)i;
		}
	}
	pthread_mutex_unlock(&post_lock);
	return found;
}

static void
quiesce(void)
{
	void (*volatile put_away)(void) = hakios_ft5x06_quiesce;

	CHECK(put_away != NULL, "the driver has nothing for the run's end (no hakios_ft5x06_quiesce())");
	if (put_away != NULL) {
		in_quiesce = true;
		put_away();
		in_quiesce = false;
	}
}

/* ---- scenarios: a report into fingers (ft5x06_report.h) -------------------------------------------------------------- */

static void
report_slots(void)
{
	CHECK(ft5x06_slots(0x00, 0, 10) == 0, "no finger, none before: the status alone");
	CHECK(ft5x06_slots(0x01, 0, 10) == 1, "one landed");
	CHECK(ft5x06_slots(0x01, 1, 10) == 2, "one stays: its slot and one more, should it be lifting");
	CHECK(ft5x06_slots(0x00, 3, 10) == 3, "none any more: the slots of the three that were");
	CHECK(ft5x06_slots(0x08, 8, 10) == 10 && ft5x06_slots(0x05, 5, 5) == 5, "never more than the chip has");
	CHECK(ft5x06_slots(0x0f, 0, 10) == 0 && ft5x06_slots(0x06, 0, 5) == 0, "a count no such chip says: nothing more read");
	CHECK(ft5x06_slots(0x31, 0, 10) == 1, "the status's upper bits are not the count's");
}

static void
report_decode(void)
{
	struct ft5x06_frame f = { .active = 0x155 };
	/* finger 3 stays at 0x123, 0x456; finger 0 lifted; finger 9 landed at 4095, 0; then nobody's slot */
	const uint8_t points[24] = { 0x81, 0x23, 0x34, 0x56, 0, 0, 0x40, 0x00, 0x00, 0x00, 0, 0, 0x0f, 0xff, 0x90, 0x00, 0, 0,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	/* a finger landing at 1081, 16 */
	uint8_t twice[12], off[6] = { 0x04, 0x39, 0x00, 0x10, 0, 0 };

	CHECK(ft5x06_decode(2, points, 4, 10, 4095, 4095, &f), "a report of two fingers and one lifted");
	CHECK(f.active == (1u << 3 | 1u << 9), "fingers 3 and 9 are down: %#x", f.active);
	CHECK(f.x[3] == 0x123 && f.y[3] == 0x456 && f.x[9] == 4095 && f.y[9] == 0, "where: %u, %u and %u, %u", f.x[3], f.y[3], f.x[9],
	    f.y[9]);
	CHECK(ft5x06_decode(0, points, 4, 10, 4095, 4095, &f) && f.active == 0, "a status of no finger is no finger, whatever the slots hold");
	f.active = 0x155;
	CHECK(!ft5x06_decode(0x0f, points, 4, 10, 4095, 4095, &f) && f.active == 0x155, "fifteen fingers: not a report, nothing taken");
	CHECK(!ft5x06_decode(6, points, 4, 5, 4095, 4095, &f), "six fingers of a chip of five: not a report");
	CHECK(!ft5x06_decode(1, points, 4, 11, 4095, 4095, &f), "a chip of eleven fingers is not of the family");
	memcpy(twice, points, 6);
	memcpy(twice + 6, points, 6);
	CHECK(!ft5x06_decode(2, twice, 2, 10, 4095, 4095, &f) && f.active == 0x155, "a finger said twice: not a report");
	CHECK(ft5x06_decode(1, off, 1, 10, 1081, 16, &f) && f.active == 1 && f.x[0] == 1081 && f.y[0] == 16,
	    "a position at the panel's far corner is taken: %u, %u", f.x[0], f.y[0]);
	f.active = 0x155;
	CHECK(!ft5x06_decode(1, off, 1, 10, 1080, 2400, &f) && !ft5x06_decode(1, off, 1, 10, 1081, 15, &f) && f.active == 0x155,
	    "a position off the panel, either way, is not taken");
	CHECK(ft5x06_decode(1, points, 1, 5, 4095, 4095, &f) && f.active == 1u << 3, "only the slots read are looked at");
	CHECK(ft5x06_decode(2, points, 3, 5, 4095, 4095, &f) && f.active == 1u << 3, "a finger numbered past the chip's is nobody");
	{
		const uint8_t none[6] = { 0xc1, 0x23, 0x24, 0x56, 0, 0 };

		CHECK(ft5x06_decode(1, none, 1, 10, 4095, 4095, &f) && f.active == 0, "a slot that says nothing happened is nobody's");
	}
}

/* ---- scenarios: the driver -------------------------------------------------------------------------------------------- */

/* without its boot argument, or with touchscreen=0: nothing */
static void
off_by_default(void)
{
	phone("phone-i2c", false);
	start("");
	start("touchscreen=0");
	usleep(50000);
	CHECK(model.writes == 0 && model.reads == 0 && mappings == 0 && !logged("ft5x06:"), "nothing touched, nothing said: %s", log_text);
	quiesce();
	CHECK(model.writes == 0 && model.reads == 0 && asks == 0 && posts == 0, "and nothing at the run's end");
}

/* on a board that does not say where the chip is: said, nothing touched */
static void
no_wiring(void)
{
	phone("phone-video", false);
	start("touchscreen=1");
	usleep(50000);
	CHECK(logged("ft5x06: disabled: the board file does not say how the controller is wired (touch-i2c { })"), "said: %s", log_text);
	CHECK(model.writes == 0 && model.reads == 0 && mappings == 0, "nothing touched");
	quiesce();
	CHECK(model.writes == 0 && model.reads == 0, "nor at the run's end");
}

/* touchscreen=-1 on a chip the firmware left running: its identity read and said, nothing powered, configured or written */
static void
look_only(void)
{
	phone("phone-i2c", true);
	one_finger(0, STAYS, 100, 100);         /* (a finger on it: a look reads no report all the same) */
	start("touchscreen=-1");
	WAIT_FOR(logged("nothing powered, configured or written"), 5000);
	CHECK(logged("ft5x06: touchscreen=-1: looking only") && logged("ft5x06: step 1: the pins as they were found"), "said: %s", log_text);
	CHECK(logged("ft5x06: gpio42 (I2C data): function 1, pull 3, 2 mA, input") &&
	    logged("ft5x06: gpio58 (reset): function 0, pull 0, 6 mA, drives high") &&
	    logged("ft5x06: gpio59 (interrupt): function 0, pull 1, 2 mA, input, reads 0"), "each pin as it was found: %s", log_text);
	CHECK(logged("ft5x06: the chip at 0x38 answers identity 0x5422, an FT5x46; firmware 0x1e, mode 0x00"), "the identity: %s", log_text);
	CHECK(logged("ft5x06: touchscreen=-1: nothing powered, configured or written"), "said: %s", log_text);
	CHECK(!logged("step 2") && !logged("step 3") && !logged("step 5") && !logged("held in reset"), "no other step: %s", log_text);
	CHECK(device.writes == 0, "%u bytes written into the chip", device.writes);
	CHECK(device.starts >= 2 && device.starts == 2 * device.stops, "read in whole transfers: %u starts, %u stops", device.starts,
	    device.stops);
	CHECK(asks == 0 && locks == 0, "RPMh asked %u times", asks);
	CHECK(mappings == 1, "only the pins mapped: %u mappings", mappings);
	CHECK(reports_read == 0 && posts == 0, "no report read (%u), nothing posted (%u)", reports_read, posts);
	only_the_bus_touched("a look");
	pins_as_found("a look", false);
	usleep(30000);
	CHECK(reports_read == 0 && posts == 0, "nor later");
	quiesce();
	CHECK(device.writes == 0 && asks == 0 && pin_high(reset_pin), "nor anything at the run's end: the chip left running");
	only_the_bus_touched("a look, at the run's end");
	pins_as_found("a look, at the run's end", false);
	CHECK(!logged("powered off"), "nothing was powered: %s", log_text);
}

/* touchscreen=-1 on a chip the firmware left in reset and unpowered: said, and left so */
static void
look_at_a_chip_in_reset(void)
{
	phone("phone-i2c", false);
	start("touchscreen=-1");
	WAIT_FOR(logged("nothing powered, configured or written"), 5000);
	CHECK(logged("ft5x06: the chip is held in reset as found (gpio58 drives low): it is not expected to answer"), "said: %s", log_text);
	CHECK(logged("ft5x06: nobody answers at 0x38"), "said: %s", log_text);
	CHECK(device.starts == 1, "asked once: %u", device.starts);
	CHECK(device.writes == 0 && asks == 0 && boots == 0, "nothing written, powered or reset");
	only_the_bus_touched("a look");
	pins_as_found("a look", false);
	quiesce();
	pins_as_found("a look, at the run's end", false);
}

static int
said_at(const char *text)
{
	const char *at = strstr(log_text, text);

	return at != NULL ? (int)(at - log_text) : -1;
}

/* touchscreen=1: the steps in their order, the supplies and the reset as the file's head has them, nothing written */
static void
brought_up_in_steps(void)
{
	static const char *const steps_said[] = { "ft5x06: step 1: the pins as they were found",
		"ft5x06: step 2: the chip held in reset, its supplies switched on (I/O ldoa8, analog ldoa17)",
		"ft5x06: regulator ldoa8 at 0x40800: 1800 mV", "ft5x06: regulator ldoa17 at 0x41100: 3008 mV",
		"ft5x06: step 3: the reset let go after 10 ms, the chip given 200 ms",
		"ft5x06: step 4: the first transfers on the I2C bus (gpio42, gpio43): the chip's identity",
		"ft5x06: the chip at 0x38 answers identity 0x5422, an FT5x46; firmware 0x1e, mode 0x00",
		"ft5x06: step 5: touch screen ready, 1080 x 2340, 10 fingers; the interrupt line (gpio59) is looked at every 8 ms" };
	int before = -1;

	ready();
	for (unsigned i = 0; i < sizeof(steps_said) / sizeof(steps_said[0]); i++) {
		CHECK(said_at(steps_said[i]) > before, "said, and after the one before: \"%s\": %s", steps_said[i], log_text);
		before = said_at(steps_said[i]);
	}
	CHECK(asks == 6 && asked[0].address == IO_SUPPLY && asked[0].value == 1800 && asked[1].address == IO_SUPPLY + 4 &&
	    asked[1].value == 1 && asked[2].address == IO_SUPPLY + 8 && asked[2].value == 7, "the I/O supply first, at 1.8 V: %u asks", asks);
	CHECK(asked[3].address == ANALOG_SUPPLY && asked[3].value == 3008 && asked[4].address == ANALOG_SUPPLY + 4 &&
	    asked[4].value == 1 && asked[5].value == 7, "then the analog supply, at the board's 3008 mV");
	CHECK(locks == 2, "each under RPMh's lock: %u", locks);
	CHECK(boots == 1, "the chip started once: %u", boots);
	CHECK(starts_while_off == 0, "%u transfers begun while the chip was unpowered or in reset", starts_while_off);
	CHECK(early_starts == 0, "%u transfers begun before the chip had its time after the reset", early_starts);
	CHECK(pin_high(reset_pin) && (model.cfg[reset_pin] & 1u << 9), "the reset let go, driven high");
	CHECK((model.cfg[int_pin] & (1u << 9 | 0x3c | 3u)) == 3u, "the interrupt's pin an input, pulled up: %#x", model.cfg[int_pin]);
	CHECK(device.writes == 0, "%u bytes written into the chip", device.writes);
	CHECK(device.driven_high == 0 && model.drove_wrong == 0 && model.elsewhere == 0, "the pins used as they are meant");
	CHECK(mappings == 3, "the pins, RPMh and its command database mapped: %u", mappings);
	CHECK(posts == 0, "nothing posted of a chip nobody touches: %u", posts);
}

/* ft5x06_steps=N: the first N steps and no more, and what they powered powered off again */
static void
stops_after_step_1(void)
{
	phone("phone-i2c", false);
	start("touchscreen=1 ft5x06_steps=1");
	WAIT_FOR(logged("stopping here"), 5000);
	CHECK(logged("ft5x06: step 1") && logged("ft5x06: stopping here (ft5x06_steps=1)") && !logged("step 2"), "said: %s", log_text);
	CHECK(model.writes == 0 && asks == 0 && device.starts == 0, "only looked at: %u writes", model.writes);
	quiesce();
	CHECK(model.writes == 0 && asks == 0, "nor anything at the run's end");
}

static void
stops_after_step_2(void)
{
	phone("phone-i2c", false);
	start("touchscreen=1 ft5x06_steps=2");
	WAIT_FOR(logged("stopping here"), 5000);
	CHECK(logged("ft5x06: step 2") && logged("ft5x06: stopping here (ft5x06_steps=2)") && !logged("step 3"), "said: %s", log_text);
	CHECK(ask_of(IO_SUPPLY + 4, 1, false) == 1 && ask_of(ANALOG_SUPPLY + 4, 1, false) == 4, "both supplies were switched on");
	CHECK(boots == 0 && device.starts == 0, "the reset never let go, the bus never used");
	WAIT_FOR(logged("powered off"), 2000);
	powered_off("stopped after step 2");
}

static void
stops_after_step_4(void)
{
	phone("phone-i2c", false);
	start("touchscreen=1 ft5x06_steps=4");
	WAIT_FOR(logged("stopping here"), 5000);
	CHECK(logged("answers identity 0x5422") && logged("ft5x06: stopping here (ft5x06_steps=4)") && !logged("step 5"), "said: %s", log_text);
	WAIT_FOR(logged("powered off"), 2000);
	CHECK(reports_read == 0 && posts == 0, "no report read");
	powered_off("stopped after step 4");
}

/* an identity the driver has no name for: said, and read as the family's */
static void
unnamed_identity(void)
{
	phone("phone-i2c", false);
	device.reg[REG_ID_HIGH] = 0x12;
	device.reg[REG_ID_LOW] = 0x00;
	start("touchscreen=1");
	WAIT_FOR(logged("touch screen ready") || logged("unavailable"), 10000);
	CHECK(logged("answers identity 0x1200, NOT ONE THIS DRIVER HAS A NAME FOR: read as the family's all the same; firmware 0x1e"),
	    "said: %s", log_text);
	CHECK(logged("touch screen ready"), "and read: %s", log_text);
	one_finger(1, LANDED, 540, 1170);
	WAIT_FOR(posts >= 1, 3000);
	post_is(0, HAKIOS_TOUCH_DOWN, 1, scaled(540, 1080), scaled(1170, 2340));
	CHECK(device.writes == 0, "nothing written into it");
}

/* a chip that runs no firmware (its registers read all ones): said, not read, powered off; nothing uploaded */
static void
no_firmware(void)
{
	phone("phone-i2c", false);
	memset(device.reg, 0xff, sizeof(device.reg));
	start("touchscreen=1");
	WAIT_FOR(logged("powered off"), 10000);
	CHECK(logged("ft5x06: the chip at 0x38 reads identity 0xff: one that runs no firmware (none is uploaded here)") &&
	    logged("ft5x06: touch screen unavailable") && !logged("ready"), "said: %s", log_text);
	CHECK(device.writes == 0, "%u bytes written into a chip without firmware", device.writes);
	CHECK(reports_read == 0 && posts == 0, "no report read of it");
	powered_off("no firmware");
}

/* a chip in its factory test mode: its reports are not read */
static void
factory_mode(void)
{
	phone("phone-i2c", false);
	device.reg[REG_MODE] = 0x40;
	start("touchscreen=1");
	WAIT_FOR(logged("powered off"), 10000);
	CHECK(logged("ft5x06: the chip is not in its working mode (0x40): its reports are not read") && logged("unavailable"), "said: %s",
	    log_text);
	CHECK(device.writes == 0 && reports_read == 0, "nothing written to put it into another mode, no report read");
	powered_off("the factory mode");
}

/* nobody at the address (another address than the board says): asked for a second, said, powered off */
static void
nobody_answers(void)
{
	phone("phone-i2c", false);
	silent = true;
	start("touchscreen=1");
	WAIT_FOR(logged("powered off"), 10000);
	CHECK(logged("ft5x06: nobody answers at 0x38: a chip that is not there, not powered or held in reset") && logged("unavailable"),
	    "said: %s", log_text);
	CHECK(device.starts == 10, "asked ten times: %u", device.starts);
	powered_off("nobody there");
}

/* RPMh does not switch the analog supply on: no reset let go, no transfer, the I/O supply's vote withdrawn */
static void
supply_refused(void)
{
	phone("phone-i2c", false);
	refused_supply = ANALOG_SUPPLY;
	start("touchscreen=1");
	WAIT_FOR(logged("powered off"), 10000);
	CHECK(logged("ft5x06: RPMh: no answer to the request") && logged("unavailable") && !logged("step 3"), "said: %s", log_text);
	CHECK(boots == 0 && device.starts == 0, "the reset not let go, the bus not used");
	CHECK(ask_of(IO_SUPPLY + 4, 0, true) > ask_of(IO_SUPPLY + 4, 1, true) && ask_of(ANALOG_SUPPLY + 4, 0, true) > 0,
	    "both votes withdrawn, the one that may have gone through too");
	CHECK(!pin_high(reset_pin), "in reset");
	pins_as_found("a supply refused", true);
}

/* one finger: down, moved, not said again where it stays, lifted -- each with the finger's number and its exact place */
static void
one_finger_down_moves_up(void)
{
	unsigned bytes, reports;

	ready();
	taken();
	reports = reports_read;
	bytes = device.bytes_out;
	taken();
	bytes = device.bytes_out - bytes;
	reports = reports_read - reports;
	CHECK(posts == 0, "nobody touches: %u posts", posts);
	CHECK(bytes <= reports + 1, "a look that finds no finger reads the status alone: %u bytes in %u reports", bytes, reports);
	one_finger(4, LANDED, 0, 0);
	taken();
	CHECK(posts == 1, "one post for a finger that landed: %u", posts);
	post_is(0, HAKIOS_TOUCH_DOWN, 4, 0, 0);
	one_finger(4, STAYS, 0, 0);
	taken();
	CHECK(posts == 1, "a finger that stays where it is is not said again: %u", posts);
	one_finger(4, STAYS, 1080, 2340);
	taken();
	CHECK(posts == 2, "one post for a move: %u", posts);
	post_is(1, HAKIOS_TOUCH_MOVE, 4, 65535, 65535);
	one_finger(4, STAYS, 271, 1999);
	taken();
	post_is(2, HAKIOS_TOUCH_MOVE, 4, 271 * 65535 / 1080, 1999 * 65535 / 2340);
	/* lifted as the chip says it: no finger counted, its slot saying so (and no position worth having) */
	{
		const struct slot lifted = { 4, LIFTED, 0, 0 };

		chip_says(0, &lifted, 1);
	}
	taken();
	CHECK(posts == 4, "one post for the lift: %u", posts);
	post_is(3, HAKIOS_TOUCH_UP, 4, 271 * 65535 / 1080, 1999 * 65535 / 2340);
	/* again, landing as "stays" (its first report missed), and lifted by a report that simply has nobody */
	one_finger(0, STAYS, 1, 2339);
	taken();
	post_is(4, HAKIOS_TOUCH_DOWN, 0, 1 * 65535 / 1080, 2339 * 65535 / 2340);
	no_finger();
	taken();
	post_is(5, HAKIOS_TOUCH_UP, 0, 1 * 65535 / 1080, 2339 * 65535 / 2340);
	taken();
	CHECK(posts == 6, "and nothing more: %u posts", posts);
	CHECK(device.writes == 0 && device.driven_high == 0, "nothing written into the chip, the bus used as one");
	CHECK(seen.bad == 0, "%u reports not believed", seen.bad);
}

/* several fingers: their numbers are the chip's, not their slots'; one lifts and the others stay; ten at once */
static void
several_fingers(void)
{
	const struct slot three[3] = { { 7, LANDED, 100, 200 }, { 0, LANDED, 1000, 2000 }, { 2, LANDED, 540, 1170 } };
	const struct slot one_lifts[3] = { { 7, STAYS, 101, 200 }, { 0, LIFTED, 1000, 2000 }, { 2, STAYS, 540, 1170 } };
	const struct slot one_gone[1] = { { 2, STAYS, 540, 1171 } };
	struct slot ten[10];
	unsigned at;

	ready();
	chip_says(3, three, 3);
	taken();
	CHECK(posts == 3, "three fingers landed: %u posts", posts);
	CHECK(post_of(HAKIOS_TOUCH_DOWN, 7, 0) >= 0 && post_of(HAKIOS_TOUCH_DOWN, 0, 0) >= 0 && post_of(HAKIOS_TOUCH_DOWN, 2, 0) >= 0,
	    "each by its own number");
	post_is((unsigned)post_of(HAKIOS_TOUCH_DOWN, 7, 0), HAKIOS_TOUCH_DOWN, 7, scaled(100, 1080), scaled(200, 2340));
	post_is((unsigned)post_of(HAKIOS_TOUCH_DOWN, 0, 0), HAKIOS_TOUCH_DOWN, 0, scaled(1000, 1080), scaled(2000, 2340));
	post_is((unsigned)post_of(HAKIOS_TOUCH_DOWN, 2, 0), HAKIOS_TOUCH_DOWN, 2, scaled(540, 1080), scaled(1170, 2340));
	/* the middle one lifts, said in its slot between the two that stay; one of those moves */
	chip_says(2, one_lifts, 3);
	taken();
	CHECK(posts == 5, "a lift and a move: %u posts", posts);
	CHECK(post_of(HAKIOS_TOUCH_UP, 0, 3) >= 0 && post_of(HAKIOS_TOUCH_MOVE, 7, 3) >= 0, "finger 0 up, finger 7 moved");
	post_is((unsigned)post_of(HAKIOS_TOUCH_UP, 0, 3), HAKIOS_TOUCH_UP, 0, scaled(1000, 1080), scaled(2000, 2340));
	post_is((unsigned)post_of(HAKIOS_TOUCH_MOVE, 7, 3), HAKIOS_TOUCH_MOVE, 7, scaled(101, 1080), scaled(200, 2340));
	/* another is simply not in the report any more */
	chip_says(1, one_gone, 1);
	taken();
	CHECK(posts == 7, "a finger gone and a move: %u posts", posts);
	post_is((unsigned)post_of(HAKIOS_TOUCH_UP, 7, 5), HAKIOS_TOUCH_UP, 7, scaled(101, 1080), scaled(200, 2340));
	post_is((unsigned)post_of(HAKIOS_TOUCH_MOVE, 2, 5), HAKIOS_TOUCH_MOVE, 2, scaled(540, 1080), scaled(1171, 2340));
	no_finger();
	taken();
	CHECK(posts == 8 && post_of(HAKIOS_TOUCH_UP, 2, 7) == 7, "the last lifted: %u posts", posts);
	/* all ten, in slots that are not in their numbers' order */
	for (unsigned i = 0; i < 10; i++) {
		ten[i] = (struct slot){ 9 - i, LANDED, 100 * i + 7, 200 * i + 11 };
	}
	chip_says(10, ten, 10);
	taken();
	CHECK(posts == 18, "ten fingers landed: %u posts", posts);
	for (unsigned i = 0; i < 10; i++) {
		at = (unsigned)post_of(HAKIOS_TOUCH_DOWN, 9 - i, 8);
		post_is(at, HAKIOS_TOUCH_DOWN, 9 - i, scaled(100 * i + 7, 1080), scaled(200 * i + 11, 2340));
	}
	no_finger();
	taken();
	CHECK(posts == 28, "and all ten lifted: %u posts", posts);
	for (unsigned i = 0; i < 10; i++) {
		CHECK(post_of(HAKIOS_TOUCH_UP, i, 18) >= 18, "finger %u lifted", i);
	}
	CHECK(seen.bad == 0, "%u reports not believed", seen.bad);
}

/* touch_flip: bit 0 mirrors left to right, bit 1 top to bottom */
static void
mirrored(const char *arguments, bool x_mirrored, bool y_mirrored)
{
	unsigned x = scaled(270, 1080), y = scaled(2000, 2340);

	ready_on("phone-i2c", arguments);
	one_finger(0, LANDED, 270, 2000);
	taken();
	post_is(0, HAKIOS_TOUCH_DOWN, 0, x_mirrored ? 65535 - x : x, y_mirrored ? 65535 - y : y);
	no_finger();
	taken();
	post_is(1, HAKIOS_TOUCH_UP, 0, x_mirrored ? 65535 - x : x, y_mirrored ? 65535 - y : y);
}

static void mirrored_left_to_right(void) { mirrored("touchscreen=1 touch_flip=1", true, false); }
static void mirrored_top_to_bottom(void) { mirrored("touchscreen=1 touch_flip=2", false, true); }
static void mirrored_both_ways(void) { mirrored("touchscreen=1 touch_flip=3", true, true); }

/*
 * The second phone: the chip at another address on other pins, five fingers, a smaller panel, its analog supply behind
 * a pin and no regulator named. Everything is that board's; RPMh is never asked; what follows the chip's five slots
 * (other registers: zeros, which as a slot would be finger 0 landing at 0, 0) is never taken for fingers.
 */
static void
wired_otherwise(void)
{
	const struct slot five[5] = { { 0, LANDED, 720, 1600 }, { 1, LANDED, 1, 2 }, { 2, LANDED, 3, 4 }, { 3, LANDED, 5, 6 },
		{ 4, LANDED, 360, 800 } };
	const struct slot two[2] = { { 3, STAYS, 5, 6 }, { 4, STAYS, 360, 800 } };
	unsigned bytes, reports;

	ready_on("phone-keys", "touchscreen=1");
	CHECK(logged("ft5x06: step 2: the chip held in reset, its supplies switched on (I/O not ours to switch, analog behind its pin)"),
	    "said: %s", log_text);
	CHECK(logged("ft5x06: the chip at 0x48 answers identity 0x5422") && logged("touch screen ready, 720 x 1600, 5 fingers; the "
	    "interrupt line (gpio8)"), "the board's address, size and fingers: %s", log_text);
	CHECK(asks == 0 && locks == 0 && mappings == 1, "RPMh neither mapped nor asked: %u asks, %u mappings", asks, mappings);
	CHECK(pin_high(78) && (model.cfg[78] & 1u << 9), "the analog supply's pin driven high");
	CHECK(early_starts == 0 && starts_while_off == 0 && boots == 1, "the chip waited for");
	for (unsigned i = 0; i < PINS; i++) {
		CHECK(pin_writes[i] == 0 || i == 160 || i == 161 || i == 8 || i == 202 || i == 78, "gpio%u is not this board's touch "
		    "controller's, and was written", i);
	}
	chip_says(5, five, 5);
	taken();
	CHECK(posts == 5, "five fingers landed: %u posts", posts);
	post_is((unsigned)post_of(HAKIOS_TOUCH_DOWN, 0, 0), HAKIOS_TOUCH_DOWN, 0, 65535, 65535);
	post_is((unsigned)post_of(HAKIOS_TOUCH_DOWN, 4, 0), HAKIOS_TOUCH_DOWN, 4, scaled(360, 720), scaled(800, 1600));
	reports = reports_read;
	bytes = device.bytes_out;
	taken();
	bytes = device.bytes_out - bytes;
	reports = reports_read - reports;
	CHECK(bytes <= (reports + 1) * 31, "never more than the status and the chip's five slots: %u bytes in %u reports", bytes, reports);
	/* three lift at once: the two left are in the first slots, the next three nobody's, and then come the zeros */
	chip_says(2, two, 2);
	taken();
	CHECK(posts == 8, "three lifted, and nobody landed out of the registers after the slots: %u posts", posts);
	CHECK(post_of(HAKIOS_TOUCH_DOWN, 0, 5) < 0, "finger 0 did not land again");
	/* a report of six fingers is not this chip's */
	chip_says(6, five, 5);
	WAIT_FOR(logged("a report that cannot be one"), 2000);
	CHECK(logged("ft5x06: a report that cannot be one: status 0x06, 0 slots"), "said: %s", log_text);
	chip_says(2, two, 2);
	taken();
	CHECK(posts == 8, "and changed nothing: %u posts", posts);
	quiesce();
	powered_off("the second phone, at the run's end");
	CHECK(!pin_high(78), "the analog supply's pin as it was found: off");
}

/*
 * Reports that cannot be ones change nothing: too many fingers, a finger twice, a position off the panel. Each is put
 * out until the driver has read one, and a report to believe after it: none stays long enough to count as silence.
 */
static unsigned moves;

static void
not_believed(const char *what, unsigned count, const struct slot *slots, unsigned n)
{
	unsigned bad = seen.bad;

	chip_says(count, slots, n);
	WAIT_FOR(seen.bad > bad, 3000);
	one_finger(3, STAYS, 501 + moves, 500);
	CHECK(seen.bad > bad, "%s: counted as a report not believed", what);
	taken();
	moves++;
	CHECK(posts == 1 + moves, "%s: posted nothing, nor lifted the finger, which then moves: %u posts", what, posts);
	post_is(moves, HAKIOS_TOUCH_MOVE, 3, scaled(500 + moves, 1080), scaled(500, 2340));
}

static void
reports_not_believed(void)
{
	const struct slot twice[2] = { { 3, STAYS, 10, 10 }, { 3, STAYS, 20, 20 } };
	const struct slot off[2] = { { 3, STAYS, 500, 500 }, { 5, LANDED, 1081, 100 } };
	const struct slot low[2] = { { 3, STAYS, 500, 500 }, { 5, LANDED, 100, 2341 } };

	ready();
	one_finger(3, LANDED, 500, 500);
	taken();
	CHECK(posts == 1, "the finger landed");
	not_believed("fifteen fingers (a bus that reads all ones)", 0xff, twice, 2);
	CHECK(logged("ft5x06: a report that cannot be one: status 0xff, 0 slots"), "the first said: %s", log_text);
	not_believed("a finger twice", 2, twice, 2);
	not_believed("a position right of the panel", 2, off, 2);
	not_believed("a position below the panel", 2, low, 2);
	CHECK(post_of(HAKIOS_TOUCH_DOWN, 5, 0) < 0, "the finger off the panel never landed");
	CHECK(!logged("lifted") && boots == 1, "nothing was lifted, nothing reset: %s", log_text);
}

/* (with the driver's waits as long as it asks: a quarter of a second is a quarter of a second) */
static void
ready_in_real_time(void)
{
	phone("phone-i2c", false);
	fake_delay_divisor = 1;
	start("touchscreen=1");
	WAIT_FOR(logged("touch screen ready") || logged("touch screen unavailable"), 20000);
	CHECK(logged("touch screen ready"), "the chip brought up: %s", log_text);
}

/* a finger whose chip falls silent is lifted after a quarter of a second: not sooner, not much later, once */
static void
silent_finger_lifted(void)
{
	uint64_t fell_silent, lifted_ms;
	int up;

	ready_in_real_time();
	one_finger(6, LANDED, 300, 400);
	taken();
	CHECK(posts == 1, "the finger landed: %u posts", posts);
	fell_silent = real_ns();
	silent = true;
	WAIT_FOR(posts >= 2, 3000);
	up = post_of(HAKIOS_TOUCH_UP, 6, 1);
	CHECK(up == 1, "the finger lifted: %u posts: %s", posts, log_text);
	post_is(1, HAKIOS_TOUCH_UP, 6, scaled(300, 1080), scaled(400, 2340));
	lifted_ms = up >= 0 ? (posted[up].t - fell_silent) / 1000000 : 0;
	CHECK(lifted_ms >= 230 && lifted_ms <= 600, "lifted %u ms after the chip fell silent, not about 250", (unsigned)lifted_ms);
	CHECK(logged("with a finger down: lifted"), "said: %s", log_text);
	usleep(100000);
	CHECK(posts == 2, "once: %u posts", posts);
	/* it answers again before the driver gives it up: the finger lands again, nothing was reset */
	silent = false;
	WAIT_FOR(posts >= 3, 3000);
	post_is(2, HAKIOS_TOUCH_DOWN, 6, scaled(300, 1080), scaled(400, 2340));
	CHECK(boots == 1, "the chip was not reset for it");
}

/* a few reads that fail lift nothing */
static void
bad_reads_lift_nothing(void)
{
	unsigned failed_before;

	ready_in_real_time();
	one_finger(6, LANDED, 300, 400);
	taken();
	failed_before = seen.reads_failed;
	silent = true;
	WAIT_FOR(seen.reads_failed >= failed_before + 3, 3000);
	silent = false;
	CHECK(seen.reads_failed >= failed_before + 3 && seen.reads_failed < failed_before + 20, "a few reads failed: %u",
	    seen.reads_failed - failed_before);
	one_finger(6, STAYS, 301, 400);
	taken();
	CHECK(posts == 2 && !logged("lifted"), "the finger stayed down through them: %u posts: %s", posts, log_text);
	post_is(1, HAKIOS_TOUCH_MOVE, 6, scaled(301, 1080), scaled(400, 2340));
	CHECK(logged("ft5x06: a report not read: nobody acknowledged"), "the failed read said: %s", log_text);
}

/* a chip that stops answering for good: its fingers lifted, a reset tried, then given up and powered off */
static void
stops_answering(void)
{
	unsigned starts;

	ready();
	one_finger(2, LANDED, 10, 20);
	taken();
	silent = true;
	WAIT_FOR(logged("powered off"), 20000);
	CHECK(posts == 2, "the finger lifted, once: %u posts", posts);
	post_is(1, HAKIOS_TOUCH_UP, 2, scaled(10, 1080), scaled(20, 2340));
	CHECK(logged("ft5x06: 125 looks in a row without a report") && logged("ft5x06: nobody answers at 0x38") &&
	    logged("ft5x06: stopped: the chip does not come back"), "said: %s", log_text);
	CHECK(boots == 2, "a reset was tried, once: %u starts", boots);
	powered_off("the chip gone");
	starts = device.starts;
	usleep(60000);
	CHECK(device.starts == starts, "and left alone from then on: %u more transfers", device.starts - starts);
	quiesce();
	CHECK(device.starts == starts && posts == 2, "at the run's end too");
}

/* a chip that hangs and that its reset brings back: reset, identified again, read again */
static void
reset_brings_it_back(void)
{
	ready();
	one_finger(2, LANDED, 10, 20);
	taken();
	hung = true;
	WAIT_FOR(boots >= 2 && logged("125 looks in a row"), 20000);
	CHECK(boots == 2, "the chip was reset: %u starts", boots);
	CHECK(shortest_reset_ns >= 1500000, "its reset held low %u us, not the 10 ms the driver means (2 ms here)",
	    (unsigned)(shortest_reset_ns / 1000));
	WAIT_FOR(posts >= 3, 5000);
	CHECK(early_starts == 0, "%u transfers begun before the chip had its time after the reset", early_starts);
	post_is(1, HAKIOS_TOUCH_UP, 2, scaled(10, 1080), scaled(20, 2340));
	post_is(2, HAKIOS_TOUCH_DOWN, 2, scaled(10, 1080), scaled(20, 2340));
	CHECK(!logged("stopped") && !logged("powered off"), "and goes on: %s", log_text);
	CHECK(device.writes == 0, "nothing written into it");
}

/* a chip that hangs again and again: reset three times, and given up the fourth */
static void
four_troubles_end_it(void)
{
	ready();
	hang_after = 3;
	hung = true;
	WAIT_FOR(logged("powered off"), 60000);
	CHECK(boots == 4, "reset three times: %u starts", boots);
	CHECK(logged("resets made for that before: 3 of 3") && logged("ft5x06: stopped: the chip does not come back"), "said: %s", log_text);
	powered_off("troubles without end");
}

/* a bus that is not at rest (its data line held low): nothing driven, said, powered off */
static void
bus_not_at_rest(void)
{
	phone("phone-i2c", false);
	device.stuck = true;
	lines();
	device.was_sda = false;
	start("touchscreen=1");
	WAIT_FOR(logged("powered off"), 10000);
	CHECK(logged("ft5x06: the I2C bus is not at rest (gpio42 reads 0, gpio43 reads 1): not touched") &&
	    logged("ft5x06: nothing read at 0x38: the I2C bus is not at rest") && logged("unavailable"), "said: %s", log_text);
	CHECK(device.starts == 0 && device.driven_high == 0, "no transfer begun on it");
	powered_off("a bus held");
}

/* the interrupt line stuck high (a board that names another pin): a finger is found all the same, by the looks in between */
static void
interrupt_line_stuck_high(void)
{
	uint64_t began;
	unsigned looks, elapsed_ms;

	phone("phone-i2c", false);
	int_stuck_high = true;
	no_finger();
	start("touchscreen=1");
	WAIT_FOR(logged("touch screen ready"), 10000);
	began = real_ns();
	looks = seen.looks;
	usleep(200000);
	elapsed_ms = (unsigned)((real_ns() - began) / 1000000);
	/* (a look every 8 ms, 1.6 ms here: of ten, one asks the chip) */
	CHECK(seen.looks - looks >= 2 && seen.looks - looks <= elapsed_ms * 10 / 16 / 10 + 3, "the chip asked on every tenth look: %u "
	    "times in %u ms", seen.looks - looks, elapsed_ms);
	one_finger(1, LANDED, 5, 5);
	WAIT_FOR(posts >= 1, 3000);
	post_is(0, HAKIOS_TOUCH_DOWN, 1, scaled(5, 1080), scaled(5, 2340));
	no_finger();
	WAIT_FOR(posts >= 2, 3000);
	post_is(1, HAKIOS_TOUCH_UP, 1, scaled(5, 1080), scaled(5, 2340));
}

/* ... and with the line low the chip is asked at every look */
static void
interrupt_line_low(void)
{
	unsigned looks;

	phone("phone-i2c", false);
	int_stuck_low = true;
	no_finger();
	start("touchscreen=1");
	WAIT_FOR(logged("touch screen ready"), 10000);
	looks = seen.looks;
	WAIT_FOR(seen.looks >= looks + 30, 400);
	CHECK(seen.looks >= looks + 30, "asked at every look while the line is low: %u times", seen.looks - looks);
	CHECK(posts == 0, "and nobody landed");
}

/* the run's end while a finger is down: no more reads, nothing posted, powered off without RPMh's lock, the pins as found */
static void
run_ends_while_polling(void)
{
	unsigned starts, lock_count;

	ready();
	one_finger(2, LANDED, 10, 20);
	taken();
	CHECK(posts == 1, "the finger landed");
	lock_count = locks;
	quiesce();
	starts = device.starts;
	CHECK(logged("ft5x06: at the run's end:") && logged("ft5x06: powered off"), "said: %s", log_text);
	CHECK(locks == lock_count && locks_in_quiesce == 0, "RPMh asked without its lock at the run's end");
	CHECK(posts_in_quiesce == 0 && posts == 1, "nothing posted from the run's end: %u posts", posts);
	powered_off("the run's end");
	usleep(60000);
	CHECK(device.starts == starts && posts == 1, "and no transfer after it: %u more", device.starts - starts);
	quiesce();
	CHECK(asks == 8, "a second run's end finds nothing to do: %u asks", asks);
}

/* the thread stopped (its core, by the run's end) while the chip was sending it a report's status: the transfer ended there */
static unsigned clock_falls;

static bool
in_the_status_byte(vm_offset_t address)
{
	/* the third fall of the clock while the chip sends its first byte (0x01: its data line held low for seven bits) */
	if (address != model.base + 0x1000 * scl_pin || !device.reading || device.bytes_out != 1 || reports_read == 0) {
		return false;
	}
	return !model.input[scl_pin] && ++clock_falls == 3;
}

static void
run_ends_in_a_transfer(void)
{
	ready();
	taken();
	device.bytes_out = 0;
	reports_read = 0;
	one_finger(0, STAYS, 0, 0);
	model.input[int_pin] = false;
	stop_after = in_the_status_byte;
	WAIT_FOR(stopped, 5000);
	CHECK(stopped, "the thread was stopped in the status byte");
	CHECK(!model.input[sda_pin], "(the chip holds the data line, in the middle of its byte)");
	stop_after = NULL;
	quiesce();
	CHECK(logged("ft5x06: at the run's end: a transfer cut off: the bus came back after"), "said: %s", log_text);
	CHECK(model.input[sda_pin] && model.input[scl_pin] && !device.active, "the bus at rest, the chip let go of it");
	CHECK(locks_in_quiesce == 0 && posts_in_quiesce == 0, "without the lock, nothing posted");
	powered_off("the run's end in a transfer");
}

/* ... or while it was taking the pins for one, before anything was sent */
static bool
taking_the_pins(vm_offset_t address)
{
	return reports_read != 0 && address == model.base + 0x1000 * sda_pin && model.cfg[sda_pin] != BUS_AS_FOUND && ++clock_falls == 1;
}

static void
run_ends_taking_the_pins(void)
{
	ready();
	taken();
	stop_after = taking_the_pins;
	WAIT_FOR(stopped, 5000);
	CHECK(stopped, "the thread was stopped taking the pins");
	stop_after = NULL;
	quiesce();
	CHECK(logged("ft5x06: at the run's end: a transfer cut off: ended, the bus at rest"), "said: %s", log_text);
	powered_off("the run's end while the pins were taken");
}

/* the run's end before the chip is ready: the bring-up stops where it is, and what it powered is powered off */
static void
run_ends_in_bring_up(void)
{
	phone("phone-i2c", false);
	fake_delay_divisor = 1;
	start("touchscreen=1");
	WAIT_FOR(logged("step 3"), 5000);
	quiesce();
	CHECK(io_on == false && analog_on == false && !pin_high(reset_pin), "powered off");
	CHECK(posts == 0 && locks_in_quiesce == 0, "nothing posted, no lock taken");
	usleep(300000);
	CHECK(!logged("touch screen ready") && reports_read == 0, "and no report read after it: %s", log_text);
	pins_as_found("the run's end in bring-up", true);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "report_slots", report_slots }, { "report_decode", report_decode },
		{ "off_by_default", off_by_default }, { "no_wiring", no_wiring },
		{ "look_only", look_only }, { "look_at_a_chip_in_reset", look_at_a_chip_in_reset },
		{ "brought_up_in_steps", brought_up_in_steps }, { "stops_after_step_1", stops_after_step_1 },
		{ "stops_after_step_2", stops_after_step_2 }, { "stops_after_step_4", stops_after_step_4 },
		{ "unnamed_identity", unnamed_identity }, { "no_firmware", no_firmware }, { "factory_mode", factory_mode },
		{ "nobody_answers", nobody_answers }, { "supply_refused", supply_refused },
		{ "one_finger_down_moves_up", one_finger_down_moves_up }, { "several_fingers", several_fingers },
		{ "mirrored_left_to_right", mirrored_left_to_right }, { "mirrored_top_to_bottom", mirrored_top_to_bottom },
		{ "mirrored_both_ways", mirrored_both_ways }, { "wired_otherwise", wired_otherwise },
		{ "reports_not_believed", reports_not_believed }, { "silent_finger_lifted", silent_finger_lifted },
		{ "bad_reads_lift_nothing", bad_reads_lift_nothing }, { "stops_answering", stops_answering },
		{ "reset_brings_it_back", reset_brings_it_back }, { "four_troubles_end_it", four_troubles_end_it },
		{ "bus_not_at_rest", bus_not_at_rest }, { "interrupt_line_stuck_high", interrupt_line_stuck_high },
		{ "interrupt_line_low", interrupt_line_low }, { "run_ends_while_polling", run_ends_while_polling },
		{ "run_ends_in_a_transfer", run_ends_in_a_transfer }, { "run_ends_taking_the_pins", run_ends_taking_the_pins },
		{ "run_ends_in_bring_up", run_ends_in_bring_up },
	};

	scenario_thread = pthread_self();
	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			if (freopen("/dev/null", "w", stdout) == NULL) {
				return 2;
			}
			scenarios[i].run();
			fprintf(stderr, "%-28s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
