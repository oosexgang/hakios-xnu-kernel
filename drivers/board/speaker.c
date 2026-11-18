/*
 * The speakers: two Awinic AW88261 amplifiers. Their bus, addresses, interrupts and the pins of the
 * sound are the board's speaker { }; nothing here is touched unless a boot-arg asks for it.
 *
 * On the phone it was written for, each speaker has an amplifier of its own, an Awinic AW88261 (chip ID 0x2113, a "smart PA"),
 * set up over I2C -- the bus of the SoC's serial engine 3, GPIO 12 data and 13 clock, on which
 * Android's device tree and its running kernel have these two and nothing else -- and fed by
 * the SoC's first TDM interface ("MI2S 0": GPIO 97 bit clock, 100 frame sync, 99 data out),
 * which the audio DSP drives (board/sound.c). In Android's tree the amplifier at 0x35 is
 * "sound-channel 0" and takes slot 0 of the frame (the left channel), the one at 0x34 is
 * channel 1 and takes slot 1; neither has a reset pin.
 *
 * Two uses:
 *
 *   speaker_probe=1    reads registers and nothing else: the pins, the power names (below)
 *   speaker_probe=2    ... and asks the amplifiers who they are (I2C, below)
 *
 *   hakios_speaker_prepare() and the rest (sys/board/audio.h), for boot-arg sound=3 of
 *   board/sound.c: the amplifiers set up and run exactly as Android's own driver does it
 *   (the vendor's open kernel, techpack/audio/asoc/codecs/aw88263s_m20: aw882xx_device.c,
 *   aw882xx_init.c and aw882xx_pid_2113_reg.h; the phone runs its version 2.0.0), with the
 *   registers' values from Android's own configuration file -- /vendor/etc/aw882xx_acf.bin of
 *   the phone, which userland/build.sh puts into /fw of the system image from fw-audio/ and
 *   which is not in this repository. Its "Music" profile is what Android plays the speakers
 *   with (tinymix: aw_dev_0_prof, aw_dev_1_prof); 55 of its 57 registers read back from the
 *   idle chips under Android exactly as the file has them, and the other two as the driver
 *   leaves them (0x04 powered down and muted, 0x08 with the feedback off).
 *
 * What the driver does and this does the same way, per amplifier:
 *
 *   prepare  reads the chip ID and the four fuse registers; a soft reset (0x55aa into
 *            register 0); the profile's registers, every one but three as the file has them:
 *            0x04 keeps the power-down, amplifier power-down and mute bits as they are and
 *            sets the mute of the power stage ("ULS"), 0x08 has the feedback off, and 0x05 (the
 *            volume) is turned down to the lowest, -90 dB, right after; 0x50 gets the value the
 *            driver works out from the fuses (8192 * (1000 + icalk) / (1000 + vcalk)), which for
 *            this phone's two chips is the value Android left in them
 *   start    only while the TDM clock runs: power-down off, 2 ms, the PLL locked (status bits 0
 *            and 4); amplifier power-down off, 1 ms, the status as the driver wants it (bits 0, 4,
 *            8, 9); the feedback on; the two mutes off; the interrupts read away; the volume
 *            stays at the lowest
 *   volume   stepwise, 6 dB a step (the driver takes one every 100 us), to `below' eighths of a dB
 *            under Android's own level (the profile's 0x05, -2.1 dB, with Android's
 *            aw_dev_N_volume 0) and never above it
 *   stop     the power stage muted, the volume stepped down to the lowest, muted, the feedback
 *            off, 1 ms, amplifier power-down, power-down
 *
 * Every write is said in the log and read back; a value that does not read back as written, a
 * byte nobody acknowledges, or a fault in the status register (under-voltage, over-current of
 * the boost or the output, over-temperature, no clock, the watchdog, over-voltage) has both
 * amplifiers powered down at once (amplifier power-down and power-down, bits 1 and 0 of 0x04)
 * and nothing more done but that. An amplifier counts as powered down only once 0x04 reads back
 * with both bits set; until it does, the power-down is tried again whenever the amplifiers are
 * stopped (hakios_speaker_stop()) and on a panic's way out (hakios_speaker_quiesce()), and
 * board/sound.c sends the DSP silence in place of Darwin's sound. What is not done, compared
 * with Android: the DSP's Awinic algorithm, which protects the speakers with their calibration
 * (aw882xx_rx_switch); in its place the samples go through board/guard.c on their way here.
 * The driver's monitor is done (hakios_speaker_monitor(), below): once a second the chips'
 * temperature and the battery's voltage, and the reduction that the file's table asks for at
 * either.
 *
 * I2C is done by hand: a pin is either pulled low or let go (and the pull-ups raise it), at 100
 * kHz at most. For each register read or written the two pins are taken from the serial engine
 * (function qup03) as GPIOs and given back as they were found right after. A bus that is not at
 * rest is not touched -- but for a power-down: an amplifier cut off in the middle of a byte (by a
 * panic on another CPU, say) holds the data line low, and is clocked until it lets go
 * (i2c_gpio_unwedge()), then the power-down done. On a panic's way out the pins are given back as
 * the transaction it stopped found them.
 *
 * Register layouts: the files above, and Linux's sound/soc/codecs/aw88261.c; pins:
 * drivers/pinctrl/qcom/pinctrl-yupik.c of the same kernel and the phone's own device tree.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/board/audio.h>

#include "kernel.h"
#include "board.h"
#include "log.h"

static vm_offset_t tlmm, gcc, cmd_db;
static uint32_t probe_level;
static bool leaving;                    /* on a panic's way out: nothing sleeps, nothing is said but to the loader */

/* Every line to the loader's log as well: what is written to the amplifiers should be known even if the kernel dies. */
HAKIOS_DEFINE_SAY(!leaving)

#include "mmio.h"
#include "time.h"

#define wait_ms(ms)             hakios_wait_ms((ms), leaving)   /* (spun on the way out) */

/* ---- pins ------------------------------------------------------------------------------------------ */

#define TLMM_PHYS               (hakios_board()->pins.tlmm)     /* the SoC file's pins { } */
#define HAKIOS_TLMM             tlmm
#include "tlmm.h"                    /* a pin's two registers, and the four things done with them */

#include "i2c_gpio.h"                /* the two lines, a bit, a byte, the bus taken and given back */

