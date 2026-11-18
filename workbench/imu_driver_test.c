/*
 * The motion sensor's driver itself (drivers/board/imu.c), run as a host program against the model of the
 * SoC's pins with an LSM6DSO on them: an I2C device (i2c_device_model.h) with the chip's identity, its
 * control registers (a software reset puts them back as they come out of reset) and its twelve output
 * bytes, which a scenario sets as the chip would read them. What is tested: a look writes nothing; another
 * chip, nobody answering, a bus that is not at rest or a write that does not stick stop the driver with
 * nothing more written; the chip is configured as the file's head says, and its samples come out as the
 * right orientation through the board's turned mounting; at the run's end it is powered down and the pins
 * are as they were found.
 *
 *   imu_driver_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <unistd.h>

#include "fakekernel.h"

/* which pins the chip is on is the board's: the model is put where the board the driver reads says */
static unsigned sda_pin = 116, scl_pin = 117, int_pin = 112;
#define I2C_DEVICE_SDA          sda_pin
#define I2C_DEVICE_SCL          scl_pin
#define I2C_DEVICE_ADDRESS      0x6a
#include "i2c_device_model.h"

extern char log_text[];

#define AS_FOUND        (1u << 2 | 3u)          /* the bus's pins: the serial engine's function, pulled up */
#define INT_AS_FOUND    3u                      /* the interrupt pin: an input, pulled up */

#define WHO_AM_I        0x0f
#define CTRL1_XL        0x10
#define CTRL3_C         0x12
#define OUTX_L_G        0x22

/* what is written into the chip */
static bool register_written[256];
static unsigned resets, written_after_reset;
static int deaf_register = -1;                  /* a register that does not take what is written into it */
static bool reset_never_ends;                   /* CTRL3_C's reset bit stays set */
static volatile unsigned output_reads;          /* reads of the first output byte: samples taken */

static void
chip_reset(void)
{
	device.reg[CTRL1_XL] = 0x00;
	device.reg[CTRL3_C] = 0x04;             /* IF_INC, as the chip comes out of reset */
}

static void
chip_written(uint8_t reg, uint8_t value)
{
	register_written[reg] = true;
	if (reg == CTRL3_C && (value & 0x01)) {
		resets++;
		chip_reset();
		if (reset_never_ends) {
			device.reg[CTRL3_C] |= 0x01;
		}
		return;
	}
	written_after_reset += resets > 0;
	if (reg != deaf_register) {
		device.reg[reg] = value;
	}
}

static uint8_t
chip_read(uint8_t reg)
{
	if (reg == OUTX_L_G) {
		output_reads++;
	}
	return device.reg[reg];
}

uint32_t
hakios_test_rd(vm_offset_t address)
{
	i2c_device_looked_at(address);
	return tlmm_model_rd(address);
}

void hakios_test_wr(vm_offset_t address, uint32_t value) { tlmm_model_wr(address, value); }
vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }

void hakios_imu_init(void);
void hakios_imu_quiesce(void);

#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#include "board/board.c"
#include "board/imu.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited = 0; !(cond) && waited < (ms); waited += 5) usleep(5000); } while (0)

/* what the chip's accelerometer reads, in its own axes and counts: the last six of the twelve output bytes */
static void
chip_reads(int16_t x, int16_t y, int16_t z)
{
	int16_t v[6] = { 11, -22, 33, x, y, z };      /* (the gyroscope's first: not what the orientation is made of) */

	for (unsigned i = 0; i < 6; i++) {
		device.reg[OUTX_L_G + 2 * i] = (uint8_t)((uint16_t)v[i] & 0xff);
		device.reg[OUTX_L_G + 2 * i + 1] = (uint8_t)((uint16_t)v[i] >> 8);
	}
}

