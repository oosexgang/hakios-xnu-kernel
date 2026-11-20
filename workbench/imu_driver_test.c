/*
 * The motion sensor's driver itself (drivers/board/imu.c), run as a host program against the model of the
 * SoC's pins with an LSM6DSO on them: an I2C device (i2c_device_model.h) with the chip's identity, its
 * control registers (a software reset puts them back as they come out of reset) and its twelve output
 * bytes, which a scenario sets as the chip would read them. What is tested: a look writes nothing; another
 * chip, nobody answering, a bus that is not at rest or a write that does not stick stop the driver with
 * nothing more written; the chip is configured as the file's head says, and its samples come out as the
 * right orientation through the board's turned mounting; at the run's end it is powered down and the pins
 * are as they were found, also when the driver's thread was stopped (as the run's end stops the other cores)
 * while it took the pins or while the chip was sending it a byte; and what it says to Darwin's side (hw.sensors.hid,
 * hw.sensors.motion, the timed hw.sensors.sample) and when; and the gyroscope: switched on only when its rates are for
 * Darwin's motion stack, given only once it is seen to answer and not once it is stuck, its counts turned onto Darwin's
 * axes and into milli-rad/s, and powered down at the run's end.
 *
 * And the same driver on a board that lists a BMI160 instead (the scenarios named bmi160_*), against a model of that
 * chip at its own address: its identity at CHIP_ID, a reset by command after which it does not acknowledge its
 * address for a while, a half's power mode changed by a command that takes some reads of PMU_STATUS to be carried out
 * and is dropped if it comes while another is, CMD reading back nothing, and the four registers of rate and range. The
 * model is the datasheet as bmi160_core.h has it, as sure and as unsure as that file: it holds the driver to what the
 * driver was written for, not to a chip. What is tested is what the LSM6DSO's scenarios test: nothing given before
 * the chip answers and is configured, the configuration and the order it is made in, the samples and the rates
 * through the board's mounting in milli-g and milli-rad/s, the gyroscope only on when its rates go to Darwin, the
 * orientation not known once the chip is lost, both halves suspended at the run's end.
 *
 *   imu_driver_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <unistd.h>

#include "fakekernel.h"

/* which pins the chip is on is the board's: the model is put where the board the driver reads says */
static unsigned sda_pin = 116, scl_pin = 117, int_pin = 112;
#define I2C_DEVICE_SDA          sda_pin
#define I2C_DEVICE_SCL          scl_pin
static unsigned chip_address = 0x6a;    /* (an LSM6DSO's; the BMI160 of the bmi160_* scenarios is at 0x68) */
#define I2C_DEVICE_ADDRESS      chip_address
#include "i2c_device_model.h"

extern char log_text[];

#define AS_FOUND        (1u << 2 | 3u)          /* the bus's pins: the serial engine's function, pulled up */
#define INT_AS_FOUND    3u                      /* the interrupt pin: an input, pulled up */

#define WHO_AM_I        0x0f
#define CTRL1_XL        0x10
#define CTRL2_G         0x11
#define CTRL3_C         0x12
#define OUTX_L_G        0x22

/* a BMI160's (the bmi160_* scenarios) */
#define BMI_CHIP_ID     0x00
#define BMI_PMU_STATUS  0x03
#define BMI_DATA_GYR    0x0c
#define BMI_ACC_CONF    0x40
#define BMI_ACC_RANGE   0x41
#define BMI_GYR_CONF    0x42
#define BMI_GYR_RANGE   0x43
#define BMI_CMD         0x7e

static bool bmi160;                             /* the chip on the bus is the model of a BMI160 */
static unsigned output_register = OUTX_L_G;     /* the first of the chip's twelve output bytes */

/* what is written into the chip */
static bool register_written[256];
static unsigned resets, written_after_reset;
static int deaf_register = -1;                  /* a register that does not take what is written into it */
static bool reset_never_ends;                   /* CTRL3_C's reset bit stays set */
static volatile unsigned output_reads;          /* reads of the first output byte: samples taken */
static volatile bool gyro_wobbles;              /* the gyroscope's x says one count more or less on every other read, as one at rest does */

/*
 * The model of a BMI160. A command that changes a half's power mode takes `bmi_command_reads' reads of PMU_STATUS
 * to be carried out (the gyroscope's start three times as many: it is the slow one), PMU_STATUS saying the old mode
 * until then; a command that comes meanwhile is dropped, as the chip drops it. After a reset the chip does not
 * acknowledge its address for `bmi_reset_transfers' transfers. CMD keeps nothing to read back.
 */
static uint8_t bmi_commands[64];                /* every command written, in order ... */
static unsigned bmi_command_count;
static unsigned bmi_dropped;                    /* ... and how many of them came while another was carried out */
static unsigned bmi_busy;                       /* reads of PMU_STATUS until the command in hand is carried out */
static uint8_t bmi_status_then;                 /* ... and what PMU_STATUS says from then on */
static unsigned bmi_command_reads = 2;
static unsigned bmi_gyro_start_reads;           /* (0: three times bmi_command_reads) */
static unsigned bmi_reset_transfers = 2;
static unsigned bmi_back_at;                    /* the transfer (device.starts) from which it answers again after a reset; 0: it answers */
static volatile bool bmi_start_held;            /* the gyroscope's start is not carried out while the scenario holds it: the reads do not count */
static bool bmi_drop_next;                      /* the chip drops the next command, as if it were carrying one out */
static int bmi_deaf_command = -1;               /* a command the chip acknowledges and does not carry out */
static unsigned bmi_written_while_busy;         /* registers written while a command was being carried out */
static unsigned bmi_written_while_suspended;    /* a half's rate or range written while the half was suspended */

static void
bmi_reset(void)
{
	device.reg[BMI_PMU_STATUS] = 0x00;      /* both halves suspended */
	device.reg[BMI_ACC_CONF] = 0x28;        /* 100 Hz, the normal filter */
	device.reg[BMI_ACC_RANGE] = 0x03;       /* +-2 g */
	device.reg[BMI_GYR_CONF] = 0x28;
	device.reg[BMI_GYR_RANGE] = 0x00;       /* +-2000 dps */
	bmi_busy = 0;
}

static void
bmi_written(uint8_t reg, uint8_t value)
{
	bool gyroscope = value == 0x14 || value == 0x15;
	uint8_t status = device.reg[BMI_PMU_STATUS];

	if (reg != BMI_CMD) {
		written_after_reset += resets > 0;
		bmi_written_while_busy += bmi_busy != 0;
		if (reg == BMI_ACC_CONF || reg == BMI_ACC_RANGE) {
			bmi_written_while_suspended += ((status >> 4) & 3) == 0;
		} else if (reg == BMI_GYR_CONF || reg == BMI_GYR_RANGE) {
			bmi_written_while_suspended += ((status >> 2) & 3) == 0;
		}
		if (reg != deaf_register) {
			device.reg[reg] = reg == BMI_ACC_RANGE ? value & 0x0f : reg == BMI_GYR_RANGE ? value & 0x07 : value;
		}
		return;
	}
	if (bmi_command_count < sizeof(bmi_commands)) {
		bmi_commands[bmi_command_count++] = value;
	}
	if (value == 0xb6) {
		resets++;
		bmi_reset();
		device.absent = true;           /* (from the next transfer on: this byte is acknowledged) */
		bmi_back_at = reset_never_ends ? ~0u : device.starts + 1 + bmi_reset_transfers;
		return;
	}
	written_after_reset += resets > 0;
	if (value != 0x10 && value != 0x11 && value != 0x14 && value != 0x15) {
		return;
	}
	if (bmi_busy != 0 || bmi_drop_next) {
		bmi_dropped++;
		bmi_drop_next = false;
		return;
	}
	if (value == bmi_deaf_command || ((status >> (gyroscope ? 2 : 4)) & 3) == (value & 1)) {
		return;                         /* (nor is there anything to carry out for a half that is in that mode) */
	}
	bmi_status_then = gyroscope ? (uint8_t)((status & ~0x0c) | (value & 1) << 2) : (uint8_t)((status & ~0x30) | (value & 1) << 4);
	bmi_busy = value == 0x15 ? (bmi_gyro_start_reads ? bmi_gyro_start_reads : 3 * bmi_command_reads) : bmi_command_reads;
}

static void
chip_reset(void)
{
	if (bmi160) {
		bmi_reset();
		return;
	}
	device.reg[CTRL1_XL] = 0x00;
	device.reg[CTRL2_G] = 0x00;
	device.reg[CTRL3_C] = 0x04;             /* IF_INC, as the chip comes out of reset */
}

static void
chip_written(uint8_t reg, uint8_t value)
{
	register_written[reg] = true;
	if (bmi160) {
		bmi_written(reg, value);
		return;
	}
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
	if (reg == output_register) {
		output_reads++;
		if (gyro_wobbles) {
			device.reg[output_register] ^= 1;
		}
	}
	if (bmi160 && reg == BMI_PMU_STATUS && bmi_busy != 0 && !(bmi_start_held && (bmi_status_then & 0x0c)) && --bmi_busy == 0) {
		device.reg[BMI_PMU_STATUS] = bmi_status_then;
	}
	return device.reg[reg];
}