/* which pins the bus is on, and each amplifier's address and interrupt pin, are the board's (speaker { }) */
static struct i2c_gpio i2c_bus;
#define PIN_SDA                 (i2c_bus.sda)
#define PIN_SCL                 (i2c_bus.scl)

static void
show_pin(unsigned pin, const char *what)
{
	uint32_t cfg = rd(GPIO_CFG(pin)), io = rd(GPIO_IO(pin));

	say("speaker: gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u\n", pin, what, (cfg >> 2) & 15, cfg & 3,
	    (((cfg >> 6) & 7) + 1) * 2, (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") : "input", io & 1);
}

static void
show_pins(void)
{
	/* the sound's own pins, as the board has them (board/sound.c sets them up) */
	static const char *const sound[4] = { "the sound's bit clock", "the sound's frame sync", "the sound's data in", "the sound's data out" };

	show_pin(PIN_SDA, "I2C data");
	show_pin(PIN_SCL, "I2C clock");
	for (unsigned i = 0; i < 4; i++) {
		show_pin(hakios_board()->speaker.sound_pin[i], sound[i]);
	}
	for (unsigned i = 0; i < 2; i++) {
		char what[40];

		snprintf(what, sizeof(what), "the amplifier %#x's interrupt", hakios_board()->speaker.address[i]);
		show_pin(hakios_board()->speaker.interrupt[i], what);
	}
}

/* ---- I2C by hand ----------------------------------------------------------------------------------- */

static bool powering_down;              /* both are being powered down at once: a bus held low is clocked free first */
static bool hushed;                     /* a power-down tried again and again: what fails is not said every time */
static volatile bool in_transaction;    /* the pins are taken (on a panic's way out: by a CPU that was stopped there) */

/*
 * The bus taken for one transaction; false if it is not at rest. Only for a power-down, a bus whose data line
 * is held low -- an amplifier cut off in the middle of a byte, by a transfer that a panic stopped on another
 * CPU, or by one of ours -- is clocked until it lets go (i2c_gpio_unwedge()); otherwise nothing is driven.
 */
static bool
bus_take(void)
{
	unsigned clocks;

	if (i2c_gpio_take(&i2c_bus)) {
		in_transaction = true;
		return true;
	}
	if (powering_down && i2c_gpio_unwedge(&i2c_bus, &clocks)) {
		say("speaker: the I2C bus was held low: an amplifier let go of the data line after %u clocks\n", clocks);
		in_transaction = true;
		return true;
	}
	if (!hushed) {
		say("speaker: the I2C bus is not at rest (gpio%u reads %u, gpio%u reads %u): %s\n", PIN_SDA, i2c_gpio_level(PIN_SDA),
		    PIN_SCL, i2c_gpio_level(PIN_SCL), powering_down ? "nine clocks did not free it" : "not touched");
	}
	return false;
}

static void
bus_give_back(void)
{
	i2c_gpio_give_back(&i2c_bus);
	in_transaction = false;
}

/* A 16-bit register of an Awinic amplifier, most significant byte first. Returns false if nobody answers. */
static bool
register_read(uint8_t address, uint8_t reg, uint16_t *value)
{
	bool answered;

	if (!bus_take()) {
		return false;
	}
	i2c_gpio_start(&i2c_bus);
	answered = i2c_gpio_write(&i2c_bus, (uint8_t)(address << 1)) && i2c_gpio_write(&i2c_bus, reg);
	if (answered) {
		i2c_gpio_start(&i2c_bus);
		answered = i2c_gpio_write(&i2c_bus, (uint8_t)(address << 1 | 1));
	}
	if (answered) {
		*value = (uint16_t)(i2c_gpio_read(&i2c_bus, true) << 8);
		*value |= i2c_gpio_read(&i2c_bus, false);
	}
	i2c_gpio_stop(&i2c_bus);
	bus_give_back();
	return answered;
}

static bool
register_write(uint8_t address, uint8_t reg, uint16_t value)
{
	bool answered;

	if (!bus_take()) {
		return false;
	}
	i2c_gpio_start(&i2c_bus);
	answered = i2c_gpio_write(&i2c_bus, (uint8_t)(address << 1)) && i2c_gpio_write(&i2c_bus, reg) && i2c_gpio_write(&i2c_bus, (uint8_t)(value >> 8)) &&
	    i2c_gpio_write(&i2c_bus, (uint8_t)value);
	i2c_gpio_stop(&i2c_bus);
	bus_give_back();
	return answered;
}

static const char *
amplifier_name(uint16_t id)
{
	switch (id) {
	case 0x1852: return "AW88194/AW88195";
	case 0x2013: return "AW88264";
	case 0x2032: return "AW88266";
	case 0x2049: return "AW88395";
	case 0x2055: return "AW88256";
	case 0x2066: return "AW88166";
	case 0x2071: return "AW88350";
	case 0x2113: return "AW88261";
	case 0x2183: return "AW88399";
	case 0x2308: return "AW88230";
	}
	return "an amplifier this list does not know";
}

/* ---- the amplifiers, as Android's driver runs them -------------------------------------------------- */

#define AW_ID                   0x00            /* reads the chip ID; 0x55aa written resets the chip */
#define AW_ID_AW88261           0x2113
#define AW_SOFT_RESET           0x55aa
#define AW_SYSST                0x01
#define AW_SYSINT               0x02            /* read: cleared */
#define AW_SYSINTM              0x03
#define AW_SYSCTRL              0x04
#define AW_SYSCTRL2             0x05            /* [9:0] the volume, attenuation: [9:6] 6 dB steps, [5:0] eighths of a dB */
#define AW_I2SCTRL3             0x08
#define AW_VBAT                 0x21            /* [9:0] the battery's voltage as the amplifier sees it: x 6025 / 1023 mV */
#define AW_TEMP                 0x22            /* [9:0] the amplifier's temperature, degrees Celsius, two's complement */
#define AW_PVDD                 0x23            /* [9:0] its boosted supply */
#define AW_ISNDAT               0x24            /* the current and voltage sense, as last measured */
#define AW_VSNDAT               0x25
#define AW_DBGCTRL              0x14
#define AW_VSNTM1               0x50
#define AW_EFRH4                0x78
#define AW_EFRH3                0x79
#define AW_EFRL4                0x7c
#define AW_EFRL3                0x7d

#define SYSCTRL_PWDN            (1u << 0)
#define SYSCTRL_AMPPD           (1u << 1)
#define SYSCTRL_HMUTE           (1u << 8)
#define SYSCTRL_ULS_HMUTE       (1u << 14)
#define I2SCTRL3_TXEN           (1u << 6)
#define DBGCTRL_EF_OR           (1u << 3)       /* the fuses' two copies are or-ed; else and-ed */
#define SYSINTM_DEFAULT         0xffff

#define SYSST_PLLS              (1u << 0)
#define SYSST_OTHS              (1u << 1)
#define SYSST_OCDS              (1u << 3)
#define SYSST_CLKS              (1u << 4)
#define SYSST_NOCLKS            (1u << 5)
#define SYSST_WDS               (1u << 6)
#define SYSST_SWS               (1u << 8)
#define SYSST_BSTS              (1u << 9)
#define SYSST_BSTOCS            (1u << 11)
#define SYSST_UVLS              (1u << 14)
#define SYSST_OVP2S             (1u << 15)
#define SYSST_PLL_CHECK         (SYSST_CLKS | SYSST_PLLS)
#define SYSST_CHECK             (SYSST_BSTS | SYSST_SWS | SYSST_CLKS | SYSST_PLLS)
#define SYSST_FAULTS            (SYSST_OVP2S | SYSST_UVLS | SYSST_BSTOCS | SYSST_WDS | SYSST_NOCLKS | SYSST_OCDS | SYSST_OTHS)

#define VOLUME_LOWEST           (90 * 8)        /* the driver's "mute volume", in eighths of a dB */
#define VOLUME_STEP             (6 * 8)
#define STATUS_TRIES            10              /* the driver's AW_DEV_SYSST_CHECK_MAX, 2 ms apart */

/* Android's configuration file ("ACF", Awinic's format; Linux: sound/soc/codecs/aw88395/aw88395_lib.c) */
#define ACF_PROFILE_MUSIC       0
#define ACF_DATA_REGISTERS      4               /* a block with a header of its own and registers in it */
#define ACF_DATA_MONITOR        10              /* the driver's "monitor": what to do when the chip is cold or the battery low */
#define MONITOR_ROWS            8
#define ACF_BLOCK_HEADER        0x3c

struct amplifier {
	uint8_t address;
	unsigned device;                /* the file's device number for it (Android's sound-channel) */
	char where[20];                 /* for the log: "0x35, slot 0" */
	uint16_t profile[64][2];        /* register, value */
	unsigned count;
	uint16_t sysctrl;               /* what the profile has for 0x04, 0x08 and 0x05 */
	uint16_t i2sctrl3;
	uint16_t volume_at;             /* Android's level: from the profile's 0x05, in eighths of a dB of attenuation */
	uint16_t volume;                /* where it is now */
	uint16_t volume_high;           /* the profile's bits 15:10 of 0x05, which the volume leaves as they are */
	bool prepared, started;
	bool down_pending;              /* told to power down, and not yet read back so: tried again (stop, quiesce) */
	unsigned down_tries;
	/* the monitor's tables (from the file) and what was last seen */
	struct { int16_t min, max; uint16_t less; } temp_rows[MONITOR_ROWS], vbat_rows[MONITOR_ROWS];
	unsigned temp_count, vbat_count;
	uint64_t looks;                 /* (64 bits: the average divides by as many as there have been, up to three) */
	int temps[3], vbats[3];
	int temp_row, vbat_row;
};

static struct amplifier amplifiers[2] = {
	{ .device = 0, .temp_row = -1, .vbat_row = -1 },
	{ .device = 1, .temp_row = -1, .vbat_row = -1 },
};
static bool broken;                     /* something went wrong: both are powered down (or that is tried again), nothing more is done */

/* The board's speaker { } taken: the bus's pins, each amplifier's address. False if the board file has none. */
static bool
wired(void)
{
	const struct hakios_board *b = hakios_board();

	if (b->speaker.sda == b->speaker.scl) {
		return false;
	}
	i2c_bus.sda = b->speaker.sda;
	i2c_bus.scl = b->speaker.scl;
	for (unsigned i = 0; i < 2; i++) {
		amplifiers[i].address = b->speaker.address[i];
		snprintf(amplifiers[i].where, sizeof(amplifiers[i].where), "%#x, slot %u", amplifiers[i].address, i);
	}
	return true;
}

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/*
 * The driver's monitor for one amplifier (Android has it on: aw_dev_N_monitor), from the file: every second it
 * reads the chip's temperature and the battery's voltage, and a table says what to do for each range of them.
 * This file's: at 7 to 100 degrees nothing; at 2 to 5 degrees 3 dB less, at -2 to 0 4.5 dB, at -50 to -5 8 dB (and
 * a lower current limit for the boost, which is not done here); from 3.85 V nothing, at 3.65 to 3.75 V and up to
 * 3.55 V a lower "vmax" for the DSP's algorithm, which does not run here. In its temperature rows a vmax goes with
 * each reduction of the volume, so the battery rows' vmax is turned into a volume reduction in the same proportion
 * (the first temperature row's: 3 dB for its vmax): 1.6 and 3.1 dB. Between the ranges the last one holds.
 *   block: checksum (of the bytes after it), version, chip name[16], ui version, the time and count of looks,
 *   switches, then for the temperatures and for the voltages each: a value, the number of rows, a row's size and
 *   the rows' offset; a row: int16 lowest, int16 highest, uint16 boost current, uint16 reduction (eighths of a
 *   dB), int32 vmax
 */
static void
acf_monitor(uint16_t device, const uint8_t *block, uint32_t length)
{
	struct amplifier *a = NULL;
	uint32_t sum = 0, temp_count, temp_size, temp_at, vbat_count, vbat_size, vbat_at;
	int64_t per_eighth = 0;

	for (unsigned i = 0; i < 2; i++) {
		if (amplifiers[i].device == device) {
			a = &amplifiers[i];
		}
	}
	if (a == NULL || length < 0x48) {
		return;
	}
	for (uint32_t i = 4; i < length; i++) {
		sum += block[i];
	}
	temp_count = le32(block + 0x2c);
	temp_size = le32(block + 0x30);
	temp_at = le32(block + 0x34);
	vbat_count = le32(block + 0x3c);
	vbat_size = le32(block + 0x40);
	vbat_at = le32(block + 0x44);
	/* (in 64 bits: a size or a place of the file's own that would wrap a 32-bit sum is not a table inside the block) */
	if (sum != le32(block) || temp_size < 12 || vbat_size < 12 || temp_count > MONITOR_ROWS || vbat_count > MONITOR_ROWS ||
	    (uint64_t)temp_at + (uint64_t)temp_count * temp_size > length ||
	    (uint64_t)vbat_at + (uint64_t)vbat_count * vbat_size > length) {
		say("speaker: device %u's monitor table is not of the shape known: not used\n", device);
		return;
	}
	for (uint32_t i = 0; i < temp_count; i++) {
		const uint8_t *row = block + temp_at + i * temp_size;
		int32_t vmax = (int32_t)le32(row + 8);

		a->temp_rows[i].min = (int16_t)(row[0] | row[1] << 8);
		a->temp_rows[i].max = (int16_t)(row[2] | row[3] << 8);
		a->temp_rows[i].less = (uint16_t)(row[6] | row[7] << 8);
		if (per_eighth == 0 && a->temp_rows[i].less != 0 && vmax < 0) {
			per_eighth = -(int64_t)vmax / a->temp_rows[i].less;
		}
	}
	for (uint32_t i = 0; i < vbat_count; i++) {
		const uint8_t *row = block + vbat_at + i * vbat_size;
		int32_t vmax = (int32_t)le32(row + 8);

		a->vbat_rows[i].min = (int16_t)(row[0] | row[1] << 8);
		a->vbat_rows[i].max = (int16_t)(row[2] | row[3] << 8);
		a->vbat_rows[i].less = (uint16_t)(row[6] | row[7] << 8);
		if (vmax < 0 && per_eighth > 0) {
			a->vbat_rows[i].less += (uint16_t)((-(int64_t)vmax + per_eighth / 2) / per_eighth);
		}
	}
	a->temp_count = temp_count;
	a->vbat_count = vbat_count;
	for (uint32_t i = 0; i < temp_count; i++) {
		say("speaker: the amplifier %s's monitor: at %d to %d degrees, %u.%03u dB less\n", a->where, a->temp_rows[i].min,
		    a->temp_rows[i].max, a->temp_rows[i].less / 8, a->temp_rows[i].less % 8 * 125);
	}
	for (uint32_t i = 0; i < vbat_count; i++) {
		say("speaker: the amplifier %s's monitor: at %d to %d mV, %u.%03u dB less\n", a->where, a->vbat_rows[i].min,
		    a->vbat_rows[i].max, a->vbat_rows[i].less / 8, a->vbat_rows[i].less % 8 * 125);
	}
}

/* The "Music" profile of every amplifier, from the file; false if the file is not of the shape known. */
static bool
acf_profiles(const uint8_t *acf, uint64_t size)
{
	uint32_t entry_size, entries, at;

	if (size < 0x50 || size > 0x100000) {
		say("speaker: the configuration file is %llu bytes: not one of Awinic's\n", size);
		return false;
	}
	entry_size = le32(acf + 0x34);
	entries = le32(acf + 0x38);
	at = le32(acf + 0x3c);
	if (entries == 0 || entries > 64 || entry_size % entries != 0 || entry_size / entries < 0x30 ||
	    (uint64_t)at + entry_size > size) {
		say("speaker: the configuration file's table (%u entries, %u bytes at %#x) is not of a shape known\n", entries,
		    entry_size, at);
		return false;
	}
	for (uint32_t e = 0; e < entries; e++) {
		const uint8_t *entry = acf + at + e * (entry_size / entries);
		char name[17];
		uint16_t device = (uint16_t)(entry[20] | entry[21] << 8), profile = (uint16_t)(entry[26] | entry[27] << 8);
		uint32_t kind = le32(entry + 28), length = le32(entry + 32), offset = le32(entry + 36);
		uint32_t checksum, sum = 0, data_length, count;
		const uint8_t *block = acf + offset;
		struct amplifier *a = NULL;

		memcpy(name, entry + 4, 16);
		name[16] = 0;
		if (kind == ACF_DATA_MONITOR) {
			acf_monitor(device, acf + offset, (uint64_t)offset + length <= size ? length : 0);
			continue;
		}
		if (kind != ACF_DATA_REGISTERS || profile != ACF_PROFILE_MUSIC || strcmp(name, "aw88261") != 0) {
			continue;
		}
		for (unsigned i = 0; i < 2; i++) {
			if (amplifiers[i].device == device) {
				a = &amplifiers[i];
			}
		}
		if (a == NULL || (uint64_t)offset + length > size || length < ACF_BLOCK_HEADER + 4) {
			say("speaker: the configuration file's entry %u (device %u) does not fit\n", e, device);
			return false;
		}
		checksum = le32(block);
		data_length = le32(block + 0x10);
		count = le32(block + ACF_BLOCK_HEADER);
		if (ACF_BLOCK_HEADER + data_length > length || data_length != 4 + 4 * count || count > 64 ||
		    le32(block + 0x20) != 2 || le32(block + 0x24) != 2) {
			say("speaker: device %u's Music profile is not a list of 16-bit registers (%u bytes, %u registers)\n", device,
			    data_length, count);
			return false;
		}
		for (uint32_t i = 4; i < ACF_BLOCK_HEADER + data_length; i++) {
			sum += block[i];
		}
		if (sum != checksum) {
			say("speaker: device %u's Music profile does not add up (%#x, the file says %#x)\n", device, sum, checksum);
			return false;
		}
		a->count = count;
		for (uint32_t i = 0; i < count; i++) {
			const uint8_t *pair = block + ACF_BLOCK_HEADER + 4 + 4 * i;

			a->profile[i][0] = (uint16_t)(pair[0] | pair[1] << 8);
			a->profile[i][1] = (uint16_t)(pair[2] | pair[3] << 8);
		}
	}
	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];
		bool have[3] = { false, false, false };

		for (unsigned r = 0; r < a->count; r++) {
			uint16_t reg = a->profile[r][0], value = a->profile[r][1];

			if (reg == AW_SYSCTRL) {
				a->sysctrl = value;
				have[0] = true;
			} else if (reg == AW_I2SCTRL3) {
				a->i2sctrl3 = value;
				have[1] = true;
			} else if (reg == AW_SYSCTRL2) {
				/* the driver's aw_pid_2113_reg_val_to_db() */
				a->volume_at = (uint16_t)(((value & 0x3ff) >> 6) * VOLUME_STEP + (value & 0x3f));
				a->volume_high = value & 0xfc00;
				have[2] = true;
			} else if (reg == AW_ID || reg >= 0x70) {
				say("speaker: device %u's Music profile writes register %#x, which is not one of the configuration's\n",
				    a->device, reg);
				return false;
			}
		}
		if (a->count == 0 || !have[0] || !have[1] || !have[2] || a->volume_at >= VOLUME_LOWEST) {
			say("speaker: no usable Music profile for device %u (the amplifier at %#x) in the configuration file\n",
			    a->device, a->address);
			return false;
		}
		say("speaker: the amplifier %s: Music profile of %u registers; Android's volume there is -%u.%03u dB (%#x)\n", a->where,
		    a->count, a->volume_at / 8, a->volume_at % 8 * 125, a->volume_at);
	}
	return true;
}

