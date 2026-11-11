/*
 * The buttons' driver itself (kernel/overlay/bsd/dev/redwood_buttons.c), run as a host program: the
 * kernel's interfaces are tests/fakekernel, the registers a model of the SoC's PMIC arbiter (version 5,
 * as Linux's spmi-pmic-arb.c has it) with the two peripherals the keys are on behind it. Where the
 * arbiter is and which peripherals the keys are on is the board's data, so the model is built from
 * the same board the driver reads: a wrong address or a wrong peripheral in the driver is a read of
 * nothing here.
 *
 *   buttons_driver_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <time.h>
#include <unistd.h>

#include "fakekernel.h"

extern char log_text[];

/* ---- the hardware's model ----------------------------------------------------------------------- */

#define CHANNELS        64
#define PON_KPDPWR      0x80
#define PON_RESIN       0x40

struct peripheral_model {
	uint16_t id;                    /* slave << 8 | peripheral */
	uint8_t reg[256];
	bool silent;                    /* nobody at that address: the transfer fails */
};

static struct {
	uint32_t core, cnfg, observer;  /* where the driver must find the three blocks (observer: this system's part) */
	uint32_t version, ee;
	uint32_t map[CHANNELS], owner[CHANNELS];
	struct peripheral_model pon, pin, other;
	/* the observer's registers, a channel at a time */
	uint32_t status[CHANNELS], data0[CHANNELS], data1[CHANNELS];
	unsigned transfers, writes_elsewhere, reads_elsewhere;
	pthread_mutex_t lock;
} m = { .lock = PTHREAD_MUTEX_INITIALIZER };

static struct peripheral_model *
peripheral_of(uint32_t channel)
{
	uint16_t id = (uint16_t)(m.map[channel] >> 8 & 0xfff);

	return id == m.pon.id ? &m.pon : id == m.pin.id ? &m.pin : id == m.other.id ? &m.other : NULL;
}

uint32_t
buttons_test_rd(vm_offset_t address)
{
	uint32_t value = 0;

	pthread_mutex_lock(&m.lock);
	if (address >= m.core && address < m.core + 0x1100) {
		uint32_t reg = (uint32_t)(address - m.core);

		value = reg == 0x000 ? m.version : reg == 0x004 ? CHANNELS :
		    reg >= 0x900 && reg < 0x900 + 4 * CHANNELS ? m.map[(reg - 0x900) / 4] : 0;
	} else if (address >= m.cnfg && address < m.cnfg + 0x1000) {
		uint32_t reg = (uint32_t)(address - m.cnfg);

		value = reg >= 0x700 && reg < 0x700 + 4 * CHANNELS ? m.owner[(reg - 0x700) / 4] : 0;
	} else if (address >= m.observer && address < m.observer + 0x80 * CHANNELS) {
		uint32_t channel = (uint32_t)(address - m.observer) / 0x80, reg = (uint32_t)(address - m.observer) % 0x80;

		value = reg == 0x08 ? m.status[channel] : reg == 0x18 ? m.data0[channel] : reg == 0x1c ? m.data1[channel] : 0;
	} else {
		m.reads_elsewhere++;
		fprintf(stderr, "MODEL: a read at %#lx, where nothing of the arbiter is\n", (unsigned long)address);
	}
	pthread_mutex_unlock(&m.lock);
	return value;
}

void
buttons_test_wr(vm_offset_t address, uint32_t value)
{
	pthread_mutex_lock(&m.lock);
	if (address >= m.observer && address < m.observer + 0x80 * CHANNELS && (address - m.observer) % 0x80 == 0) {
		/* a command: "read count bytes from register reg" (the only thing this driver may write) */
		uint32_t channel = (uint32_t)(address - m.observer) / 0x80, reg = value >> 4 & 0xff, count = (value & 7) + 1;
		struct peripheral_model *p = peripheral_of(channel);
		uint8_t bytes[8] = { 0 };

		m.transfers++;
		if (!(value & 1u << 27)) {
			m.writes_elsewhere++;
			fprintf(stderr, "MODEL: a command that is not a read: %#x\n", value);
		} else if (p == NULL || p->silent) {
			m.status[channel] = 0x1 | 0x2;          /* done, failed */
		} else {
			for (uint32_t i = 0; i < count; i++) {
				bytes[i] = p->reg[(reg + i) & 0xff];
			}
			m.data0[channel] = bytes[0] | bytes[1] << 8 | bytes[2] << 16 | (uint32_t)bytes[3] << 24;
			m.data1[channel] = bytes[4] | bytes[5] << 8 | bytes[6] << 16 | (uint32_t)bytes[7] << 24;
			m.status[channel] = 0x1;                /* done */
		}
	} else {
		m.writes_elsewhere++;
		fprintf(stderr, "MODEL: a write of %#x at %#lx: this driver only reads\n", value, (unsigned long)address);
	}
	pthread_mutex_unlock(&m.lock);
}

vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }

/* the driver's includes are the kernel's: the fake kernel answers for them, the input queue is below.
 * (The kernel's uint64_t is an unsigned long long, which is what the driver prints it as; the host's is
 * not everywhere, and what is tested here is not the spelling of a format.) */
#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define BUTTONS_TEST_MMIO 1
#include "hakios_board.c"
#include "redwood_buttons.c"

/* ---- what the driver reports: the records it posts to the input queue --------------------------- */

static struct { uint32_t type, button; } posted[64];
static unsigned posted_n;
static pthread_mutex_t posted_lock = PTHREAD_MUTEX_INITIALIZER;

void
redwood_touch_post(uint32_t type, uint32_t finger, uint32_t x __unused, uint32_t y __unused)
{
	pthread_mutex_lock(&posted_lock);
	if (posted_n < 64) {
		posted[posted_n].type = type;
		posted[posted_n].button = finger;
		posted_n++;
	}
	pthread_mutex_unlock(&posted_lock);
}

static unsigned
posts(void)
{
	unsigned n;

	pthread_mutex_lock(&posted_lock);
	n = posted_n;
	pthread_mutex_unlock(&posted_lock);
	return n;
}

static int failures;
/* (to stderr: this driver also prints every line of its log, which main() sends nowhere) */
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited = 0; !(cond) && waited < (ms); waited += 5) usleep(5000); } while (0)

/* ---- a phone: the arbiter and the keys where its board says, in the state the firmware leaves ----- */

static void
set_pon(uint8_t status)
{
	pthread_mutex_lock(&m.lock);
	m.pon.reg[0x10] = status;
	pthread_mutex_unlock(&m.lock);
}

static void
set_volume_up(bool held)
{
	pthread_mutex_lock(&m.lock);
	m.pin.reg[0x10] = held ? 0 : 1;         /* the key pulls the pin low */
	pthread_mutex_unlock(&m.lock);
}

static void
phone(const char *name)
{
	const struct hakios_board *b;

	board = name;
	b = hakios_board();
	if (b == NULL) {
		return;
	}
	m.core = b->spmi.core;
	m.cnfg = b->spmi.cnfg;
	m.ee = b->spmi.ee;
	m.observer = b->spmi.observer + 0x10000 * m.ee;
	m.version = 0x50000000;
	m.pon.id = b->buttons.pon;
	m.pin.id = b->buttons.volume_up;
	m.other.id = 0x3c0;                     /* somebody else's peripheral, in the table between the two */
	/* channels 3, 7 and 9; the pin also has a channel of another system's, as real tables do */
	m.map[3] = (uint32_t)m.pon.id << 8;
	m.map[5] = (uint32_t)m.pin.id << 8;
	m.owner[5] = 3;
	m.map[7] = (uint32_t)m.other.id << 8;
	m.map[9] = (uint32_t)m.pin.id << 8;
	m.owner[9] = m.ee;
	m.owner[3] = m.ee;
	m.pon.reg[0x04] = 0x01;                 /* a power-on block, the half for the operating system */
	m.pon.reg[0x05] = 0x09;
	m.pin.reg[0x04] = 0x10;                 /* a GPIO of the low voltage kind */
	m.pin.reg[0x05] = 0x10;
	m.pin.reg[0x40] = 0x00;                 /* an input */
	m.pin.reg[0x42] = 0x00;                 /* pulled up */
	m.pin.reg[0x46] = 0x80;                 /* switched on */
	set_pon(0);
	set_volume_up(false);
}

static void
press_and_release(void (*press)(void), void (*release)(void), uint32_t button, const char *name)
{
	unsigned before = posts();

	press();
	WAIT_FOR(posts() == before + 1, 2000);
	CHECK(posts() == before + 1 && posted[before].type == REDWOOD_BUTTON_DOWN && posted[before].button == button,
	    "%s pressed: %u records, the last of type %u for button %u", name, posts() - before, posted[before].type,
	    posted[before].button);
	release();
	WAIT_FOR(posts() == before + 2, 2000);
	CHECK(posts() == before + 2 && posted[before + 1].type == REDWOOD_BUTTON_UP && posted[before + 1].button == button,
	    "%s released", name);
}

