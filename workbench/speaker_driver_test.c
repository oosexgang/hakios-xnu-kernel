/*
 * The speaker amplifiers' driver itself (drivers/board/speaker.c), run as a host
 * program against the model of the SoC's pins with two amplifiers on them (i2c_device_model.h: two
 * addresses, registers of two bytes) and a command database (cmd_db_image.h). What is tested: its
 * look, speaker_probe=1 and 2, which is to read and nothing else; and preparing the amplifiers for
 * sound from Awinic's configuration file, where a wrong value could reach a speaker -- what is
 * written, in which order; starting them, the volume, stopping; and that both are powered down
 * at once when anything is not as expected -- and the power-down tried again until it reads back,
 * a bus held low clocked free for it.
 *
 *   speaker_driver_test SCENARIO     one scenario a process: the driver keeps its state in statics
 */
#include <unistd.h>

#include "fakekernel.h"

/* which pins the bus is on and where the amplifiers answer is the board's: the model is put where it says */
static unsigned sda_pin = 12, scl_pin = 13, first_address = 0x35;
#define I2C_DEVICE_SDA          sda_pin
#define I2C_DEVICE_SCL          scl_pin
#define I2C_DEVICE_ADDRESS      first_address
#include "i2c_device_model.h"
#include "cmd_db_image.h"

extern char log_text[];

#define AS_FOUND        (1u << 2 | 3u)          /* the bus's pins: the serial engine's function, pulled up */

uint32_t
hakios_test_rd(vm_offset_t address)
{
	i2c_device_looked_at(address);
	return tlmm_model_rd(address);
}

void hakios_test_wr(vm_offset_t address, uint32_t value) { tlmm_model_wr(address, value); }
vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }
/* the command database is memory the boot loader left: the image */
vm_offset_t ml_io_map_wcomb(vm_offset_t phys __unused, vm_size_t size __unused) { return (vm_offset_t)&image; }

/* what is written into the amplifiers, for the scenarios that prepare them: [0] the first, [1] the second */
static bool register_written[2][128];
static int first_written[2] = { -1, -1 };       /* the register each one was written first */
static unsigned soft_resets[2], writes_into[2];
static unsigned uncalibrated;                   /* the profile's own value written where the calibration goes */
static int loudest[2] = { -1, -1 }, last_volume[2] = { -1, -1 };       /* the attenuation written, eighths of a dB */
static int biggest_step;                        /* ... and the largest change from one write to the next */
static unsigned amplifier_on[2];                /* writes of 0x04 with both power-down bits clear: the power stage runs */
static int deaf_register = -1;                  /* a register of the first amplifier that does not take what is written */
static unsigned volumes_into_second;            /* volumes written into the second amplifier */
static unsigned fault_at_volume;                /* the second reports an over-current from that volume on (0: never) */

static void
amplifier_written(uint8_t index, uint8_t value)
{
	unsigned which = device.bank == device.reg_b, reg = index / 2;

	writes_into[which]++;
	register_written[which][reg & 127] = true;
	if (first_written[which] < 0) {
		first_written[which] = (int)reg;
	}
	uncalibrated += reg == 0x50 && (index & 1) && value == 0x77;
	if (reg == 0) {
		/* 0x55aa into the chip ID's register resets the chip; the ID stays what it is */
		if ((index & 1) && value == 0xaa) {
			soft_resets[which]++;
			/* powered down; so that each of the three bits the driver is to keep differs from a profile's
			 * somewhere, the first comes up muted with its amplifier on, the second the other way round */
			device.bank[0x04 * 2] = which ? 0x02 : 0x03;
			device.bank[0x04 * 2 + 1] = which ? 0x03 : 0x01;
		}
		return;
	}
	if (which == 0 && (int)reg == deaf_register) {
		return;
	}
	device.bank[index] = value;
	if (reg == 0x05 && (index & 1)) {
		int word = device.bank[index - 1] << 8 | value, attenuation = ((word & 0x3ff) >> 6) * 48 + (word & 0x3f);

		if (last_volume[which] >= 0 && abs(attenuation - last_volume[which]) > biggest_step) {
			biggest_step = abs(attenuation - last_volume[which]);
		}
		if (loudest[which] < 0 || attenuation < loudest[which]) {
			loudest[which] = attenuation;
		}
		last_volume[which] = attenuation;
		if (which == 1 && ++volumes_into_second == fault_at_volume) {
			device.bank[0x01 * 2 + 1] |= 1u << 3;   /* (its status: over-current) */
		}
	}
	amplifier_on[which] += reg == 0x04 && (index & 1) && !(value & 3);
}

void hakios_speaker_init(void);
void hakios_speaker_quiesce(void);

#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#define get speaker_get
#define put speaker_put
#include "board/board.c"
#include "board/speaker.c"
#undef get
#undef put

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited = 0; !(cond) && waited < (ms); waited += 5) usleep(5000); } while (0)

static void
wide(uint8_t *bank, uint8_t reg, uint16_t value)
{
	bank[reg * 2] = (uint8_t)(value >> 8);
	bank[reg * 2 + 1] = (uint8_t)value;
}

/* a phone with both amplifiers powered and at rest, and a command database with the audio subsystem's supplies */
static void
phone(const char *name)
{
	static const struct resource supplies[] = {
		{ "lcx.lvl", 0x30000, NULL, 0 }, { "lmx.lvl", 0x30010, NULL, 0 }, { "cx.lvl", 0x30020, NULL, 0 },
		{ "mx.lvl", 0x30030, NULL, 0 },         /* (no "xo.lvl": the driver is to say so) */
	};

	board = name;
	fake_delay_divisor = 5;
	tlmm_model_at(hakios_board() != NULL ? hakios_board()->pins.tlmm : 0);
	device.address_b = 0x34;
	if (hakios_board() != NULL && hakios_board()->speaker.sda != hakios_board()->speaker.scl) {
		sda_pin = hakios_board()->speaker.sda;
		scl_pin = hakios_board()->speaker.scl;
		first_address = hakios_board()->speaker.address[0];
		device.address_b = hakios_board()->speaker.address[1];
	}
	model.after_write = changed;
	model.cfg[I2C_DEVICE_SDA] = model.cfg[I2C_DEVICE_SCL] = AS_FOUND;
	device.wide = true;
	wide(device.reg, 0x00, 0x2113);
	wide(device.reg, 0x01, 0x0311);
	wide(device.reg, 0x04, 0x4303);
	wide(device.reg, 0x06, 0x14e8);
	wide(device.reg_b, 0x00, 0x2113);
	wide(device.reg_b, 0x01, 0x0211);
	wide(device.reg_b, 0x04, 0x4302);
	wide(device.reg_b, 0x06, 0x24e8);
	lines();
	device.was_sda = device.was_scl = true;
	empty();
	slave(0, CMD_DB_ARC, supplies, sizeof(supplies) / sizeof(supplies[0]));
}

/* what a look never does */
static void
only_read(void)
{
	CHECK(device.writes == 0, "%u bytes were written into an amplifier's registers", device.writes);
	CHECK(device.driven_high == 0, "a line of the bus was driven high %u times", device.driven_high);
	CHECK(model.elsewhere == 0, "%u accesses outside the pins' registers", model.elsewhere);
	CHECK(model.cfg[I2C_DEVICE_SDA] == AS_FOUND && model.cfg[I2C_DEVICE_SCL] == AS_FOUND, "the bus's pins are not as they were found");
	for (unsigned i = 0; i < PINS; i++) {
		if (i != I2C_DEVICE_SDA && i != I2C_DEVICE_SCL && (model.cfg[i] != 0 || model.io[i] != 0)) {
			CHECK(false, "pin %u was touched", i);
			break;
		}
	}
	for (unsigned i = 0; i < 64; i++) {
		if (image.bytes[SIZE + i] != 0xee) {
			CHECK(false, "the driver wrote after the command database");
			break;
		}
	}
}

static void
look(const char *args)
{
	boot_args = args;
	hakios_speaker_init();
	WAIT_FOR(logged("speaker: done"), 20000);
	CHECK(logged("speaker: done"), "the look ended: %s", log_text);
}

/* ---- the configuration file, as Awinic's tool writes it (the part the driver reads) ---------------- */