/*
 * One amplifier powered down at once: amplifier power-down, power-down and mute set in 0x04. It counts as
 * stopped only once 0x04 reads back with both power-down bits set; until then it is a power-down still to
 * be done, which hakios_speaker_stop() and hakios_speaker_quiesce() try again.
 */
static bool
power_down_one(struct amplifier *a)
{
	uint16_t value = 0, after = 0;
	bool down;

	a->down_pending = true;
	a->down_tries++;
	if (!register_read(a->address, AW_SYSCTRL, &value)) {
		value = a->sysctrl;
	}
	value |= SYSCTRL_PWDN | SYSCTRL_AMPPD | SYSCTRL_HMUTE;
	if (!register_write(a->address, AW_SYSCTRL, value) || !register_read(a->address, AW_SYSCTRL, &after)) {
		if (!hushed) {
			say("speaker: the amplifier %s: 0x04 <- 0x%04x NOT ACKNOWLEDGED: NOT KNOWN TO BE POWERED DOWN, to be tried again\n",
			    a->where, value);
		}
		return false;
	}
	down = (after & (SYSCTRL_PWDN | SYSCTRL_AMPPD)) == (SYSCTRL_PWDN | SYSCTRL_AMPPD);
	say("speaker: the amplifier %s: 0x04 <- 0x%04x, reads 0x%04x%s\n", a->where, value, after, down ? "" :
	    ": NOT POWERED DOWN, to be tried again");
	if (!down) {
		return false;
	}
	if (a->down_tries > 1) {
		say("speaker: THE AMPLIFIER %s IS POWERED DOWN, at try %u\n", a->where, a->down_tries);
	}
	a->down_pending = false;
	a->down_tries = 0;
	a->started = false;
	return true;
}

