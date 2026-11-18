/*
 * A vibration motor: a linear resonant actuator driven by an
 * Awinic AW86927 (chip ID 0x9270), Android's "haptic_hv" (Xiaomi's driver aw8692x, version 1.7.0).
 * Which pins the chip is on and the motor's calibration are the board's (haptic { }, board/board.h);
 * the chip's registers and Android's values for them are this driver's. On the phone it was written
 * for, from Android's own device tree, its kernel's log and the chip's registers as Android leaves
 * them (read from sysfs, read-only):
 *
 *   I2C        the serial engine 9 (qupv3_se9_i2c at 0xa84000, function qup11 on GPIO 36 data and
 *              37 clock, internal pull-ups, 400 kHz under Android), address 0x5a; done by hand here
 *              (board/i2c_gpio.h), the two pins taken for each transaction and given back as they
 *              were found
 *   reset      GPIO 35, high = running (Android: low for 5 ms, then high, at its driver's start)
 *   interrupt  GPIO 18, pulled up, low when the chip has something to say (not used: polled)
 *   the motor  f0 175.5 Hz as Android calibrated it (f0_pre 170.0 Hz, calibration data 0x0d)
 *   effects    Android's RAM waveforms, /vendor/firmware/aw8697_haptic.bin of the phone (fw-haptic/
 *              here, not in the repository; userland/build.sh puts it into /fw of the system image):
 *              eleven of them, 1..10 short (Android's effects 0..9), 11 the one looped for a
 *              vibration of some length
 *
 * Nothing here is done unless a boot-arg asks for it:
 *
 *   haptic=1   a look, reading only: the pins, and whether the chip answers (its ID and registers,
 *              against what Android leaves in them). Out of Android's bootloader the reset pin is an
 *              input with a pull-down: the chip is held in reset and does not answer.
 *   haptic=2   ... and the reset released as Android's driver releases it (low 5 ms, then high; it
 *              stays released until the run ends), and the chip's registers read as they come out
 *              of reset, against Android's. Nothing is written to the chip.
 *   haptic=3   ... and Android's configuration written (every write read back), Android's
 *              waveforms loaded into its RAM and read back; then standby. Nothing moves.
 *   haptic=4   ... and one short effect played (haptic_effect=N, default 1: Android's effect 0)
 *   haptic=5   ... configured and loaded as for 3, and instead of the effect the motor for Darwin until
 *              the run ends: what Darwin plays on its "Actuator" device (the HAL plug-in's,
 *              components/audio/audio_plugin.c: the system's feedbacks and vibrations, CoreHaptics'
 *              patterns, UIKit's feedbacks) is written to /dev/hakios-haptic as samples of one
 *              channel, and the motor plays Android's waveforms by it: each waveform of Darwin's (a
 *              stretch of samples after a pause) a short tap (waveform 1), and one that goes on for
 *              longer than a tap the waveform Android loops for vibrations (11), until it ends -- or
 *              for HAPTIC_MAX_MS at most, after which the motor rests for as long before it plays
 *              again. The gain follows how loud Darwin's waveform is: Android's own (0x80) from a third
 *              of full scale up, five eighths of it for the softest, never above it (gain_for()).
 *
 *   haptic_tap_gain=N  with 5: every tap at this one gain (1..0x80, Android's; more is taken as 0x80), whatever
 *              Darwin's loudness, and as the shortest of Android's short waveforms (1..10, by their lengths
 *              in the file), started as soon as Darwin's waveform is heard. 0, the default: taps as above.
 *              Vibrations are played as above either way. A tap chosen by the first milliseconds of
 *              Darwin's waveform comes out strong or weak by chance, a tap of 23 to 35 ms is felt as a
 *              buzz rather than a click, and so is the short one played strong: one short tick at a low
 *              fixed gain is what felt like a click. After the OnePlus Nord 2 port of iOS
 *              (frxncyyy/ios-denniz, its haptic pieces 0611 to 0618, the finding only), whose user settled
 *              on an 11.8 ms tick at gain 0x57 for an AW8697.
 *
 *   4 and 5 play Android's waveforms only once they are loaded: without the file (or with one that does
 *   not add up) nothing is played, and that is said -- not whatever the chip's RAM holds after its reset.
 *
 * Limits, whatever asks: Android's own values only (its gain 0x80, its boost and drive levels, its
 * waveforms); no effect longer than HAPTIC_MAX_MS, and the chip is told to stop and put into
 * standby when that is over whether it has stopped or not; any fault the chip reports (under-
 * voltage, over-current, over-temperature, the boost's short circuit or over-voltage), a write that
 * does not read back or a byte nobody acknowledges, and the chip is put into reset and nothing more
 * is done for the rest of the run. At the run's end (and on a panic's way out) the chip goes back
 * into reset. Nothing is written anywhere but to the chip's registers and its RAM.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/board/audio.h>
#include <sys/board/input.h>

#include "kernel.h"
#include "board.h"
#include "log.h"

extern void HakiosHapticsReady(void);             /* iokit/Kernel/BoardHaptics.cpp */
extern void HakiosHapticsGone(void);

static vm_offset_t tlmm;
static uint32_t level;
static bool broken, released, leaving;
static uint32_t tap_gain;                       /* haptic_tap_gain=: every tap's gain, 0 for gain_for()'s */
static unsigned tap_waveform = 1;               /* the waveform a tap plays: 1, or with tap_gain the shortest */

