/*
 * The vibration motor's driver itself (drivers/board/haptic.c), run as a host program
 * against the model of the SoC's pins with the motor's chip on them: an I2C device (i2c_device_model.h)
 * that answers only while its reset pin is driven high. This driver moves a motor and switches a
 * boost converter; what is tested here is what it does before it writes anything to the chip, and
 * that the chip is back in reset when something is not as expected or the run ends.
 *
 *   haptic_driver_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <unistd.h>

#include "fakekernel.h"

/* which pins the chip is on is the board's: the model is put where the board the driver reads says */
static unsigned sda_pin = 36, scl_pin = 37, reset_pin_of_chip = 35;
#define I2C_DEVICE_SDA          sda_pin
#define I2C_DEVICE_SCL          scl_pin
#define I2C_DEVICE_ADDRESS      0x5a
#include "i2c_device_model.h"

extern char log_text[];

#define RESET           reset_pin_of_chip
#define AS_FOUND        (1u << 2 | 3u)          /* the bus's pins: the serial engine's function, pulled up */
#define HELD_IN_RESET   1u                      /* the reset pin as a boot loader leaves it: an input, pulled down */

static unsigned resets_released;                /* times the reset pin went from low to high */
static bool was_high;

/* what is written into the chip, for the levels that write */
static bool register_written[256];
static unsigned analog_while_closed;            /* a write to the analog registers (0x70..) without their lock opened */
static int deaf_register = -1;                  /* a register that does not take what is written into it */

/* the chip's RAM, behind its address registers (0x40 high, 0x41 low) and its data port (0x42), the address
 * counting up with each byte either way; ram_wrong_at: the address that reads back other than it was written */
static uint8_t ram[0x1000];
static unsigned ram_at;
static int ram_wrong_at = -1;
static unsigned ram_written_closed;             /* bytes into the RAM's data port while the RAM was not opened (0x45 bit 2) */

/* the waveforms started (0x09 <- go), and with keeps_playing a chip that plays on until it is told to stop: when it
 * was started and when told to stop, by the monotonic clock (the driver's mach_absolute_time()), in ms */
static unsigned waveforms_started;
static bool keeps_playing;
static uint64_t go_ms, stop_ms;

static uint64_t
clock_ms(void)
{
	return mach_absolute_time() / 1000000;
}

static void
chip_written(uint8_t reg, uint8_t value)
{
	register_written[reg] = true;
	analog_while_closed += reg >= 0x70 && device.reg[0x5b] != 0x7d;
	if (reg == 0x40) {
		ram_at = (unsigned)value << 8 | (ram_at & 0xff);
	} else if (reg == 0x41) {
		ram_at = (ram_at & 0xff00) | value;
	} else if (reg == 0x42) {
		ram_written_closed += (device.reg[0x45] & 0x04) == 0;
		ram[ram_at++ % sizeof(ram)] = value;
	} else if (reg == 0x09 && value == 0x01) {
		waveforms_started++;
		go_ms = clock_ms();
		device.reg[0x3f] = keeps_playing ? 7 : device.reg[0x3f];        /* (its state: playing) */
	} else if (reg == 0x09 && value == 0x02 && keeps_playing) {
		stop_ms = clock_ms();
		device.reg[0x3f] = 0;                                           /* (standby) */
	}
	if (reg != deaf_register) {
		device.reg[reg] = value;
	}
}

static uint8_t
chip_read(uint8_t reg)
{
	if (reg == 0x42) {
		uint8_t value = ram[ram_at % sizeof(ram)];

		return (int)ram_at++ == ram_wrong_at ? value ^ 0xff : value;
	}
	return device.reg[reg];
}

/* the chip answers while its reset pin is driven high */
static void
pins_changed(void)
{
	bool high = (model.cfg[RESET] & 1u << 9) && (model.io[RESET] & 2);

	resets_released += high && !was_high;
	was_high = high;
	device.absent = !high;
	changed();
}

uint32_t
hakios_test_rd(vm_offset_t address)
{
	i2c_device_looked_at(address);
	return tlmm_model_rd(address);
}

void hakios_test_wr(vm_offset_t address, uint32_t value) { tlmm_model_wr(address, value); }
/* the IOKit service Darwin's haptic server waits for (iokit/Kernel/BoardHaptics.cpp): counted, not made; and taken away */
static unsigned haptics_published, haptics_gone;
void HakiosHapticsReady(void);
void HakiosHapticsReady(void) { haptics_published++; }
void HakiosHapticsGone(void);
void HakiosHapticsGone(void) { haptics_gone++; }
vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }

/*
 * The small image's files, which only the levels that load waveforms ask for: none, unless a scenario makes
 * Android's file of waveforms -- its checksum, the address the RAM's contents begin at (0x0800), a version
 * byte, each waveform's start and end, the waveforms
 */
static uint8_t waveforms[1024];
static size_t waveforms_size;

const uint8_t *
hakios_wifi_file(const char *name, uint64_t *size)
{
	if (waveforms_size == 0 || strcmp(name, "aw8697_haptic.bin") != 0) {
		return NULL;
	}
	*size = waveforms_size;
	return waveforms;
}