/* Both powered down at once, without the driver's ceremony: a fault, or a value that did not stick. */
static void
power_down_now(const char *why)
{
	broken = true;
	say("speaker: %s: BOTH AMPLIFIERS ARE POWERED DOWN AT ONCE (0x04: amplifier power-down and power-down set)\n", why);
	powering_down = true;
	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];

		if (!a->prepared && !a->started && !a->down_pending) {
			continue;
		}
		(void)power_down_one(a);
	}
	powering_down = false;
}

/* The power-downs that did not read back, tried again: said the first few times, and when one takes. */
static void
power_down_again(void)
{
	powering_down = true;
	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];

		if (!a->down_pending) {
			continue;
		}
		hushed = a->down_tries >= 3;
		(void)power_down_one(a);
	}
	hushed = false;
	powering_down = false;
}

/* One register written, said and read back; false (and both powered down) if it did not stick. */
static bool
put(struct amplifier *a, uint8_t reg, uint16_t value)
{
	uint16_t after = 0;

	if (broken) {
		return false;
	}
	if (!register_write(a->address, reg, value)) {
		say("speaker: the amplifier %s: 0x%02x <- 0x%04x NOT ACKNOWLEDGED\n", a->where, reg, value);
		power_down_now("a write was not acknowledged");
		return false;
	}
	if (!register_read(a->address, reg, &after)) {
		say("speaker: the amplifier %s: 0x%02x <- 0x%04x, NOT READ BACK\n", a->where, reg, value);
		power_down_now("a register could not be read back");
		return false;
	}
	say("speaker: the amplifier %s: 0x%02x <- 0x%04x, reads 0x%04x%s\n", a->where, reg, value, after, after == value ? "" :
	    ": NOT AS WRITTEN");
	if (after != value) {
		power_down_now("a register did not read back as written");
		return false;
	}
	return true;
}