/* every line to the loader's log as well (and to the console when that may be written) */
HAKIOS_DEFINE_SAY(!leaving && ml_get_interrupts_enabled())

#include "mmio.h"
#include "time.h"

#define wait_ms(ms)             hakios_wait_ms((ms), leaving)   /* (spun on the way out) */

/* ---- pins ------------------------------------------------------------------------------------------ */

#define TLMM_PHYS               (hakios_board()->pins.tlmm)     /* the SoC file's pins { } */
#define HAKIOS_TLMM             tlmm
#include "tlmm.h"                    /* a pin's two registers, and the four things done with them */

/* which pins: the board's haptic { } (on the phone this was written for: 36 data, 37 clock, 35 reset, 18 interrupt) */
#define PIN_SDA                 (hakios_board()->haptic.sda)
#define PIN_SCL                 (hakios_board()->haptic.scl)
#define PIN_RESET               (hakios_board()->haptic.reset)
#define PIN_INT                 (hakios_board()->haptic.interrupt)

static void
show_pins(void)
{
	const struct {
		unsigned pin;
		const char *what;
	} pins[] = { { PIN_SDA, "I2C data" }, { PIN_SCL, "I2C clock" }, { PIN_RESET, "reset" }, { PIN_INT, "interrupt" } };

	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg = rd(GPIO_CFG(pins[i].pin)), io = rd(GPIO_IO(pins[i].pin));

		say("haptic: gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u\n", pins[i].pin, pins[i].what, (cfg >> 2) & 15,
		    cfg & 3, (((cfg >> 6) & 7) + 1) * 2, (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") : "input", io & 1);
	}
}

/* the reset pin as an output of the GPIO function, driving `high' (2 mA, no pull, as Android has it) */
static void
reset_pin(bool high)
{
	wr(GPIO_IO(PIN_RESET), high ? 2 : 0);
	wr(GPIO_CFG(PIN_RESET), CFG_OUTPUT);
	released = high;
}

/* ---- I2C by hand (as in board/speaker.c) ---------------------------------------------------------- */

#include "i2c_gpio.h"                /* the two lines, a bit, a byte, the bus taken and given back */

static struct i2c_gpio i2c_bus;          /* (its two pins: the board's, at the start) */

static bool
bus_take(void)
{
	if (i2c_gpio_take(&i2c_bus)) {
		return true;
	}
	say("haptic: the I2C bus is not at rest (gpio%u reads %u, gpio%u reads %u): not touched\n", PIN_SDA,
	    i2c_gpio_level(PIN_SDA), PIN_SCL, i2c_gpio_level(PIN_SCL));
	return false;
}

static void
bus_give_back(void)
{
	i2c_gpio_give_back(&i2c_bus);
}

#define AW_ADDRESS              0x5a

/* `count' 8-bit registers from `reg' on (the chip's address counts up, but for its RAM data port) */
static bool
regs_read(uint8_t reg, uint8_t *values, unsigned count)
{
	bool answered;

	if (!bus_take()) {
		return false;
	}
	i2c_gpio_start(&i2c_bus);
	answered = i2c_gpio_write(&i2c_bus, AW_ADDRESS << 1) && i2c_gpio_write(&i2c_bus, reg);
	if (answered) {
		i2c_gpio_start(&i2c_bus);
		answered = i2c_gpio_write(&i2c_bus, AW_ADDRESS << 1 | 1);
	}
	for (unsigned i = 0; answered && i < count; i++) {
		values[i] = i2c_gpio_read(&i2c_bus, i + 1 < count);
	}
	i2c_gpio_stop(&i2c_bus);
	bus_give_back();
	return answered;
}

static bool
regs_write(uint8_t reg, const uint8_t *values, unsigned count)
{
	bool answered;

	if (!bus_take()) {
		return false;
	}
	i2c_gpio_start(&i2c_bus);
	answered = i2c_gpio_write(&i2c_bus, AW_ADDRESS << 1) && i2c_gpio_write(&i2c_bus, reg);
	for (unsigned i = 0; answered && i < count; i++) {
		answered = i2c_gpio_write(&i2c_bus, values[i]);
	}
	i2c_gpio_stop(&i2c_bus);
	bus_give_back();
	return answered;
}

/* ---- the chip -------------------------------------------------------------------------------------- */

#define AW_RSTCFG               0x00            /* 0xaa: a soft reset */
#define AW_SYSST                0x01
#define AW_SYSINT               0x02            /* read: cleared */
#define AW_SYSINTM              0x03
#define AW_PLAYCFG1             0x06            /* [7] boost on, [6:0] its voltage */
#define AW_PLAYCFG2             0x07            /* the gain */
#define AW_PLAYCFG3             0x08            /* [1:0] the play mode: 0 RAM */
#define AW_PLAYCFG4             0x09            /* [1] stop, [0] go */
#define AW_WAVCFG1              0x0a            /* the first waveform to play, 1..127 (0: the end) */
#define AW_WAVCFG2              0x0b
#define AW_WAVCFG9              0x12            /* [7:4] how often the first is repeated, 15: until told to stop */
#define AW_BASEADDRH            0x2d
#define AW_BASEADDRL            0x2e
#define AW_GLBRD5               0x3f            /* [3:0] the state: 0 standby */
#define AW_RAMADDRH             0x40
#define AW_RAMADDRL             0x41
#define AW_RAMDATA              0x42
#define AW_SYSCTRL3             0x45            /* [5] standby, [2] the RAM open for loading */
#define AW_TMCFG                0x5b            /* 0x7d opens the analog registers 0x70.. for writing, 0 closes them */
#define AW_TRIM_LRA             0x79            /* the motor's f0 calibration */
#define AW_CHIPIDH              0x57
#define AW_CHIPIDL              0x58
#define AW_CHIPID               0x9270

#define SYSINT_FAULTS           0xe6            /* boost short circuit and over-voltage, under-voltage, over-current, over-temperature */
#define HAPTIC_MAX_MS           1000

static void
give_up(const char *why)
{
	broken = true;
	say("haptic: %s: THE CHIP IS PUT INTO RESET (gpio%u low) and nothing more is done\n", why, PIN_RESET);
	if (tlmm != 0) {
		reset_pin(false);
	}
}

static bool
get(uint8_t reg, uint8_t *value)
{
	if (!regs_read(reg, value, 1)) {
		say("haptic: 0x%02x NOT READ\n", reg);
		return false;
	}
	return true;
}

/* One register written and read back. */
static bool
put(uint8_t reg, uint8_t value)
{
	uint8_t after = 0;

	if (broken) {
		return false;
	}
	if (!regs_write(reg, &value, 1)) {
		say("haptic: 0x%02x <- 0x%02x NOT ACKNOWLEDGED\n", reg, value);
		give_up("a write was not acknowledged");
		return false;
	}
	if (!regs_read(reg, &after, 1)) {
		give_up("a register could not be read back");
		return false;
	}
	say("haptic: 0x%02x <- 0x%02x, reads 0x%02x%s\n", reg, value, after, after == value ? "" : ": NOT AS WRITTEN");
	if (after != value) {
		give_up("a register did not read back as written");
		return false;
	}
	return true;
}

/* The chip's interrupt flags (reading clears them): false, and the chip given up on, if one is a fault. */
static bool
healthy(const char *when)
{
	uint8_t flags = 0;

	if (!get(AW_SYSINT, &flags)) {
		give_up("the flags could not be read");
		return false;
	}
	if (flags & SYSINT_FAULTS) {
		say("haptic: %s: the chip reports 0x%02x:%s%s%s%s%s\n", when, flags, flags & 0x80 ? " BOOST SHORT CIRCUIT" : "",
		    flags & 0x40 ? " BOOST OVER-VOLTAGE" : "", flags & 0x20 ? " UNDER-VOLTAGE" : "", flags & 0x04 ? " OVER-CURRENT" : "",
		    flags & 0x02 ? " OVER-TEMPERATURE" : "");
		give_up("a fault");
		return false;
	}
	return true;
}

/* Android's registers as its driver leaves them, read from the phone's sysfs (fw-haptic/android/haptic-sysfs.txt) */
static const uint8_t android_regs[0x7d] = {
	[0x00] = 0x82, [0x01] = 0x10, [0x02] = 0x11, [0x03] = 0x59, [0x04] = 0x09, [0x05] = 0x00, [0x06] = 0x47, [0x07] = 0x80,
	[0x08] = 0x07, [0x09] = 0x00, [0x0a] = 0x0b, [0x12] = 0xf0, [0x18] = 0x1e, [0x19] = 0x8d, [0x1a] = 0x71, [0x1b] = 0x06,
	[0x1c] = 0x58, [0x1d] = 0xcf, [0x1e] = 0x28, [0x1f] = 0x04, [0x20] = 0x06, [0x21] = 0x08, [0x22] = 0x0c, [0x23] = 0x0c,
	[0x24] = 0x62, [0x25] = 0x08, [0x26] = 0xa0, [0x27] = 0x08, [0x28] = 0x8c, [0x2d] = 0x08, [0x2f] = 0x46, [0x33] = 0x01,
	[0x34] = 0x01, [0x35] = 0x01, [0x36] = 0x02, [0x37] = 0x02, [0x38] = 0x02, [0x3b] = 0x05, [0x3c] = 0x08, [0x3e] = 0x1b,
	[0x40] = 0x08, [0x41] = 0x03, [0x45] = 0x12, [0x46] = 0x09, [0x47] = 0x5a, [0x49] = 0x28, [0x4a] = 0xbf, [0x4b] = 0x32,
	[0x4d] = 0x24, [0x4e] = 0x04, [0x4f] = 0x0a, [0x51] = 0x07, [0x57] = 0x92, [0x58] = 0x70, [0x59] = 0x04, [0x5a] = 0x98,
	[0x5d] = 0xf5, [0x5e] = 0x0d, [0x5f] = 0xf5, [0x60] = 0x41, [0x61] = 0xf6, [0x62] = 0x5c, [0x63] = 0x0e, [0x64] = 0xf9,
	[0x68] = 0x05, [0x69] = 0x21, [0x6b] = 0x02, [0x6c] = 0x15, [0x6d] = 0x89, [0x6e] = 0xc2, [0x6f] = 0x48, [0x70] = 0x0f,
	[0x71] = 0x93, [0x72] = 0x69, [0x73] = 0xa4, [0x74] = 0x89, [0x75] = 0x88, [0x77] = 0x40, [0x79] = 0x0d,
};

/* What the chip holds now, against Android's (registers 0x00..0x7c; 0x32 and 0x42 are data ports, not read). */
static void
show_registers(void)
{
	unsigned differ = 0;

	for (unsigned reg = 0; reg <= 0x7c; reg++) {
		uint8_t value = 0;

		if (reg == 0x32 || reg == 0x42) {
			continue;
		}
		if (!regs_read((uint8_t)reg, &value, 1)) {
			say("haptic: 0x%02x NOT READ\n", reg);
			return;
		}
		if (value != android_regs[reg]) {
			differ++;
			say("haptic: 0x%02x = 0x%02x (Android leaves 0x%02x)\n", reg, value, android_regs[reg]);
		}
	}
	say("haptic: %u registers differ from what Android leaves in them\n", differ);
}

static bool
chip_answers(void)
{
	uint8_t id[2] = { 0, 0 };

	if (!regs_read(AW_CHIPIDH, id, 2)) {
		say("haptic: nobody answers at %#x on the bus of gpio%u/%u\n", AW_ADDRESS, PIN_SDA, PIN_SCL);
		return false;
	}
	say("haptic: the chip at %#x is 0x%04x%s\n", AW_ADDRESS, id[0] << 8 | id[1], (id[0] << 8 | id[1]) == AW_CHIPID ? ", an AW86927" :
	    ": NOT THE AW86927 EXPECTED");
	return (id[0] << 8 | id[1]) == AW_CHIPID;
}

/*
 * Android's configuration: the registers its driver sets (and the mainline Linux driver aw86927.c sets the
 * same way), with Android's values -- those that differ from the chip's own after a reset, as the phone
 * showed (haptic=2). Not written: the status and interrupt flags, the RAM's address and data ports, the
 * results of the chip's measurements (0x25..0x28: they do not take what is written, they are what its f0
 * tracking found under Android; 0x4f, 0x51, 0x59, 0x5a likewise), and what is set for each playing (0x08 the mode,
 * 0x0a/0x12 the waveform and its repeats). The values are all read from the chip as Android leaves it.
 */
static const uint8_t android_config[][2] = {
	{ 0x03, 0x59 },         /* the interrupt mask */
	{ 0x06, 0x47 },         /* PLAYCFG1: the boost in bypass, its voltage setting 0x47 (Android's vmax 7938 mV) */
	{ 0x1a, 0x71 },         /* CONTCFG3 */
	{ 0x1d, 0xcf },         /* CONTCFG6: f0 tracking on, the first drive level 0x4f */
	{ 0x1e, 0x28 },         /* CONTCFG7: the second drive level */
	{ 0x2f, 0x46 },         /* the FIFO's thresholds */
	{ 0x36, 0x02 }, { 0x37, 0x02 }, { 0x38, 0x02 },  /* the trigger pins' waveforms (the triggers stay off) */
	{ 0x39, 0x00 }, { 0x3a, 0x00 },  /* ... and off */
	{ 0x46, 0x09 },         /* SYSCTRL4: 24 kHz waveforms, the gain applied as it is */
	{ 0x47, 0x5a },
};
/*
 * ... and the analog ones, behind AW_TMCFG (the boost: skip mode, peak current 3.45 A, peak back, slew).
 * These too are Android's: what the phone's registers hold under Android (they differ from the chip's
 * own after a reset, so Android's driver set them); mainline Linux's aw86927.c sets the same values.
 */
static const uint8_t android_analog[][2] = {
	{ 0x70, 0x0f }, { 0x71, 0x93 }, { 0x72, 0x69 }, { 0x74, 0x89 }, { 0x75, 0x88 },
};
/* ... and Android's f0 calibration of the motor (its log: "f0_cali_data = 0x0D" on the first phone): the board's haptic { } */

/* The analog registers opened or closed for writing (AW_TMCFG reads back as 0 either way: what tells is that the writes stick). */
static bool
unlock(bool open)
{
	uint8_t value = open ? 0x7d : 0x00;

	if (broken || !regs_write(AW_TMCFG, &value, 1)) {
		give_up("the analog registers could not be opened");
		return false;
	}
	say("haptic: 0x%02x <- 0x%02x (the analog registers %s)\n", AW_TMCFG, value, open ? "opened" : "closed");
	return true;
}

static bool
configure(void)
{
	say("haptic: Android's configuration is written\n");
	for (unsigned i = 0; i < sizeof(android_config) / sizeof(android_config[0]); i++) {
		if (!put(android_config[i][0], android_config[i][1])) {
			return false;
		}
	}
	if (!unlock(true)) {
		return false;
	}
	for (unsigned i = 0; i < sizeof(android_analog) / sizeof(android_analog[0]); i++) {
		if (!put(android_analog[i][0], android_analog[i][1])) {
			(void)unlock(false);
			return false;
		}
	}
	if (!put(AW_TRIM_LRA, hakios_board()->haptic.f0_calibration)) {
		(void)unlock(false);
		return false;
	}
	return unlock(false) && healthy("after the configuration");
}

static uint32_t states_seen;           /* the GLBRD5 states seen by the last wait, a bit each */

/*
 * Until the chip says it is in standby (GLBRD5 state 0), at the latest until the clock says `until' (now_us()).
 * By the clock, not by the waits counted: each look is a read on the bus by hand, which takes its own time
 * (more if the chip holds the clock), and counted waits would let the limit grow by all of them.
 */
static bool
wait_standby_until(uint64_t until)
{
	states_seen = 0;
	for (;;) {
		uint8_t state = 0xff;

		if (!get(AW_GLBRD5, &state)) {
			return false;
		}
		states_seen |= 1u << (state & 0x0f);
		if ((state & 0x0f) == 0) {
			return true;
		}
		if (now_us() >= until) {
			return false;
		}
		wait_ms(2);
	}
}

/* ... at most `ms' from now (0: one look) */
static bool
wait_standby(unsigned ms)
{
	return wait_standby_until(now_us() + ms * 1000ull);
}

/*
 * With haptic_tap_gain=: the shortest of Android's short waveforms (1..10; the first of them on a tie) for every
 * tap. The RAM's contents begin with a version byte, then each waveform's start and end address, two bytes each.
 */
static void
choose_tap(const uint8_t *file, uint64_t size, uint32_t base)
{
	uint32_t first = (uint32_t)(file[5] << 8 | file[6]), shortest = 0;
	unsigned count = first > base ? (first - base - 1) / 4 : 0;

	for (unsigned w = 1; w <= 10 && w <= count && 5 + 4ull * w <= size; w++) {
		const uint8_t *entry = file + 5 + 4 * (w - 1);
		uint32_t start = (uint32_t)(entry[0] << 8 | entry[1]), end = (uint32_t)(entry[2] << 8 | entry[3]);

		if (end >= start && (shortest == 0 || end - start + 1 < shortest)) {
			shortest = end - start + 1;
			tap_waveform = w;
		}
	}
	say("haptic: taps at the fixed gain 0x%02x (haptic_tap_gain), as waveform %u, the shortest (%u samples, about %u ms "
	    "at 24 kHz)\n", tap_gain, tap_waveform, shortest, (shortest + 12) / 24);
}

/*
 * Android's waveforms into the chip's RAM: the file is a checksum (16 bits, of all that follows), the
 * address the RAM's contents begin at (0x0800), and those contents -- a version byte, the start and end
 * address of each waveform, the waveforms. Written through the data port with the address counting up,
 * then read back the same way and compared.
 */
static bool
load_waveforms(void)
{
	uint64_t size = 0;
	const uint8_t *file = hakios_wifi_file("aw8697_haptic.bin", &size);
	uint32_t sum = 0, base, length;
	uint8_t address[2], back[64];

	if (file == NULL || size < 8) {
		say("haptic: no aw8697_haptic.bin in fw/ of the small image (fw-haptic/ when it is built)\n");
		return false;
	}
	for (uint64_t i = 2; i < size; i++) {
		sum += file[i];
	}
	if ((sum & 0xffff) != (uint32_t)(file[0] << 8 | file[1])) {
		say("haptic: aw8697_haptic.bin does not add up (%#x, it says %#x): not loaded\n", sum & 0xffff, file[0] << 8 | file[1]);
		return false;
	}
	base = (uint32_t)(file[2] << 8 | file[3]);
	length = (uint32_t)size - 4;
	say("haptic: Android's waveforms: %u bytes from %#x on, %u waveforms\n", length, base, (unsigned)(((file[5] << 8 | file[6]) -
	    base - 1) / 4));
	if (!wait_standby(250) || !put(AW_SYSCTRL3, (uint8_t)(android_regs[AW_SYSCTRL3] | 0x04))) {
		give_up("the RAM could not be opened");
		return false;
	}
	address[0] = (uint8_t)(base >> 8);
	address[1] = (uint8_t)base;
	if (!put(AW_BASEADDRH, address[0]) || !put(AW_BASEADDRL, address[1]) || !regs_write(AW_RAMADDRH, address, 2)) {
		give_up("the RAM's address could not be set");
		return false;
	}
	for (uint32_t at = 0; at < length; at += 32) {
		unsigned n = length - at < 32 ? length - at : 32;

		if (!regs_write(AW_RAMDATA, file + 4 + at, n)) {
			give_up("the RAM could not be written");
			return false;
		}
	}
	if (!regs_write(AW_RAMADDRH, address, 2)) {
		give_up("the RAM's address could not be set again");
		return false;
	}
	for (uint32_t at = 0; at < length; at += sizeof(back)) {
		unsigned n = length - at < sizeof(back) ? length - at : sizeof(back);

		if (!regs_read(AW_RAMDATA, back, n) || memcmp(back, file + 4 + at, n) != 0) {
			say("haptic: the RAM reads back differently at %#x\n", base + at);
			give_up("the waveforms did not load");
			return false;
		}
	}
	if (!put(AW_SYSCTRL3, android_regs[AW_SYSCTRL3])) {
		return false;
	}
	say("haptic: Android's waveforms are in the chip's RAM and read back as written\n");
	if (tap_gain != 0) {
		choose_tap(file, size, base);
	}
	return healthy("after the waveforms");
}

/* One waveform played once (Android's effect `effect' - 1), stopped at HAPTIC_MAX_MS at the latest. */
static bool
play_effect(unsigned effect)
{
	uint64_t began = mach_absolute_time(), ended, started;
	bool done;

	say("haptic: waveform %u is played once, at Android's gain %#x\n", effect, android_regs[AW_PLAYCFG2]);
	if (!wait_standby(250) || !put(AW_PLAYCFG3, 0x04) || !put(AW_WAVCFG1, (uint8_t)effect) || !put(AW_WAVCFG2, 0x00) ||
	    !put(AW_WAVCFG9, 0x00) || !put(AW_PLAYCFG2, android_regs[AW_PLAYCFG2]) || !healthy("before playing")) {
		return false;
	}
	{
		uint8_t go = 0x01;

		if (!regs_write(AW_PLAYCFG4, &go, 1)) {
			give_up("the start was not acknowledged");
			return false;
		}
		started = now_us();
	}
	wait_ms(2);
	done = wait_standby_until(started + HAPTIC_MAX_MS * 1000ull);     /* (HAPTIC_MAX_MS from the start, by the clock) */
	absolutetime_to_nanoseconds(mach_absolute_time() - began, &ended);
	if (!done) {
		uint8_t stop = 0x02;

		say("haptic: STILL PLAYING after %u ms: stopped\n", HAPTIC_MAX_MS);
		(void)regs_write(AW_PLAYCFG4, &stop, 1);
		if (!wait_standby(50)) {
			give_up("it does not stop");
			return false;
		}
	}
	say("haptic: the waveform has played: back in standby after %llu ms (states seen meanwhile: %#x, a bit each; 0 is "
	    "standby)\n", ended / NSEC_PER_MSEC, states_seen);
	return healthy("after playing");
}

/* ---- haptic=5: the motor for Darwin ------------------------------------------------------------------- */

#define ACTIVE_US               30000           /* a write that was not silence counts for this long */
#define SILENT_BELOW            256             /* of 32768: samples no larger are silence */
#define TAP_US                  60000           /* longer than this, and it is a vibration, not a tap */
#define SETTLE_US               8000            /* how long Darwin's waveform is listened to before a tap: its loudness */
#define FOLLOW_US               20000           /* how often a vibration's strength follows Darwin's */

#define TOLD_EVENTS             24              /* the first so many taps and vibrations are each said */

static volatile uint64_t heard_us;              /* when the last write that was not silence came */
static volatile uint64_t quiet_us;              /* when the last write that was silence came */
static volatile uint64_t onset_us;              /* when Darwin's last waveform began: the first write not silence after a pause */
static volatile uint32_t heard_peak;            /* the largest sample of that waveform, or since the service last looked */
static uint64_t written_frames;
static unsigned taps, loops, capped;            /* what has been played, for the run's end */

/* From the audio server's I/O thread: never waits. */
static int
haptic_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	int16_t samples[256];
	int error = 0;

	while (uio_resid(uio) >= 2) {
		user_ssize_t bytes = uio_resid(uio);
		uint32_t peak = 0;

		if (bytes > (user_ssize_t)sizeof(samples)) {
			bytes = sizeof(samples);
		}
		bytes &= ~(user_ssize_t)1;
		if ((error = uiomove((caddr_t)samples, (int)bytes, uio)) != 0) {
			break;
		}
		for (unsigned i = 0; i < bytes / 2; i++) {
			uint32_t v = samples[i] < 0 ? (uint32_t)-(int32_t)samples[i] : (uint32_t)samples[i];

			peak = v > peak ? v : peak;
		}
		written_frames += (uint64_t)bytes / 2;
		if (peak > SILENT_BELOW) {
			uint64_t now = now_us();

			if (heard_us == 0 || quiet_us > heard_us || now - heard_us >= ACTIVE_US) {
				onset_us = now;
				heard_peak = 0;
			}
			heard_us = now;
			if (peak > heard_peak) {
				heard_peak = peak;
			}
		} else {
			quiet_us = now_us();
		}
	}
	return error;
}

