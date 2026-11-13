/*
 * The buttons' driver itself (kernel/overlay/bsd/dev/board/buttons.c), run as a host program: the
 * kernel's interfaces are workbench/fakekernel, the registers a model of the SoC's PMIC arbiter (version 5,
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

/* ---- the hardware's model: the arbiter (spmi_model.h) and the two peripherals the keys are on ---- */

#include "spmi_model.h"

#define PON_KPDPWR      0x80
#define PON_RESIN       0x40

static struct spmi_peripheral pon_model, pin_model, other_model;

uint32_t hakios_test_rd(vm_offset_t address) { return spmi_model_rd(address); }
void hakios_test_wr(vm_offset_t address, uint32_t value) { spmi_model_wr(address, value); }

vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }

/* the driver's includes are the kernel's: the fake kernel answers for them, the input queue is below.
 * (The kernel's uint64_t is an unsigned long long, which is what the driver prints it as; the host's is
 * not everywhere, and what is tested here is not the spelling of a format.) */
#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#include "board/board.c"
#include "board/buttons.c"

/* ---- what the driver reports: the records it posts to the input queue --------------------------- */

static struct { uint32_t type, button; } posted[64];
static unsigned posted_n;
static pthread_mutex_t posted_lock = PTHREAD_MUTEX_INITIALIZER;

void
hakios_touch_post(uint32_t type, uint32_t finger, uint32_t x __unused, uint32_t y __unused)
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
	spmi_set(&pon_model, 0x10, status);
}

static void
set_volume_up(bool held)
{
	spmi_set(&pin_model, 0x10, held ? 0 : 1);       /* the key pulls the pin low */
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
	spmi.core = b->spmi.core;
	spmi.cnfg = b->spmi.cnfg;
	spmi.channels = b->spmi.channels;
	spmi.ee = b->spmi.ee;
	spmi.observer = b->spmi.observer + 0x10000 * spmi.ee;
	pon_model.id = b->buttons.pon;
	pin_model.id = b->buttons.volume_up;
	other_model.id = 0x3c0;                 /* somebody else's peripheral, in the table between the two */
	/* channels 3, 7 and 9; the pin also has a channel of another system's, as real tables do */
	spmi_channel(3, &pon_model, spmi.ee);
	spmi_channel(5, &pin_model, 3);
	spmi_channel(7, &other_model, spmi.ee);
	spmi_channel(9, &pin_model, spmi.ee);
	pon_model.reg[0x04] = 0x01;             /* a power-on block, the half for the operating system */
	pon_model.reg[0x05] = 0x09;
	pin_model.reg[0x04] = 0x10;             /* a GPIO of the low voltage kind */
	pin_model.reg[0x05] = 0x10;
	pin_model.reg[0x40] = 0x00;             /* an input */
	pin_model.reg[0x42] = 0x00;             /* pulled up */
	pin_model.reg[0x46] = 0x80;             /* switched on */
	set_pon(0);
	set_volume_up(false);
}