static bool
get(struct amplifier *a, uint8_t reg, uint16_t *value)
{
	if (!register_read(a->address, reg, value)) {
		say("speaker: the amplifier %s: 0x%02x NOT READ\n", a->where, reg);
		return false;
	}
	return true;
}

/* The bits of 0x04 named in `bits' set or cleared, the rest as they read. */
static bool
sysctrl(struct amplifier *a, uint16_t bits, bool set)
{
	uint16_t value;

	if (broken) {
		return false;
	}
	if (!get(a, AW_SYSCTRL, &value)) {
		/* (as a write that does not stick: an amplifier half stopped, or half started, is not left running) */
		power_down_now("0x04 could not be read");
		return false;
	}
	return put(a, AW_SYSCTRL, set ? (value | bits) : (value & ~bits));
}

/* The volume register at `volume' eighths of a dB of attenuation (the driver's aw_pid_2113_set_volume()). */
static bool
set_volume(struct amplifier *a, uint16_t volume)
{
	uint16_t value;

	if (volume > VOLUME_LOWEST) {
		volume = VOLUME_LOWEST;
	}
	if (volume < a->volume_at) {
		volume = a->volume_at;  /* never louder than Android */
	}
	/* (the other bits as the profile has them and as they were written: not read first, for a quick fade) */
	value = (uint16_t)(a->volume_high | (volume / VOLUME_STEP) << 6 | volume % VOLUME_STEP);
	if (broken || !put(a, AW_SYSCTRL2, value)) {
		return false;
	}
	a->volume = volume;
	return true;
}

/* What the status register says, and whether it says a fault. A status that could not be read is a fault, and reads as 0xffff. */
static bool
faulty(struct amplifier *a, uint16_t *status)
{
	if (!get(a, AW_SYSST, status)) {
		*status = 0xffff;       /* (said in the line that reports the fault: not what was in memory) */
		return true;
	}
	return (*status & SYSST_FAULTS) != 0;
}