static void power_down(void) { set_pon(PON_KPDPWR); }
static void volume_down_down(void) { set_pon(PON_RESIN); }
static void pon_up(void) { set_pon(0); }
static void volume_up_down(void) { set_volume_up(true); }
static void volume_up_up(void) { set_volume_up(false); }

static void
no_write(void)
{
	CHECK(m.writes_elsewhere == 0, "the driver wrote something that is not a read command (%u)", m.writes_elsewhere);
	CHECK(m.reads_elsewhere == 0, "the driver read where nothing of the arbiter is (%u)", m.reads_elsewhere);
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

/* the three keys, each pressed and released, read where the board says they are */
static void
three_keys(void)
{
	phone("phone-cmd");
	boot_args = "buttons=1";
	redwood_buttons_init();
	WAIT_FOR(logged("step 5: watching power volume-up volume-down"), 3000);
	CHECK(logged("step 5: watching power volume-up volume-down"), "the driver watches all three: %s", log_text);
	CHECK(logged("channel 3 is the PMK8350's power-on block"), "the power-on block's channel, by the board's name for it");
	CHECK(logged("channel 5 is GPIO6 of the PM8350 (0x18d00); written by system 3"), "the pin's channel of another system");
	CHECK(logged("channel 9 is GPIO6 of the PM8350 (0x18d00); written by system 0"), "... and the one that is this system's");
	CHECK(logged("read the type of GPIO6 of the PM8350 through channel 9"), "which is the one read through");
	usleep(100 * 1000);
	CHECK(posts() == 0, "nothing is reported while nothing is pressed (%u)", posts());
	press_and_release(power_down, pon_up, REDWOOD_BUTTON_LOCK, "power");
	press_and_release(volume_down_down, pon_up, REDWOOD_BUTTON_VOLUME_DOWN, "volume down");
	press_and_release(volume_up_down, volume_up_up, REDWOOD_BUTTON_VOLUME_UP, "volume up");
	no_write();
}

/* another phone: the arbiter elsewhere, this system its third, the keys on other peripherals. All of it
 * is the board's and its SoC's data, so the same driver reads the same three keys. */
static void
another_wiring(void)
{
	phone("phone-keys");
	boot_args = "buttons=1";
	redwood_buttons_init();
	WAIT_FOR(logged("step 5: watching power volume-up volume-down"), 3000);
	CHECK(logged("map the PMIC arbiter (0x4440000, 0x440a000, 0x6600000)"), "the arbiter where the SoC file has it: %s", log_text);
	CHECK(logged("channel 3 is the PMX200's power-on block (0x20800); written by system 2"), "its power-on block");
	CHECK(logged("channel 9 is GPIO9 of the PMX200 (0x2c800); written by system 2"), "its pin, through this system's channel");
	CHECK(logged("registers 0xc840 to 0xc847, 0xc810"), "the registers said are the peripheral's own");
	CHECK(logged("step 5: watching power volume-up volume-down"), "all three are watched");
	press_and_release(power_down, pon_up, REDWOOD_BUTTON_LOCK, "power");
	press_and_release(volume_down_down, pon_up, REDWOOD_BUTTON_VOLUME_DOWN, "volume down");
	press_and_release(volume_up_down, volume_up_up, REDWOOD_BUTTON_VOLUME_UP, "volume up");
	no_write();
}

/* on with the touch screen when nothing is said of the buttons, off when buttons=0 says so */
static void
with_the_touch_screen(void)
{
	phone("phone-cmd");
	boot_args = "touchscreen=1";
	redwood_buttons_init();
	WAIT_FOR(logged("step 5: watching"), 3000);
	CHECK(logged("step 5: watching"), "the keys come with the touch screen");
}

static void
off_when_told(void)
{
	phone("phone-cmd");
	boot_args = "touchscreen=1 buttons=0";
	redwood_buttons_init();
	usleep(200 * 1000);
	CHECK(m.transfers == 0 && log_text[0] == 0, "buttons=0: not a read, not a word (%u transfers)", m.transfers);
}

/* a key that is held when the driver first looks is not believed until it has been seen released */
static void
held_at_start(void)
{
	phone("phone-cmd");
	set_volume_up(true);
	boot_args = "buttons=1";
	redwood_buttons_init();
	WAIT_FOR(logged("step 5: watching"), 3000);
	CHECK(logged("a key reads as held before anything was watched: volume-up"), "said in the log");
	usleep(200 * 1000);
	CHECK(posts() == 0, "a pin that reads low from the start is not a key held down (%u records)", posts());
	set_volume_up(false);
	WAIT_FOR(logged("volume up is released now"), 2000);
	CHECK(logged("volume up is released now: believed from here on"), "believed once released");
	press_and_release(volume_up_down, volume_up_up, REDWOOD_BUTTON_VOLUME_UP, "volume up");
	no_write();
}

/* a peripheral that is not of the kind the driver knows is left alone; the other keys work */
static void
another_part_at_the_pin(void)
{
	phone("phone-cmd");
	m.pin.reg[0x04] = 0x21;                 /* not a GPIO */
	boot_args = "buttons=1";
	redwood_buttons_init();
	WAIT_FOR(logged("step 5: watching power volume-down,"), 3000);
	CHECK(logged("which is not what this driver knows"), "said in the log");
	CHECK(logged("step 5: watching power volume-down,"), "power and volume down are watched, volume up is not");
	press_and_release(power_down, pon_up, REDWOOD_BUTTON_LOCK, "power");
	set_volume_up(true);
	usleep(200 * 1000);
	CHECK(posts() == 2, "nothing is believed of a pin that is something else (%u records)", posts());
	no_write();
}

/* a pin that is not an enabled input with a pull-up is not made one: that would be a write to the PMIC */
static void
pin_not_set_up(void)
{
	phone("phone-cmd");
	m.pin.reg[0x42] = 0x05;                 /* no pull */
	boot_args = "buttons=1";
	redwood_buttons_init();
	WAIT_FOR(logged("step 5: watching"), 3000);
	CHECK(logged("volume up is left alone"), "said in the log");
	no_write();
}

/* an arbiter of another version has other registers: nothing is read through it */
static void
another_arbiter(void)
{
	phone("phone-cmd");
	m.version = 0x70000000;
	boot_args = "buttons=1";
	redwood_buttons_init();
	WAIT_FOR(logged("no buttons"), 3000);
	CHECK(logged("this driver knows version 5 of the arbiter only"), "said in the log: %s", log_text);
	usleep(100 * 1000);
	CHECK(m.transfers == 0, "not one transfer on the bus (%u)", m.transfers);
	no_write();
}

/* nobody at the power-on block's address: its keys are left out, the read is said to have failed */
static void
nobody_there(void)
{
	phone("phone-cmd");
	m.pon.silent = true;
	boot_args = "buttons=1";
	redwood_buttons_init();
	WAIT_FOR(logged("step 5: watching volume-up,"), 3000);
	CHECK(logged("the transfer failed"), "said in the log");
	CHECK(logged("step 5: watching volume-up,"), "volume up alone is watched");
	no_write();
}

/* only the first steps: the arbiter is looked at, nothing is read from a PMIC */
static void
first_steps_only(void)
{
	phone("phone-cmd");
	boot_args = "buttons=1 buttons_steps=2";
	redwood_buttons_init();
	WAIT_FOR(logged("stopping here (buttons_steps=2)"), 3000);
	CHECK(logged("stopping here (buttons_steps=2)"), "stops where told");
	CHECK(m.transfers == 0, "no transfer on the bus yet (%u)", m.transfers);
}

/* the emulator's board lists no keys: the driver says which line is missing and touches nothing */
static void
board_without_keys(void)
{
	phone("qemu-virt");
	boot_args = "buttons=1";
	redwood_buttons_init();
	usleep(100 * 1000);
	CHECK(logged("lists no \"buttons-pmic\""), "said in the log: %s", log_text);
	CHECK(m.transfers == 0 && m.reads_elsewhere == 0 && m.writes_elsewhere == 0, "nothing read or written");
}

static void
unknown_board(void)
{
	board = "a-phone-nobody-described";
	boot_args = "buttons=1";
	redwood_buttons_init();
	usleep(100 * 1000);
	CHECK(logged("lists no \"buttons-pmic\""), "said in the log: %s", log_text);
	CHECK(m.reads_elsewhere == 0 && m.writes_elsewhere == 0, "nothing read or written");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "three_keys", three_keys }, { "another_wiring", another_wiring }, { "with_the_touch_screen", with_the_touch_screen }, { "off_when_told", off_when_told },
		{ "held_at_start", held_at_start }, { "another_part_at_the_pin", another_part_at_the_pin },
		{ "pin_not_set_up", pin_not_set_up }, { "another_arbiter", another_arbiter }, { "nobody_there", nobody_there },
		{ "first_steps_only", first_steps_only }, { "board_without_keys", board_without_keys },
		{ "unknown_board", unknown_board },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			/* the driver's own printing of its log is not the test's result (the log is kept: logged()) */
			if (freopen("/dev/null", "w", stdout) == NULL) {
				return 2;
			}
			scenarios[i].run();
			fprintf(stderr, "%-20s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