static void
press_and_release(void (*press)(void), void (*release)(void), uint32_t button, const char *name)
{
	unsigned before = posts();

	press();
	WAIT_FOR(posts() == before + 1, 2000);
	CHECK(posts() == before + 1 && posted[before].type == HAKIOS_BUTTON_DOWN && posted[before].button == button,
	    "%s pressed: %u records, the last of type %u for button %u", name, posts() - before, posted[before].type,
	    posted[before].button);
	release();
	WAIT_FOR(posts() == before + 2, 2000);
	CHECK(posts() == before + 2 && posted[before + 1].type == HAKIOS_BUTTON_UP && posted[before + 1].button == button,
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
	CHECK(spmi.writes == 0 && spmi.denied == 0, "the driver wrote to a PMIC (%u writes, %u refused)", spmi.writes, spmi.denied);
	CHECK(spmi.elsewhere == 0, "the driver read or wrote where nothing of the arbiter is (%u)", spmi.elsewhere);
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

/* the three keys, each pressed and released, read where the board says they are */
static void
three_keys(void)
{
	phone("phone-cmd");
	boot_args = "buttons=1";
	hakios_buttons_init();
	WAIT_FOR(logged("step 5: watching power volume-up volume-down"), 3000);
	CHECK(logged("step 5: watching power volume-up volume-down"), "the driver watches all three: %s", log_text);
	CHECK(logged("channel 3 is the PMK8350's power-on block"), "the power-on block's channel, by the board's name for it");
	CHECK(logged("channel 5 is GPIO6 of the PM8350 (0x18d00); written by system 3"), "the pin's channel of another system");
	CHECK(logged("channel 9 is GPIO6 of the PM8350 (0x18d00); written by system 0"), "... and the one that is this system's");
	CHECK(logged("read the type of GPIO6 of the PM8350 through channel 9"), "which is the one read through");
	usleep(100 * 1000);
	CHECK(posts() == 0, "nothing is reported while nothing is pressed (%u)", posts());
	press_and_release(power_down, pon_up, HAKIOS_BUTTON_LOCK, "power");
	press_and_release(volume_down_down, pon_up, HAKIOS_BUTTON_VOLUME_DOWN, "volume down");
	press_and_release(volume_up_down, volume_up_up, HAKIOS_BUTTON_VOLUME_UP, "volume up");
	no_write();
}

/* another phone: the arbiter elsewhere, this system its third, the keys on other peripherals. All of it
 * is the board's and its SoC's data, so the same driver reads the same three keys. */
static void
another_wiring(void)
{
	phone("phone-keys");
	boot_args = "buttons=1";
	hakios_buttons_init();
	WAIT_FOR(logged("step 5: watching power volume-up volume-down"), 3000);
	CHECK(logged("map the PMIC arbiter (0x4440000, 0x440a000, 0x6600000)"), "the arbiter where the SoC file has it: %s", log_text);
	CHECK(logged("channel 3 is the PMX200's power-on block (0x20800); written by system 2"), "its power-on block");
	CHECK(logged("channel 9 is GPIO9 of the PMX200 (0x2c800); written by system 2"), "its pin, through this system's channel");
	CHECK(logged("registers 0xc840 to 0xc847, 0xc810"), "the registers said are the peripheral's own");
	CHECK(logged("step 5: watching power volume-up volume-down"), "all three are watched");
	press_and_release(power_down, pon_up, HAKIOS_BUTTON_LOCK, "power");
	press_and_release(volume_down_down, pon_up, HAKIOS_BUTTON_VOLUME_DOWN, "volume down");
	press_and_release(volume_up_down, volume_up_up, HAKIOS_BUTTON_VOLUME_UP, "volume up");
	no_write();
}

/* on with the touch screen when nothing is said of the buttons, off when buttons=0 says so */
static void
with_the_touch_screen(void)
{
	phone("phone-cmd");
	boot_args = "touchscreen=1";
	hakios_buttons_init();
	WAIT_FOR(logged("step 5: watching"), 3000);
	CHECK(logged("step 5: watching"), "the keys come with the touch screen");
}

static void
off_when_told(void)
{
	phone("phone-cmd");
	boot_args = "touchscreen=1 buttons=0";
	hakios_buttons_init();
	usleep(200 * 1000);
	CHECK(spmi.transfers == 0 && log_text[0] == 0, "buttons=0: not a read, not a word (%u transfers)", spmi.transfers);
}

/* a key that is held when the driver first looks is not believed until it has been seen released */
static void
held_at_start(void)
{
	phone("phone-cmd");
	set_volume_up(true);
	boot_args = "buttons=1";
	hakios_buttons_init();
	WAIT_FOR(logged("step 5: watching"), 3000);
	CHECK(logged("a key reads as held before anything was watched: volume-up"), "said in the log");
	usleep(200 * 1000);
	CHECK(posts() == 0, "a pin that reads low from the start is not a key held down (%u records)", posts());
	set_volume_up(false);
	WAIT_FOR(logged("volume up is released now"), 2000);
	CHECK(logged("volume up is released now: believed from here on"), "believed once released");
	press_and_release(volume_up_down, volume_up_up, HAKIOS_BUTTON_VOLUME_UP, "volume up");
	no_write();
}

/*
 * What the power-on block says at each of the driver's looks, one after another: for what a key that
 * bounces, or a read that comes out wrong once, must not be taken for.
 */
static const uint8_t *plan;
static unsigned plan_length, plan_at;

static void
pon_looked_at(struct spmi_peripheral *p, uint8_t reg)
{
	if (reg == 0x10 && plan_at < plan_length) {
		p->reg[0x10] = plan[plan_at++];
	}
}

static unsigned
said_times(const char *text)
{
	unsigned count = 0;

	for (const char *at = log_text; (at = strstr(at, text)) != NULL; at += strlen(text)) {
		count++;
	}
	return count;
}

/* a key is believed when two looks in a row agree: one look that differs is nothing, pressed or released */
static void
one_look_is_not_a_press(void)
{
	static const uint8_t looks[] = {
		0, PON_KPDPWR, 0, 0,                    /* pressed at one look only: nothing */
		PON_KPDPWR, PON_KPDPWR,                 /* at two: pressed */
		0, PON_KPDPWR, PON_KPDPWR,              /* released at one look only while held: still held */
		0, 0,                                   /* at two: released */
		PON_RESIN, 0, PON_RESIN, 0, 0,          /* the other key of the block, never twice in a row: nothing */
	};

	phone("phone-cmd");
	boot_args = "buttons=1";
	hakios_buttons_init();
	WAIT_FOR(logged("step 5: watching power volume-up volume-down"), 3000);
	usleep(100 * 1000);
	CHECK(posts() == 0, "nothing before (%u)", posts());
	plan = looks;
	plan_length = sizeof(looks);
	pon_model.looked_at = pon_looked_at;
	WAIT_FOR(plan_at == sizeof(looks), 3000);
	usleep(100 * 1000);
	CHECK(plan_at == sizeof(looks), "every look of the plan was taken: %u", plan_at);
	CHECK(posts() == 2 && posted[0].type == HAKIOS_BUTTON_DOWN && posted[0].button == HAKIOS_BUTTON_LOCK &&
	    posted[1].type == HAKIOS_BUTTON_UP && posted[1].button == HAKIOS_BUTTON_LOCK, "one press and one release of power, and nothing else: %u records",
	    posts());
	no_write();
}

/* what the driver says of a run: how many presses of each key, in the summary it gives early on */
static void
presses_are_counted(void)
{
	phone("phone-cmd");
	fake_delay_divisor = 20;                /* the summary comes after 10 s of looks: half a second here */
	boot_args = "buttons=1";
	hakios_buttons_init();
	WAIT_FOR(logged("step 5: watching power volume-up volume-down"), 3000);
	press_and_release(power_down, pon_up, HAKIOS_BUTTON_LOCK, "power");
	press_and_release(power_down, pon_up, HAKIOS_BUTTON_LOCK, "power");
	press_and_release(volume_up_down, volume_up_up, HAKIOS_BUTTON_VOLUME_UP, "volume up");
	WAIT_FOR(logged("buttons: so far"), 30000);       /* (as many looks as 10 s hold: long on a busy machine) */
	CHECK(logged("buttons: so far 2 presses of power, 1 of volume up, 0 of volume down; now none held"), "the summary: %s", log_text);
	no_write();
}

/* a PMIC that stops answering while its keys are watched: said a few times, not at every look; then the driver gives up */
static void
bus_stops_answering(void)
{
	unsigned transfers;

	phone("phone-cmd");
	fake_delay_divisor = 40;
	boot_args = "buttons=1";
	hakios_buttons_init();
	WAIT_FOR(logged("step 5: watching power volume-up volume-down"), 3000);
	pon_model.silent = true;
	pin_model.silent = true;
	WAIT_FOR(logged("giving up"), 20000);
	CHECK(logged("more than half of") && logged("reads have failed: giving up"), "said: gives up");
	CHECK(said_times("buttons: reading ") == 8, "the failing reads are said 8 times, not %u", said_times("buttons: reading "));
	CHECK(posts() == 0, "a read that fails is no key (%u records)", posts());
	transfers = spmi.transfers;
	usleep(200 * 1000);
	CHECK(spmi.transfers == transfers, "and the bus is left alone after that (%u more transfers)", spmi.transfers - transfers);
	no_write();
}

/* a peripheral that is not of the kind the driver knows is left alone; the other keys work */
static void
another_part_at_the_pin(void)
{
	phone("phone-cmd");
	pin_model.reg[0x04] = 0x21;                 /* not a GPIO */
	boot_args = "buttons=1";
	hakios_buttons_init();
	WAIT_FOR(logged("step 5: watching power volume-down,"), 3000);
	CHECK(logged("which is not what this driver knows"), "said in the log");
	CHECK(logged("step 5: watching power volume-down,"), "power and volume down are watched, volume up is not");
	press_and_release(power_down, pon_up, HAKIOS_BUTTON_LOCK, "power");
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
	pin_model.reg[0x42] = 0x05;                 /* no pull */
	boot_args = "buttons=1";
	hakios_buttons_init();
	WAIT_FOR(logged("step 5: watching"), 3000);
	CHECK(logged("volume up is left alone"), "said in the log");
	no_write();
}

/* an arbiter of another version has other registers: nothing is read through it */
static void
another_arbiter(void)
{
	phone("phone-cmd");
	spmi.version = 0x70000000;
	boot_args = "buttons=1";
	hakios_buttons_init();
	WAIT_FOR(logged("no buttons"), 3000);
	CHECK(logged("this driver knows version 5 of the arbiter only"), "said in the log: %s", log_text);
	usleep(100 * 1000);
	CHECK(spmi.transfers == 0, "not one transfer on the bus (%u)", spmi.transfers);
	no_write();
}

/* nobody at the power-on block's address: its keys are left out, the read is said to have failed */
static void
nobody_there(void)
{
	phone("phone-cmd");
	pon_model.silent = true;
	boot_args = "buttons=1";
	hakios_buttons_init();
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
	hakios_buttons_init();
	WAIT_FOR(logged("stopping here (buttons_steps=2)"), 3000);
	CHECK(logged("stopping here (buttons_steps=2)"), "stops where told");
	CHECK(spmi.transfers == 0, "no transfer on the bus yet (%u)", spmi.transfers);
}

/* the emulator's board lists no keys: the driver says which line is missing and touches nothing */
static void
board_without_keys(void)
{
	phone("qemu-virt");
	boot_args = "buttons=1";
	hakios_buttons_init();
	usleep(100 * 1000);
	CHECK(logged("lists no \"buttons-pmic\""), "said in the log: %s", log_text);
	CHECK(spmi.transfers == 0 && spmi.elsewhere == 0 && spmi.writes == 0, "nothing read or written");
}

static void
unknown_board(void)
{
	board = "a-phone-nobody-described";
	boot_args = "buttons=1";
	hakios_buttons_init();
	usleep(100 * 1000);
	CHECK(logged("lists no \"buttons-pmic\""), "said in the log: %s", log_text);
	CHECK(spmi.elsewhere == 0 && spmi.writes == 0, "nothing read or written");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "three_keys", three_keys }, { "another_wiring", another_wiring }, { "with_the_touch_screen", with_the_touch_screen }, { "off_when_told", off_when_told },
		{ "held_at_start", held_at_start }, { "one_look_is_not_a_press", one_look_is_not_a_press },
		{ "presses_are_counted", presses_are_counted }, { "bus_stops_answering", bus_stops_answering }, { "another_part_at_the_pin", another_part_at_the_pin },
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