bool
hakios_speaker_prepare(const uint8_t *acf, uint64_t size)
{
	if (!hakios_board_has(HAKIOS_HAS_SPEAKER_AW88261) || hakios_board()->pins.tlmm == 0) {
		say("speaker: not on this board: its board.dts lists no \"speaker-aw88261\", or its SoC file has no pins { }\n");
		return false;
	}
	if (!wired()) {
		say("speaker: the board file does not say which pins the amplifiers are on (speaker { })\n");
		return false;
	}
	if (tlmm == 0) {
		tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	}
	broken = false;
	say("speaker: the amplifiers are prepared as Android's driver prepares them; the I2C pins as found:\n");
	show_pins();
	if (!acf_profiles(acf, size)) {
		return false;
	}
	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];
		uint16_t id = 0, h4 = 0, h3 = 0, l4 = 0, l3 = 0, icalk_bits, vcalk_bits;
		int icalk, vcalk;
		uint16_t vcalb, ef_or = 0;

		if (!get(a, AW_ID, &id)) {
			say("speaker: nobody at I2C address %#x: not powered, or not there\n", a->address);
			power_down_now("an amplifier does not answer");
			return false;
		}
		say("speaker: the amplifier %s: chip ID 0x%04x, %s\n", a->where, id, amplifier_name(id));
		if (id != AW_ID_AW88261) {
			return false;
		}
		if (!get(a, AW_EFRH4, &h4) || !get(a, AW_EFRH3, &h3) || !get(a, AW_EFRL4, &l4) || !get(a, AW_EFRL3, &l3)) {
			return false;
		}
		for (unsigned r = 0; r < a->count; r++) {
			if (a->profile[r][0] == AW_DBGCTRL) {
				ef_or = a->profile[r][1] & DBGCTRL_EF_OR;
			}
		}
		/* the driver's aw_dev_get_icalk(), aw_dev_get_vcalk() and aw_dev_set_vcalb() */
		icalk_bits = ef_or ? ((h4 & 0x3ff) | (l4 & 0x3ff)) : ((h4 & 0x3ff) & (l4 & 0x3ff));
		vcalk_bits = ef_or ? ((h3 & 0x3ff) | (l3 & 0x3ff)) : ((h3 & 0x3ff) & (l3 & 0x3ff));
		icalk = 1000 + ((icalk_bits & 0x200) ? (int)icalk_bits - 0x400 : (int)icalk_bits);
		vcalk = 1000 + ((vcalk_bits & 0x200) ? (int)vcalk_bits - 0x400 : (int)vcalk_bits);
		if (vcalk <= 0 || icalk <= 0) {
			say("speaker: the amplifier %s: fuses %#x %#x %#x %#x make no sense\n", a->where, h4, h3, l4, l3);
			return false;
		}
		vcalb = (uint16_t)(8192 * icalk / vcalk);
		say("speaker: the amplifier %s: fuses 0x%04x 0x%04x 0x%04x 0x%04x: icalk %d, vcalk %d, so 0x50 is to be 0x%04x\n", a->where,
		    h4, h3, l4, l3, icalk, vcalk, vcalb);

		say("speaker: the amplifier %s: soft reset (0x00 <- 0x55aa), as the driver does it first\n", a->where);
		a->prepared = true;
		if (!register_write(a->address, AW_ID, AW_SOFT_RESET)) {
			power_down_now("the soft reset was not acknowledged");
			return false;
		}
		wait_ms(2);
		for (unsigned r = 0; r < a->count; r++) {
			uint8_t reg = (uint8_t)a->profile[r][0];
			uint16_t value = a->profile[r][1], now;

			switch (reg) {
			case AW_SYSINTM:
				value = SYSINTM_DEFAULT;
				break;
			case AW_SYSCTRL:
				/* the three power and mute bits stay as they are; the power stage's mute on */
				if (!get(a, AW_SYSCTRL, &now)) {
					power_down_now("0x04 could not be read");
					return false;
				}
				value = (uint16_t)((value & ~(SYSCTRL_PWDN | SYSCTRL_AMPPD | SYSCTRL_HMUTE)) |
				    (now & (SYSCTRL_PWDN | SYSCTRL_AMPPD | SYSCTRL_HMUTE)) | SYSCTRL_ULS_HMUTE);
				break;
			case AW_I2SCTRL3:
				value &= (uint16_t)~I2SCTRL3_TXEN;
				break;
			case AW_VSNTM1:
				continue;
			}
			if (!put(a, reg, value)) {
				return false;
			}
		}
		if (!put(a, AW_VSNTM1, vcalb) || !set_volume(a, VOLUME_LOWEST)) {
			return false;
		}
	}
	say("speaker: BOTH AMPLIFIERS ARE CONFIGURED as Android's Music profile has them: powered down, muted, at the lowest volume\n");
	return true;
}

bool
hakios_speaker_start(void)
{
	/*
	 * Both prepared, before either is started. A preparing that stopped at the second (another chip there, its fuses
	 * not read) leaves the first prepared and not broken: asked to start, the first would be powered up and unmuted,
	 * and the second's "not prepared" would then end it with false -- the first left running and unmuted, nothing
	 * powered down, by a start that says it did not start.
	 */
	if (broken || !amplifiers[0].prepared || !amplifiers[1].prepared) {
		return false;
	}
	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];
		uint16_t status = 0, interrupts = 0;
		unsigned tries;

		if (broken || !a->prepared) {
			return false;
		}
		say("speaker: the amplifier %s: power-down off\n", a->where);
		a->started = true;
		if (!sysctrl(a, SYSCTRL_PWDN, false)) {
			return false;
		}
		wait_ms(2);
		for (tries = 0; tries < STATUS_TRIES; tries++) {
			if (!get(a, AW_SYSST, &status)) {
				power_down_now("the status could not be read");
				return false;
			}
			if ((status & SYSST_PLL_CHECK) == SYSST_PLL_CHECK) {
				break;
			}
			wait_ms(2);
		}
		say("speaker: the amplifier %s: status 0x%04x: its PLL %s\n", a->where, status, tries < STATUS_TRIES ?
		    "IS LOCKED to the bit clock" : "DOES NOT LOCK: no clock?");
		if (tries == STATUS_TRIES) {
			power_down_now("an amplifier's PLL does not lock");
			return false;
		}
		say("speaker: the amplifier %s: amplifier power-down off\n", a->where);
		if (!sysctrl(a, SYSCTRL_AMPPD, false)) {
			return false;
		}
		wait_ms(1);
		for (tries = 0; tries < STATUS_TRIES; tries++) {
			if (faulty(a, &status)) {
				break;
			}
			if ((status & SYSST_CHECK) == SYSST_CHECK) {
				break;
			}
			wait_ms(2);
		}
		say("speaker: the amplifier %s: status 0x%04x\n", a->where, status);
		if ((status & SYSST_FAULTS) != 0 || (status & SYSST_CHECK) != SYSST_CHECK) {
			power_down_now((status & SYSST_FAULTS) ? "an amplifier reports a fault" : "an amplifier's status is not as "
			    "the driver wants it");
			return false;
		}
		if (!put(a, AW_I2SCTRL3, a->i2sctrl3) ||        /* the feedback on, as the profile has it */
		    !sysctrl(a, SYSCTRL_ULS_HMUTE, false) || !sysctrl(a, SYSCTRL_HMUTE, false)) {
			return false;
		}
		if (!get(a, AW_SYSINT, &interrupts) || !get(a, AW_SYSINT, &interrupts) || !put(a, AW_SYSINTM, SYSINTM_DEFAULT)) {
			power_down_now("the interrupts could not be read");
			return false;
		}
		if (faulty(a, &status)) {
			say("speaker: the amplifier %s: status 0x%04x\n", a->where, status);
			power_down_now("an amplifier reports a fault");
			return false;
		}
		say("speaker: THE AMPLIFIER %s RUNS, unmuted at the lowest volume (status 0x%04x)\n", a->where, status);
	}
	return true;
}