/* ... each waveform `lengths[w]' bytes long, or `each' for all of them when there is no `lengths' */
static void
make_waveforms_of(unsigned count, unsigned each, const unsigned *lengths)
{
	unsigned at = 4, start = 0x0800 + 1 + 4 * count, sum = 0;

	waveforms[2] = 0x08;
	waveforms[3] = 0x00;
	waveforms[at++] = 0x01;
	for (unsigned w = 0; w < count; w++) {
		unsigned length = lengths != NULL ? lengths[w] : each, end = start + length - 1;

		waveforms[at++] = (uint8_t)(start >> 8);
		waveforms[at++] = (uint8_t)start;
		waveforms[at++] = (uint8_t)(end >> 8);
		waveforms[at++] = (uint8_t)end;
		start = end + 1;
	}
	for (unsigned w = 0; w < count; w++) {
		for (unsigned i = 0; i < (lengths != NULL ? lengths[w] : each); i++) {
			waveforms[at++] = (uint8_t)(w * 31 + i * 7);
		}
	}
	for (unsigned i = 2; i < at; i++) {
		sum += waveforms[i];
	}
	waveforms[0] = (uint8_t)(sum >> 8);
	waveforms[1] = (uint8_t)sum;
	waveforms_size = at;
}

static void
make_waveforms(unsigned count, unsigned each)
{
	make_waveforms_of(count, each, NULL);
}
void hakios_haptic_init(void);
void hakios_haptic_quiesce(void);

#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#define get haptic_get
#include "board/board.c"
#include "board/haptic.c"
#undef get

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited = 0; !(cond) && waited < (ms); waited += 5) usleep(5000); } while (0)

/* a phone with the part: the pins where its SoC file has them, the chip held in reset, the bus at rest */
static void
phone(const char *name, uint16_t chip)
{
	board = name;
	fake_delay_divisor = 5;
	tlmm_model_at(hakios_board() != NULL ? hakios_board()->pins.tlmm : 0);
	if (hakios_board() != NULL && hakios_board()->haptic.sda != hakios_board()->haptic.scl) {
		sda_pin = hakios_board()->haptic.sda;
		scl_pin = hakios_board()->haptic.scl;
		reset_pin_of_chip = hakios_board()->haptic.reset;
	}
	model.after_write = pins_changed;
	model.cfg[I2C_DEVICE_SDA] = model.cfg[I2C_DEVICE_SCL] = AS_FOUND;
	model.cfg[RESET] = HELD_IN_RESET;
	memset(device.reg, 0, sizeof(device.reg));
	device.reg[0x57] = (uint8_t)(chip >> 8);
	device.reg[0x58] = (uint8_t)chip;
	device.written = chip_written;
	device.read = chip_read;
	device.streams = true;
	device.stream_register = 0x42;
	device.absent = true;
	lines();
	device.was_sda = device.was_scl = true;
}

static bool
in_reset(void)
{
	return !((model.cfg[RESET] & 1u << 9) && (model.io[RESET] & 2));
}