uint32_t
hakios_test_rd(vm_offset_t address)
{
	if (bmi_back_at != 0 && device.starts >= bmi_back_at) {
		bmi_back_at = 0;                /* the BMI160's reset is over: it answers again */
		device.absent = false;
	}
	i2c_device_looked_at(address);
	return tlmm_model_rd(address);
}

/*
 * The driver's thread stopped where a scenario says, as the run's end stops the other cores: after each of its
 * writes to the pins `stop_after' is asked, with what the register held before, and once it says so the thread
 * does nothing more. The scenario's own thread is never stopped.
 */
static pthread_t scenario_thread;
static bool (*volatile stop_after)(vm_offset_t address, uint32_t before);
static volatile bool stopped;

void
hakios_test_wr(vm_offset_t address, uint32_t value)
{
	uint32_t offset = (uint32_t)(address - model.base), pin = offset / 0x1000;
	uint32_t before = address >= model.base && pin < PINS ? (offset % 0x1000 == 0 ? model.cfg[pin] : model.io[pin]) : 0;

	if (bmi_back_at != 0 && device.starts >= bmi_back_at) {
		bmi_back_at = 0;
		device.absent = false;
	}
	tlmm_model_wr(address, value);
	if (stop_after != NULL && !pthread_equal(pthread_self(), scenario_thread) && stop_after(address, before)) {
		stopped = true;
		for (;;) {
			usleep(100000);
		}
	}
}

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

/* what the chip's gyroscope reads, in its own axes and counts: the first six of the twelve output bytes */
static int16_t rates[3] = { 11, -22, 33 };      /* (not what the orientation is made of; the same each read: not answering) */

static void
output_word(unsigned i, int16_t v)
{
	device.reg[output_register + 2 * i] = (uint8_t)((uint16_t)v & 0xff);
	device.reg[output_register + 2 * i + 1] = (uint8_t)((uint16_t)v >> 8);
}

static void
chip_rates(int16_t x, int16_t y, int16_t z)
{
	rates[0] = x;
	rates[1] = y;
	rates[2] = z;
	for (unsigned i = 0; i < 3; i++) {
		output_word(i, rates[i]);
	}
}