bool
hakios_speaker_volume(uint32_t below, uint32_t step_ms)
{
	uint16_t status;

	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];
		uint32_t target = below > VOLUME_LOWEST ? VOLUME_LOWEST : a->volume_at + below;

		if (broken || !a->started) {
			return false;
		}
		if (target > VOLUME_LOWEST) {
			target = VOLUME_LOWEST;
		}
		say("speaker: the amplifier %s: the volume to -%u.%03u dB (Android's level less %u.%03u dB), 6 dB a step every %u ms\n",
		    a->where, target / 8, target % 8 * 125, (target - a->volume_at) / 8, (target - a->volume_at) % 8 * 125, step_ms);
	}
	/* both together, a step every step_ms, the status looked at after every step */
	for (;;) {
		bool moved = false;

		for (unsigned i = 0; i < 2; i++) {
			struct amplifier *a = &amplifiers[i];
			uint32_t target = below > VOLUME_LOWEST ? VOLUME_LOWEST : a->volume_at + below, next;

			if (target > VOLUME_LOWEST) {
				target = VOLUME_LOWEST;
			}
			if (a->volume == target) {
				continue;
			}
			next = a->volume > target ? (a->volume - target > VOLUME_STEP ? (uint32_t)a->volume - VOLUME_STEP : target) :
			    (target - a->volume > VOLUME_STEP ? (uint32_t)a->volume + VOLUME_STEP : target);
			if (!set_volume(a, (uint16_t)next)) {
				return false;
			}
			/* (a slow fade looks at the status after every step, a quick one once it is over) */
			if (step_ms >= 10 && faulty(a, &status)) {
				say("speaker: the amplifier %s: status 0x%04x\n", a->where, status);
				power_down_now("an amplifier reports a fault");
				return false;
			}
			moved = true;
		}
		if (!moved) {
			break;
		}
		if (step_ms) {
			wait_ms(step_ms);
		}
	}
	return hakios_speaker_healthy();
}

bool
hakios_speaker_healthy(void)
{
	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];
		uint16_t status;

		if (broken) {
			return false;
		}
		if (!a->started) {
			continue;
		}
		if (faulty(a, &status)) {
			say("speaker: the amplifier %s: status 0x%04x\n", a->where, status);
			power_down_now("an amplifier reports a fault");
			return false;
		}
	}
	return true;
}

/* the row of a monitor table that a value falls in; between rows, the one before */
static int
monitor_row(const struct amplifier *a, bool temperature, int value, int before)
{
	unsigned count = temperature ? a->temp_count : a->vbat_count;

	for (unsigned i = 0; i < count; i++) {
		int min = temperature ? a->temp_rows[i].min : a->vbat_rows[i].min;
		int max = temperature ? a->temp_rows[i].max : a->vbat_rows[i].max;

		if (value >= min && value <= max) {
			return (int)i;
		}
	}
	return before;
}

/*
 * The driver's monitor, once: the temperature and the battery voltage of every amplifier that runs (the average of
 * the last three looks, as the file's count says), and the reduction their tables ask for, the larger of the two
 * amplifiers'. With `tell', what was read (also the boosted supply and the current and voltage sense, raw).
 */
uint32_t
hakios_speaker_monitor(bool tell)
{
	uint32_t wanted = 0;

	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];
		uint16_t vbat = 0, temp = 0, pvdd = 0, isn = 0, vsn = 0;
		int t = 0, v = 0;
		uint32_t less = 0;

		if (broken || !a->started) {
			continue;
		}
		if (!get(a, AW_VBAT, &vbat) || !get(a, AW_TEMP, &temp)) {
			/* (as a status that could not be read, faulty(): a byte nobody acknowledges) */
			power_down_now("an amplifier's temperature or the battery could not be read");
			return wanted;
		}
		a->temps[a->looks % 3] = (temp & 0x200) ? (int)(temp & 0x3ff) - 0x400 : (int)(temp & 0x3ff);
		a->vbats[a->looks % 3] = (int)(vbat & 0x3ff) * 6025 / 1023;
		a->looks++;
		for (unsigned k = 0; k < 3 && k < a->looks; k++) {
			t += a->temps[k];
			v += a->vbats[k];
		}
		t /= (int)(a->looks < 3 ? a->looks : 3);
		v /= (int)(a->looks < 3 ? a->looks : 3);
		a->temp_row = monitor_row(a, true, t, a->temp_row);
		a->vbat_row = monitor_row(a, false, v, a->vbat_row);
		less = (a->temp_row >= 0 ? a->temp_rows[a->temp_row].less : 0) + (a->vbat_row >= 0 ? a->vbat_rows[a->vbat_row].less : 0);
		if (less > wanted) {
			wanted = less;
		}
		if (tell) {
			if (!get(a, AW_PVDD, &pvdd) || !get(a, AW_ISNDAT, &isn) || !get(a, AW_VSNDAT, &vsn)) {
				power_down_now("an amplifier's supply or sense could not be read");
				return wanted;
			}
			say("speaker: the amplifier %s: %d degrees, the battery %d mV (0x%03x, 0x%03x; supply 0x%03x, sense 0x%04x 0x%04x): "
			    "%u.%03u dB less\n", a->where, t, v, temp, vbat, pvdd, isn, vsn, less / 8, less % 8 * 125);
		}
	}
	return wanted;
}