/* a phone with the part: the pins where its board file has them, the chip as it comes out of reset, the bus at rest */
static void
phone(const char *name, uint8_t identity)
{
	board = name;
	fake_delay_divisor = 5;
	tlmm_model_at(hakios_board() != NULL ? hakios_board()->pins.tlmm : 0);
	if (hakios_board() != NULL && hakios_board()->imu.sda != hakios_board()->imu.scl) {
		sda_pin = hakios_board()->imu.sda;
		scl_pin = hakios_board()->imu.scl;
		int_pin = hakios_board()->imu.interrupt;
	}
	model.after_write = changed;
	model.cfg[I2C_DEVICE_SDA] = model.cfg[I2C_DEVICE_SCL] = AS_FOUND;
	model.cfg[int_pin] = INT_AS_FOUND;
	memset(device.reg, 0, sizeof(device.reg));
	device.reg[WHO_AM_I] = identity;
	chip_reset();
	chip_reads(0, 0, 0);
	device.written = chip_written;
	device.read = chip_read;
	lines();
	device.was_sda = device.was_scl = true;
}

/* the bus used as a bus, and the pins as they were found */
static void
pins_as_found(void)
{
	CHECK(device.driven_high == 0, "a line of the bus was driven high %u times", device.driven_high);
	CHECK(model.elsewhere == 0, "%u accesses outside the pins' registers", model.elsewhere);
	CHECK(model.cfg[I2C_DEVICE_SDA] == AS_FOUND && model.cfg[I2C_DEVICE_SCL] == AS_FOUND, "the bus's pins are not as they were found: "
	    "%#x %#x", model.cfg[I2C_DEVICE_SDA], model.cfg[I2C_DEVICE_SCL]);
	CHECK(model.cfg[int_pin] == INT_AS_FOUND && model.io[int_pin] == 0, "the interrupt pin was touched");
	for (unsigned i = 0; i < PINS; i++) {
		if (i != I2C_DEVICE_SDA && i != I2C_DEVICE_SCL && i != int_pin && (model.cfg[i] != 0 || model.io[i] != 0)) {
			CHECK(false, "pin %u was touched", i);
			break;
		}
	}
}

/* only the two control registers, if anything */
static bool
only_the_control_registers(void)
{
	for (unsigned reg = 0; reg < 256; reg++) {
		if (register_written[reg] && reg != CTRL1_XL && reg != CTRL3_C) {
			fprintf(stderr, "register 0x%02x was written\n", reg);
			return false;
		}
	}
	return true;
}