static uint8_t acf[0x600];
static unsigned acf_size, acf_entries, registers_end;   /* (where the last list of registers ends) */

static void
put32(uint8_t *at, uint32_t value)
{
	at[0] = (uint8_t)value;
	at[1] = (uint8_t)(value >> 8);
	at[2] = (uint8_t)(value >> 16);
	at[3] = (uint8_t)(value >> 24);
}

/* one more entry: a list of 16-bit registers for that device (Android's sound-channel) and profile */
static void
acf_registers(unsigned device_number, unsigned profile, const uint16_t (*pairs)[2], unsigned count)
{
	uint8_t *entry = acf + 0x50 + acf_entries * 0x30, *block = acf + acf_size;
	uint32_t length = 0x3c + 4 + 4 * count, sum = 0;

	memcpy(entry + 4, "aw88261", 8);
	entry[20] = (uint8_t)device_number;
	entry[26] = (uint8_t)profile;
	put32(entry + 28, 4);                   /* registers */
	put32(entry + 32, length);
	put32(entry + 36, acf_size);
	put32(block + 0x10, 4 + 4 * count);
	put32(block + 0x20, 2);                 /* a register's number and its value are two bytes each */
	put32(block + 0x24, 2);
	put32(block + 0x3c, count);
	for (unsigned i = 0; i < count; i++) {
		block[0x40 + 4 * i] = (uint8_t)pairs[i][0];
		block[0x41 + 4 * i] = (uint8_t)(pairs[i][0] >> 8);
		block[0x42 + 4 * i] = (uint8_t)pairs[i][1];
		block[0x43 + 4 * i] = (uint8_t)(pairs[i][1] >> 8);
	}
	for (uint32_t i = 4; i < length; i++) {
		sum += block[i];
	}
	put32(block, sum);
	acf_size += length;
	registers_end = acf_size;
	acf_entries++;
	put32(acf + 0x34, acf_entries * 0x30);
	put32(acf + 0x38, acf_entries);
}

/* one more entry: that device's monitor table -- rows of lowest, highest, (boost current,) reduction, vmax */
struct row { int16_t min, max; uint16_t less; int32_t vmax; };

static void
acf_monitor_table(unsigned device_number, const struct row *temps, unsigned temp_count, const struct row *vbats, unsigned vbat_count)
{
	uint8_t *entry = acf + 0x50 + acf_entries * 0x30, *block = acf + acf_size;
	uint32_t length = 0x48 + 12 * (temp_count + vbat_count), sum = 0;

	memcpy(entry + 4, "aw88261", 8);
	entry[20] = (uint8_t)device_number;
	put32(entry + 28, 10);                  /* a monitor's table */
	put32(entry + 32, length);
	put32(entry + 36, acf_size);
	put32(block + 0x2c, temp_count);
	put32(block + 0x30, 12);
	put32(block + 0x34, 0x48);
	put32(block + 0x3c, vbat_count);
	put32(block + 0x40, 12);
	put32(block + 0x44, 0x48 + 12 * temp_count);
	for (unsigned i = 0; i < temp_count + vbat_count; i++) {
		const struct row *row = i < temp_count ? &temps[i] : &vbats[i - temp_count];
		uint8_t *at = block + 0x48 + 12 * i;

		at[0] = (uint8_t)row->min;
		at[1] = (uint8_t)((uint16_t)row->min >> 8);
		at[2] = (uint8_t)row->max;
		at[3] = (uint8_t)((uint16_t)row->max >> 8);
		at[6] = (uint8_t)row->less;
		at[7] = (uint8_t)(row->less >> 8);
		put32(at + 8, (uint32_t)row->vmax);
	}
	for (uint32_t i = 4; i < length; i++) {
		sum += block[i];
	}
	put32(block, sum);
	acf_size += length;
	acf_entries++;
	put32(acf + 0x34, acf_entries * 0x30);
	put32(acf + 0x38, acf_entries);
}

/* warm: nothing; cool: 3 dB less (and the vmax that goes with it), cold more; a low battery: a vmax only */
static const struct row temps[] = { { 7, 100, 0, 0 }, { 2, 5, 24, -2400 }, { -2, 0, 36, -3600 }, { -50, -5, 64, -6400 } };
static const struct row vbats[] = { { 3850, 5000, 0, 0 }, { 3650, 3750, 0, -1300 }, { 0, 3550, 0, -2500 } };

/* The Music profile of the two: the first's fuses are to be or-ed (0x14 bit 3), the second's and-ed. */
static const uint16_t music_a[][2] = {
	{ 0x03, 0x1234 }, { 0x04, 0x0202 }, { 0x05, 0x1403 }, { 0x06, 0x34e8 }, { 0x08, 0x4a4c }, { 0x14, 0x0018 },
	{ 0x50, 0x7777 }, { 0x52, 0x1f0c },
};
static const uint16_t music_b[][2] = {
	{ 0x03, 0x1234 }, { 0x04, 0x0300 }, { 0x05, 0x1403 }, { 0x06, 0x35e8 }, { 0x08, 0x4a4c }, { 0x14, 0x0010 },
	{ 0x50, 0x7777 }, { 0x52, 0x1f0c },
};
static const uint16_t another_profile[][2] = { { 0x04, 0xdead }, { 0x05, 0x0000 }, { 0x08, 0xbeef }, { 0x60, 0xffff } };

static void
file(void)
{
	memset(acf, 0, sizeof(acf));
	acf_entries = 0;
	acf_size = 0x50 + 6 * 0x30;             /* the header, then room for six entries */
	put32(acf + 0x3c, 0x50);
	acf_registers(0, 0, music_a, 8);
	acf_registers(0, 1, another_profile, 4);        /* (a profile that is not Music: not to be used) */
	acf_registers(1, 0, music_b, 8);
	acf_monitor_table(0, temps, 4, vbats, 3);
	acf_monitor_table(1, temps, 4, vbats, 3);
}

/* a phone whose amplifiers take what is written, with their fuses, and the file */
static void
phone_with_file(const char *name)
{
	phone(name);
	device.written = amplifier_written;
	wide(device.reg, 0x78, 0x0005);         /* the calibration fuses, two copies of each */
	wide(device.reg, 0x7c, 0x0003);
	wide(device.reg, 0x79, 0x03fe);
	wide(device.reg, 0x7d, 0x03fe);
	memcpy(device.reg_b + 0x78 * 2, device.reg + 0x78 * 2, 12);
	file();
}

static uint16_t
reads(const uint8_t *bank, uint8_t reg)
{
	return (uint16_t)(bank[reg * 2] << 8 | bank[reg * 2 + 1]);
}