void
hakios_speaker_stop(void)
{
	if (broken) {
		power_down_again();     /* (already powered down, but for one that was not read back so) */
		return;
	}
	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];

		if (!a->started) {
			continue;
		}
		say("speaker: the amplifier %s is stopped as Android's driver stops it\n", a->where);
		if (!sysctrl(a, SYSCTRL_ULS_HMUTE, true)) {
			return;
		}
		while (a->volume < VOLUME_LOWEST) {
			if (!set_volume(a, (uint16_t)(a->volume + VOLUME_STEP > VOLUME_LOWEST ? VOLUME_LOWEST : a->volume +
			    VOLUME_STEP))) {
				return;
			}
			wait_ms(1);
		}
		if (!sysctrl(a, SYSCTRL_HMUTE, true) || !put(a, AW_I2SCTRL3, (uint16_t)(a->i2sctrl3 & ~I2SCTRL3_TXEN))) {
			return;
		}
		wait_ms(1);
		if (!sysctrl(a, SYSCTRL_AMPPD, true) || !sysctrl(a, SYSCTRL_PWDN, true)) {
			return;
		}
		a->started = false;
		say("speaker: THE AMPLIFIER %s IS POWERED DOWN\n", a->where);
	}
}

void
hakios_speaker_quiesce(void)
{
	bool any = false, interrupted;
	struct i2c_gpio as_found;

	for (unsigned i = 0; i < 2; i++) {
		any |= amplifiers[i].started || amplifiers[i].down_pending;
	}
	if (!any || tlmm == 0) {
		return;
	}
	leaving = true;
	hushed = false;                 /* (as a power-down tried again on a CPU the panic stopped may have left it) */
	/*
	 * A transaction that the panic stopped on another CPU has the pins as GPIOs, and perhaps an amplifier holding
	 * the data line in the middle of a byte (clocked free by the power-down's first take). What it found them as
	 * -- the serial engine's -- is what they are given back as at the end, not what this one finds.
	 */
	interrupted = in_transaction;
	as_found = i2c_bus;
	power_down_now("the kernel is on its way out");
	if (interrupted) {
		i2c_gpio_give_back(&as_found);
	}
}

/* ---- speaker_probe=N: a look ------------------------------------------------------------------------ */

static void
show_amplifiers(void)
{
	/* the chip ID, then registers that every chip of the family has at the same place and that reading does not change */
	static const struct {
		uint8_t reg;
		const char *name;
	} more[] = { { 0x01, "status" }, { 0x04, "system control" }, { 0x06, "I2S control" } };

	for (unsigned i = 0; i < 2; i++) {
		uint16_t id = 0, value = 0;

		if (!register_read(amplifiers[i].address, AW_ID, &id)) {
			say("speaker: nobody at I2C address %#x: not powered, or not there\n", amplifiers[i].address);
			continue;
		}
		say("speaker: the amplifier %s is there: chip ID 0x%04x, %s\n", amplifiers[i].where, id, amplifier_name(id));
		for (unsigned r = 0; r < sizeof(more) / sizeof(more[0]); r++) {
			if (register_read(amplifiers[i].address, more[r].reg, &value)) {
				say("speaker:   register 0x%02x (%s) = 0x%04x\n", more[r].reg, more[r].name, value);
			}
		}
	}
}

/* ---- power names of the audio subsystem ------------------------------------------------------------- */

#define CMD_DB_PHYS             (hakios_board()->rpmh.cmd_db)           /* the SoC file's rpmh { } */
#define CMD_DB_SIZE             (hakios_board()->rpmh.cmd_db_size)
#define GCC_PHYS                (hakios_board()->clocks.gcc)    /* the SoC file's clocks { } */
#define GCC_SIZE                0x60000UL

#include "cmd_db.h"

/* The RPMh address of a resource by its name in the command database the boot loader left; 0 = none. */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;

	return hakios_cmd_db_address(db, CMD_DB_SIZE, name);
}

static void
show_power(void)
{
	/* the subsystem's two supplies and, for the DSP, the processor's own two and the crystal */
	static const char *const supplies[] = { "lcx.lvl", "lmx.lvl", "cx.lvl", "mx.lvl", "xo.lvl" };

	for (unsigned i = 0; i < sizeof(supplies) / sizeof(supplies[0]); i++) {
		uint32_t address = rpmh_address(supplies[i]);

		if (address) {
			say("speaker: supply \"%s\" can be asked for at RPMh address %#x\n", supplies[i], address);
		} else {
			say("speaker: no supply \"%s\" in the command database\n", supplies[i]);
		}
	}
	/*
	 * (The audio subsystem's bus clock, GCC_CFG_NOC_LPASS_CBCR, is not read: on the phone that read
	 * stops the kernel with a synchronous external abort -- the register is not the main processors'
	 * to read while the audio subsystem belongs to its DSP.)
	 */
}

static void
probe_thread(__unused void *parameter, __unused wait_result_t result)
{
	if (tlmm == 0) {
		tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	}
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);      /* memory, not registers: no alignment rules */
	say("speaker: a look at the phone's sound hardware (level %u; nothing is switched on)\n", probe_level);
	show_pins();
	show_power();
	if (probe_level >= 2) {
		show_amplifiers();
	}
	say("speaker: done\n");
	thread_terminate(current_thread());
}

void
hakios_speaker_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("speaker_probe", &probe_level, sizeof(probe_level)) || probe_level == 0) {
		return;
	}
	if (!hakios_board_has(HAKIOS_HAS_SPEAKER_AW88261)) {
		/* every address below is one phone's: not on a board that does not say it is wired the same */
		say("speaker: disabled: this board's board.dts lists no \"speaker-aw88261\"\n");
		probe_level = 0;
		return;
	}
	if (hakios_board()->pins.tlmm == 0 || hakios_board()->clocks.gcc == 0 || hakios_board()->rpmh.cmd_db == 0) {
		say("speaker: disabled: the SoC file does not say where the pins, the clock controller and RPMh are (pins { }, clocks { }, rpmh { })\n");
		probe_level = 0;
		return;
	}
	if (!wired()) {
		say("speaker: disabled: the board file does not say which pins the amplifiers are on (speaker { })\n");
		probe_level = 0;
		return;
	}
	if (kernel_thread_start(probe_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