static void
never(void)
{
	CHECK(device.writes == 0, "%u bytes were written into the chip's registers", device.writes);
	CHECK(device.driven_high == 0, "a line of the bus was driven high %u times", device.driven_high);
	CHECK(model.elsewhere == 0, "%u accesses outside the pins' registers", model.elsewhere);
	CHECK(model.cfg[I2C_DEVICE_SDA] == AS_FOUND && model.cfg[I2C_DEVICE_SCL] == AS_FOUND, "the bus's pins are not as they were found");
	for (unsigned i = 0; i < PINS; i++) {
		if (i != I2C_DEVICE_SDA && i != I2C_DEVICE_SCL && i != RESET && (model.cfg[i] != 0 || model.io[i] != 0)) {
			CHECK(false, "pin %u was touched", i);
			break;
		}
	}
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

/* without its boot argument: nothing */
static void
off_by_default(void)
{
	phone("phone-video", 0x9270);
	boot_args = "";
	hakios_haptic_init();
	usleep(50000);
	CHECK(model.writes == 0 && !logged("haptic:"), "nothing written, nothing said: %s", log_text);
	hakios_haptic_quiesce();
	CHECK(model.writes == 0, "and nothing at the run's end");
}

/* haptic=1: a look. The chip is in reset and stays there: it does not answer, and that is said */
static void
look_only(void)
{
	phone("phone-video", 0x9270);
	boot_args = "haptic=1";
	hakios_haptic_init();
	WAIT_FOR(logged("haptic: done"), 5000);
	CHECK(logged("haptic: level 1: a look, reading only"), "the level is said: %s", log_text);
	CHECK(logged("nobody answers at 0x5a") && logged("does not drive high: the chip is held in reset or not powered"),
	    "the chip in reset does not answer, and why is said");
	CHECK(model.cfg[RESET] == HELD_IN_RESET && resets_released == 0, "the reset pin is as it was found: %#x", model.cfg[RESET]);
	CHECK(!logged("PUT INTO RESET"), "nothing is given up that was never taken");
	CHECK(logged("function 0, pull 1, 2 mA, input") && logged("function 1, pull 3, 2 mA, input"),
	    "each pin as it was found, its drive strength in mA: %s", log_text);
	never();
	hakios_haptic_quiesce();
	CHECK(model.cfg[RESET] == HELD_IN_RESET, "nor at the run's end");
}

/* haptic=2: the reset released as Android releases it, the chip found, and nothing written to it */
static void
reset_released(void)
{
	phone("phone-video", 0x9270);
	device.reg[0x7c] = 0x11;                /* the last register compared, other than Android's */
	device.reg[0x32] = 0x22;                /* a data port: not read, so not compared */
	boot_args = "haptic=2";
	hakios_haptic_init();
	WAIT_FOR(logged("haptic: done"), 10000);
	CHECK(logged("haptic: the reset is released: gpio35 low for 5 ms, then high") && resets_released == 1, "released once: %u",
	    resets_released);
	CHECK(logged("haptic: 0x7c = 0x11 (Android leaves 0x00)") && logged("haptic: 0x03 = 0x00 (Android leaves 0x59)"),
	    "the registers that differ are said, up to the last: %s", log_text);
	CHECK(!logged("haptic: 0x32 =") && !logged("haptic: 0x42 =") && !logged("haptic: 0x57 =") && !logged("haptic: 0x58 ="),
	    "the data ports are not read, and a register as Android leaves it is not said: %s", log_text);
	CHECK(logged("haptic: the chip at 0x5a is 0x9270, an AW86927"), "the chip answers and is the one expected: %s", log_text);
	CHECK(logged("registers differ from what Android leaves in them"), "its registers are read and compared");
	CHECK(!in_reset(), "the chip stays out of reset while the run goes on");
	never();
	hakios_haptic_quiesce();
	CHECK(in_reset(), "at the run's end the chip is back in reset");
	never();
}

/* the reset found released already (a run before this that did not get to put it back): this run's end puts it back */
static void
found_released(void)
{
	phone("phone-video", 0x9270);
	model.cfg[RESET] = 1u << 9;             /* an output ... */
	model.io[RESET] = 2;                    /* ... driving high */
	pins_changed();                         /* (the chip out of reset, as it was left) */
	resets_released = 0;
	boot_args = "haptic=2";
	hakios_haptic_init();
	WAIT_FOR(logged("haptic: done"), 10000);
	CHECK(logged("haptic: the reset is found released already (gpio35 drives high)") && resets_released == 0,
	    "said, not released again: %u", resets_released);
	CHECK(!in_reset(), "out of reset while the run goes on");
	hakios_haptic_quiesce();
	CHECK(in_reset(), "at the run's end the chip is put in reset all the same");
	never();
}

/* the registers Android's driver sets, and no others */
static bool
only_androids_registers(void)
{
	static const uint8_t allowed[] = { 0x03, 0x06, 0x1a, 0x1d, 0x1e, 0x2f, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x46, 0x47, 0x5b,
		                           0x70, 0x71, 0x72, 0x74, 0x75, 0x79 };

	for (unsigned reg = 0; reg < 256; reg++) {
		bool ok = false;

		for (unsigned i = 0; i < sizeof(allowed); i++) {
			ok |= allowed[i] == reg;
		}
		if (register_written[reg] && !ok) {
			fprintf(stderr, "register 0x%02x was written\n", reg);
			return false;
		}
	}
	return true;
}

/* haptic=3: Android's configuration, each write read back; the analog registers only while their lock is open */
static void
configured(void)
{
	/* what Android leaves in them, written down here a second time: the boost's voltage and the drive
	 * levels are the motor's limits, and a changed number in the driver is to fail this test */
	static const uint8_t androids[][2] = {
		{ 0x03, 0x59 }, { 0x06, 0x47 }, { 0x1a, 0x71 }, { 0x1d, 0xcf }, { 0x1e, 0x28 }, { 0x2f, 0x46 }, { 0x36, 0x02 },
		{ 0x37, 0x02 }, { 0x38, 0x02 }, { 0x39, 0x00 }, { 0x3a, 0x00 }, { 0x46, 0x09 }, { 0x47, 0x5a },
		{ 0x70, 0x0f }, { 0x71, 0x93 }, { 0x72, 0x69 }, { 0x74, 0x89 }, { 0x75, 0x88 }, { 0x79, 0x0d },
	};

	phone("phone-video", 0x9270);
	boot_args = "haptic=3";
	hakios_haptic_init();
	WAIT_FOR(logged("haptic: done"), 20000);
	CHECK(logged("haptic: Android's configuration is written"), "said: %s", log_text);
	for (unsigned i = 0; i < sizeof(androids) / sizeof(androids[0]); i++) {
		CHECK(register_written[androids[i][0]] && device.reg[androids[i][0]] == androids[i][1], "register 0x%02x holds 0x%02x, Android's is 0x%02x",
		    androids[i][0], device.reg[androids[i][0]], androids[i][1]);
	}
	CHECK(device.reg[0x5b] == 0 && analog_while_closed == 0, "their lock was open for them and is closed again (%u written while closed)",
	    analog_while_closed);
	CHECK(only_androids_registers(), "nothing but the registers Android's driver sets was written");
	CHECK(logged("no aw8697_haptic.bin in fw/"), "without the waveforms' file nothing is loaded, and that is said");
	CHECK(!logged("PUT INTO RESET") && !in_reset(), "nothing was wrong: the chip stays out of reset for the run");
	CHECK(device.driven_high == 0 && model.elsewhere == 0, "the bus was used as a bus");
	hakios_haptic_quiesce();
	CHECK(in_reset(), "at the run's end the chip is back in reset");
}

/* the same chip on other pins of another SoC, and another motor: all of it from the board */
static void
another_wiring(void)
{
	phone("phone-keys", 0x9270);
	boot_args = "haptic=3";
	hakios_haptic_init();
	WAIT_FOR(logged("haptic: done"), 20000);
	CHECK(model.base == 0x01100000 && sda_pin == 110 && scl_pin == 111 && reset_pin_of_chip == 9, "that board's pins: %#x, %u/%u, %u",
	    model.base, sda_pin, scl_pin, reset_pin_of_chip);
	CHECK(logged("haptic: the reset is released: gpio9 low for 5 ms, then high") && logged("is 0x9270, an AW86927"),
	    "the chip is found there: %s", log_text);
	CHECK(device.reg[0x79] == 0x1a && device.reg[0x06] == 0x47, "that motor's calibration, Android's values otherwise: %#x",
	    device.reg[0x79]);
	CHECK(model.cfg[36] == 0 && model.cfg[37] == 0 && model.cfg[35] == 0 && model.io[35] == 0, "the first phone's pins are not touched");
	CHECK(device.driven_high == 0 && model.elsewhere == 0 && analog_while_closed == 0, "and the rest is as on the first");
	hakios_haptic_quiesce();
	CHECK(in_reset(), "back in reset at the run's end");
}

/* a register that does not read back as written: the chip into reset, and nothing more written */
static void
write_does_not_stick(void)
{
	phone("phone-video", 0x9270);
	deaf_register = 0x1d;
	boot_args = "haptic=3";
	hakios_haptic_init();
	WAIT_FOR(logged("haptic: done"), 20000);
	CHECK(logged("0x1d <- 0xcf, reads 0x00: NOT AS WRITTEN"), "said which and what it reads: %s", log_text);
	CHECK(logged("a register did not read back as written: THE CHIP IS PUT INTO RESET") && in_reset(), "into reset");
	CHECK(!register_written[0x1e] && !register_written[0x70] && !register_written[0x5b], "and nothing after it was written");
	CHECK(only_androids_registers(), "nor anything else");
}

/* the chip reports a fault after the configuration: into reset */
static void
fault_reported(void)
{
	phone("phone-video", 0x9270);
	device.reg[0x02] = 0x20;
	boot_args = "haptic=3";
	hakios_haptic_init();
	WAIT_FOR(logged("haptic: done"), 20000);
	CHECK(logged("after the configuration: the chip reports 0x20: UNDER-VOLTAGE"), "said which: %s", log_text);
	CHECK(logged("a fault: THE CHIP IS PUT INTO RESET") && in_reset(), "into reset");
	CHECK(device.reg[0x5b] == 0, "the analog registers' lock is not left open");
}

/* a chip that is another: said, and put back into reset */
static void
another_chip(void)
{
	phone("phone-video", 0x9260);
	boot_args = "haptic=3";
	hakios_haptic_init();
	WAIT_FOR(logged("haptic: done"), 10000);
	CHECK(logged("haptic: the chip at 0x5a is 0x9260: NOT THE AW86927 EXPECTED"), "said: %s", log_text);
	CHECK(logged("THE CHIP IS PUT INTO RESET") && in_reset(), "and it is back in reset");
	never();
}

/* nobody answers after the reset is released: back into reset, nothing more */
static void
nobody_there(void)
{
	phone("phone-video", 0x9270);
	model.after_write = changed;            /* (no chip: the reset pin frees nobody) */
	boot_args = "haptic=4";
	hakios_haptic_init();
	WAIT_FOR(logged("haptic: done"), 10000);
	CHECK(logged("nobody answers at 0x5a") && logged("drives high: the chip should answer"), "said: %s", log_text);
	CHECK(logged("the chip does not answer: THE CHIP IS PUT INTO RESET") && in_reset(), "the pin is low again");
	never();
}

/* haptic=3 with Android's file: the waveforms into the chip's RAM from 0x0800 on, read back, the RAM closed again */
static void
waveforms_loaded(void)
{
	phone("phone-video", 0x9270);
	make_waveforms(3, 40);
	waveforms[waveforms_size] = 0x55;       /* (past the file's end: not in its sum) */
	boot_args = "haptic=3";
	hakios_haptic_init();
	WAIT_FOR(logged("haptic: done"), 20000);
	CHECK(ram_written_closed == 0 && register_written[0x2d] && register_written[0x2e], "the RAM opened before it is written, "
	    "both bytes of its base said: %u bytes written closed", ram_written_closed);
	CHECK(logged("haptic: Android's waveforms: 133 bytes from 0x800 on, 3 waveforms"), "the file as it is: %s", log_text);
	CHECK(memcmp(ram + 0x800, waveforms + 4, waveforms_size - 4) == 0, "every byte of it in the RAM, where it goes");
	CHECK(ram[0x7ff] == 0 && ram[0x800 + waveforms_size - 4] == 0, "and nothing around it");
	CHECK(device.reg[0x2d] == 0x08 && device.reg[0x2e] == 0x00, "the RAM's base said to the chip: %#x %#x", device.reg[0x2d], device.reg[0x2e]);
	CHECK(logged("are in the chip's RAM and read back as written") && (device.reg[0x45] & 0x04) == 0,
	    "read back, and the RAM closed again: %#x", device.reg[0x45]);
	CHECK(!in_reset(), "nothing was wrong");
}

/* a file whose checksum is not what it holds: not loaded */
static void
waveforms_do_not_add_up(void)
{
	phone("phone-video", 0x9270);
	make_waveforms(3, 40);
	waveforms[1] ^= 1;
	boot_args = "haptic=3";
	hakios_haptic_init();
	WAIT_FOR(logged("haptic: done"), 20000);
	CHECK(logged("aw8697_haptic.bin does not add up"), "said: %s", log_text);
	CHECK(!register_written[0x42] && !logged("are in the chip's RAM"), "nothing of it written");
}

/* the RAM reads back other than it was written: the chip into reset */
static void
ram_reads_back_wrong(void)
{
	phone("phone-video", 0x9270);
	make_waveforms(3, 40);
	ram_wrong_at = 0x800 + 50;
	boot_args = "haptic=3";
	hakios_haptic_init();
	WAIT_FOR(logged("haptic: done"), 20000);
	CHECK(logged("the RAM reads back differently at 0x800"), "said where: %s", log_text);
	CHECK(logged("THE CHIP IS PUT INTO RESET") && in_reset(), "into reset");
}

/* a file too short to hold even its header: as if there were none */
static void
waveforms_too_short(void)
{
	phone("phone-video", 0x9270);
	memset(waveforms, 0x11, 7);
	waveforms_size = 7;
	boot_args = "haptic=3";
	hakios_haptic_init();
	WAIT_FOR(logged("haptic: done"), 20000);
	CHECK(logged("no aw8697_haptic.bin in fw/") && !logged("does not add up"), "said: %s", log_text);
	CHECK(!register_written[0x42] && !register_written[0x2d] && !in_reset(), "nothing of it written, nothing wrong");
}

/* a file whose RAM part is a whole number of the pieces it is written and read in (64 bytes): all of it, no more */
static void
waveforms_in_whole_pieces(void)
{
	phone("phone-video", 0x9270);
	make_waveforms(1, 59);
	boot_args = "haptic=3";
	hakios_haptic_init();
	WAIT_FOR(logged("haptic: done"), 20000);
	CHECK(logged("haptic: Android's waveforms: 64 bytes from 0x800 on, 1 waveforms") &&
	    logged("are in the chip's RAM and read back as written"), "said: %s", log_text);
	CHECK(memcmp(ram + 0x800, waveforms + 4, 64) == 0 && ram[0x840] == 0, "the 64 bytes, and nothing after");
	CHECK(!in_reset(), "nothing was wrong");
}

/* haptic=4 haptic_effect=N: waveform N played once from the RAM, at Android's gain, then back in standby */
static void
an_effect_played(void)
{
	phone("phone-video", 0x9270);
	make_waveforms(3, 40);
	boot_args = "haptic=4 haptic_effect=3";
	hakios_haptic_init();
	WAIT_FOR(logged("haptic: done"), 20000);
	CHECK(logged("haptic: waveform 3 is played once") && logged("the waveform has played"), "said: %s", log_text);
	CHECK(device.reg[0x0a] == 3 && device.reg[0x0b] == 0 && device.reg[0x12] == 0, "waveform 3, then the end, not repeated");
	CHECK(device.reg[0x08] == 0x04 && register_written[0x09] && device.reg[0x09] == 0x01, "from the RAM, and started");
	CHECK(register_written[0x07], "at the gain set");
}

/* haptic_effect=10: the last of the short ones is played; 11 is not one of them, and waveform 1 is played instead */
static void
effect_played(const char *boot, unsigned played)
{
	phone("phone-video", 0x9270);
	make_waveforms(12, 40);
	boot_args = boot;
	hakios_haptic_init();
	WAIT_FOR(logged("haptic: done"), 20000);
	CHECK(device.reg[0x0a] == played, "waveform %u played (%u)", played, device.reg[0x0a]);
}

static void
the_last_short_effect(void)
{
	effect_played("haptic=4 haptic_effect=10", 10);
	CHECK(logged("haptic: waveform 10 is played once"), "said: %s", log_text);
}

static void
not_a_short_effect(void)
{
	effect_played("haptic=4 haptic_effect=11", 1);
	CHECK(logged("haptic: waveform 1 is played once") && !logged("waveform 11"), "said: %s", log_text);
}

/* what Darwin plays on its actuator, `ms' of it at that level, written in 5 ms pieces as the audio server writes */
static void
darwin_plays(int16_t level, unsigned ms)
{
	int16_t samples[240];

	for (unsigned i = 0; i < 240; i++) {
		samples[i] = (int16_t)(i % 2 ? -level : level);
	}
	for (unsigned t = 0; t < ms; t += 5) {
		CHECK(fake_dev_write("hakios-haptic", samples, sizeof(samples)) == 0, "written");
		usleep(5000);
	}
}

/* haptic=5: a short waveform of Darwin's is a tap, Android's waveform 1, at Android's gain when it is loud */
static void
a_tap_for_darwin(void)
{
	phone("phone-video", 0x9270);
	make_waveforms(11, 8);
	boot_args = "haptic=5";
	hakios_haptic_init();
	WAIT_FOR(logged("THE MOTOR PLAYS WHAT DARWIN PLAYS"), 20000);
	darwin_plays(20000, 20);
	WAIT_FOR(logged("haptic: tap 1:"), 2000);
	CHECK(logged("haptic: tap 1: Darwin's waveform peaks at 20000 of 32768: Android's waveform 1 at gain 0x80"), "said: %s", log_text);
	CHECK(device.reg[0x0a] == 1 && device.reg[0x12] == 0, "waveform 1, once");
	CHECK(device.reg[0x07] == 0x80 && device.reg[0x08] == 0x04, "at that gain, from the RAM: %#x %#x", device.reg[0x07], device.reg[0x08]);
	CHECK(!logged("haptic: vibration"), "a tap, not a vibration");
}

/*
 * Android's waveforms of different lengths: the third the shortest of the short ones, the eleventh (the looped one,
 * not a tap's) shorter still
 */
static const unsigned uneven[11] = { 40, 30, 12, 50, 20, 30, 30, 30, 30, 30, 8 };

/* haptic_tap_gain=0x57: a loud tap and a soft one, each at 0x57, both the shortest short waveform */
static void
a_tap_at_a_fixed_gain(void)
{
	phone("phone-video", 0x9270);
	make_waveforms_of(11, 0, uneven);
	boot_args = "haptic=5 haptic_tap_gain=0x57";
	hakios_haptic_init();
	WAIT_FOR(logged("THE MOTOR PLAYS WHAT DARWIN PLAYS"), 20000);
	CHECK(logged("haptic: taps at the fixed gain 0x57 (haptic_tap_gain), as waveform 3, the shortest (12 samples"),
	    "the tap chosen by its length, and said: %s", log_text);
	darwin_plays(30000, 20);
	WAIT_FOR(logged("haptic: tap 1:"), 2000);
	CHECK(logged("haptic: tap 1: Darwin's waveform peaks at 30000 of 32768: Android's waveform 3 at gain 0x57"), "said: %s",
	    log_text);
	CHECK(device.reg[0x0a] == 3 && device.reg[0x12] == 0 && device.reg[0x07] == 0x57, "waveform 3, once, at 0x57: %u %#x %#x",
	    device.reg[0x0a], device.reg[0x12], device.reg[0x07]);
	usleep(150000);                         /* (a pause: the next is a tap of its own) */
	darwin_plays(1500, 20);
	WAIT_FOR(logged("haptic: tap 2:"), 2000);
	CHECK(logged("haptic: tap 2: Darwin's waveform peaks at 1500 of 32768: Android's waveform 3 at gain 0x57"),
	    "a soft one no softer: %s", log_text);
	CHECK(device.reg[0x07] == 0x57 && !logged("haptic: vibration"), "the same gain, taps only: %#x", device.reg[0x07]);
}

/* without haptic_tap_gain the same file plays as before: waveform 1, at gain_for()'s gain */
static void
a_tap_by_default_unchanged(void)
{
	phone("phone-video", 0x9270);
	make_waveforms_of(11, 0, uneven);
	boot_args = "haptic=5";
	hakios_haptic_init();
	WAIT_FOR(logged("THE MOTOR PLAYS WHAT DARWIN PLAYS"), 20000);
	darwin_plays(20000, 20);
	WAIT_FOR(logged("haptic: tap 1:"), 2000);
	CHECK(logged("haptic: tap 1: Darwin's waveform peaks at 20000 of 32768: Android's waveform 1 at gain 0x80"), "said: %s",
	    log_text);
	CHECK(device.reg[0x0a] == 1 && device.reg[0x07] == 0x80, "waveform 1 at 0x80: %u %#x", device.reg[0x0a], device.reg[0x07]);
	CHECK(!logged("haptic_tap_gain"), "nothing said of a fixed gain: %s", log_text);
}

/* a fixed gain above Android's is held to Android's */
static void
a_tap_gain_bounded(void)
{
	phone("phone-video", 0x9270);
	make_waveforms_of(11, 0, uneven);
	boot_args = "haptic=5 haptic_tap_gain=0x200";
	hakios_haptic_init();
	WAIT_FOR(logged("THE MOTOR PLAYS WHAT DARWIN PLAYS"), 20000);
	CHECK(logged("haptic: haptic_tap_gain=0x200 is above Android's gain: 0x80"), "said: %s", log_text);
	darwin_plays(1500, 20);
	WAIT_FOR(logged("haptic: tap 1:"), 2000);
	CHECK(logged("Android's waveform 3 at gain 0x80") && device.reg[0x07] == 0x80, "at Android's gain, no more: %#x",
	    device.reg[0x07]);
}

/* Darwin's waveform a sample a write, the least there is: heard all the same */
static void
a_sample_at_a_time(void)
{
	int16_t sample = 20000;

	phone("phone-video", 0x9270);
	make_waveforms(11, 8);
	boot_args = "haptic=5";
	hakios_haptic_init();
	WAIT_FOR(logged("THE MOTOR PLAYS WHAT DARWIN PLAYS"), 20000);
	for (unsigned t = 0; t < 20 && !logged("haptic: tap 1:"); t++) {
		CHECK(fake_dev_write("hakios-haptic", &sample, sizeof(sample)) == 0, "written");
		usleep(1000);
	}
	WAIT_FOR(logged("haptic: tap 1:"), 2000);
	CHECK(logged("haptic: tap 1: Darwin's waveform peaks at 20000 of 32768"), "said: %s", log_text);
}

/* the microphone's stand-in for Darwin (hakios_haptic_pretend): 256 is silence, as from Darwin; louder, a tap */
static void
pretended(void)
{
	phone("phone-video", 0x9270);
	make_waveforms(11, 8);
	boot_args = "haptic=5";
	hakios_haptic_init();
	WAIT_FOR(logged("THE MOTOR PLAYS WHAT DARWIN PLAYS"), 20000);
	for (unsigned t = 0; t < 20; t++) {
		hakios_haptic_pretend(256);
		usleep(5000);
	}
	usleep(100000);
	CHECK(!logged("haptic: tap") && !logged("haptic: vibration"), "silence: %s", log_text);
	for (unsigned t = 0; t < 4; t++) {
		hakios_haptic_pretend(3000);
		usleep(5000);
	}
	WAIT_FOR(logged("haptic: tap 1:"), 2000);
	CHECK(logged("haptic: tap 1: Darwin's waveform peaks at 3000 of 32768"), "a tap: %s", log_text);
}

/* ... and one that goes on, a vibration: Android's waveform 11 over and over */
static void
a_vibration_for_darwin(void)
{
	phone("phone-video", 0x9270);
	make_waveforms(11, 8);
	boot_args = "haptic=5";
	hakios_haptic_init();
	WAIT_FOR(logged("THE MOTOR PLAYS WHAT DARWIN PLAYS"), 20000);
	darwin_plays(20000, 300);
	WAIT_FOR(logged("haptic: vibration 1:"), 2000);
	CHECK(logged("haptic: vibration 1: Darwin goes on playing"), "said: %s", log_text);
	CHECK(device.reg[0x0a] == 11 && device.reg[0x12] == 0xf0, "waveform 11, until told to stop: %u %#x", device.reg[0x0a], device.reg[0x12]);
}

/* a write longer than the driver's own buffer (256 samples): taken a bufferful at a time, every sample counted, nothing read
 * past the buffer (the sanitizers' to see) */
static void
a_long_write(void)
{
	static int16_t samples[600];

	phone("phone-video", 0x9270);
	make_waveforms(11, 8);
	boot_args = "haptic=5";
	hakios_haptic_init();
	WAIT_FOR(logged("THE MOTOR PLAYS WHAT DARWIN PLAYS"), 20000);
	for (unsigned i = 0; i < 600; i++) {
		samples[i] = (int16_t)(i % 2 ? -100 : 100);
	}
	CHECK(fake_dev_write("hakios-haptic", samples, sizeof(samples)) == 0 && written_frames == 600, "every sample taken: %llu",
	    (unsigned long long)written_frames);
}

/* a vibration the chip does not stop when told (it stays playing): the chip is put into reset, and nothing more is done */
static void
a_vibration_that_does_not_stop(void)
{
	phone("phone-video", 0x9270);
	make_waveforms(11, 8);
	boot_args = "haptic=5";
	hakios_haptic_init();
	WAIT_FOR(logged("THE MOTOR PLAYS WHAT DARWIN PLAYS"), 20000);
	darwin_plays(20000, 300);
	WAIT_FOR(logged("haptic: vibration 1:"), 2000);
	device.reg[0x3f] = 7;                   /* (its state: playing, and it stays so) */
	WAIT_FOR(logged("it does not stop"), 3000);
	CHECK(logged("it does not stop: THE CHIP IS PUT INTO RESET") && in_reset(), "said, and in reset: %s", log_text);
}

/* a vibration that goes soft: its strength follows Darwin's down (2000 of 32768: five eighths of Android's gain and
 * a little more, 0x58); Darwin stops, and so does the motor */
static void
a_vibration_that_softens(void)
{
	phone("phone-video", 0x9270);
	make_waveforms(11, 8);
	boot_args = "haptic=5";
	hakios_haptic_init();
	WAIT_FOR(logged("THE MOTOR PLAYS WHAT DARWIN PLAYS"), 20000);
	/* (again, up to three times, if this test's thread was not run for 30 ms on a busy computer: a pause in
	 * Darwin's waveform, and the motor rightly stopped before the soft part) */
	for (unsigned tries = 0; tries < 3 && !logged("last to gain 0x58)"); tries++) {
		darwin_plays(20000, 200);
		darwin_plays(2000, 300);
		usleep(300000);
	}
	CHECK(logged("Darwin stopped, and so does the motor after") && logged("last to gain 0x58)"), "said: %s", log_text);
	{
		/* (the vibration that reached 0x58 changed; an earlier try a pause cut short may have said 0) */
		const char *last = strstr(log_text, "last to gain 0x58)"), *start = last;

		while (start != NULL && start > log_text && start[-1] != '\n') {
			start--;
		}
		char line[512] = "";

		if (start != NULL) {
			size_t n = (size_t)(last - start) < sizeof(line) - 1 ? (size_t)(last - start) : sizeof(line) - 1;
			memcpy(line, start, n);
			line[n] = 0;
		}
		CHECK(start != NULL && strstr(line, "changed 0 times") == NULL, "it changed: %s", log_text);
	}
	CHECK(device.reg[0x07] == 0x58, "the gain followed: %#x", device.reg[0x07]);
}

/* ... and one that goes on past the limit is cut there */
static void
a_vibration_cut_at_the_limit(void)
{
	phone("phone-video", 0x9270);
	make_waveforms(11, 8);
	boot_args = "haptic=5";
	hakios_haptic_init();
	WAIT_FOR(logged("THE MOTOR PLAYS WHAT DARWIN PLAYS"), 20000);
	/* (again, as above, if a pause of this test's own let the motor stop before the limit) */
	for (unsigned tries = 0; tries < 3 && !logged("CUT at the limit"); tries++) {
		darwin_plays(20000, 1300);
		usleep(300000);
	}
	CHECK(logged("CUT at the limit after 1"), "cut at a second (and not before: a cut comes past it): %s", log_text);
}

/* what is no louder than 256 of 32768 is silence: nothing played */
static void
a_whisper_is_not_played(void)
{
	phone("phone-video", 0x9270);
	make_waveforms(11, 8);
	boot_args = "haptic=5";
	hakios_haptic_init();
	WAIT_FOR(logged("THE MOTOR PLAYS WHAT DARWIN PLAYS"), 20000);
	register_written[0x09] = false;
	darwin_plays(256, 300);
	usleep(200000);
	CHECK(!logged("haptic: tap") && !logged("haptic: vibration"), "nothing played: %s", log_text);
	CHECK(!register_written[0x09], "the chip not started");
}

/* haptic=5 without Android's waveforms in the RAM: nothing is played -- not what the RAM holds after the chip's reset */
static void
nothing_to_play(bool corrupt)
{
	phone("phone-video", 0x9270);
	if (corrupt) {
		make_waveforms(11, 8);
		waveforms[1] ^= 1;
	}
	boot_args = "haptic=5";
	hakios_haptic_init();
	WAIT_FOR(logged("haptic: done") || logged("THE MOTOR PLAYS WHAT DARWIN PLAYS"), 20000);
	/* what would be a tap, and then a vibration, if the motor were Darwin's (the microphone's stand-in: no device needed) */
	for (unsigned t = 0; t < 60; t++) {
		hakios_haptic_pretend(20000);
		usleep(5000);
	}
	usleep(100000);
	CHECK(waveforms_started == 0 && !logged("haptic: tap") && !logged("haptic: vibration"), "no waveform started (%u): %s",
	    waveforms_started, log_text);
	CHECK(logged(corrupt ? "aw8697_haptic.bin does not add up" : "no aw8697_haptic.bin in fw/") &&
	    logged("Android's waveforms are not in the chip's RAM: nothing is played"), "and why is said: %s", log_text);
	CHECK(!logged("THE MOTOR PLAYS WHAT DARWIN PLAYS") && logged("haptic: done"), "the motor is not offered to Darwin");
	CHECK(!register_written[0x42] && !in_reset(), "nothing of the file written, nothing wrong");
	hakios_haptic_quiesce();
	CHECK(in_reset(), "back in reset at the run's end");
}

static void no_waveforms_nothing_played(void) { nothing_to_play(false); }
static void waveforms_corrupt_nothing_played(void) { nothing_to_play(true); }

/* haptic=4 on a chip that plays on and is slow to read (it holds the clock before each acknowledge): the effect is
 * stopped at HAPTIC_MAX_MS by the clock, not after HAPTIC_MAX_MS of the driver's own waits and every read besides */
static void
an_effect_cut_on_a_slow_bus(void)
{
	phone("phone-video", 0x9270);
	fake_delay_divisor = 1;                 /* (the driver's waits as long as it asks, as on the phone) */
	make_waveforms(3, 40);
	device.stretch = 90;                    /* (about half a millisecond each acknowledge: a read a millisecond or more) */
	keeps_playing = true;
	boot_args = "haptic=4 haptic_effect=2";
	hakios_haptic_init();
	WAIT_FOR(logged("haptic: done"), 30000);
	CHECK(logged("haptic: waveform 2 is played once") && logged("STILL PLAYING after 1000 ms: stopped"), "said: %s", log_text);
	/* (the look under way when the limit comes, and the stop's own write: a few ms past it, no more) */
	CHECK(go_ms != 0 && stop_ms >= go_ms + HAPTIC_MAX_MS && stop_ms <= go_ms + HAPTIC_MAX_MS + 100,
	    "told to stop %llu ms after the start, at %u ms by the clock", (unsigned long long)(stop_ms - go_ms), HAPTIC_MAX_MS);
	CHECK(waveforms_started == 1 && !logged("PUT INTO RESET") && !in_reset(), "it stopped when told, nothing wrong");
}

/* a fault the chip latches in a tap, after the driver's last look at it while it played: found before the next tap
 * is started, not once it plays (the model's flags do not clear when read: as if latched in the tap's last moment) */
static void
a_fault_after_a_tap(void)
{
	unsigned started;

	phone("phone-video", 0x9270);
	make_waveforms(11, 8);
	boot_args = "haptic=5";
	hakios_haptic_init();
	WAIT_FOR(logged("THE MOTOR PLAYS WHAT DARWIN PLAYS"), 20000);
	darwin_plays(20000, 20);
	WAIT_FOR(logged("haptic: tap 1:"), 2000);
	usleep(300000);                         /* (the tap over, the driver idle: it looks at the chip no more) */
	CHECK(logged("haptic: tap 1:") && !logged("PUT INTO RESET"), "a tap, nothing wrong: %s", log_text);
	CHECK(haptics_published == 1 && haptics_gone == 0, "the service published, and there (%u, %u taken away)", haptics_published,
	    haptics_gone);
	started = waveforms_started;
	device.reg[0x02] = 0x20;
	darwin_plays(20000, 20);
	WAIT_FOR(logged("PUT INTO RESET"), 2000);
	CHECK(logged("before a tap: the chip reports 0x20: UNDER-VOLTAGE") && logged("a fault: THE CHIP IS PUT INTO RESET") &&
	    in_reset(), "found before the tap, and into reset: %s", log_text);
	CHECK(waveforms_started == started && !logged("haptic: tap 2:"), "the next tap is not started (%u started, %u before)",
	    waveforms_started, started);
	/* the motor given up on: the service Darwin's haptic server was told of is taken away, once */
	WAIT_FOR(logged("the motor is given up on"), 2000);
	EVENTUALLY(haptics_gone == 1, 1000, "the service taken away (%u times)", haptics_gone);
}

/* a board that does not list the part, and a SoC file without the pins */
static void
not_listed(void)
{
	phone("qemu-virt", 0x9270);
	boot_args = "haptic=5";
	hakios_haptic_init();
	usleep(50000);
	CHECK(logged("haptic: disabled: this board's board.dts lists no \"haptic-aw86927\""), "said: %s", log_text);
	CHECK(model.writes == 0 && model.elsewhere == 0, "nothing touched");
	hakios_haptic_quiesce();
	CHECK(model.writes == 0, "nor at the run's end");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "off_by_default", off_by_default }, { "look_only", look_only }, { "reset_released", reset_released }, { "found_released", found_released },
		{ "configured", configured }, { "another_wiring", another_wiring },
		{ "write_does_not_stick", write_does_not_stick }, { "fault_reported", fault_reported }, { "another_chip", another_chip }, { "nobody_there", nobody_there }, { "not_listed", not_listed },
		{ "waveforms_loaded", waveforms_loaded }, { "waveforms_do_not_add_up", waveforms_do_not_add_up },
		{ "ram_reads_back_wrong", ram_reads_back_wrong },
		{ "waveforms_too_short", waveforms_too_short }, { "waveforms_in_whole_pieces", waveforms_in_whole_pieces }, { "an_effect_played", an_effect_played },
		{ "the_last_short_effect", the_last_short_effect }, { "not_a_short_effect", not_a_short_effect },
		{ "a_tap_for_darwin", a_tap_for_darwin }, { "a_vibration_for_darwin", a_vibration_for_darwin },
		{ "a_vibration_that_does_not_stop", a_vibration_that_does_not_stop }, { "a_long_write", a_long_write },
		{ "a_vibration_that_softens", a_vibration_that_softens },
		{ "a_vibration_cut_at_the_limit", a_vibration_cut_at_the_limit }, { "a_whisper_is_not_played", a_whisper_is_not_played },
		{ "a_sample_at_a_time", a_sample_at_a_time }, { "pretended", pretended },
		{ "no_waveforms_nothing_played", no_waveforms_nothing_played }, { "waveforms_corrupt_nothing_played", waveforms_corrupt_nothing_played },
		{ "an_effect_cut_on_a_slow_bus", an_effect_cut_on_a_slow_bus }, { "a_fault_after_a_tap", a_fault_after_a_tap },
		{ "a_tap_at_a_fixed_gain", a_tap_at_a_fixed_gain }, { "a_tap_by_default_unchanged", a_tap_by_default_unchanged },
		{ "a_tap_gain_bounded", a_tap_gain_bounded },
	};

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
			fprintf(stderr, "%-20s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