/* what the chip's accelerometer reads, in its own axes and counts: the last six of the twelve output bytes */
static void
chip_reads(int16_t x, int16_t y, int16_t z)
{
	int16_t v[3] = { x, y, z };

	for (unsigned i = 0; i < 3; i++) {
		output_word(i, rates[i]);
		output_word(3 + i, v[i]);
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
	device.reg[bmi160 ? BMI_CHIP_ID : WHO_AM_I] = identity;
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

/* only the three control registers, if anything */
static bool
only_the_control_registers(void)
{
	for (unsigned reg = 0; reg < 256; reg++) {
		if (register_written[reg] && reg != CTRL1_XL && reg != CTRL2_G && reg != CTRL3_C) {
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
	CHECK(only_the_control_registers(), "nothing but CTRL3_C, CTRL1_XL and CTRL2_G was written");
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
	CHECK(device.reg[CTRL2_G] == 0 && logged("imu: at the run's end: the gyroscope is powered down (CTRL2_G 0)"),
	    "the gyroscope too: %#x, %s", device.reg[CTRL2_G], log_text);
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

/* the chip stops answering while the orientation is followed: hw.sensors.orientation says not known (0), not the last one */
static void
lost_is_not_known(void)
{
	following();
	WAIT_FOR(get("orientation") == 1, 3000);
	CHECK(get("orientation") == 1, "portrait (1) first: %u", get("orientation"));
	device.absent = true;
	WAIT_FOR(logged("imu: done"), 5000);
	CHECK(logged("a byte was not acknowledged: NOTHING MORE IS DONE"), "said: %s", log_text);
	CHECK(get("orientation") == 0, "not known (0) once nothing more is read: %u", get("orientation"));
	device.absent = false;
	hakios_imu_quiesce();
}

/*
 * Darwin is to be given the orientation (hw.sensors.hid, which backboardd's library looks at:
 * userland/userland/src/hid_orientation.h) only while it is followed: not before the chip is configured, 1 from then
 * on, 0 again once nothing more is read -- the chip lost, or the run's end.
 */
static void
given_to_darwin(void)
{
	phone("phone-video", 0x6c);
	CHECK(get("hid") == 0, "nothing to give before anything is followed: %u", get("hid"));
	following();
	CHECK(get("hid") == 1, "1 while followed: %u", get("hid"));
	CHECK(logged("Darwin is given it (hw.sensors.hid 1)"), "said: %s", log_text);
	device.absent = true;
	WAIT_FOR(logged("imu: done"), 5000);
	CHECK(get("hid") == 0, "0 once the chip is lost: %u", get("hid"));
	device.absent = false;
	hakios_imu_quiesce();
	CHECK(get("hid") == 0, "and after the run's end: %u", get("hid"));
}

/* ... and at the run's end, with the thread still reading */
static void
given_until_the_end(void)
{
	following();
	CHECK(get("hid") == 1, "1 while followed: %u", get("hid"));
	hakios_imu_quiesce();
	CHECK(get("hid") == 0, "0 from the run's end on: %u", get("hid"));
}

/* orientation_hid=0: the orientation followed and said as ever, and Darwin not given it */
static void
kept_from_darwin(void)
{
	phone("phone-video", 0x6c);
	chip_reads(-8192, 0, 0);
	boot_args = "sensors=2 orientation_hid=0";
	hakios_imu_init();
	WAIT_FOR(get("orientation") == 1, 5000);
	CHECK(get("orientation") == 1, "followed all the same: portrait (1): %u", get("orientation"));
	CHECK(get("hid") == 0, "not given to Darwin: %u", get("hid"));
	CHECK(logged("Darwin is not given it (orientation_hid=0)"), "said: %s", log_text);
	hakios_imu_quiesce();
}

/*
 * The samples for Darwin's motion stack (hw.sensors.motion, which backboardd's library looks at:
 * userland/userland/src/hid_motion.h): 1 only while hw.sensors.hid is -- set no later than it, gone with it at the
 * run's end -- and never with motion_hid=0, nor with orientation_hid=0, which leaves Darwin nothing.
 */
static void
motion_given_to_darwin(void)
{
	phone("phone-video", 0x6c);
	CHECK(get("motion") == 0, "nothing to give before anything is followed: %u", get("motion"));
	following();
	CHECK(get("motion") == 1 && get("hid") == 1, "1 while followed: motion %u, hid %u", get("motion"), get("hid"));
	CHECK(logged("and its motion stack the samples (hw.sensors.motion 1)"), "said: %s", log_text);
	hakios_imu_quiesce();
	CHECK(get("motion") == 0, "0 from the run's end on: %u", get("motion"));
}

static void
motion_kept_from_darwin(void)
{
	phone("phone-video", 0x6c);
	chip_reads(-8192, 0, 0);
	boot_args = "sensors=2 motion_hid=0";
	hakios_imu_init();
	WAIT_FOR(get("orientation") == 1, 5000);
	CHECK(get("hid") == 1, "the orientation given as ever: %u", get("hid"));
	CHECK(get("motion") == 0, "the samples not given: %u", get("motion"));
	CHECK(logged("not its motion stack the samples (motion_hid=0)"), "said: %s", log_text);
	hakios_imu_quiesce();
}

static void
motion_not_without_the_orientation(void)
{
	phone("phone-video", 0x6c);
	chip_reads(-8192, 0, 0);
	boot_args = "sensors=2 orientation_hid=0";
	hakios_imu_init();
	WAIT_FOR(get("orientation") == 1, 5000);
	CHECK(get("hid") == 0 && get("motion") == 0, "hid %u, motion %u", get("hid"), get("motion"));
	hakios_imu_quiesce();
}

/*
 * hw.sensors.sample: "NUMBER TIME X Y Z", the five of one sample. "0 0 0 0 0" before the first; then the number
 * counts the samples (what hw.sensors.samples says, or one behind it), the time is the host's clock when it was read
 * and grows with the number, and x y z are hw.sensors.accel's milli-g.
 */
static void
timed_sample(void)
{
	unsigned number = 0, later = 0;
	unsigned long long time = 0, later_time = 0;
	int mg[3] = { 0, 0, 0 };
	uint64_t before;

	phone("phone-video", 0x6c);
	CHECK(strcmp(get_string("sample"), "0 0 0 0 0") == 0, "nothing read yet: %s", get_string("sample"));
	before = mach_absolute_time();
	following();
	WAIT_FOR(get("orientation") == 1, 3000);
	CHECK(sscanf(get_string("sample"), "%u %llu %d %d %d", &number, &time, &mg[0], &mg[1], &mg[2]) == 5, "five numbers: %s",
	    get_string("sample"));
	CHECK(number >= 1 && number <= get("samples") && get("samples") - number <= 1, "number %u, %u samples read", number,
	    get("samples"));
	CHECK(time > before && time <= mach_absolute_time(), "read at %llu, not between %llu and now", time,
	    (unsigned long long)before);
	CHECK(mg[0] == 0 && mg[1] == -1000 && mg[2] == 0, "portrait's milli-g: %d %d %d", mg[0], mg[1], mg[2]);
	chip_reads(0, 0, 8192);
	WAIT_FOR(sscanf(get_string("sample"), "%u %llu %d %d %d", &later, &later_time, &mg[0], &mg[1], &mg[2]) == 5 &&
	    mg[2] == -1000, 3000);
	CHECK(mg[0] == 0 && mg[1] == 0 && mg[2] == -1000, "face up's milli-g: %d %d %d", mg[0], mg[1], mg[2]);
	CHECK(later > number && later_time > time, "sample %u at %llu after sample %u at %llu", later, later_time, number, time);
	CHECK(strcmp(get_string("accel"), "0 0 -1000") == 0, "hw.sensors.accel says the same: %s", get_string("accel"));
	hakios_imu_quiesce();
}

/*
 * The chip's output reads all zeros at first (its registers as a reset leaves them, before its first sample is
 * ready): not taken for a phone face down. The orientation stays not known until a real sample, and is named from
 * that one at once, not after a climb from zero.
 */
static void
zeros_before_the_first_sample(void)
{
	uint32_t before;

	phone("phone-video", 0x6c);
	boot_args = "sensors=2";
	hakios_imu_init();
	WAIT_FOR(logged("the orientation is followed from here on") && get("samples") >= 3, 10000);
	CHECK(get("samples") >= 3, "read: %u", get("samples"));
	CHECK(get("orientation") == 0, "zeros are not known (0): %u", get("orientation"));
	CHECK(!logged("imu: face down"), "not said to be face down: %s", log_text);
	before = get("samples");
	chip_reads(-8192, 0, 0);
	WAIT_FOR(get("orientation") == 1, 3000);
	CHECK(get("orientation") == 1, "portrait (1): %u", get("orientation"));
	CHECK(get("samples") - before <= 3, "named from the first real sample: %u samples", get("samples") - before);
	hakios_imu_quiesce();
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

/* stopped inside i2c_gpio_take(): the data pin has just been made a GPIO from what it was found as */
static bool
taking_the_pins(vm_offset_t address, uint32_t before)
{
	return address == GPIO_CFG(I2C_DEVICE_SDA) && before == AS_FOUND;
}

/* stopped in a byte the chip is sending, the clock just down and the chip holding the data line low for its next bit */
static bool
chip_holding_the_data_line(vm_offset_t address, uint32_t before)
{
	return device.reading && device.sda_low;
}

/* the thread stopped in the identity's read, the chip holding the data line low: the bus clocked free, the pins given back */
static void
stopped_in_a_read(const char *args)
{
	phone("phone-video", 0x6c);
	stop_after = chip_holding_the_data_line;
	boot_args = args;
	hakios_imu_init();
	WAIT_FOR(stopped, 5000);
	CHECK(stopped && !model.input[I2C_DEVICE_SDA], "stopped while the chip holds the data line low: %s", log_text);
	hakios_imu_quiesce();
	CHECK(model.input[I2C_DEVICE_SDA] && model.input[I2C_DEVICE_SCL], "the bus at rest after the run's end: data %u, clock %u",
	    model.input[I2C_DEVICE_SDA], model.input[I2C_DEVICE_SCL]);
	CHECK(logged("imu: at the run's end: the bus came back after"), "said: %s", log_text);
	CHECK(device.writes == 0, "nothing written to the chip: %u bytes", device.writes);
	pins_as_found();
}

/* ... at a look, where nothing is to be powered down */
static void
stopped_in_a_read_at_a_look(void)
{
	stopped_in_a_read("sensors=1");
}

/* ... and before the accelerometer is switched on */
static void
stopped_in_a_read_before_switched_on(void)
{
	stopped_in_a_read("sensors=2");
}

/* the thread stopped inside its first take of the pins, the data pin a GPIO already: the pins given back as found */
static void
stopped_taking_the_pins_first(void)
{
	phone("phone-video", 0x6c);
	stop_after = taking_the_pins;
	boot_args = "sensors=1";
	hakios_imu_init();
	WAIT_FOR(stopped, 5000);
	CHECK(stopped && model.cfg[I2C_DEVICE_SDA] != AS_FOUND, "stopped with the data pin taken: %#x", model.cfg[I2C_DEVICE_SDA]);
	hakios_imu_quiesce();
	CHECK(device.writes == 0, "nothing written to the chip: %u bytes", device.writes);
	pins_as_found();
}

/* ... and inside a take while it follows the orientation: the pins given back as found, the accelerometer powered down */
static void
stopped_taking_the_pins(void)
{
	following();
	stop_after = taking_the_pins;
	WAIT_FOR(stopped, 5000);
	CHECK(stopped && model.cfg[I2C_DEVICE_SDA] != AS_FOUND, "stopped with the data pin taken: %#x", model.cfg[I2C_DEVICE_SDA]);
	hakios_imu_quiesce();
	CHECK(device.reg[CTRL1_XL] == 0 && logged("is powered down (CTRL1_XL 0)"), "powered down: %s", log_text);
	pins_as_found();
}

/* ---- the gyroscope ------------------------------------------------------------------------------- */

/* hw.sensors.sample's numbers: how many it says (5, or 8 with the gyroscope's rates), and them */
static int
sample_says(int mg[3], int rate[3])
{
	unsigned number;
	unsigned long long time;

	return sscanf(get_string("sample"), "%u %llu %d %d %d %d %d %d", &number, &time, &mg[0], &mg[1], &mg[2], &rate[0], &rate[1],
	    &rate[2]);
}

/* until hw.sensors.sample carries these rates (milli-rad/s on Darwin's axes), 3 s at most */
static bool
rates_said(int x, int y, int z)
{
	int mg[3], rate[3];

	for (int waited = 0; waited < 3000; waited += 5) {
		if (sample_says(mg, rate) == 8 && rate[0] == x && rate[1] == y && rate[2] == z) {
			return true;
		}
		usleep(5000);
	}
	fprintf(stderr, "hw.sensors.sample: %s, not the rates %d %d %d\n", get_string("sample"), x, y, z);
	return false;
}

/* a gyroscope that answers (its x wobbling by a count), until it is given: sensors=2 and whatever else ARGS says */
static void
gyro_answering(const char *args)
{
	phone("phone-video", 0x6c);
	chip_reads(-8192, 0, 0);
	chip_rates(1000, 0, 0);
	gyro_wobbles = true;
	boot_args = args;
	hakios_imu_init();
	WAIT_FOR(get("gyro") == 1, 10000);
}

/*
 * The samples are for Darwin's motion stack (sensors=2, by default): the gyroscope is switched on with the
 * accelerometer, at the same 104 Hz and +-2000 dps (CTRL2_G 0x4c), the write read back -- but not given before it is
 * seen to answer; at the run's end it is powered down (CTRL2_G 0) with the accelerometer.
 */
static void
gyro_configured(void)
{
	following();
	CHECK(device.reg[CTRL2_G] == 0x4c, "CTRL2_G: 104 Hz (4 << 4) at +-2000 dps (3 << 2), 0x4c: %#x", device.reg[CTRL2_G]);
	CHECK(logged("imu: 0x11 <- 0x4c, reads 0x4c") && logged("its gyroscope too, for Darwin's motion stack: 104 Hz, +-2000 dps"),
	    "said: %s", log_text);
	CHECK(logged("with the gyroscope's rates once it is seen to answer (hw.sensors.gyro)"), "said: %s", log_text);
	CHECK(get("gyro") == 0, "the rates given before the gyroscope was seen to answer: %u", get("gyro"));
	hakios_imu_quiesce();
	CHECK(device.reg[CTRL2_G] == 0 && device.reg[CTRL1_XL] == 0, "powered down: CTRL2_G %#x, CTRL1_XL %#x", device.reg[CTRL2_G],
	    device.reg[CTRL1_XL]);
	CHECK(logged("imu: at the run's end: the gyroscope is powered down (CTRL2_G 0)"), "said: %s", log_text);
	CHECK(only_the_control_registers(), "nothing else written");
}

/*
 * The rates, once the gyroscope answers: from its fifth read (three let settle, the fourth what the fifth is held
 * against), in the same sample as the accelerometer's milli-g, turned onto Darwin's axes as the accelerometer is but
 * with the board's signs turned back (the board: "-y", "x", "-z" for the accelerometer; for the rates Darwin's x is
 * the chip's +y, its y the chip's -x, its z the chip's +z) and into milli-rad/s at 70 mdps a count: 1000 counts are 70
 * dps, 1.2217 rad/s.
 */
static void
gyro_rates_through_the_mounting(void)
{
	int mg[3], rate[3];

	gyro_answering("sensors=2");
	CHECK(get("gyro") == 1, "the rates not given: %s", log_text);
	CHECK(logged("imu: the gyroscope answers at its read 5 ("), "given from another read than its fifth: %s", log_text);
	CHECK(logged("its rates are given to Darwin's motion stack (hw.sensors.gyro 1)"), "said: %s", log_text);
	gyro_wobbles = false;
	chip_rates(1000, 0, 0);
	CHECK(rates_said(0, -1222, 0), "the chip's x is Darwin's -y");
	CHECK(sample_says(mg, rate) == 8 && mg[0] == 0 && mg[1] == -1000 && mg[2] == 0, "with the accelerometer's portrait: %s",
	    get_string("sample"));
	chip_rates(0, 2000, 0);
	CHECK(rates_said(2443, 0, 0), "the chip's y is Darwin's x: 140 dps, 2.4435 rad/s");
	chip_rates(0, 0, -32768);
	CHECK(rates_said(0, 0, -40034), "the chip's z is Darwin's z, all the range: -2293.76 dps, -40.0335 rad/s");
	chip_rates(-1, 1, 1);
	CHECK(rates_said(1, 1, 1), "a count is 1.22 milli-rad/s, rounded to the nearest");
	CHECK(only_the_control_registers(), "reading wrote nothing");
	hakios_imu_quiesce();
	CHECK(get("gyro") == 0, "given after the run's end: %u", get("gyro"));
	CHECK(device.reg[CTRL2_G] == 0, "not powered down at the run's end: %#x", device.reg[CTRL2_G]);
}

/* What keeps the gyroscope off: never written, never given, no sample with rates -- and nothing to power down at the end. */
static void
gyro_kept_off(const char *args, const char *said)
{
	int mg[3], rate[3];

	gyro_answering(args);
	WAIT_FOR(get("samples") >= 12, 10000);
	CHECK(get("samples") >= 12, "followed: %u samples", get("samples"));
	CHECK(!register_written[CTRL2_G] && device.reg[CTRL2_G] == 0, "the gyroscope switched on: %#x", device.reg[CTRL2_G]);
	CHECK(get("gyro") == 0 && sample_says(mg, rate) == 5, "rates given: hw.sensors.gyro %u, %s", get("gyro"), get_string("sample"));
	CHECK(!logged("imu: the gyroscope answers"), "said: %s", log_text);
	CHECK(said == NULL || logged(said), "not said \"%s\": %s", said, log_text);
	hakios_imu_quiesce();
	CHECK(!register_written[CTRL2_G] && !logged("the gyroscope is powered down"), "written at the run's end: %s", log_text);
}

/* gyro_hid=0: the accelerometer's samples given as ever, the gyroscope powered down */
static void
gyro_hid_off(void)
{
	gyro_kept_off("sensors=2 gyro_hid=0", "without the gyroscope's rates (gyro_hid=0)");
	CHECK(get("motion") == 0 && logged("and its motion stack the samples (hw.sensors.motion 1)"), "the samples not given: %s",
	    log_text);
}

/* ... nor without the samples for the motion stack (motion_hid=0), nor without Darwin given anything (orientation_hid=0) */
static void
gyro_not_without_motion(void)
{
	gyro_kept_off("sensors=2 motion_hid=0", NULL);
}

static void
gyro_not_without_darwin(void)
{
	gyro_kept_off("sensors=2 orientation_hid=0", NULL);
}

/* a board whose accelerometer mount is no mirror: the gyroscope's rates could only be guessed the right way round: off */
static void
gyro_board_mount_refused(void)
{
	static struct hakios_board mirrored;

	phone("phone-video", 0x6c);
	mirrored = *hakios_board();
	mirrored.imu.sign[0] = (int8_t)-mirrored.imu.sign[0];   /* ("y", "x", "-z": a turn, which no accelerometer's mount is) */
	found = &mirrored;
	gyro_kept_off("sensors=2", "the gyroscope is not switched on: the board's mount (imu { }) is not a turn");
	CHECK(logged("without the gyroscope's rates (the board's mount)"), "said: %s", log_text);
}

/* a gyroscope that reads the same zeros each time (one that does not answer): never given, and powered down again */
static void
gyro_zeros_not_given(void)
{
	int mg[3], rate[3];
	bool given = false;

	phone("phone-video", 0x6c);
	chip_reads(-8192, 0, 0);
	chip_rates(0, 0, 0);
	boot_args = "sensors=2";
	hakios_imu_init();
	for (int waited = 0; waited < 15000 && !logged("does not answer"); waited += 5) {
		given |= get("gyro") != 0 || sample_says(mg, rate) == 8;
		usleep(5000);
	}
	CHECK(!given, "rates given for zeros");
	CHECK(logged("imu: the gyroscope has read 0 0 0 (counts) 100 times in a row: it does not answer, its rates NOT GIVEN; "
	    "it is powered down (CTRL2_G 0)"), "said: %s", log_text);
	CHECK(device.reg[CTRL2_G] == 0, "not powered down: %#x", device.reg[CTRL2_G]);
	WAIT_FOR(get("orientation") == 1, 3000);
	CHECK(get("orientation") == 1 && get("motion") == 1, "the accelerometer not followed as ever");
	hakios_imu_quiesce();
}

/* a gyroscope that answered and then reads the very same for ten seconds: held stuck, its rates taken back */
static void
gyro_stuck_taken_back(void)
{
	int mg[3], rate[3];
	uint32_t before;

	gyro_answering("sensors=2");
	CHECK(get("gyro") == 1, "the rates not given: %s", log_text);
	gyro_wobbles = false;
	WAIT_FOR(logged("stuck"), 15000);
	CHECK(logged("times in a row: stuck, its rates NOT GIVEN ANY MORE; it is powered down (CTRL2_G 0)"), "said: %s", log_text);
	CHECK(get("gyro") == 0, "still given: %u", get("gyro"));
	CHECK(device.reg[CTRL2_G] == 0, "not powered down: %#x", device.reg[CTRL2_G]);
	before = get("samples");
	WAIT_FOR(get("samples") >= before + 2, 3000);
	CHECK(sample_says(mg, rate) == 5 && get("samples") >= before + 2, "the samples go on without rates: %s",
	    get_string("sample"));
	gyro_wobbles = true;                    /* (and it is not taken back up: stuck is for the run) */
	WAIT_FOR(get("samples") >= before + 10, 3000);
	CHECK(get("gyro") == 0 && sample_says(mg, rate) == 5, "given again: %s", get_string("sample"));
	hakios_imu_quiesce();
}

/* the chip lost: the rates are not given any more (hw.sensors.gyro 0); the run's end powers it down all the same */
static void
gyro_lost_with_the_chip(void)
{
	gyro_answering("sensors=2");
	CHECK(get("gyro") == 1, "the rates not given: %s", log_text);
	device.absent = true;
	WAIT_FOR(logged("imu: done"), 5000);
	CHECK(get("gyro") == 0 && get("motion") == 0, "given after the chip was lost: gyro %u, motion %u", get("gyro"), get("motion"));
	device.absent = false;
	hakios_imu_quiesce();
	CHECK(device.reg[CTRL2_G] == 0 && logged("the gyroscope is powered down (CTRL2_G 0)"), "not powered down: %s", log_text);
}

/* ---- a BMI160 ------------------------------------------------------------------------------------ */

/*
 * The test phone with a BMI160 where its LSM6DSO is: its board's row with the one feature for the other and the
 * chip's address, +-RANGE g; the model of the chip on the same pins, answering IDENTITY at CHIP_ID.
 */
static void
bmi160_phone(uint8_t identity, uint8_t range)
{
	static struct hakios_board with_bmi160;

	bmi160 = true;
	chip_address = 0x68;
	output_register = BMI_DATA_GYR;
	phone("phone-video", identity);
	with_bmi160 = *hakios_board();
	with_bmi160.features = (with_bmi160.features & ~HAKIOS_HAS_IMU_LSM6DSO) | HAKIOS_HAS_IMU_BMI160;
	with_bmi160.imu.address = 0x68;
	with_bmi160.imu.full_scale = range;
	found = &with_bmi160;
}

/* the commands the chip was given, in order: exactly these */
static bool
bmi160_commands_were(const char *what, unsigned count, ...)
{
	va_list list;
	bool same = bmi_command_count == count;

	va_start(list, count);
	for (unsigned i = 0; i < count; i++) {
		unsigned command = va_arg(list, unsigned);

		same = same && bmi_commands[i] == command;
	}
	va_end(list);
	if (!same) {
		fprintf(stderr, "%s: the commands were", what);
		for (unsigned i = 0; i < bmi_command_count && i < sizeof(bmi_commands); i++) {
			fprintf(stderr, " 0x%02x", bmi_commands[i]);
		}
		fprintf(stderr, "\n");
	}
	return same;
}

/* only CMD and the four registers of rate and range, if anything; none while a command was carried out, none dropped */
static bool
bmi160_written_as_it_wants(void)
{
	for (unsigned reg = 0; reg < 256; reg++) {
		if (register_written[reg] && reg != BMI_CMD && (reg < BMI_ACC_CONF || reg > BMI_GYR_RANGE)) {
			fprintf(stderr, "register 0x%02x was written\n", reg);
			return false;
		}
	}
	if (bmi_dropped != 0 || bmi_written_while_busy != 0 || bmi_written_while_suspended != 0) {
		fprintf(stderr, "%u commands came while another was carried out, %u registers were written meanwhile, %u while their "
		    "half was suspended\n", bmi_dropped, bmi_written_while_busy, bmi_written_while_suspended);
		return false;
	}
	return true;
}

/* nothing of the chip's is given to Darwin, and nothing was read of it */
static void
bmi160_nothing_given(const char *when)
{
	CHECK(get("hid") == 0 && get("motion") == 0 && get("gyro") == 0, "%s: hid %u, motion %u, gyro %u", when, get("hid"),
	    get("motion"), get("gyro"));
	CHECK(get("orientation") == 0 && get("samples") == 0 && strcmp(get_string("sample"), "0 0 0 0 0") == 0 &&
	    strcmp(get_string("accel"), "0 0 0") == 0, "%s: orientation %u, %u samples, sample %s", when, get("orientation"),
	    get("samples"), get_string("sample"));
	CHECK(output_reads == 0, "%s: the output was read %u times", when, output_reads);
}

/* sensors=2 (and whatever else ARGS says) on the phone with a BMI160 at +-4 g, held upright, until the orientation is followed */
static void
bmi160_following(const char *args)
{
	bmi160_phone(0xd1, 4);
	chip_reads(-8192, 0, 0);
	chip_rates(1000, 0, 0);
	gyro_wobbles = true;
	boot_args = args;
	hakios_imu_init();
	WAIT_FOR(logged("the orientation is followed from here on") && get("samples") >= 2, 10000);
	CHECK(logged("the orientation is followed from here on"), "followed: %s", log_text);
}

/* sensors=1: the identity read at CHIP_ID and said, nothing written, no samples, nothing given; nor at the run's end */
static void
bmi160_look_only(void)
{
	bmi160_phone(0xd1, 4);
	boot_args = "sensors=1";
	hakios_imu_init();
	WAIT_FOR(logged("imu: done"), 5000);
	CHECK(logged("imu: the chip at 0x68 answers CHIP_ID 0xd1, a BMI160 (0xd1)"), "the identity, against the BMI160's: %s", log_text);
	CHECK(device.writes == 0 && device.starts >= 2 && bmi_command_count == 0, "read, and nothing written: %u bytes", device.writes);
	CHECK(!logged("followed") && !logged("NOTHING MORE"), "no samples at a look, nothing wrong: %s", log_text);
	bmi160_nothing_given("at a look");
	pins_as_found();
	hakios_imu_quiesce();
	CHECK(device.writes == 0, "nor at the run's end: %u", device.writes);
	pins_as_found();
}

/* another chip at the address (0x24, which a BMI270 is remembered to answer; any number but 0xd1 does): refused, nothing written, nothing given */
static void
bmi160_another_chip_refused(void)
{
	bmi160_phone(0x24, 4);
	device.reg[WHO_AM_I] = 0x6c;            /* (what the other row's identity register would say: not looked at) */
	boot_args = "sensors=2";
	hakios_imu_init();
	WAIT_FOR(logged("imu: done"), 5000);
	CHECK(logged("imu: the chip at 0x68 answers CHIP_ID 0x24: NOT THE BMI160'S 0xd1") && logged("another chip: NOTHING MORE IS DONE"),
	    "said: %s", log_text);
	CHECK(device.writes == 0 && resets == 0, "not reset, not configured: %u bytes written", device.writes);
	bmi160_nothing_given("another chip");
	hakios_imu_quiesce();
	CHECK(device.writes == 0, "nor written at the run's end");
	pins_as_found();
}

/* nobody at the address: said, nothing written, nothing given */
static void
bmi160_nobody_there(void)
{
	bmi160_phone(0xd1, 4);
	device.absent = true;
	boot_args = "sensors=2";
	hakios_imu_init();
	WAIT_FOR(logged("imu: done"), 5000);
	CHECK(logged("imu: nobody answers at 0x68 on the bus of gpio116/117") &&
	    logged("a byte was not acknowledged: NOTHING MORE IS DONE"), "said: %s", log_text);
	bmi160_nothing_given("nobody there");
	device.absent = false;
	hakios_imu_quiesce();
	CHECK(device.writes == 0, "nothing written, nor at the run's end: %u", device.writes);
	pins_as_found();
}

/* the board lists the LSM6DSO where the chip is a BMI160 (the test phone as it is, the BMI160's model at its address): refused */
static void
bmi160_under_the_other_feature(void)
{
	bmi160 = true;
	output_register = BMI_DATA_GYR;
	phone("phone-video", 0xd1);             /* (at 0x6a, where the board says its LSM6DSO is) */
	boot_args = "sensors=2";
	hakios_imu_init();
	WAIT_FOR(logged("imu: done"), 5000);
	/* (the LSM6DSO's identity register is the high byte of the BMI160's gyroscope y: here -22's 0xff, and whatever turns it) */
	CHECK(logged("answers WHO_AM_I 0xff: NOT THE LSM6DSO'S 0x6c"), "said: %s", log_text);
	CHECK(device.writes == 0, "nothing written: %u bytes", device.writes);
	bmi160_nothing_given("the other feature's chip");
}

/*
 * sensors=2: the reset command, the chip waited for while it does not answer; the accelerometer's command, then, once
 * PMU_STATUS says it measures, 100 Hz and the board's +-4 g, each read back; the gyroscope's command, then, once it
 * measures, 100 Hz and +-2000 dps. In that order, no command while another is carried out, nothing else written.
 * Nothing is given before all of it is done.
 */
static void
bmi160_configured(void)
{
	bmi160_phone(0xd1, 4);
	chip_reads(-8192, 0, 0);
	boot_args = "sensors=2";
	bmi160_nothing_given("before the driver starts");
	hakios_imu_init();
	for (int waited = 0; waited < 10000 && !logged("the orientation is followed from here on"); waited += 1) {
		/*
		 * (looked at all the while: nothing given, nothing read of the output, until the last register is written -- what
		 * is given looked at first, so that a chip not yet configured after it is one not configured before it)
		 */
		uint32_t hid = get("hid"), motion = get("motion"), read = get("samples");
		unsigned reads = output_reads;

		if (device.reg[BMI_PMU_STATUS] != 0x14 || !register_written[BMI_GYR_RANGE]) {
			CHECK(hid == 0 && motion == 0 && read == 0 && reads == 0, "given before the chip was configured: hid %u, %u samples "
			    "(PMU_STATUS %#x)", hid, read, device.reg[BMI_PMU_STATUS]);
		}
		usleep(1000);
	}
	WAIT_FOR(get("samples") >= 2, 5000);
	CHECK(logged("the orientation is followed from here on") && get("hid") == 1 && get("motion") == 1, "followed: %s", log_text);
	CHECK(bmi160_commands_were("configured", 3, 0xb6, 0x11, 0x15), "reset, the accelerometer on, the gyroscope on");
	CHECK(resets == 1 && logged("imu: 0x7e <- 0xb6, the chip is back after ") && logged(": CHIP_ID 0xd1, PMU_STATUS 0x00"),
	    "reset once and waited for: %s", log_text);
	CHECK(device.reg[BMI_PMU_STATUS] == 0x14, "PMU_STATUS: both halves normal, 0x14: %#x", device.reg[BMI_PMU_STATUS]);
	CHECK(register_written[BMI_ACC_CONF] && device.reg[BMI_ACC_CONF] == 0x28, "ACC_CONF: 100 Hz (8), the normal filter (2 << 4), "
	    "0x28: %#x", device.reg[BMI_ACC_CONF]);
	CHECK(register_written[BMI_ACC_RANGE] && device.reg[BMI_ACC_RANGE] == 0x05, "ACC_RANGE: +-4 g, 0x05: %#x",
	    device.reg[BMI_ACC_RANGE]);
	CHECK(register_written[BMI_GYR_CONF] && device.reg[BMI_GYR_CONF] == 0x28, "GYR_CONF: 100 Hz, 0x28: %#x", device.reg[BMI_GYR_CONF]);
	CHECK(register_written[BMI_GYR_RANGE] && device.reg[BMI_GYR_RANGE] == 0x00, "GYR_RANGE: +-2000 dps, 0: %#x",
	    device.reg[BMI_GYR_RANGE]);
	CHECK(logged("imu: 0x7e <- 0x11, PMU_STATUS reads 0x10: the accelerometer measures") &&
	    logged("imu: 0x7e <- 0x15, PMU_STATUS reads 0x14: the gyroscope measures"), "each command held to PMU_STATUS: %s", log_text);
	CHECK(logged("imu: 0x40 <- 0x28, reads 0x28") && logged("imu: 0x41 <- 0x05, reads 0x05") && logged("imu: 0x42 <- 0x28, reads 0x28") &&
	    logged("imu: 0x43 <- 0x00, reads 0x00"), "each register read back: %s", log_text);
	CHECK(logged("its accelerometer switched on: 100 Hz, +-4 g; its gyroscope too, for Darwin's motion stack: 100 Hz, +-2000 dps"),
	    "the rate and the board's range said: %s", log_text);
	CHECK(bmi160_written_as_it_wants(), "written as the chip wants it");
	CHECK(device.driven_high == 0 && model.elsewhere == 0, "the bus was used as a bus");
	hakios_imu_quiesce();
}

/*
 * The samples through the board's mounting ("-y", "x", "-z", as the LSM6DSO's scenario has it), at +-4 g, 8192 counts
 * a g: upright the chip reads -1 g along its x, Darwin's y -1000 milli-g, portrait; and so on.
 */
static void
bmi160_orientation_through_the_mounting(void)
{
	bmi160_following("sensors=2");
	WAIT_FOR(get("orientation") == 1, 3000);
	CHECK(get("orientation") == 1, "portrait (1): %u", get("orientation"));
	CHECK(strcmp(get_string("accel"), "0 -1000 0") == 0, "hw.sensors.accel in milli-g on Darwin's axes: %s", get_string("accel"));
	CHECK(logged("imu: portrait (0 -1000 0 milli-g; the chip's -8192 0 0)"), "said: %s", log_text);
	chip_reads(0, 8192, 0);
	/* (the accelerometer's three too: a read may fall between two of the words the scenario sets) */
	WAIT_FOR(get("orientation") == 3 && strcmp(get_string("accel"), "-1000 0 0") == 0, 5000);
	CHECK(get("orientation") == 3, "landscape left (3): %u", get("orientation"));
	CHECK(strcmp(get_string("accel"), "-1000 0 0") == 0, "%s", get_string("accel"));
	chip_reads(0, 0, 8192);
	WAIT_FOR(get("orientation") == 5 && strcmp(get_string("accel"), "0 0 -1000") == 0, 5000);
	CHECK(get("orientation") == 5, "face up (5): %u", get("orientation"));
	CHECK(strcmp(get_string("accel"), "0 0 -1000") == 0, "%s", get_string("accel"));
	chip_reads(4096, -4096, 3);
	WAIT_FOR(strcmp(get_string("accel"), "500 500 0") == 0, 2000);
	CHECK(strcmp(get_string("accel"), "500 500 0") == 0, "half a g each, and three counts that round to nothing: %s",
	    get_string("accel"));
	chip_reads(-32768, 32767, 100);
	WAIT_FOR(strcmp(get_string("accel"), "-4000 -4000 -12") == 0, 2000);
	CHECK(strcmp(get_string("accel"), "-4000 -4000 -12") == 0, "all the range (Darwin's x the chip's -y, its y the chip's x), and 100 "
	    "counts are 12.2 milli-g: %s", get_string("accel"));
	CHECK(bmi160_commands_were("reading", 3, 0xb6, 0x11, 0x15) && bmi160_written_as_it_wants(), "reading wrote nothing");
	hakios_imu_quiesce();
}

/* a board at +-16 g: ACC_RANGE 0x0c, 2048 counts a g */
static void
bmi160_the_board_s_range(void)
{
	bmi160_phone(0xd1, 16);
	chip_reads(-2048, 0, 0);
	boot_args = "sensors=2";
	hakios_imu_init();
	WAIT_FOR(get("orientation") == 1, 10000);
	CHECK(device.reg[BMI_ACC_RANGE] == 0x0c && logged("imu: 0x41 <- 0x0c, reads 0x0c") && logged("100 Hz, +-16 g"),
	    "ACC_RANGE: +-16 g, 0x0c: %#x; %s", device.reg[BMI_ACC_RANGE], log_text);
	CHECK(get("orientation") == 1 && strcmp(get_string("accel"), "0 -1000 0") == 0, "2048 counts are a g: %s", get_string("accel"));
	chip_reads(0, 1024, -3072);
	WAIT_FOR(strcmp(get_string("accel"), "-500 0 1500") == 0, 3000);
	CHECK(strcmp(get_string("accel"), "-500 0 1500") == 0, "%s", get_string("accel"));
	hakios_imu_quiesce();
}

/* a range the chip has not (the board's row by hand: tools/boards.py refuses the file): disabled, nothing touched */
static void
bmi160_no_such_range(void)
{
	bmi160_phone(0xd1, 3);
	boot_args = "sensors=2";
	hakios_imu_init();
	usleep(50000);
	CHECK(logged("imu: disabled: the board file does not say which pins the chip is on, its address, its range"), "said: %s", log_text);
	CHECK(model.writes == 0 && model.reads == 0, "nothing touched");
	hakios_imu_quiesce();
	CHECK(model.writes == 0, "nor at the run's end");
}

/*
 * The gyroscope's rates: from its fifth read, in the sample of the accelerometer's milli-g, through the board's
 * mounting with the signs turned back (Darwin's x the chip's +y, its y the chip's -x, its z the chip's +z) and into
 * milli-rad/s at 16.4 counts a dps: 16400 counts are 1000 dps, 17.453 rad/s; 1000 counts 60.976 dps, 1.0642 rad/s.
 */
static void
bmi160_gyro_rates_through_the_mounting(void)
{
	int mg[3], rate[3];

	bmi160_following("sensors=2");
	WAIT_FOR(get("gyro") == 1, 10000);
	CHECK(get("gyro") == 1, "the rates not given: %s", log_text);
	CHECK(logged("imu: the gyroscope answers at its read 5 ("), "given from another read than its fifth: %s", log_text);
	gyro_wobbles = false;
	chip_rates(1000, 0, 0);
	CHECK(rates_said(0, -1064, 0), "the chip's x is Darwin's -y");
	CHECK(sample_says(mg, rate) == 8 && mg[0] == 0 && mg[1] == -1000 && mg[2] == 0, "with the accelerometer's portrait: %s",
	    get_string("sample"));
	chip_rates(0, 16400, 0);
	CHECK(rates_said(17453, 0, 0), "the chip's y is Darwin's x: 1000 dps, 17.453 rad/s");
	chip_rates(0, 0, -32768);
	CHECK(rates_said(0, 0, -34873), "the chip's z is Darwin's z, all the range: -1998.06 dps, -34.8728 rad/s");
	chip_rates(-1, 1, 1);
	CHECK(rates_said(1, 1, 1), "a count is 1.06 milli-rad/s, rounded to the nearest");
	chip_rates(2000, -2000, 3);
	CHECK(rates_said(-2128, -2128, 3), "2000 counts are 2.1285 rad/s, three 3.19 milli-rad/s");
	CHECK(bmi160_commands_were("reading", 3, 0xb6, 0x11, 0x15) && bmi160_written_as_it_wants(), "reading wrote nothing");
	hakios_imu_quiesce();
	CHECK(get("gyro") == 0, "given after the run's end: %u", get("gyro"));
	CHECK(device.reg[BMI_PMU_STATUS] == 0, "not powered down at the run's end: PMU_STATUS %#x", device.reg[BMI_PMU_STATUS]);
}

/*
 * What keeps the gyroscope off: its command never given, its registers never written, PMU_STATUS says it suspended
 * all along, no sample has rates -- and the run's end has only the accelerometer to power down.
 */
static void
bmi160_gyro_kept_off(const char *args, const char *said)
{
	int mg[3], rate[3];

	bmi160_following(args);
	WAIT_FOR(get("samples") >= 12, 10000);
	CHECK(get("samples") >= 12 && get("orientation") == 1, "followed: %u samples", get("samples"));
	CHECK(bmi160_commands_were("the gyroscope kept off", 2, 0xb6, 0x11), "reset and the accelerometer on, no more");
	CHECK(!register_written[BMI_GYR_CONF] && !register_written[BMI_GYR_RANGE] && device.reg[BMI_PMU_STATUS] == 0x10,
	    "the gyroscope switched on or written to: PMU_STATUS %#x", device.reg[BMI_PMU_STATUS]);
	CHECK(get("gyro") == 0 && sample_says(mg, rate) == 5, "rates given: hw.sensors.gyro %u, %s", get("gyro"), get_string("sample"));
	CHECK(!logged("imu: the gyroscope answers") && !logged("its gyroscope too"), "said: %s", log_text);
	CHECK(said == NULL || logged(said), "not said \"%s\": %s", said, log_text);
	hakios_imu_quiesce();
	CHECK(bmi160_commands_were("the run's end", 3, 0xb6, 0x11, 0x10) && !logged("the gyroscope is powered down"),
	    "the accelerometer suspended, the gyroscope not written to: %s", log_text);
	CHECK(device.reg[BMI_PMU_STATUS] == 0, "PMU_STATUS %#x", device.reg[BMI_PMU_STATUS]);
	CHECK(bmi160_written_as_it_wants(), "written as the chip wants it");
}

static void
bmi160_gyro_hid_off(void)
{
	bmi160_gyro_kept_off("sensors=2 gyro_hid=0", "without the gyroscope's rates (gyro_hid=0)");
	CHECK(logged("and its motion stack the samples (hw.sensors.motion 1)"), "the samples not given: %s", log_text);
}

static void
bmi160_gyro_not_without_motion(void)
{
	bmi160_gyro_kept_off("sensors=2 motion_hid=0", "not its motion stack the samples (motion_hid=0)");
}

static void
bmi160_gyro_not_without_darwin(void)
{
	bmi160_gyro_kept_off("sensors=2 orientation_hid=0", "Darwin is not given it (orientation_hid=0)");
}

/*
 * The run's end: the reading stops; the gyroscope's suspend command, then, once PMU_STATUS says it suspended, the
 * accelerometer's; both said; the pins as they were found.
 */
static void
bmi160_teardown(void)
{
	unsigned taken;

	bmi160_following("sensors=2");
	WAIT_FOR(get("gyro") == 1, 10000);
	CHECK(get("hid") == 1 && get("motion") == 1 && get("gyro") == 1, "given while the run goes on: %s", log_text);
	CHECK(device.reg[BMI_PMU_STATUS] == 0x14, "on while the run goes on: %#x", device.reg[BMI_PMU_STATUS]);
	hakios_imu_quiesce();
	CHECK(get("hid") == 0 && get("motion") == 0 && get("gyro") == 0, "given after the run's end");
	CHECK(bmi160_commands_were("the run's end", 5, 0xb6, 0x11, 0x15, 0x14, 0x10), "the gyroscope suspended, then the accelerometer");
	CHECK(device.reg[BMI_PMU_STATUS] == 0, "PMU_STATUS 0, both suspended, at the run's end: %#x", device.reg[BMI_PMU_STATUS]);
	CHECK(logged("imu: at the run's end: the gyroscope is powered down (CMD 0x14, PMU_STATUS: suspended)") &&
	    logged("the accelerometer is powered down (CMD 0x10, PMU_STATUS: suspended)"), "said: %s", log_text);
	CHECK(bmi160_written_as_it_wants(), "no command dropped, nothing else written");
	pins_as_found();
	usleep(300000);
	taken = output_reads;
	usleep(300000);
	CHECK(output_reads == taken, "no sample after the run's end: %u then %u", taken, output_reads);
	CHECK(logged("imu: done"), "the thread has ended: %s", log_text);
}

/*
 * The run ends while the gyroscope is still starting (its command given, not yet carried out, PMU_STATUS still saying
 * it suspended): the thread lets go of the bus and writes nothing more, the run's end lets the chip finish the start
 * -- a suspend command now would be dropped, and PMU_STATUS would seem to say it was carried out -- and then
 * suspends both halves. (The model's start is held until the scenario has seen the command, then takes forty reads
 * more; the driver's waits as long as it asks for, so that it is still waiting when the scenario looks.)
 */
static void
bmi160_end_while_the_gyroscope_starts(void)
{
	bmi160_phone(0xd1, 4);
	fake_delay_divisor = 1;
	bmi_gyro_start_reads = 40;
	bmi_start_held = true;
	boot_args = "sensors=2";
	hakios_imu_init();
	for (int waited = 0; waited < 50000 && !(bmi_command_count >= 3 && bmi_busy != 0); waited++) {
		usleep(200);
	}
	CHECK(bmi160_commands_were("the gyroscope starting", 3, 0xb6, 0x11, 0x15) && bmi_busy != 0, "the gyroscope's command in hand");
	CHECK(!logged("NOT CARRIED OUT"), "the scenario looked too late: the driver's wait for the gyroscope was over: %s", log_text);
	__atomic_store_n(&leaving, true, __ATOMIC_SEQ_CST);     /* (as the run's end does first: the start goes on from here) */
	bmi_start_held = false;
	hakios_imu_quiesce();
	CHECK(logged("imu: the run ends: NOTHING MORE IS DONE") && !logged("the gyroscope measures") && !register_written[BMI_GYR_CONF] &&
	    !register_written[BMI_GYR_RANGE], "the thread went on with the chip after the run's end began: %s", log_text);
	CHECK(bmi_dropped == 0, "%u commands came while the gyroscope was starting, and were dropped", bmi_dropped);
	CHECK(device.reg[BMI_PMU_STATUS] == 0 && bmi_busy == 0, "both suspended all the same: PMU_STATUS %#x", device.reg[BMI_PMU_STATUS]);
	CHECK(logged("the gyroscope is powered down (CMD 0x14") && logged("the accelerometer is powered down (CMD 0x10"), "said: %s",
	    log_text);
	CHECK(bmi_commands[bmi_command_count - 1] == 0x10 && bmi_commands[bmi_command_count - 2] == 0x14, "the gyroscope, then the "
	    "accelerometer: 0x%02x 0x%02x last", bmi_commands[bmi_command_count - 2], bmi_commands[bmi_command_count - 1]);
	CHECK(get("hid") == 0 && get("samples") == 0 && output_reads == 0, "nothing was given or read: hid %u, %u samples", get("hid"),
	    get("samples"));
	pins_as_found();
	WAIT_FOR(logged("imu: done"), 3000);
	CHECK(logged("imu: done") && device.reg[BMI_PMU_STATUS] == 0, "the thread ended and left the chip suspended: %s", log_text);
}

/* the chip drops the gyroscope's suspend command at the run's end (once): said again, both suspended */
static void
bmi160_suspend_said_again(void)
{
	bmi160_following("sensors=2");
	bmi_drop_next = true;
	hakios_imu_quiesce();
	CHECK(bmi_dropped == 1 && bmi160_commands_were("the run's end", 6, 0xb6, 0x11, 0x15, 0x14, 0x14, 0x10), "said again, once");
	CHECK(device.reg[BMI_PMU_STATUS] == 0 && logged("the gyroscope is powered down (CMD 0x14") &&
	    logged("the accelerometer is powered down (CMD 0x10"), "both suspended: PMU_STATUS %#x, %s", device.reg[BMI_PMU_STATUS], log_text);
}

/* a gyroscope that will not be suspended: said so, not claimed; the accelerometer suspended all the same */
static void
bmi160_gyroscope_does_not_stop(void)
{
	bmi160_following("sensors=2");
	bmi_deaf_command = 0x14;
	hakios_imu_quiesce();
	CHECK(logged("imu: at the run's end: the gyroscope COULD NOT BE POWERED DOWN") &&
	    logged("the accelerometer is powered down (CMD 0x10"), "said: %s", log_text);
	CHECK(device.reg[BMI_PMU_STATUS] == 0x04, "PMU_STATUS %#x", device.reg[BMI_PMU_STATUS]);
}

/* the chip stops answering: the orientation is not known (0), nothing is given any more; the run's end powers it down when it is back */
static void
bmi160_lost_is_not_known(void)
{
	unsigned taken;

	bmi160_following("sensors=2");
	WAIT_FOR(get("orientation") == 1 && get("gyro") == 1, 10000);
	CHECK(get("orientation") == 1 && get("hid") == 1 && get("gyro") == 1, "portrait (1) first, and given: %u", get("orientation"));
	device.absent = true;
	WAIT_FOR(logged("imu: done"), 5000);
	CHECK(logged("a byte was not acknowledged: NOTHING MORE IS DONE"), "said: %s", log_text);
	CHECK(get("orientation") == 0, "not known (0) once nothing more is read: %u", get("orientation"));
	CHECK(get("hid") == 0 && get("motion") == 0 && get("gyro") == 0, "given after the chip was lost: hid %u, motion %u, gyro %u",
	    get("hid"), get("motion"), get("gyro"));
	taken = output_reads;
	usleep(200000);
	CHECK(output_reads == taken, "no more reads");
	device.absent = false;
	hakios_imu_quiesce();
	CHECK(device.reg[BMI_PMU_STATUS] == 0 && logged("the accelerometer is powered down (CMD 0x10"), "powered down at the run's end: "
	    "PMU_STATUS %#x, %s", device.reg[BMI_PMU_STATUS], log_text);
	pins_as_found();
}

/* ... and when it is still not there at the run's end: said, not claimed */
static void
bmi160_lost_for_good(void)
{
	bmi160_following("sensors=2");
	device.absent = true;
	WAIT_FOR(logged("imu: done"), 5000);
	hakios_imu_quiesce();
	CHECK(logged("the gyroscope COULD NOT BE POWERED DOWN") && logged("the accelerometer COULD NOT BE POWERED DOWN") &&
	    !logged("is powered down"), "said: %s", log_text);
	pins_as_found();
}

/* a reset the chip does not come back from: stopped, nothing switched on, nothing given, nothing to power down */
static void
bmi160_reset_does_not_end(void)
{
	bmi160_phone(0xd1, 4);
	reset_never_ends = true;
	boot_args = "sensors=2";
	hakios_imu_init();
	WAIT_FOR(logged("imu: done"), 10000);
	CHECK(logged("the chip's reset does not end: NOTHING MORE IS DONE"), "said: %s", log_text);
	CHECK(bmi160_commands_were("the reset", 1, 0xb6) && written_after_reset == 0, "nothing written after the reset");
	bmi160_nothing_given("the reset not ended");
	hakios_imu_quiesce();
	CHECK(bmi_command_count == 1, "and nothing to switch off at the run's end");
}

/* a chip slow to come back from its reset (twenty transfers unanswered): waited for */
static void
bmi160_reset_takes_its_time(void)
{
	bmi_reset_transfers = 20;
	bmi160_following("sensors=2");
	CHECK(resets == 1 && device.reg[BMI_PMU_STATUS] == 0x14, "configured after it: PMU_STATUS %#x", device.reg[BMI_PMU_STATUS]);
	hakios_imu_quiesce();
}

/* the accelerometer's command acknowledged and not carried out: stopped, no register written, nothing given */
static void
bmi160_accelerometer_does_not_start(void)
{
	bmi160_phone(0xd1, 4);
	bmi_deaf_command = 0x11;
	boot_args = "sensors=2";
	hakios_imu_init();
	WAIT_FOR(logged("imu: done"), 10000);
	CHECK(logged("imu: 0x7e <- 0x11, PMU_STATUS reads 0x00: NOT CARRIED OUT in 20 ms") &&
	    logged("a command was not carried out: NOTHING MORE IS DONE"), "said: %s", log_text);
	CHECK(bmi160_commands_were("the accelerometer deaf", 2, 0xb6, 0x11) && !register_written[BMI_ACC_CONF] &&
	    !register_written[BMI_ACC_RANGE], "nothing written after it");
	bmi160_nothing_given("the accelerometer not started");
	hakios_imu_quiesce();
	CHECK(device.reg[BMI_PMU_STATUS] == 0, "suspended at the run's end: %#x", device.reg[BMI_PMU_STATUS]);
}

/* ... and the gyroscope's: stopped too, as for an LSM6DSO whose CTRL2_G does not read back; both suspended at the run's end */
static void
bmi160_gyroscope_does_not_start(void)
{
	bmi160_phone(0xd1, 4);
	bmi_deaf_command = 0x15;
	boot_args = "sensors=2";
	hakios_imu_init();
	WAIT_FOR(logged("imu: done"), 10000);
	CHECK(logged("imu: 0x7e <- 0x15, PMU_STATUS reads 0x10: NOT CARRIED OUT in 150 ms") &&
	    logged("a command was not carried out: NOTHING MORE IS DONE"), "said: %s", log_text);
	CHECK(!register_written[BMI_GYR_CONF] && !register_written[BMI_GYR_RANGE], "nothing written after it");
	bmi160_nothing_given("the gyroscope not started");
	hakios_imu_quiesce();
	CHECK(device.reg[BMI_PMU_STATUS] == 0 && logged("the accelerometer is powered down (CMD 0x10"), "suspended at the run's end: "
	    "%#x, %s", device.reg[BMI_PMU_STATUS], log_text);
}

/* a register that does not take what is written: stopped, nothing given; the accelerometer suspended at the run's end */
static void
bmi160_write_does_not_stick(void)
{
	bmi160_phone(0xd1, 4);
	deaf_register = BMI_ACC_RANGE;
	boot_args = "sensors=2";
	hakios_imu_init();
	WAIT_FOR(logged("imu: done"), 10000);
	CHECK(logged("imu: 0x41 <- 0x05, reads 0x03: NOT AS WRITTEN") && logged("did not read back as written: NOTHING MORE IS DONE"),
	    "said: %s", log_text);
	CHECK(bmi160_commands_were("a deaf register", 2, 0xb6, 0x11), "the gyroscope not switched on after it");
	bmi160_nothing_given("a register not as written");
	hakios_imu_quiesce();
	CHECK(bmi160_commands_were("the run's end", 3, 0xb6, 0x11, 0x10) && device.reg[BMI_PMU_STATUS] == 0, "the accelerometer "
	    "suspended at the run's end: %#x", device.reg[BMI_PMU_STATUS]);
	pins_as_found();
}

/* a gyroscope that answered and then reads the very same for a hundred reads: held stuck, its rates taken back, it alone suspended */
static void
bmi160_gyro_stuck_taken_back(void)
{
	int mg[3], rate[3];
	uint32_t before;

	bmi160_following("sensors=2");
	WAIT_FOR(get("gyro") == 1, 10000);
	CHECK(get("gyro") == 1, "the rates not given: %s", log_text);
	gyro_wobbles = false;
	WAIT_FOR(logged("stuck"), 15000);
	CHECK(logged("times in a row: stuck, its rates NOT GIVEN ANY MORE; it is powered down (CMD 0x14, PMU_STATUS: suspended)"),
	    "said: %s", log_text);
	CHECK(get("gyro") == 0 && device.reg[BMI_PMU_STATUS] == 0x10, "still given, or not it alone suspended: gyro %u, PMU_STATUS %#x",
	    get("gyro"), device.reg[BMI_PMU_STATUS]);
	before = get("samples");
	WAIT_FOR(get("samples") >= before + 2, 3000);
	CHECK(sample_says(mg, rate) == 5 && get("samples") >= before + 2 && get("motion") == 1, "the samples go on without rates: %s",
	    get_string("sample"));
	hakios_imu_quiesce();
	CHECK(device.reg[BMI_PMU_STATUS] == 0 && bmi_dropped == 0, "PMU_STATUS %#x at the run's end", device.reg[BMI_PMU_STATUS]);
}

/* a board that lists both chips (its row by hand: tools/boards.py refuses the file): said, nothing touched */
static void
bmi160_and_lsm6dso_listed(void)
{
	static struct hakios_board both;

	phone("phone-video", 0x6c);
	both = *hakios_board();
	both.features |= HAKIOS_HAS_IMU_BMI160;
	found = &both;
	boot_args = "sensors=2";
	hakios_imu_init();
	usleep(50000);
	CHECK(logged("imu: disabled: this board's board.dts lists both \"imu-lsm6dso\" and \"imu-bmi160\": a board has one motion sensor"),
	    "said: %s", log_text);
	CHECK(model.writes == 0 && model.reads == 0 && model.elsewhere == 0, "nothing touched");
	hakios_imu_quiesce();
	CHECK(model.writes == 0, "nor at the run's end");
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
	CHECK(logged("imu: disabled: this board's board.dts lists no \"imu-lsm6dso\" nor \"imu-bmi160\""), "said: %s", log_text);
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
		{ "lost_is_not_known", lost_is_not_known }, { "zeros_before_the_first_sample", zeros_before_the_first_sample },
		{ "given_to_darwin", given_to_darwin }, { "given_until_the_end", given_until_the_end },
		{ "kept_from_darwin", kept_from_darwin }, { "motion_given_to_darwin", motion_given_to_darwin },
		{ "motion_kept_from_darwin", motion_kept_from_darwin },
		{ "motion_not_without_the_orientation", motion_not_without_the_orientation }, { "timed_sample", timed_sample },
		{ "write_does_not_stick", write_does_not_stick }, { "reset_does_not_end", reset_does_not_end },
		{ "level_past_the_last", level_past_the_last }, { "not_listed", not_listed },
		{ "stopped_in_a_read_at_a_look", stopped_in_a_read_at_a_look },
		{ "stopped_in_a_read_before_switched_on", stopped_in_a_read_before_switched_on },
		{ "stopped_taking_the_pins_first", stopped_taking_the_pins_first }, { "stopped_taking_the_pins", stopped_taking_the_pins },
		{ "gyro_configured", gyro_configured }, { "gyro_rates_through_the_mounting", gyro_rates_through_the_mounting },
		{ "gyro_hid_off", gyro_hid_off }, { "gyro_not_without_motion", gyro_not_without_motion },
		{ "gyro_not_without_darwin", gyro_not_without_darwin }, { "gyro_board_mount_refused", gyro_board_mount_refused },
		{ "gyro_zeros_not_given", gyro_zeros_not_given }, { "gyro_stuck_taken_back", gyro_stuck_taken_back },
		{ "gyro_lost_with_the_chip", gyro_lost_with_the_chip },
		{ "bmi160_look_only", bmi160_look_only }, { "bmi160_another_chip_refused", bmi160_another_chip_refused },
		{ "bmi160_nobody_there", bmi160_nobody_there }, { "bmi160_under_the_other_feature", bmi160_under_the_other_feature },
		{ "bmi160_configured", bmi160_configured },
		{ "bmi160_orientation_through_the_mounting", bmi160_orientation_through_the_mounting },
		{ "bmi160_the_board_s_range", bmi160_the_board_s_range }, { "bmi160_no_such_range", bmi160_no_such_range },
		{ "bmi160_gyro_rates_through_the_mounting", bmi160_gyro_rates_through_the_mounting },
		{ "bmi160_gyro_hid_off", bmi160_gyro_hid_off }, { "bmi160_gyro_not_without_motion", bmi160_gyro_not_without_motion },
		{ "bmi160_gyro_not_without_darwin", bmi160_gyro_not_without_darwin }, { "bmi160_teardown", bmi160_teardown },
		{ "bmi160_end_while_the_gyroscope_starts", bmi160_end_while_the_gyroscope_starts },
		{ "bmi160_suspend_said_again", bmi160_suspend_said_again },
		{ "bmi160_gyroscope_does_not_stop", bmi160_gyroscope_does_not_stop },
		{ "bmi160_lost_is_not_known", bmi160_lost_is_not_known }, { "bmi160_lost_for_good", bmi160_lost_for_good },
		{ "bmi160_reset_does_not_end", bmi160_reset_does_not_end }, { "bmi160_reset_takes_its_time", bmi160_reset_takes_its_time },
		{ "bmi160_accelerometer_does_not_start", bmi160_accelerometer_does_not_start },
		{ "bmi160_gyroscope_does_not_start", bmi160_gyroscope_does_not_start },
		{ "bmi160_write_does_not_stick", bmi160_write_does_not_stick },
		{ "bmi160_gyro_stuck_taken_back", bmi160_gyro_stuck_taken_back },
		{ "bmi160_and_lsm6dso_listed", bmi160_and_lsm6dso_listed },
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
			fprintf(stderr, "%-40s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