/* what preparing never does, however it ends: a line driven high, a pin other than the bus's touched, a fuse written */
static void
never(void)
{
	CHECK(device.driven_high == 0, "a line of the bus was driven high %u times", device.driven_high);
	CHECK(model.elsewhere == 0, "%u accesses outside the pins' registers", model.elsewhere);
	CHECK(model.cfg[I2C_DEVICE_SDA] == AS_FOUND && model.cfg[I2C_DEVICE_SCL] == AS_FOUND, "the bus's pins are not as they were found");
	for (unsigned i = 0; i < PINS; i++) {
		if (i != I2C_DEVICE_SDA && i != I2C_DEVICE_SCL && (model.cfg[i] != 0 || model.io[i] != 0)) {
			CHECK(false, "pin %u was touched", i);
			break;
		}
	}
	for (unsigned which = 0; which < 2; which++) {
		for (unsigned reg = 0x70; reg < 128; reg++) {
			CHECK(!register_written[which][reg], "the fuses' register %#x was written", reg);
		}
	}
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

/* without its boot argument: nothing */
static void
off_by_default(void)
{
	phone("phone-video");
	boot_args = "";
	hakios_speaker_init();
	usleep(50000);
	CHECK(model.writes == 0 && model.reads == 0 && !logged("speaker:"), "nothing read, nothing written, nothing said: %s", log_text);
	boot_args = "speaker_probe=0";
	hakios_speaker_init();
	usleep(50000);
	CHECK(model.writes == 0 && model.reads == 0 && !logged("speaker:"), "nor with speaker_probe=0: %s", log_text);
}

/* level 1: the pins and the supplies' names; not one register of the pins is written, the bus is not used */
static void
pins_and_supplies(void)
{
	phone("phone-video");
	look("speaker_probe=1");
	CHECK(model.writes == 0, "%u writes to the pins' registers", model.writes);
	CHECK(device.starts == 0 && device.bytes_in == 0 && device.bytes_out == 0, "the bus was used");
	CHECK(logged("gpio12 (I2C data): function 1, pull 3, 2 mA, input, reads 1"), "the data pin as found: %s", log_text);
	CHECK(logged("gpio13 (I2C clock): function 1, pull 3"), "the clock pin as found: %s", log_text);
	CHECK(logged("gpio97 (the sound's bit clock)") && logged("gpio100 (the sound's frame sync)") && logged("gpio98 (the sound's data in)") &&
	    logged("gpio99 (the sound's data out)"), "the sound's own four pins, as the board has them: %s", log_text);
	CHECK(logged("gpio102 (the amplifier 0x35's interrupt): function 0, pull 0"), "the first one's interrupt: %s", log_text);
	CHECK(logged("gpio45 (the amplifier 0x34's interrupt): function 0, pull 0"), "the last pin of the list: %s", log_text);
	CHECK(logged("supply \"lcx.lvl\" can be asked for at RPMh address 0x30000"), "the first supply: %s", log_text);
	CHECK(logged("supply \"lmx.lvl\" can be asked for at RPMh address 0x30010"), "the second: %s", log_text);
	CHECK(logged("supply \"cx.lvl\" can be asked for at RPMh address 0x30020"), "the third: %s", log_text);
	CHECK(logged("supply \"mx.lvl\" can be asked for at RPMh address 0x30030"), "the fourth: %s", log_text);
	CHECK(logged("no supply \"xo.lvl\" in the command database"), "one that is not there: %s", log_text);
	CHECK(!logged("chip ID"), "level 1 does not ask the amplifiers: %s", log_text);
	only_read();
}

/* level 2: both amplifiers are asked who they are and three registers more; each answers with its own */
static void
both_answer(void)
{
	phone("phone-video");
	look("speaker_probe=2");
	CHECK(logged("the amplifier 0x35, slot 0 is there: chip ID 0x2113, AW88261"), "the first: %s", log_text);
	CHECK(logged("the amplifier 0x34, slot 1 is there: chip ID 0x2113, AW88261"), "the second: %s", log_text);
	CHECK(logged("register 0x01 (status) = 0x0311") && logged("register 0x01 (status) = 0x0211"), "each one's status: %s", log_text);
	CHECK(logged("register 0x04 (system control) = 0x4303") && logged("register 0x04 (system control) = 0x4302"), "0x04: %s", log_text);
	CHECK(logged("register 0x06 (I2S control) = 0x14e8") && logged("register 0x06 (I2S control) = 0x24e8"), "0x06: %s", log_text);
	CHECK(device.bytes_out == 16, "two bytes of each of eight registers were read, not %u", device.bytes_out);
	CHECK(device.starts == 16 && device.stops == 8, "eight transfers of two starts and a stop: %u starts, %u stops", device.starts, device.stops);
	only_read();
}

/* one of the two is not there (not powered): said, and the other is still read */
static void
one_is_missing(void)
{
	phone("phone-video");
	device.address_b = 0;
	look("speaker_probe=2");
	CHECK(logged("the amplifier 0x35, slot 0 is there: chip ID 0x2113"), "the first: %s", log_text);
	CHECK(logged("nobody at I2C address 0x34: not powered, or not there"), "the second: %s", log_text);
	CHECK(device.bytes_out == 8, "only the first one's registers were read: %u bytes", device.bytes_out);
	only_read();
}

/* neither answers */
static void
nobody_there(void)
{
	phone("phone-video");
	device.absent = true;
	look("speaker_probe=2");
	CHECK(logged("nobody at I2C address 0x35") && logged("nobody at I2C address 0x34"), "both said: %s", log_text);
	CHECK(device.bytes_out == 0 && !logged("chip ID"), "nothing read: %s", log_text);
	only_read();
}

/* a chip of the family that is not this driver's: named, read, and nothing else */
static void
another_amplifier(void)
{
	phone("phone-video");
	wide(device.reg_b, 0x00, 0x2032);
	wide(device.reg, 0x00, 0x1234);
	look("speaker_probe=2");
	CHECK(logged("0x34, slot 1 is there: chip ID 0x2032, AW88266"), "named: %s", log_text);
	CHECK(logged("0x35, slot 0 is there: chip ID 0x1234, an amplifier this list does not know"), "not known: %s", log_text);
	only_read();
}

/* a phone with the amplifiers on another bus, at other addresses, on a chip whose pins are elsewhere */
static void
another_wiring(void)
{
	phone("phone-keys");
	look("speaker_probe=2");
	CHECK(logged("gpio60 (I2C data): function 1, pull 3") && logged("gpio61 (I2C clock): function 1, pull 3"), "its bus: %s", log_text);
	CHECK(logged("gpio70 (the amplifier 0x36's interrupt)") && logged("gpio71 (the amplifier 0x37's interrupt)"), "its interrupts: %s", log_text);
	CHECK(!logged("gpio12 ") && !logged("gpio13 ") && !logged("gpio102 ") && !logged("gpio45 "), "not the first phone's pins: %s", log_text);
	CHECK(logged("gpio120 (the sound's bit clock)") && logged("gpio121 (the sound's frame sync)") && logged("gpio122 (the sound's data in)") &&
	    logged("gpio123 (the sound's data out)"), "its sound's pins: %s", log_text);
	CHECK(!logged("gpio97 ") && !logged("gpio98 ") && !logged("gpio99 ") && !logged("gpio100 "), "not the first phone's: %s", log_text);
	CHECK(logged("the amplifier 0x36, slot 0 is there: chip ID 0x2113, AW88261"), "the first: %s", log_text);
	CHECK(logged("the amplifier 0x37, slot 1 is there: chip ID 0x2113, AW88261"), "the second: %s", log_text);
	CHECK(logged("register 0x01 (status) = 0x0311") && logged("register 0x01 (status) = 0x0211"), "each one's status: %s", log_text);
	CHECK(device.bytes_out == 16, "two bytes of each of eight registers were read, not %u", device.bytes_out);
	only_read();
}

/* the data line is held low by something: the bus is not touched, and that is said */
static void
bus_not_at_rest(void)
{
	phone("phone-video");
	device.stuck = true;
	lines();
	device.was_sda = false;
	look("speaker_probe=2");
	CHECK(logged("the I2C bus is not at rest (gpio12 reads 0, gpio13 reads 1): not touched"), "said: %s", log_text);
	CHECK(device.bytes_out == 0 && !logged("chip ID"), "nothing read: %s", log_text);
	only_read();
}

/*
 * Prepared for sound: each amplifier is reset first, then gets its Music profile and nothing else, with the
 * interrupts masked, the transmitter off, the calibration computed from its own fuses, the lowest volume --
 * and stays powered down and muted.
 */
static void
prepared(void)
{
	static const uint8_t expected[] = { 0x00, 0x03, 0x04, 0x05, 0x06, 0x08, 0x14, 0x50, 0x52 };

	phone_with_file("phone-video");
	CHECK(hakios_speaker_prepare(acf, acf_size), "prepared: %s", log_text);
	CHECK(logged("BOTH AMPLIFIERS ARE CONFIGURED"), "said: %s", log_text);
	for (unsigned which = 0; which < 2; which++) {
		const uint8_t *bank = which ? device.reg_b : device.reg;

		CHECK(first_written[which] == 0 && soft_resets[which] == 1, "amplifier %u: reset first, once (first %d, %u resets)", which,
		    first_written[which], soft_resets[which]);
		for (unsigned reg = 0; reg < 128; reg++) {
			CHECK(register_written[which][reg] == (memchr(expected, (int)reg, sizeof(expected)) != NULL),
			    "amplifier %u: register %#x %s", which, reg, register_written[which][reg] ? "was written" : "was not written");
		}
		CHECK(reads(bank, 0x03) == 0xffff, "amplifier %u: every interrupt masked, not %#x", which, reads(bank, 0x03));
		/* the profile's with the chip's own power-down, amplifier power-down and mute (not the profile's), and the power stage's mute */
		CHECK(reads(bank, 0x04) == (which ? 0x4203 : 0x4301), "amplifier %u: 0x04 is %#x", which, reads(bank, 0x04));
		CHECK(reads(bank, 0x08) == 0x4a0c, "amplifier %u: the transmitter is off: %#x", which, reads(bank, 0x08));
		CHECK(reads(bank, 0x05) == 0x17c0, "amplifier %u: the lowest volume, the profile's other bits: %#x", which, reads(bank, 0x05));
		CHECK(reads(bank, 0x06) == (which ? 0x35e8 : 0x34e8) && reads(bank, 0x52) == 0x1f0c, "amplifier %u: the rest as the profile has it", which);
		/* 8192 * icalk / vcalk: fuses 5 and 3 or-ed are 7, and-ed 1; 0x3fe is -2 */
		CHECK(reads(bank, 0x50) == (which ? 8192 * 1001 / 998 : 8192 * 1007 / 998), "amplifier %u: the calibration is %#x", which,
		    reads(bank, 0x50));
		CHECK(uncalibrated == 0, "the profile's own value was written where the calibration goes");
		CHECK(reads(bank, 0x60) == 0, "amplifier %u: another profile's register was written", which);
	}
	never();
}

/* a chip that is not the one the profile is for: nothing is written into either */
static void
not_this_chip(void)
{
	phone_with_file("phone-video");
	wide(device.reg, 0x00, 0x2032);
	CHECK(!hakios_speaker_prepare(acf, acf_size), "refused: %s", log_text);
	CHECK(logged("chip ID 0x2032, AW88266"), "named: %s", log_text);
	CHECK(writes_into[0] == 0 && writes_into[1] == 0, "nothing was written: %u, %u bytes", writes_into[0], writes_into[1]);
	never();
}

/* the second does not answer: the first, already configured, is powered down at once */
static void
second_is_missing(void)
{
	phone_with_file("phone-video");
	device.address_b = 0;
	wide(device.reg, 0x04, 0x4000);         /* (as if it had been left running) */
	CHECK(!hakios_speaker_prepare(acf, acf_size), "refused: %s", log_text);
	CHECK(logged("nobody at I2C address 0x34") && logged("BOTH AMPLIFIERS ARE POWERED DOWN AT ONCE"), "said: %s", log_text);
	CHECK((reads(device.reg, 0x04) & 0x103) == 0x103, "the first is powered down and muted: %#x", reads(device.reg, 0x04));
	{
		char line[64];

		snprintf(line, sizeof(line), "0x04 <- 0x%04x, reads 0x%04x", reads(device.reg, 0x04), reads(device.reg, 0x04));
		CHECK(logged(line), "what it reads afterwards is said, read from it: %s", log_text);
	}
	CHECK(writes_into[1] == 0, "nothing went into the second");
	never();
}

/* a register that does not read back as written: both powered down, nothing more is written */
static void
write_does_not_stick(void)
{
	unsigned before;

	phone_with_file("phone-video");
	deaf_register = 0x06;
	wide(device.reg, 0x04, 0x4000);
	CHECK(!hakios_speaker_prepare(acf, acf_size), "refused: %s", log_text);
	CHECK(logged("0x06 <- 0x34e8, reads 0x14e8: NOT AS WRITTEN") && logged("BOTH AMPLIFIERS ARE POWERED DOWN AT ONCE"), "said: %s", log_text);
	CHECK((reads(device.reg, 0x04) & 0x103) == 0x103, "the first is powered down and muted: %#x", reads(device.reg, 0x04));
	CHECK(!register_written[0][0x08] && !register_written[0][0x50], "nothing after the register that failed");
	CHECK(writes_into[1] == 0, "nothing went into the second");
	before = writes_into[0];
	CHECK(!hakios_speaker_start(), "nor is it started after that");
	CHECK(writes_into[0] == before && writes_into[1] == 0, "and nothing more is written");
	never();
}

/* a file that is not what it says: its sum is off, it is cut short, it writes a fuse -- the bus is not used */
static void
file_not_trusted(void)
{
	static const uint16_t fuse[][2] = { { 0x04, 0x0300 }, { 0x05, 0x1403 }, { 0x08, 0x4a4c }, { 0x78, 0x0001 } };

	phone_with_file("phone-video");
	acf[registers_end - 2] ^= 1;            /* a value of the second amplifier's profile */
	CHECK(!hakios_speaker_prepare(acf, acf_size), "a sum that is off: %s", log_text);
	CHECK(logged("device 1's Music profile does not add up"), "said: %s", log_text);
	file();
	CHECK(!hakios_speaker_prepare(acf, registers_end - 8), "a file cut short: %s", log_text);
	CHECK(logged("does not fit"), "said: %s", log_text);
	CHECK(!hakios_speaker_prepare(acf, 0x20), "a file of 32 bytes: %s", log_text);
	CHECK(logged("not one of Awinic's"), "said: %s", log_text);
	memset(acf, 0, sizeof(acf));
	acf_entries = 0;
	acf_size = 0x50 + 4 * 0x30;
	put32(acf + 0x3c, 0x50);
	acf_registers(0, 0, fuse, 4);
	acf_registers(1, 0, music_b, 8);
	CHECK(!hakios_speaker_prepare(acf, acf_size), "a profile that writes a fuse: %s", log_text);
	CHECK(logged("writes register 0x78, which is not one of the configuration's"), "said: %s", log_text);
	CHECK(device.starts == 0 && writes_into[0] == 0 && writes_into[1] == 0, "the bus was not used");
	never();
}

static bool down(const uint8_t *bank);

/* profiles the driver cannot use: one without the system control register, one that writes 0x70 -- the bus is not used */
static void
profiles_not_usable(void)
{
	static const uint16_t no_sysctrl[][2] = { { 0x05, 0x1403 }, { 0x06, 0x34e8 }, { 0x08, 0x4a4c } };
	static const uint16_t at_0x70[][2] = { { 0x04, 0x0202 }, { 0x05, 0x1403 }, { 0x08, 0x4a4c }, { 0x70, 0x0001 } };

	phone_with_file("phone-video");
	memset(acf, 0, sizeof(acf));
	acf_entries = 0;
	acf_size = 0x50 + 4 * 0x30;
	put32(acf + 0x3c, 0x50);
	acf_registers(0, 0, no_sysctrl, 3);
	acf_registers(1, 0, music_b, 8);
	CHECK(!hakios_speaker_prepare(acf, acf_size) && logged("no usable Music profile for device 0"), "no 0x04: %s", log_text);
	memset(acf, 0, sizeof(acf));
	acf_entries = 0;
	acf_size = 0x50 + 4 * 0x30;
	put32(acf + 0x3c, 0x50);
	acf_registers(0, 0, at_0x70, 4);
	acf_registers(1, 0, music_b, 8);
	CHECK(!hakios_speaker_prepare(acf, acf_size) && logged("writes register 0x70, which is not one of the configuration's"),
	    "0x70: %s", log_text);
	CHECK(device.starts == 0 && writes_into[0] == 0 && writes_into[1] == 0, "the bus was not used");
	never();
}

/* monitor tables at the edge: eight rows, the most there may be, are taken; battery rows with a vmax when no
 * temperature row has one stay as they are (there is nothing to turn the vmax into dB with); a table of temperature
 * rows only, which end where its block ends, is taken */
static void
monitor_tables_at_the_edge(void)
{
	static const struct row eight[] = { { 90, 100, 0, 0 }, { 80, 89, 0, 0 }, { 70, 79, 0, 0 }, { 60, 69, 0, 0 },
		                            { 50, 59, 0, 0 }, { 40, 49, 0, 0 }, { 30, 39, 0, 0 }, { 2, 5, 24, 0 } };

	phone_with_file("phone-video");
	memset(acf, 0, sizeof(acf));
	acf_entries = 0;
	acf_size = 0x50 + 4 * 0x30;
	put32(acf + 0x3c, 0x50);
	acf_registers(0, 0, music_a, 8);
	acf_registers(1, 0, music_b, 8);
	acf_monitor_table(0, eight, 8, vbats, 3);
	acf_monitor_table(1, temps, 4, vbats, 0);
	CHECK(hakios_speaker_prepare(acf, acf_size), "prepared: %s", log_text);
	CHECK(logged("the amplifier 0x34, slot 1's monitor: at 2 to 5 degrees, 3.000 dB less") &&
	    !logged("the amplifier 0x34, slot 1's monitor: at 3650"), "no battery rows: the temperature rows taken: %s", log_text);
	CHECK(!logged("not of the shape known") && logged("the amplifier 0x35, slot 0's monitor: at 2 to 5 degrees, 3.000 dB less"),
	    "eight rows taken: %s", log_text);
	CHECK(logged("the amplifier 0x35, slot 0's monitor: at 90 to 100 degrees, 0.000 dB less") &&
	    logged("the amplifier 0x35, slot 0's monitor: at 3650 to 3750 mV, 0.000 dB less"), "no vmax in dB for the first: %s", log_text);
	never();
}

/* started before it was prepared: nothing */
static void
started_unprepared(void)
{
	phone_with_file("phone-video");
	CHECK(!hakios_speaker_start(), "not started");
	CHECK(!logged("power-down off") && !logged("status"), "nothing tried: %s", log_text);
	CHECK(device.starts == 0 && writes_into[0] == 0 && writes_into[1] == 0, "the bus was not used");
}

/*
 * The preparing stopped at the second (a chip that is not this driver's there), the first already configured: a start
 * asked for all the same starts neither -- the first is not powered up and unmuted by a start that then says it failed.
 */
static void
started_with_the_second_unprepared(void)
{
	phone_with_file("phone-video");
	wide(device.reg_b, 0x00, 0x2032);
	CHECK(!hakios_speaker_prepare(acf, acf_size), "refused: %s", log_text);
	CHECK(soft_resets[0] == 1 && writes_into[1] == 0, "(the first configured, nothing into the second)");
	CHECK(!hakios_speaker_start(), "not started");
	CHECK(amplifier_on[0] == 0 && !logged("THE AMPLIFIER 0x35, slot 0 RUNS"), "the first was powered up: %s", log_text);
	CHECK(down(device.reg) && (reads(device.reg, 0x04) & 0x4100), "the first powered down and muted as prepared: %#x",
	    reads(device.reg, 0x04));
	never();
}

/* a board without the part, though its SoC file has the pins: nothing */
static void
board_without_the_part(void)
{
	phone("phone-cmd");
	CHECK(!hakios_speaker_prepare(acf, acf_size) && logged("speaker: not on this board"), "said: %s", log_text);
	CHECK(device.starts == 0, "the bus was not used");
}

/* the second's status lacks what the driver waits for (no fault said, the power stage not up): both down */
static void
status_never_ready(void)
{
	phone_with_file("phone-video");
	wide(device.reg_b, 0x01, 0x0211);
	CHECK(hakios_speaker_prepare(acf, acf_size), "prepared: %s", log_text);
	CHECK(!hakios_speaker_start(), "not started: %s", log_text);
	CHECK(logged("an amplifier's status is not as the driver wants it: BOTH AMPLIFIERS ARE POWERED DOWN AT ONCE") &&
	    !logged("THE AMPLIFIER 0x34, slot 1 RUNS"), "said: %s", log_text);
	CHECK(down(device.reg) && down(device.reg_b), "both down: %#x, %#x", reads(device.reg, 0x04), reads(device.reg_b, 0x04));
	never();
}

/* both prepared and started on a phone whose amplifiers see the clock and report nothing */
static void
running(void)
{
	phone_with_file("phone-video");
	wide(device.reg_b, 0x01, 0x0311);
	CHECK(hakios_speaker_prepare(acf, acf_size), "prepared: %s", log_text);
	/* (the volume is watched from here on: while they are powered down the profile's own goes through) */
	loudest[0] = loudest[1] = -1;
	biggest_step = 0;
	CHECK(hakios_speaker_start(), "started: %s", log_text);
}

static bool
down(const uint8_t *bank)
{
	return (reads(bank, 0x04) & 1) == 1;            /* powered down */
}

/* Started: powered up and unmuted, the transmitter as the profile has it, still at the lowest volume. */
static void
started(void)
{
	running();
	CHECK(logged("THE AMPLIFIER 0x35, slot 0 RUNS") && logged("THE AMPLIFIER 0x34, slot 1 RUNS"), "said: %s", log_text);
	for (unsigned which = 0; which < 2; which++) {
		const uint8_t *bank = which ? device.reg_b : device.reg;

		CHECK((reads(bank, 0x04) & 0x4103) == 0, "amplifier %u: powered up, both mutes off: %#x", which, reads(bank, 0x04));
		CHECK(reads(bank, 0x08) == 0x4a4c, "amplifier %u: the transmitter as the profile has it: %#x", which, reads(bank, 0x08));
		CHECK(reads(bank, 0x05) == 0x17c0 && loudest[which] == -1, "amplifier %u: still at the lowest volume", which);
		CHECK(reads(bank, 0x03) == 0xffff, "amplifier %u: the interrupts masked", which);
	}
	never();
}

/* The volume: in steps of 6 dB at most, never louder than Android's level; stopped: faded, muted, powered down. */
static void
volume_and_stop(void)
{
	running();
	CHECK(hakios_speaker_volume(100, 0), "to 12.5 dB below Android's level: %s", log_text);
	CHECK(reads(device.reg, 0x05) == 0x1487 && reads(device.reg_b, 0x05) == 0x1487, "3 + 100 eighths of a dB: %#x", reads(device.reg, 0x05));
	CHECK(hakios_speaker_volume(0xfffffffe, 0), "a level past the lowest: %s", log_text);
	CHECK(reads(device.reg, 0x05) == 0x17c0 && reads(device.reg_b, 0x05) == 0x17c0, "the lowest, not a sum that wrapped: %#x",
	    reads(device.reg, 0x05));
	CHECK(hakios_speaker_volume(0, 0), "to Android's level: %s", log_text);
	CHECK(reads(device.reg, 0x05) == 0x1403 && reads(device.reg_b, 0x05) == 0x1403, "the profile's own: %#x", reads(device.reg, 0x05));
	CHECK(loudest[0] == 3 && loudest[1] == 3, "never louder than Android's level: %d, %d", loudest[0], loudest[1]);
	CHECK(biggest_step == 48, "6 dB a step at most: %d eighths", biggest_step);
	CHECK(hakios_speaker_healthy(), "healthy: %s", log_text);
	hakios_speaker_stop();
	CHECK(logged("THE AMPLIFIER 0x35, slot 0 IS POWERED DOWN") && logged("THE AMPLIFIER 0x34, slot 1 IS POWERED DOWN"), "said: %s", log_text);
	for (unsigned which = 0; which < 2; which++) {
		const uint8_t *bank = which ? device.reg_b : device.reg;

		CHECK(down(bank) && (reads(bank, 0x04) & 0x4000), "amplifier %u: powered down, both mutes on: %#x", which, reads(bank, 0x04));
		CHECK(reads(bank, 0x08) == 0x4a0c, "amplifier %u: the transmitter off: %#x", which, reads(bank, 0x08));
		CHECK(reads(bank, 0x05) == 0x17c0, "amplifier %u: at the lowest volume: %#x", which, reads(bank, 0x05));
	}
	CHECK(biggest_step == 48, "the fade out in steps of 6 dB too: %d eighths", biggest_step);
	CHECK(!hakios_speaker_volume(0, 0), "no volume for amplifiers that are stopped");
	never();
}

/*
 * the bus stops answering as the amplifiers are stopped: it is said, nothing more is asked of them -- and they are not
 * taken for powered down: once they answer again, the next stop powers both down
 */
static void
stop_without_an_answer(void)
{
	running();
	device.absent = true;
	hakios_speaker_stop();
	CHECK(logged("0x04 could not be read: BOTH AMPLIFIERS ARE POWERED DOWN AT ONCE") &&
	    logged("NOT KNOWN TO BE POWERED DOWN"), "said: %s", log_text);
	CHECK(!hakios_speaker_volume(0, 0) && !hakios_speaker_healthy(), "and nothing more asked of them");
	CHECK(!down(device.reg) && !down(device.reg_b), "(the model: neither heard the power-down)");
	device.absent = false;
	hakios_speaker_stop();
	CHECK(down(device.reg) && down(device.reg_b), "powered down once they answer: %#x, %#x", reads(device.reg, 0x04),
	    reads(device.reg_b, 0x04));
	CHECK(logged("THE AMPLIFIER 0x35, slot 0 IS POWERED DOWN, at try 2"), "said: %s", log_text);
	never();
}

/* a slow fade, as sound.c asks for one at level 3 (a step every 50 ms): the status looked at after every step, and a
 * fault in the middle of it ends it there, both powered down */
static void
slow_fade(void)
{
	running();
	CHECK(hakios_speaker_volume(100, 10) && hakios_speaker_healthy(), "a slow fade, healthy all through: %s", log_text);
	CHECK(reads(device.reg, 0x05) == 0x1487 && reads(device.reg_b, 0x05) == 0x1487, "there: %#x", reads(device.reg, 0x05));
	CHECK(biggest_step == 48 && !logged("reports a fault"), "in steps of 6 dB: %d", biggest_step);
	fault_at_volume = volumes_into_second + 2;      /* the second step of the next one */
	CHECK(!hakios_speaker_volume(0, 10), "the fault ends it: %s", log_text);
	CHECK(logged("the amplifier 0x34, slot 1: status 0x0319") && logged("an amplifier reports a fault: BOTH AMPLIFIERS ARE POWERED DOWN AT ONCE"),
	    "said: %s", log_text);
	CHECK(down(device.reg) && down(device.reg_b), "both are down");
	CHECK(loudest[0] > 3 && loudest[1] > 3, "and never at Android's level: the fade went no further: %d, %d", loudest[0], loudest[1]);
	never();
}

/* the second one's PLL does not lock (no bit clock at it): its power stage stays off, the first, which ran, is powered down */
static void
no_clock(void)
{
	phone_with_file("phone-video");
	wide(device.reg_b, 0x01, 0x0300);
	CHECK(hakios_speaker_prepare(acf, acf_size), "prepared: %s", log_text);
	CHECK(!hakios_speaker_start(), "not started: %s", log_text);
	CHECK(logged("THE AMPLIFIER 0x35, slot 0 RUNS") && logged("the amplifier 0x34, slot 1: status 0x0300: its PLL DOES NOT LOCK") &&
	    logged("BOTH AMPLIFIERS ARE POWERED DOWN AT ONCE"), "said: %s", log_text);
	CHECK((reads(device.reg, 0x04) & 0x103) == 0x103 && (reads(device.reg_b, 0x04) & 0x103) == 0x103, "both are down and muted: %#x, %#x",
	    reads(device.reg, 0x04), reads(device.reg_b, 0x04));
	CHECK(amplifier_on[1] == 0, "the second one's power stage was never switched on without a clock");
	CHECK((reads(device.reg_b, 0x04) & 0x4000) && reads(device.reg_b, 0x08) == 0x4a0c, "nor was it unmuted, or its transmitter switched on");
	never();
}

/* an amplifier reports a fault while both play: both are powered down at once, and stay so */
static void
fault_while_playing(void)
{
	unsigned before[2];

	running();
	CHECK(hakios_speaker_volume(0, 0) && hakios_speaker_healthy(), "playing: %s", log_text);
	wide(device.reg_b, 0x01, 0x0311 | 1u << 3);      /* over-current */
	CHECK(!hakios_speaker_healthy(), "the fault is seen: %s", log_text);
	CHECK(logged("the amplifier 0x34, slot 1: status 0x0319") && logged("an amplifier reports a fault: BOTH AMPLIFIERS ARE POWERED DOWN AT ONCE"),
	    "said: %s", log_text);
	CHECK(down(device.reg) && down(device.reg_b) && (reads(device.reg, 0x04) & reads(device.reg_b, 0x04) & 0x100), "both are down and muted: %#x, %#x",
	    reads(device.reg, 0x04), reads(device.reg_b, 0x04));
	before[0] = writes_into[0];
	before[1] = writes_into[1];
	wide(device.reg_b, 0x01, 0x0311);
	CHECK(!hakios_speaker_volume(0, 0) && !hakios_speaker_healthy() && !hakios_speaker_start(), "nothing is done after that");
	hakios_speaker_stop();
	CHECK(writes_into[0] == before[0] && writes_into[1] == before[1], "and nothing more is written");
	never();
}

/* the amplifiers stop answering while they play: a fault, said as a status of 0xffff (not what memory held), and down */
static void
silent_while_playing(void)
{
	running();
	CHECK(hakios_speaker_volume(0, 0) && hakios_speaker_healthy(), "playing: %s", log_text);
	device.absent = true;
	CHECK(!hakios_speaker_healthy(), "an amplifier that does not answer is not healthy: %s", log_text);
	CHECK(logged("status 0xffff") && logged("an amplifier reports a fault"), "said, with the status it could not read: %s", log_text);
	never();
}

/* what an amplifier measures: its temperature (ten bits, two's complement) and the battery (x 6025 / 1023 mV) */
static void
measures(uint8_t *bank, int degrees, int millivolts)
{
	wide(bank, 0x22, (uint16_t)(degrees & 0x3ff));
	wide(bank, 0x21, (uint16_t)((millivolts * 1023 + 6024) / 6025));
}

/*
 * The monitor: the average of the last three looks picks a row of the file's table; between rows the last
 * one holds; the reduction is the larger of the two amplifiers'. It reads and never writes.
 */
static void
monitor(void)
{
	unsigned before[2];

	running();
	CHECK(logged("the amplifier 0x35, slot 0's monitor: at 2 to 5 degrees, 3.000 dB less"), "the table was read: %s", log_text);
	/* (a battery row has a vmax only: turned into dB in the proportion of the first temperature row that has both) */
	CHECK(logged("the amplifier 0x34, slot 1's monitor: at 3650 to 3750 mV, 1.625 dB less"), "the battery's rows: %s", log_text);
	before[0] = writes_into[0];
	before[1] = writes_into[1];
	measures(device.reg, 25, 4000);
	measures(device.reg_b, 25, 4000);
	CHECK(hakios_speaker_monitor(false) == 0, "warm, a full battery: nothing less");
	measures(device.reg, 3, 4000);
	CHECK(hakios_speaker_monitor(false) == 0, "one cool look of two (14 degrees on average): nothing yet");
	CHECK(hakios_speaker_monitor(false) == 0, "two of three (10 degrees)");
	CHECK(hakios_speaker_monitor(false) == 24, "three cool looks: 3 dB less");
	measures(device.reg, 6, 4000);
	for (int look = 0; look < 3; look++) {
		CHECK(hakios_speaker_monitor(false) == 24, "6 degrees is between two rows: the last one holds");
	}
	measures(device.reg, -5, 4000);
	hakios_speaker_monitor(false);
	hakios_speaker_monitor(false);
	CHECK(hakios_speaker_monitor(false) == 64, "cold (a negative reading): 8 dB less");
	measures(device.reg, 25, 4000);
	measures(device.reg_b, 25, 3700);
	hakios_speaker_monitor(false);
	hakios_speaker_monitor(false);
	CHECK(hakios_speaker_monitor(true) == 13, "the first warm again, the second sees a low battery: 1.625 dB less");
	CHECK(logged("the amplifier 0x34, slot 1: 25 degrees, the battery 3704 mV"), "told when asked (629 x 6025 / 1023): %s", log_text);
	measures(device.reg, 3, 4000);
	hakios_speaker_monitor(false);
	hakios_speaker_monitor(false);
	CHECK(hakios_speaker_monitor(false) == 24, "the larger of the two amplifiers' reductions");
	CHECK(writes_into[0] == before[0] && writes_into[1] == before[1], "the monitor wrote into an amplifier");
	hakios_speaker_stop();
	CHECK(hakios_speaker_monitor(false) == 0, "amplifiers that are stopped are not asked");
	never();
}

/* a row of the monitor's table holds from its lowest to its highest, both included */
static void
monitor_row_edges(void)
{
	static const struct { int degrees; uint32_t less; const char *what; } looks[] = {
		{ 2, 24, "2 degrees, the lowest of a row: in it" }, { 7, 0, "7, the lowest of the warm row" },
		{ 5, 24, "5, the highest of a row" }, { 0, 36, "0, the highest of the next" }, { -50, 64, "-50, the coldest there is" },
	};

	running();
	measures(device.reg_b, 25, 4000);
	for (unsigned i = 0; i < sizeof(looks) / sizeof(looks[0]); i++) {
		measures(device.reg, looks[i].degrees, 4000);
		hakios_speaker_monitor(false);
		hakios_speaker_monitor(false);
		CHECK(hakios_speaker_monitor(false) == looks[i].less, "%s", looks[i].what);
	}
	never();
}

/* a monitor table whose sum is off is not used: the amplifiers still play, nothing is ever reduced */
static void
monitor_table_not_trusted(void)
{
	phone_with_file("phone-video");
	wide(device.reg_b, 0x01, 0x0311);
	acf[acf_size - 1] ^= 0x40;              /* in the second amplifier's table */
	CHECK(hakios_speaker_prepare(acf, acf_size) && hakios_speaker_start(), "prepared and started all the same: %s", log_text);
	CHECK(logged("device 1's monitor table is not of the shape known: not used"), "said: %s", log_text);
	measures(device.reg, 25, 4000);
	measures(device.reg_b, -20, 3000);
	for (int look = 0; look < 4; look++) {
		CHECK(hakios_speaker_monitor(false) == 0, "no table, no reduction");
	}
	never();
}

/* a monitor table of a device that is neither amplifier: passed over, the others' read as ever */
static void
a_table_for_no_amplifier(void)
{
	phone_with_file("phone-video");
	acf_monitor_table(7, temps, 4, vbats, 3);
	wide(device.reg_b, 0x01, 0x0311);
	CHECK(hakios_speaker_prepare(acf, acf_size) && hakios_speaker_start(), "prepared and started: %s", log_text);
	CHECK(logged("the amplifier 0x35, slot 0's monitor: at 2 to 5 degrees, 3.000 dB less"), "the amplifiers' own tables: %s", log_text);
	CHECK(!logged("device 7"), "and nothing of device 7's: %s", log_text);
}

/* a monitor table whose sum is right but whose rows run past its end: not used */
static void
monitor_rows_past_the_end(void)
{
	uint32_t length = 0x48 + 12 * (4 + 3), sum = 0;
	uint8_t *block;

	phone_with_file("phone-video");
	block = acf + acf_size - length;        /* (the second amplifier's, the last in the file) */
	put32(block + 0x34, length - 12);       /* its temperatures' four rows begin one row before its end */
	for (uint32_t i = 4; i < length; i++) {
		sum += block[i];
	}
	put32(block, sum);
	wide(device.reg_b, 0x01, 0x0311);
	CHECK(hakios_speaker_prepare(acf, acf_size) && hakios_speaker_start(), "prepared and started all the same: %s", log_text);
	CHECK(logged("device 1's monitor table is not of the shape known: not used"), "said: %s", log_text);
}

/* a row size whose product with the rows' count wraps 32 bits (4 x 0x40000000): refused, not rows read a gigabyte away */
static void
monitor_size_that_wraps(void)
{
	uint32_t length = 0x48 + 12 * (4 + 3), sum = 0;
	uint8_t *block;

	phone_with_file("phone-video");
	block = acf + acf_size - length;
	CHECK(le32(block + 0x2c) == 4, "four rows of temperatures: %u", le32(block + 0x2c));
	put32(block + 0x30, 0x40000000);
	for (uint32_t i = 4; i < length; i++) {
		sum += block[i];
	}
	put32(block, sum);
	wide(device.reg_b, 0x01, 0x0311);
	CHECK(hakios_speaker_prepare(acf, acf_size) && hakios_speaker_start(), "prepared and started all the same: %s", log_text);
	CHECK(logged("device 1's monitor table is not of the shape known: not used"), "said: %s", log_text);
}

/* the kernel on its way out while they play: both powered down at once */
static void
run_ends(void)
{
	running();
	hakios_speaker_quiesce();
	CHECK(logged("the kernel is on its way out: BOTH AMPLIFIERS ARE POWERED DOWN AT ONCE"), "said: %s", log_text);
	CHECK(down(device.reg) && down(device.reg_b), "both are down: %#x, %#x", reads(device.reg, 0x04), reads(device.reg_b, 0x04));
	never();
}

/* how many times the log has a line with `text' in it */
static unsigned
times_logged(const char *text)
{
	unsigned n = 0;

	for (const char *at = strstr(log_text, text); at != NULL; at = strstr(at + 1, text)) {
		n++;
	}
	return n;
}

/*
 * A power-down whose write nobody acknowledges (the amplifiers stopped answering as a fault was seen) is not taken
 * for done: the amplifiers' stop tries it again, until 0x04 reads back powered down -- and then not again.
 */
static void
power_down_tried_again_by_stop(void)
{
	running();
	CHECK(hakios_speaker_volume(0, 0), "playing: %s", log_text);
	device.absent = true;
	CHECK(!hakios_speaker_healthy(), "a status that cannot be read is a fault: %s", log_text);
	CHECK(logged("NOT ACKNOWLEDGED: NOT KNOWN TO BE POWERED DOWN"), "said: %s", log_text);
	hakios_speaker_stop();
	CHECK(!down(device.reg) && !down(device.reg_b), "(the model: still nobody there)");
	device.absent = false;
	hakios_speaker_stop();
	CHECK(down(device.reg) && down(device.reg_b) && (reads(device.reg, 0x04) & reads(device.reg_b, 0x04) & 0x100),
	    "both powered down and muted at last: %#x, %#x", reads(device.reg, 0x04), reads(device.reg_b, 0x04));
	CHECK(logged("THE AMPLIFIER 0x34, slot 1 IS POWERED DOWN, at try 3"), "said: %s", log_text);
	{
		unsigned before = writes_into[0] + writes_into[1];

		hakios_speaker_stop();
		hakios_speaker_quiesce();
		CHECK(writes_into[0] + writes_into[1] == before, "nothing more written once they read back powered down");
	}
	never();
}

/* a power-down that is acknowledged but does not read back powered down (0x04 kept nothing): tried again too */
static void
power_down_that_does_not_read_back(void)
{
	running();
	deaf_register = 0x04;                   /* (the first amplifier's 0x04 keeps what it had: running) */
	wide(device.reg_b, 0x01, 0x0311 | 1u << 3);
	CHECK(!hakios_speaker_healthy(), "the fault is seen: %s", log_text);
	CHECK(logged("NOT POWERED DOWN, to be tried again") && down(device.reg_b) && !down(device.reg), "said: %s", log_text);
	deaf_register = -1;
	hakios_speaker_stop();
	CHECK(down(device.reg), "the first powered down when it is tried again: %#x", reads(device.reg, 0x04));
	never();
}

/* on a panic's way out after a power-down that was not acknowledged: tried again, though neither counts as running */
static void
power_down_tried_again_on_the_way_out(void)
{
	running();
	device.absent = true;
	CHECK(!hakios_speaker_healthy(), "a status that cannot be read is a fault: %s", log_text);
	device.absent = false;
	hakios_speaker_quiesce();
	CHECK(logged("the kernel is on its way out: BOTH AMPLIFIERS ARE POWERED DOWN AT ONCE"), "said: %s", log_text);
	CHECK(down(device.reg) && down(device.reg_b), "both are down: %#x, %#x", reads(device.reg, 0x04), reads(device.reg_b, 0x04));
	never();
}

/* an amplifier cut off in the middle of a byte holds the data line: clocked free, and both powered down */
static void
bus_held_in_the_middle_of_a_byte(void)
{
	running();
	CHECK(hakios_speaker_volume(0, 0), "playing: %s", log_text);
	device.midbyte = 5;
	lines();
	device.was_sda = false;
	hakios_speaker_stop();
	CHECK(logged("the I2C bus is not at rest (gpio12 reads 0, gpio13 reads 1): not touched") &&
	    logged("0x04 could not be read: BOTH AMPLIFIERS ARE POWERED DOWN AT ONCE"), "said: %s", log_text);
	CHECK(logged("the I2C bus was held low: an amplifier let go of the data line after 5 clocks"), "clocked free: %s", log_text);
	CHECK(down(device.reg) && down(device.reg_b), "both are down: %#x, %#x", reads(device.reg, 0x04), reads(device.reg_b, 0x04));
	never();
}

/*
 * The data line held for good: the power-down is tried again at every stop, said the first few times only, and once
 * the line is let go it is done.
 */
static void
bus_held_for_good(void)
{
	running();
	device.stuck = true;
	lines();
	device.was_sda = false;
	for (int stop = 0; stop < 20; stop++) {
		hakios_speaker_stop();
	}
	CHECK(logged("nine clocks did not free it"), "said: %s", log_text);
	CHECK(times_logged("nine clocks did not free it") <= 12, "not said at every try: %u times", times_logged("nine clocks did not free it"));
	CHECK(!down(device.reg) && !down(device.reg_b), "(the model: nothing reached them)");
	device.stuck = false;
	lines();
	hakios_speaker_stop();
	CHECK(down(device.reg) && down(device.reg_b), "both are down once it is let go: %#x, %#x", reads(device.reg, 0x04),
	    reads(device.reg_b, 0x04));
	never();
}

/*
 * A panic while another CPU was in the middle of a transaction, an amplifier holding the data line in the middle of a
 * byte: clocked free, both powered down, and the pins given back to the serial engine as that transaction found them.
 */
static void
panic_in_the_middle_of_a_transaction(void)
{
	running();
	CHECK(bus_take(), "(the other CPU's transaction)");
	i2c_gpio_start(&i2c_bus);
	i2c_gpio_write(&i2c_bus, (uint8_t)(0x35 << 1));
	device.midbyte = 3;
	lines();
	device.was_sda = false;
	hakios_speaker_quiesce();
	CHECK(logged("the kernel is on its way out: BOTH AMPLIFIERS ARE POWERED DOWN AT ONCE"), "said: %s", log_text);
	CHECK(logged("the I2C bus was held low"), "clocked free: %s", log_text);
	CHECK(down(device.reg) && down(device.reg_b), "both are down: %#x, %#x", reads(device.reg, 0x04), reads(device.reg_b, 0x04));
	never();
}

/* the monitor cannot read an amplifier's temperature or the battery: a byte nobody acknowledges, both powered down */
static void
monitor_without_an_answer(void)
{
	running();
	measures(device.reg, 25, 4000);
	measures(device.reg_b, 25, 4000);
	CHECK(hakios_speaker_monitor(false) == 0, "a look: %s", log_text);
	device.absent = true;
	hakios_speaker_monitor(false);
	CHECK(logged("could not be read: BOTH AMPLIFIERS ARE POWERED DOWN AT ONCE"), "said: %s", log_text);
	device.absent = false;
	CHECK(!hakios_speaker_healthy() && !hakios_speaker_volume(0, 0), "and nothing more asked of them");
	hakios_speaker_stop();
	CHECK(down(device.reg) && down(device.reg_b), "both are down: %#x, %#x", reads(device.reg, 0x04), reads(device.reg_b, 0x04));
	never();
}

/* a board that does not list the part, and a chip whose file does not say where the pins are */
static void
not_listed(void)
{
	phone("qemu-virt");
	boot_args = "speaker_probe=2";
	hakios_speaker_init();
	usleep(50000);
	CHECK(logged("speaker: disabled: this board's board.dts lists no \"speaker-aw88261\""), "said: %s", log_text);
	CHECK(model.reads == 0 && model.writes == 0, "nothing was read or written");
	CHECK(!hakios_speaker_prepare(NULL, 0), "nor is it prepared for sound");
	CHECK(model.reads == 0 && model.writes == 0, "nothing was read or written then either");
	hakios_speaker_quiesce();
	CHECK(model.reads == 0 && model.writes == 0, "nor at the run's end");
}

/*
 * The edges of the file's shape: 0x50 bytes is the least, a megabyte the most; 64 entries the most; a table that
 * ends just where the file does fits. (The table moved behind everything else, in a file of its own, to have
 * room for 64 entries.)
 */
static void
acf_edges(void)
{
	static uint8_t big[0x1c00];

	file();
	amplifiers[0].device = 0;
	amplifiers[1].device = 1;
	(void)acf_profiles(acf, 0x4f);
	CHECK(logged("the configuration file is 79 bytes: not one of Awinic's"), "79 bytes refused: %s", log_text);
	(void)acf_profiles(acf, 0x50);
	(void)acf_profiles(acf, 0x100000);
	CHECK(!logged("is 80 bytes") && !logged("is 1048576 bytes"), "80 bytes and a megabyte are not refused for their size");
	(void)acf_profiles(acf, 0x100001);
	CHECK(logged("is 1048577 bytes"), "a byte more than a megabyte is");

	memcpy(big, acf, sizeof(acf));
	memcpy(big + 0x1000, acf + 0x50, 5 * 0x30);
	put32(big + 0x3c, 0x1000);
	put32(big + 0x38, 64);
	put32(big + 0x34, 64 * 0x30);
	CHECK(acf_profiles(big, sizeof(big)), "64 entries, the table ending where the file does: taken: %s", log_text);
	put32(big + 0x38, 65);
	put32(big + 0x34, 65 * 0x30);
	(void)acf_profiles(big, sizeof(big) + 0x30);
	CHECK(logged("the configuration file's table (65 entries"), "65 are not: %s", log_text);
}

/* the first amplifier's Music profile (the file's first block) changed at `at' to `value': refused as no list of
 * 16-bit registers */
static void
not_a_list(unsigned at, uint32_t value)
{
	file();
	amplifiers[0].device = 0;
	amplifiers[1].device = 1;
	put32(acf + 0x50 + 6 * 0x30 + at, value);
	CHECK(!acf_profiles(acf, acf_size) && logged("device 0's Music profile is not a list of 16-bit registers"),
	    "refused: %s", log_text);
}

static void eight_bit_numbers(void) { not_a_list(0x20, 1); }
static void eight_bit_values(void) { not_a_list(0x24, 1); }
static void a_register_fewer(void) { not_a_list(0x3c, 7); }      /* (its length still that of 8) */

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "off_by_default", off_by_default }, { "pins_and_supplies", pins_and_supplies }, { "both_answer", both_answer },
		{ "one_is_missing", one_is_missing }, { "nobody_there", nobody_there }, { "another_amplifier", another_amplifier },
		{ "another_wiring", another_wiring }, { "bus_not_at_rest", bus_not_at_rest }, { "prepared", prepared }, { "not_this_chip", not_this_chip },
		{ "second_is_missing", second_is_missing }, { "write_does_not_stick", write_does_not_stick },
		{ "file_not_trusted", file_not_trusted }, { "started", started }, { "volume_and_stop", volume_and_stop },
		{ "slow_fade", slow_fade }, { "stop_without_an_answer", stop_without_an_answer }, { "monitor_row_edges", monitor_row_edges },
		{ "no_clock", no_clock }, { "fault_while_playing", fault_while_playing }, { "silent_while_playing", silent_while_playing }, { "monitor", monitor },
		{ "profiles_not_usable", profiles_not_usable }, { "monitor_tables_at_the_edge", monitor_tables_at_the_edge },
		{ "started_unprepared", started_unprepared }, { "started_with_the_second_unprepared", started_with_the_second_unprepared },
		{ "board_without_the_part", board_without_the_part },
		{ "status_never_ready", status_never_ready },
		{ "monitor_table_not_trusted", monitor_table_not_trusted }, { "a_table_for_no_amplifier", a_table_for_no_amplifier },
		{ "monitor_rows_past_the_end", monitor_rows_past_the_end }, { "monitor_size_that_wraps", monitor_size_that_wraps }, { "run_ends", run_ends }, { "not_listed", not_listed },
		{ "power_down_tried_again_by_stop", power_down_tried_again_by_stop },
		{ "power_down_that_does_not_read_back", power_down_that_does_not_read_back },
		{ "power_down_tried_again_on_the_way_out", power_down_tried_again_on_the_way_out },
		{ "bus_held_in_the_middle_of_a_byte", bus_held_in_the_middle_of_a_byte }, { "bus_held_for_good", bus_held_for_good },
		{ "panic_in_the_middle_of_a_transaction", panic_in_the_middle_of_a_transaction },
		{ "monitor_without_an_answer", monitor_without_an_answer },
		{ "acf_edges", acf_edges }, { "eight_bit_numbers", eight_bit_numbers }, { "eight_bit_values", eight_bit_values },
		{ "a_register_fewer", a_register_fewer },
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