/*
 * A test's stand-in for Darwin's waveform (board/mic.c, mic_tap=1: the microphone hears the motor): as if
 * a write of this loudness (of 32767) had just come, or silence for 0. The service plays it with all its
 * own limits, as it plays Darwin's.
 */
void
hakios_haptic_pretend(uint32_t peak)
{
	uint64_t now = now_us();

	if (peak <= SILENT_BELOW) {
		quiet_us = now;
		return;
	}
	if (heard_us == 0 || quiet_us > heard_us || now - heard_us >= ACTIVE_US) {
		onset_us = now;
		heard_peak = 0;
	}
	heard_us = now;
	if (peak > heard_peak) {
		heard_peak = peak;
	}
}

static int
haptic_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw haptic_cdevsw = {
	.d_open = haptic_open, .d_close = haptic_open, .d_read = eno_rdwrt, .d_write = haptic_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* Start a waveform without waiting for it: once, or (`forever') until stopped. */
static bool
start_waveform(unsigned waveform, bool forever, uint8_t gain)
{
	uint8_t go = 0x01;

	if (!put(AW_PLAYCFG3, 0x04) || !put(AW_WAVCFG1, (uint8_t)waveform) || !put(AW_WAVCFG2, 0x00) ||
	    !put(AW_WAVCFG9, forever ? 0xf0 : 0x00) || !put(AW_PLAYCFG2, gain)) {
		return false;
	}
	if (!regs_write(AW_PLAYCFG4, &go, 1)) {
		give_up("the start was not acknowledged");
		return false;
	}
	return true;
}

static bool
stop_waveform(void)
{
	uint8_t stop = 0x02;

	if (!regs_write(AW_PLAYCFG4, &stop, 1) || !wait_standby(50)) {
		give_up("it does not stop");
		return false;
	}
	return true;
}

/* the chip's state (GLBRD5: 0 standby, 7 and 11 seen while a waveform plays), 0xff if it cannot be read */
static unsigned
chip_state(void)
{
	uint8_t state = 0xff;

	return get(AW_GLBRD5, &state) ? state & 0x0f : 0xff;
}

/*
 * How strong to play what Darwin plays. Darwin drives its Taptic Engine with waveforms far under full
 * scale for most feedbacks (a tap's peak anywhere from 1000 to 33000 of 32768, a vibration's
 * about 17000), and followed in proportion (gain = Android's * peak / full scale) most of them came
 * out at a tenth of Android's gain or less: not felt at all. Android plays its own clicks and
 * vibrations at its gain, 0x80. So: Android's gain from a third of full scale up, and below that
 * down to five eighths of it for the softest -- never above Android's.
 */
static uint8_t
gain_for(uint32_t peak)
{
	uint32_t top = android_regs[AW_PLAYCFG2], low = top * 5 / 8, third = 32767 / 3;

	return (uint8_t)(peak >= third ? top : low + (top - low) * peak / third);
}

static void
serve(void)
{
	enum { IDLE, TAP, LOOP, REST } state = IDLE;
	uint64_t since = 0, rest_until = 0, last_look = 0, tap_onset = 0, followed = 0;
	uint8_t playing_gain = 0;
	unsigned follows = 0;                   /* how often the vibration playing now has changed strength */
	int major = cdevsw_add(-1, &haptic_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-haptic") == NULL) {
		say("haptic: /dev/hakios-haptic cannot be made\n");
		return;
	}
	say("haptic: THE MOTOR PLAYS WHAT DARWIN PLAYS ON ITS ACTUATOR (/dev/hakios-haptic) from here on, until the run ends\n");
	HakiosHapticsReady();           /* the service Darwin's haptic server waits for (iokit/Kernel/BoardHaptics.cpp) */
	while (!broken) {
		uint64_t now = now_us();
		bool active = heard_us != 0 && now - heard_us < ACTIVE_US;
		uint32_t peak = heard_peak;
		uint8_t gain = gain_for(peak);
		uint8_t tap = tap_gain != 0 ? (uint8_t)tap_gain : gain ? gain : 1;
		uint64_t settle = tap_gain != 0 ? 0 : SETTLE_US;        /* (a fixed gain needs no loudness heard first) */

		switch (state) {
		case IDLE:
		case TAP:
			/*
			 * A waveform of Darwin's that has begun (after a pause: a tap of a series is a tap of its own) is
			 * a tap -- once its first milliseconds are heard: a waveform begins softly, and they say how
			 * loud it is; one that goes on without a pause, a vibration.
			 */
			if (active && onset_us != tap_onset && now - onset_us >= settle) {
				uint64_t began = onset_us;

				if (state == TAP && !wait_standby(0) && !stop_waveform()) {
					break;
				}
				/* (a fault the chip latched in the last tap, after the last look below, is found here, not once the next plays) */
				if (!healthy("before a tap") || !start_waveform(tap_waveform, false, tap)) {
					break;
				}
				state = TAP;
				since = now;
				tap_onset = began;
				taps++;
				if (taps + loops <= TOLD_EVENTS) {
					wait_ms(4);
					say("haptic: tap %u: Darwin's waveform peaks at %u of 32768: Android's waveform %u at gain 0x%02x; the "
					    "chip's state %u\n", taps, peak, tap_waveform, tap, chip_state());
				}
				break;
			}
			if (state == IDLE) {
				break;
			}
			if (active && quiet_us < tap_onset && now - since > TAP_US) {
				if (stop_waveform() && healthy("before a vibration") && start_waveform(11, true, gain ? gain : 1)) {
					state = LOOP;
					since = followed = now;
					playing_gain = gain ? gain : 1;
					follows = 0;
					loops++;
					heard_peak = 0;
					if (taps + loops <= TOLD_EVENTS) {
						wait_ms(4);
						say("haptic: vibration %u: Darwin goes on playing (peak %u): Android's waveform 11 over and over at "
						    "gain 0x%02x, %u ms at most; the chip's state %u\n", loops, peak, gain ? gain : 1,
						    HAPTIC_MAX_MS, chip_state());
					}
				}
			} else if (!active && now - since > TAP_US) {
				state = wait_standby(0) || stop_waveform() ? IDLE : state;
			}
			break;
		case LOOP:
			/* as strong as Darwin's waveform is now (Android's gain at full scale, never more) */
			if (active && now - followed >= FOLLOW_US) {
				followed = now;
				heard_peak = 0;
				if (peak != 0 && (gain > playing_gain + 8 || gain + 8 < playing_gain)) {
					if (!put(AW_PLAYCFG2, gain)) {
						break;
					}
					playing_gain = gain;
					follows++;
				}
			}
			if (!active || now - since > HAPTIC_MAX_MS * 1000ull) {
				bool cut = now - since > HAPTIC_MAX_MS * 1000ull;

				if (cut) {
					capped++;
					rest_until = now + HAPTIC_MAX_MS * 1000ull;
				}
				if (stop_waveform()) {
					state = rest_until > now ? REST : IDLE;
					if (taps + loops <= TOLD_EVENTS + 1) {
						say("haptic: vibration %u: %s after %llu ms (its strength changed %u times, last to gain 0x%02x); "
						    "the chip is in standby\n", loops, cut ? "CUT at the limit" : "Darwin stopped, and so does the motor",
						    (now - since) / 1000, follows, playing_gain);
					}
				}
			}
			break;
		case REST:
			if (now >= rest_until) {
				state = IDLE;
			}
			break;
		}
		if (!broken && state != IDLE && state != REST && now - last_look > 50000) {
			last_look = now;
			if (!healthy("while playing")) {
				break;
			}
		}
		wait_ms(4);
	}
	say("haptic: the motor is given up on (%u taps and %u vibrations played, %u cut at %u ms)\n", taps, loops, capped,
	    HAPTIC_MAX_MS);
	/* the service taken away: audiomxd is not to go on being told that the firmware is ready (not on the way out, where
	 * the chip was put into reset by the run's end, not by a fault, and IOKit is not to be asked to do anything) */
	if (!leaving) {
		HakiosHapticsGone();
	}
}

static void
haptic_thread(__unused void *parameter, __unused wait_result_t result)
{
	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	say("haptic: level %u: %s\n", level, level == 1 ? "a look, reading only" : level == 2 ? "the reset released, registers read" :
	    level == 3 ? "the chip configured and its waveforms loaded, nothing played" : level == 4 ? "one effect played" :
	    "the motor for Darwin");
	show_pins();
	if (level >= 2 && !((rd(GPIO_CFG(PIN_RESET)) & CFG_OUTPUT) && (rd(GPIO_IO(PIN_RESET)) & 2))) {
		/* (Android's driver: the pin low, 5 ms, high, and the chip ready for I2C 8 ms later) */
		say("haptic: the reset is released: gpio%u low for 5 ms, then high\n", PIN_RESET);
		reset_pin(false);
		wait_ms(5);
		reset_pin(true);
		wait_ms(8);
	} else if (level >= 2) {
		/*
		 * Found released (a run before this one that did not get to put it back, a panic on the way out): this run is
		 * the chip's all the same, and the run's end holds it in reset as if this run had let it out.
		 */
		say("haptic: the reset is found released already (gpio%u drives high)\n", PIN_RESET);
		released = true;
	}
	if (!chip_answers()) {
		say("haptic: (the reset pin %s)\n", (rd(GPIO_CFG(PIN_RESET)) & CFG_OUTPUT) && (rd(GPIO_IO(PIN_RESET)) & 2) ?
		    "drives high: the chip should answer" : "does not drive high: the chip is held in reset or not powered");
		if (released) {
			give_up("the chip does not answer");
		}
	} else {
		show_registers();
		if (level >= 3 && configure()) {
			if (!load_waveforms()) {
				if (level >= 4 && !broken) {
					say("haptic: Android's waveforms are not in the chip's RAM: nothing is played (not whatever the RAM "
					    "holds after the chip's reset)\n");
				}
			} else if (level == 4) {
				uint32_t effect = 1;

				PE_parse_boot_argn("haptic_effect", &effect, sizeof(effect));
				if (effect < 1 || effect > 10) {
					effect = 1;     /* (the short ones only) */
				}
				(void)play_effect(effect);
			} else if (level == 5) {
				serve();
			}
		}
	}
	say("haptic: done\n");
	thread_terminate(current_thread());
}

void
hakios_haptic_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("haptic", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (!hakios_board_has(HAKIOS_HAS_HAPTIC_AW86927)) {
		/* every address below is one phone's: not on a board that does not say it is wired the same */
		say("haptic: disabled: this board's board.dts lists no \"haptic-aw86927\"\n");
		level = 0;
		return;
	}
	if (hakios_board()->pins.tlmm == 0) {
		say("haptic: disabled: the SoC file does not say where the pins are (pins { })\n");
		level = 0;
		return;
	}
	if (hakios_board()->haptic.sda == hakios_board()->haptic.scl) {
		say("haptic: disabled: the board file does not say which pins the chip is on (haptic { })\n");
		level = 0;
		return;
	}
	i2c_bus.sda = PIN_SDA;
	i2c_bus.scl = PIN_SCL;
	if (level > 5) {
		level = 5;
	}
	if (PE_parse_boot_argn("haptic_tap_gain", &tap_gain, sizeof(tap_gain)) && tap_gain > android_regs[AW_PLAYCFG2]) {
		say("haptic: haptic_tap_gain=%#x is above Android's gain: 0x%02x\n", tap_gain, android_regs[AW_PLAYCFG2]);
		tap_gain = android_regs[AW_PLAYCFG2];
	}
	if (kernel_thread_start(haptic_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end and on a panic's way out: the chip back into reset, if it was let out. */
void
hakios_haptic_quiesce(void)
{
	if (released && tlmm != 0) {
		leaving = true;
		reset_pin(false);
		if (level == 5) {
			say("haptic: at the run's end: %u taps and %u vibrations played (%u cut at %u ms), from %llu frames Darwin "
			    "wrote; the chip is held in reset\n", taps, loops, capped, HAPTIC_MAX_MS, written_frames);
		}
	}
}