/* sensors=2 on the test phone, until the orientation is followed */
static void
following(void)
{
	phone("phone-video", 0x6c);
	chip_reads(-8192, 0, 0);
	boot_args = "sensors=2";
	hakios_imu_init();
	WAIT_FOR(logged("the orientation is followed from here on") && get("samples") >= 2, 10000);
	CHECK(logged("the orientation is followed from here on"), "followed: %s", log_text);
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

/* without its boot argument: nothing */
static void
off_by_default(void)
{
	phone("phone-video", 0x6c);
	boot_args = "";
	hakios_imu_init();
	usleep(50000);
	CHECK(model.writes == 0 && model.reads == 0 && !logged("imu:"), "nothing touched, nothing said: %s", log_text);
	hakios_imu_quiesce();
	CHECK(model.writes == 0, "and nothing at the run's end");
}

/* sensors=1: the identity read and said, nothing written, the pins given back, no samples */
static void
look_only(void)
{
	phone("phone-video", 0x6c);
	boot_args = "sensors=1";
	hakios_imu_init();
	WAIT_FOR(logged("imu: done"), 5000);
	CHECK(logged("imu: level 1: a look, reading only"), "the level is said: %s", log_text);
	CHECK(logged("imu: the chip at 0x6a answers WHO_AM_I 0x6c, an LSM6DSO (0x6c)"), "the identity, against the LSM6DSO's: %s", log_text);
	CHECK(logged("imu: gpio116 (I2C data): function 1, pull 3, 2 mA, input") && logged("imu: gpio112 (interrupt): function 0, pull 3"),
	    "each pin as it was found: %s", log_text);
	CHECK(device.writes == 0 && device.starts >= 2, "read, and nothing written: %u bytes", device.writes);
	CHECK(output_reads == 0 && !logged("followed"), "no samples at a look");
	CHECK(!logged("NOTHING MORE"), "nothing was wrong");
	pins_as_found();
	hakios_imu_quiesce();
	CHECK(device.writes == 0, "nor at the run's end: %u", device.writes);
	pins_as_found();
}

/* sensors=1 with another chip there: said, nothing written */
static void
look_at_another_chip(void)
{
	phone("phone-video", 0x6a);
	boot_args = "sensors=1";
	hakios_imu_init();
	WAIT_FOR(logged("imu: done"), 5000);
	CHECK(logged("answers WHO_AM_I 0x6a: NOT THE LSM6DSO'S 0x6c"), "said: %s", log_text);
	CHECK(device.writes == 0, "nothing written");
	pins_as_found();
}

/* sensors=2 and the identity is another chip's: refused, nothing written, then or at the run's end */
static void
another_chip_refused(void)
{
	phone("phone-video", 0x69);             /* (an LSM6DS3's) */
	boot_args = "sensors=2";
	hakios_imu_init();
	WAIT_FOR(logged("imu: done"), 5000);
	CHECK(logged("answers WHO_AM_I 0x69: NOT THE LSM6DSO'S 0x6c") && logged("another chip: NOTHING MORE IS DONE"), "said: %s", log_text);
	CHECK(device.writes == 0 && resets == 0 && output_reads == 0, "not reset, not configured, not read: %u bytes written", device.writes);
	hakios_imu_quiesce();
	CHECK(device.writes == 0, "nor written at the run's end");
	pins_as_found();
}

/* nobody at the address: said, nothing more */
static void
nobody_there(void)
{
	phone("phone-video", 0x6c);
	device.absent = true;
	boot_args = "sensors=2";
	hakios_imu_init();
	WAIT_FOR(logged("imu: done"), 5000);
	CHECK(logged("imu: nobody answers at 0x6a on the bus of gpio116/117") &&
	    logged("a byte was not acknowledged: NOTHING MORE IS DONE"), "said: %s", log_text);
	CHECK(device.writes == 0, "nothing written");
	pins_as_found();
}

/* a line of the bus held low: the bus is not taken, nothing driven */
static void
bus_not_at_rest(void)
{
	phone("phone-video", 0x6c);
	device.stuck = true;
	lines();
	device.was_sda = false;                 /* (held low from before the driver looks) */
	boot_args = "sensors=2";
	hakios_imu_init();
	WAIT_FOR(logged("imu: done"), 5000);
	CHECK(logged("imu: the I2C bus is not at rest (gpio116 reads 0, gpio117 reads 1): not touched") &&
	    logged("the I2C bus is not at rest: NOTHING MORE IS DONE") && !logged("nobody answers"), "said: %s", log_text);
	CHECK(device.starts == 0 && device.bytes_in == 0 && device.writes == 0, "no transfer begun: %u", device.starts);
	pins_as_found();
}

/* sensors=2: reset, then BDU and IF_INC, then 104 Hz at the board's +-4 g; nothing else written */
static void
configured(void)
{
	following();
	CHECK(resets == 1 && register_written[CTRL3_C] && register_written[CTRL1_XL], "reset once, then configured: %u", resets);
	CHECK(device.reg[CTRL3_C] == 0x44, "CTRL3_C: BDU | IF_INC, 0x44: %#x", device.reg[CTRL3_C]);
	CHECK(device.reg[CTRL1_XL] == 0x48, "CTRL1_XL: 104 Hz (4 << 4) at +-4 g (2 << 2), 0x48: %#x", device.reg[CTRL1_XL]);
	CHECK(logged("imu: 0x12 <- 0x44, reads 0x44") && logged("imu: 0x10 <- 0x48, reads 0x48"), "each write read back: %s", log_text);
	CHECK(logged("104 Hz, +-4 g"), "the board's range said: %s", log_text);
	CHECK(only_the_control_registers(), "nothing but CTRL3_C and CTRL1_XL was written");
	CHECK(device.driven_high == 0 && model.elsewhere == 0, "the bus was used as a bus");
	hakios_imu_quiesce();
}

/*
 * The board turns the chip a quarter ("-y", "x", "-z"): what the chip reads along its x is Darwin's y. Held
 * upright, the chip reads -1 g along its x (Darwin: y -1000, portrait); on its side, +1 g along its y (Darwin:
 * x -1000, landscape left); flat on its back, +1 g along its z (Darwin: z -1000, face up). Read square, the
 * first would be landscape: the mounting is what makes it portrait.
 */
static void
orientation_through_the_mounting(void)
{
	following();
	WAIT_FOR(get("orientation") == 1, 3000);
	CHECK(get("orientation") == 1, "portrait (1): %u", get("orientation"));
	CHECK(strcmp(get_string("accel"), "0 -1000 0") == 0, "hw.sensors.accel in milli-g on Darwin's axes: %s", get_string("accel"));
	CHECK(logged("imu: portrait (0 -1000 0 milli-g; the chip's -8192 0 0)"), "said: %s", log_text);
	chip_reads(0, 8192, 0);
	WAIT_FOR(get("orientation") == 3, 5000);
	CHECK(get("orientation") == 3, "landscape left (3): %u", get("orientation"));
	CHECK(strcmp(get_string("accel"), "-1000 0 0") == 0, "%s", get_string("accel"));
	CHECK(logged("imu: landscape left"), "said: %s", log_text);
	chip_reads(0, 0, 8192);
	WAIT_FOR(get("orientation") == 5, 5000);
	CHECK(get("orientation") == 5, "face up (5): %u", get("orientation"));
	CHECK(strcmp(get_string("accel"), "0 0 -1000") == 0, "%s", get_string("accel"));
	/* half a g in each of x and y, as from a phone held at a slant: rounded to the nearest milli-g */
	chip_reads(4096, -4096, 0);
	WAIT_FOR(strcmp(get_string("accel"), "500 500 0") == 0, 2000);
	CHECK(strcmp(get_string("accel"), "500 500 0") == 0, "%s", get_string("accel"));
	CHECK(only_the_control_registers(), "reading wrote nothing");
	hakios_imu_quiesce();
}

/* the run's end: the reading stops, the accelerometer is powered down, the pins are as they were found */
static void
teardown(void)
{
	unsigned taken;

	following();
	CHECK(device.reg[CTRL1_XL] != 0, "on while the run goes on");
	hakios_imu_quiesce();
	CHECK(device.reg[CTRL1_XL] == 0, "CTRL1_XL 0, power-down, at the run's end: %#x", device.reg[CTRL1_XL]);
	CHECK(logged("imu: at the run's end:") && logged("the accelerometer is powered down (CTRL1_XL 0)"), "said: %s", log_text);
	pins_as_found();
	usleep(300000);                         /* (the thread's wait, spun now: a poll's) */
	taken = output_reads;
	usleep(300000);
	CHECK(output_reads == taken, "no sample after the run's end: %u then %u", taken, output_reads);
	CHECK(logged("imu: done"), "the thread has ended: %s", log_text);
	CHECK(only_the_control_registers(), "nothing else written");
}

/* the chip stops answering while it is read: the driver stops; the run's end powers it down all the same */
static void
lost_while_following(void)
{
	unsigned taken;

	following();
	device.absent = true;
	WAIT_FOR(logged("imu: done"), 5000);
	CHECK(logged("a byte was not acknowledged: NOTHING MORE IS DONE"), "said: %s", log_text);
	taken = output_reads;
	usleep(200000);
	CHECK(output_reads == taken, "no more reads");
	device.absent = false;
	hakios_imu_quiesce();
	CHECK(device.reg[CTRL1_XL] == 0 && logged("is powered down (CTRL1_XL 0)"), "powered down at the run's end: %s", log_text);
	pins_as_found();
}

/* a chip that holds the data line down at the run's end (cut off in a byte it was sending): clocked free, then powered down */
static void
wedged_at_the_end(void)
{
	following();
	device.absent = true;
	WAIT_FOR(logged("imu: done"), 5000);
	device.absent = false;
	device.midbyte = 5;
	lines();
	device.was_sda = false;                 /* (held low from before the run's end looks) */
	hakios_imu_quiesce();
	CHECK(logged("imu: at the run's end: the bus came back after"), "said: %s", log_text);
	CHECK(device.reg[CTRL1_XL] == 0 && logged("is powered down (CTRL1_XL 0)"), "powered down: %s", log_text);
	pins_as_found();
}

/* a control register that does not take what is written: stopped, the accelerometer never switched on */
static void
write_does_not_stick(void)
{
	phone("phone-video", 0x6c);
	deaf_register = CTRL3_C;
	boot_args = "sensors=2";
	hakios_imu_init();
	WAIT_FOR(logged("imu: done"), 5000);
	CHECK(logged("imu: 0x12 <- 0x44, reads 0x04: NOT AS WRITTEN") && logged("did not read back as written: NOTHING MORE IS DONE"),
	    "said: %s", log_text);
	CHECK(!register_written[CTRL1_XL] && output_reads == 0, "not switched on, not read");
	hakios_imu_quiesce();
	CHECK(!register_written[CTRL1_XL], "and nothing to switch off at the run's end");
	pins_as_found();
}

/* a reset that does not end: stopped before anything is switched on */
static void
reset_does_not_end(void)
{
	phone("phone-video", 0x6c);
	reset_never_ends = true;
	boot_args = "sensors=2";
	hakios_imu_init();
	WAIT_FOR(logged("imu: done"), 5000);
	CHECK(logged("the chip's reset does not end: NOTHING MORE IS DONE"), "said: %s", log_text);
	CHECK(!register_written[CTRL1_XL] && written_after_reset == 0, "nothing written after the reset");
}

/* a level past the last is the last */
static void
level_past_the_last(void)
{
	phone("phone-video", 0x6c);
	boot_args = "sensors=7";
	hakios_imu_init();
	WAIT_FOR(logged("the orientation is followed from here on"), 10000);
	CHECK(logged("imu: level 2:"), "level 2: %s", log_text);
	hakios_imu_quiesce();
}

/* a board that does not list the part: said, nothing touched */
static void
not_listed(void)
{
	phone("phone-keys", 0x6c);
	boot_args = "sensors=2";
	hakios_imu_init();
	usleep(50000);
	CHECK(logged("imu: disabled: this board's board.dts lists no \"imu-lsm6dso\""), "said: %s", log_text);
	CHECK(model.writes == 0 && model.reads == 0 && model.elsewhere == 0, "nothing touched");
	hakios_imu_quiesce();
	CHECK(model.writes == 0, "nor at the run's end");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "off_by_default", off_by_default }, { "look_only", look_only }, { "look_at_another_chip", look_at_another_chip },
		{ "another_chip_refused", another_chip_refused }, { "nobody_there", nobody_there }, { "bus_not_at_rest", bus_not_at_rest },
		{ "configured", configured }, { "orientation_through_the_mounting", orientation_through_the_mounting },
		{ "teardown", teardown }, { "lost_while_following", lost_while_following }, { "wedged_at_the_end", wedged_at_the_end },
		{ "write_does_not_stick", write_does_not_stick }, { "reset_does_not_end", reset_does_not_end },
		{ "level_past_the_last", level_past_the_last }, { "not_listed", not_listed },
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
			fprintf(stderr, "%-34s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
