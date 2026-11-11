/*
 * The speaker amplifiers' driver itself (kernel/overlay/bsd/dev/redwood_speaker.c), run as a host
 * program against the model of the SoC's pins with two amplifiers on them (i2c_device_model.h: two
 * addresses, registers of two bytes) and a command database (cmd_db_image.h). What is tested is its
 * look, speaker_probe=1 and 2, which is to read and nothing else: nothing is written into an
 * amplifier, no pin is left other than it was found.
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

void redwood_speaker_init(void);
void redwood_speaker_quiesce(void);

#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#define get speaker_get
#define put speaker_put
#include "hakios_board.c"
#include "redwood_speaker.c"
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
	redwood_speaker_init();
	WAIT_FOR(logged("speaker: done"), 20000);
	CHECK(logged("speaker: done"), "the look ended: %s", log_text);
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

/* without its boot argument: nothing */
static void
off_by_default(void)
{
	phone("phone-video");
	boot_args = "";
	redwood_speaker_init();
	usleep(50000);
	CHECK(model.writes == 0 && model.reads == 0 && !logged("speaker:"), "nothing read, nothing written, nothing said: %s", log_text);
	boot_args = "speaker_probe=0";
	redwood_speaker_init();
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

/* a board that does not list the part, and a chip whose file does not say where the pins are */
static void
not_listed(void)
{
	phone("qemu-virt");
	boot_args = "speaker_probe=2";
	redwood_speaker_init();
	usleep(50000);
	CHECK(logged("speaker: disabled: this board's board.toml lists no \"speaker-aw88261\""), "said: %s", log_text);
	CHECK(model.reads == 0 && model.writes == 0, "nothing was read or written");
	CHECK(!redwood_speaker_prepare(NULL, 0), "nor is it prepared for sound");
	CHECK(model.reads == 0 && model.writes == 0, "nothing was read or written then either");
	redwood_speaker_quiesce();
	CHECK(model.reads == 0 && model.writes == 0, "nor at the run's end");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "off_by_default", off_by_default }, { "pins_and_supplies", pins_and_supplies }, { "both_answer", both_answer },
		{ "one_is_missing", one_is_missing }, { "nobody_there", nobody_there }, { "another_amplifier", another_amplifier },
		{ "another_wiring", another_wiring }, { "bus_not_at_rest", bus_not_at_rest }, { "not_listed", not_listed },
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
