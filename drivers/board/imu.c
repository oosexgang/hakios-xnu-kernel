/*
 * The motion sensor: an ST LSM6DSO (or LSM6DSOX) accelerometer and gyroscope on I2C, read here for the
 * phone's orientation -- which way up it is held, what Darwin's auto-rotate reads. Which pins the chip is
 * on, its address, the range it measures and how it is turned on the board are the board's (imu { },
 * board/board.h); the chip's registers are board/lsm6dso_core.h's, the turn onto Darwin's axes and the
 * units board/sensor_axes_core.h's, the orientation board/orientation_core.h's, and the three together
 * board/sensors_core.h's. This file is the bus, the levels and the thread.
 *
 *   I2C        done by hand (board/i2c_gpio.h) on the board's two pins, taken for each transaction and given
 *              back as they were found
 *   interrupt  the chip's INT1, only looked at: the output is polled
 *
 * On most Snapdragon phones the chip is the sensor DSP's, on a bus the application processor does not
 * drive; a board lists "imu-lsm6dso" only where it is on the SoC's own pins.
 *
 * Nothing here is done unless a boot-arg asks for it:
 *
 *   sensors=1  a look, reading only: the pins, and the chip's identity (WHO_AM_I, against the LSM6DSO's
 *              0x6c). Nothing is written to the chip.
 *   sensors=2  ... and, the identity being the LSM6DSO's, the chip reset (CTRL3_C), its output kept whole
 *              while it is read and the register address counting up across a read (BDU, IF_INC), and
 *              the accelerometer switched on at 104 Hz and the board's range (CTRL1_XL; the gyroscope
 *              stays off), each write read back. Then its output read about every 100 ms, turned onto
 *              Darwin's axes and into milli-g, each change of orientation said, and the latest in
 *              hw.sensors.orientation (a UIDeviceOrientation: 1 portrait ... 6 face down, 0 not known yet)
 *              and hw.sensors.accel (milli-g, "x y z" on Darwin's axes).
 *
 * Limits, whatever asks: another chip's identity, a byte nobody acknowledges, a write that does not read
 * back or a bus that is not at rest, and nothing more is done for the rest of the run, with a line that
 * says why. At the run's end (and on a panic's way out) the reading stops and the accelerometer is
 * powered down (CTRL1_XL 0), if it was switched on, and the pins are as they were found. Nothing is
 * written anywhere but to the chip's two control registers and the bus's two pins.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include "kernel.h"
#include "board.h"
#include "log.h"
#include "lsm6dso_core.h"
#include "sensors_core.h"

static vm_offset_t tlmm;
static uint32_t level;
static bool switched_on;                /* the accelerometer was (or may have been) switched on: the run's end switches it off */
static bool leaving;                    /* the run's end has the chip (hakios_imu_quiesce()) */
static bool busy;                       /* the thread is using the bus: the run's end waits for it */
static bool on_bus;                     /* ... between taking the pins and giving them back */

/* every line to the loader's log as well (and to the console when that may be written) */
HAKIOS_DEFINE_SAY(!__atomic_load_n(&leaving, __ATOMIC_SEQ_CST) && ml_get_interrupts_enabled())

#include "mmio.h"
#include "time.h"

#define wait_ms(ms)             hakios_wait_ms((ms), __atomic_load_n(&leaving, __ATOMIC_SEQ_CST))   /* (spun on the way out) */

#define POLL_MS                 100     /* how often the output is read */
#define ODR_104_HZ              4       /* CTRL1_XL's output data rate code for 104 Hz (lsm6dso_core.h: lsm6dso_ctrl1_xl()) */

/* ---- pins ------------------------------------------------------------------------------------------ */

#define TLMM_PHYS               (hakios_board()->pins.tlmm)     /* the SoC file's pins { } */
#define HAKIOS_TLMM             tlmm
#include "tlmm.h"                    /* a pin's two registers */

/* which pins, and where on the bus: the board's imu { } */
#define PIN_SDA                 (hakios_board()->imu.sda)
#define PIN_SCL                 (hakios_board()->imu.scl)
#define PIN_INT                 (hakios_board()->imu.interrupt)
#define ADDRESS                 (hakios_board()->imu.address)

static void
show_pins(void)
{
	const struct {
		unsigned pin;
		const char *what;
	} pins[] = { { PIN_SDA, "I2C data" }, { PIN_SCL, "I2C clock" }, { PIN_INT, "interrupt" } };

	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg = rd(GPIO_CFG(pins[i].pin)), io = rd(GPIO_IO(pins[i].pin));

		say("imu: gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u\n", pins[i].pin, pins[i].what, (cfg >> 2) & 15,
		    cfg & 3, (((cfg >> 6) & 7) + 1) * 2, (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") : "input", io & 1);
	}
}

/* ---- I2C by hand (as in board/haptic.c) ---------------------------------------------------------- */

#include "i2c_gpio.h"                /* the two lines, a bit, a byte, the bus taken and given back */

static struct i2c_gpio i2c_bus;          /* (its two pins: the board's, at the start) */
static const char not_at_rest[] = "the I2C bus is not at rest", not_acknowledged[] = "a byte was not acknowledged";
static const char *failed;              /* why the last transfer did not go through: one of the two */

static bool
bus_take(void)
{
	if (i2c_gpio_take(&i2c_bus)) {
		on_bus = true;
		return true;
	}
	say("imu: the I2C bus is not at rest (gpio%u reads %u, gpio%u reads %u): not touched\n", PIN_SDA,
	    i2c_gpio_level(PIN_SDA), PIN_SCL, i2c_gpio_level(PIN_SCL));
	failed = not_at_rest;
	return false;
}

static void
bus_give_back(void)
{
	i2c_gpio_give_back(&i2c_bus);
	on_bus = false;
}

/* `count' registers from `reg' on (the chip counts the address up: IF_INC, on from its reset) */
static bool
regs_read(uint8_t reg, uint8_t *values, unsigned count)
{
	bool answered;

	if (!bus_take()) {
		return false;
	}
	i2c_gpio_start(&i2c_bus);
	answered = i2c_gpio_write(&i2c_bus, (uint8_t)(ADDRESS << 1)) && i2c_gpio_write(&i2c_bus, reg);
	if (answered) {
		i2c_gpio_start(&i2c_bus);
		answered = i2c_gpio_write(&i2c_bus, (uint8_t)(ADDRESS << 1 | 1));
	}
	for (unsigned i = 0; answered && i < count; i++) {
		values[i] = i2c_gpio_read(&i2c_bus, i + 1 < count);
	}
	i2c_gpio_stop(&i2c_bus);
	bus_give_back();
	if (!answered) {
		failed = not_acknowledged;
	}
	return answered;
}

static bool
reg_write(uint8_t reg, uint8_t value)
{
	bool answered;

	if (!bus_take()) {
		return false;
	}
	i2c_gpio_start(&i2c_bus);
	answered = i2c_gpio_write(&i2c_bus, (uint8_t)(ADDRESS << 1)) && i2c_gpio_write(&i2c_bus, reg) &&
	    i2c_gpio_write(&i2c_bus, value);
	i2c_gpio_stop(&i2c_bus);
	bus_give_back();
	if (!answered) {
		failed = not_acknowledged;
	}
	return answered;
}

/* ---- the chip -------------------------------------------------------------------------------------- */

static void
give_up(const char *why)
{
	say("imu: %s: NOTHING MORE IS DONE with the chip in this run\n", why);
}

/* One register written and read back. */
static bool
put(uint8_t reg, uint8_t value)
{
	uint8_t after = 0;

	if (!reg_write(reg, value)) {
		say("imu: 0x%02x <- 0x%02x NOT WRITTEN\n", reg, value);
		give_up(failed);
		return false;
	}
	if (!regs_read(reg, &after, 1)) {
		give_up(failed);
		return false;
	}
	say("imu: 0x%02x <- 0x%02x, reads 0x%02x%s\n", reg, value, after, after == value ? "" : ": NOT AS WRITTEN");
	if (after != value) {
		give_up("a register did not read back as written");
		return false;
	}
	return true;
}

/* Is the chip there, and is it an LSM6DSO? */
static bool
identified(void)
{
	uint8_t id = 0;

	if (!regs_read(LSM6DSO_REG_WHO_AM_I, &id, 1)) {
		if (failed == not_acknowledged) {
			say("imu: nobody answers at %#x on the bus of gpio%u/%u\n", ADDRESS, PIN_SDA, PIN_SCL);
		}
		give_up(failed);
		return false;
	}
	say("imu: the chip at %#x answers WHO_AM_I 0x%02x%s\n", ADDRESS, id, id == LSM6DSO_WHO_AM_I ? ", an LSM6DSO (0x6c)" :
	    ": NOT THE LSM6DSO'S 0x6c");
	if (id != LSM6DSO_WHO_AM_I) {
		give_up("another chip");
		return false;
	}
	return true;
}

static uint8_t full_scale;              /* the board's range as CTRL1_XL has it (LSM6DSO_FS_*) */

/* reset, whole samples read in one go, then the accelerometer on at 104 Hz and the board's range */
static bool
configure(void)
{
	uint8_t control = LSM6DSO_CTRL3_SW_RESET;

	say("imu: the chip is reset and its accelerometer switched on: 104 Hz, +-%u g\n", hakios_board()->imu.full_scale);
	if (!reg_write(LSM6DSO_REG_CTRL3_C, LSM6DSO_CTRL3_SW_RESET)) {
		say("imu: 0x%02x <- 0x%02x NOT WRITTEN\n", LSM6DSO_REG_CTRL3_C, LSM6DSO_CTRL3_SW_RESET);
		give_up(failed);
		return false;
	}
	/* (the chip clears the bit when its reset is done: read until then, 10 ms at most) */
	for (unsigned waited = 0; control & LSM6DSO_CTRL3_SW_RESET; waited++) {
		wait_ms(1);
		if (!regs_read(LSM6DSO_REG_CTRL3_C, &control, 1)) {
			give_up(failed);
			return false;
		}
		if (waited == 10 && (control & LSM6DSO_CTRL3_SW_RESET)) {
			give_up("the chip's reset does not end");
			return false;
		}
	}
	if (!put(LSM6DSO_REG_CTRL3_C, LSM6DSO_CTRL3_BDU | LSM6DSO_CTRL3_IF_INC)) {
		return false;
	}
	switched_on = true;             /* (from the write on, whether it went through or not: the run's end switches it off) */
	return put(LSM6DSO_REG_CTRL1_XL, lsm6dso_ctrl1_xl(ODR_104_HZ, full_scale));
}

/* ---- the orientation, for Darwin: hw.sensors ----------------------------------------------------- */

static struct sensors_accel accel;      /* the board's mounting and scale, and the orientation detector */
static volatile int32_t shown_orientation;      /* hw.sensors.orientation: ORIENT_* */
static volatile int32_t shown_mg[3];            /* hw.sensors.accel: the latest sample, milli-g on Darwin's axes */
static volatile uint32_t samples;               /* hw.sensors.samples: how many were read */
static unsigned changes;

static const char *const orientation_names[] = {
	[ORIENT_UNKNOWN] = "not known", [ORIENT_PORTRAIT] = "portrait", [ORIENT_PORTRAIT_UPSIDE_DOWN] = "portrait upside down",
	[ORIENT_LANDSCAPE_LEFT] = "landscape left", [ORIENT_LANDSCAPE_RIGHT] = "landscape right",
	[ORIENT_FACE_UP] = "face up", [ORIENT_FACE_DOWN] = "face down",
};

SYSCTL_NODE(_hw, OID_AUTO, sensors, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "the motion sensor");

static int
sysctl_accel SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char text[48];

	snprintf(text, sizeof(text), "%d %d %d", shown_mg[0], shown_mg[1], shown_mg[2]);
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}

SYSCTL_INT(_hw_sensors, OID_AUTO, orientation, CTLFLAG_RD | CTLFLAG_LOCKED, __DEVOLATILE(int32_t *, &shown_orientation), 0,
    "the phone's orientation (UIDeviceOrientation): 0 not known, 1 portrait, 2 upside down, 3 landscape left, 4 right, 5 face up, 6 down");
SYSCTL_PROC(_hw_sensors, OID_AUTO, accel, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_accel, "A",
    "the latest accelerometer sample: x y z in milli-g, on Darwin's axes");
SYSCTL_UINT(_hw_sensors, OID_AUTO, samples, CTLFLAG_RD | CTLFLAG_LOCKED, __DEVOLATILE(uint32_t *, &samples), 0,
    "accelerometer samples read");

/* The bus is the thread's from here until it calls done(); false when the run's end has it already. */
static bool
begin(void)
{
	__atomic_store_n(&busy, true, __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&leaving, __ATOMIC_SEQ_CST)) {
		__atomic_store_n(&busy, false, __ATOMIC_SEQ_CST);
		return false;
	}
	return true;
}

static void
done(void)
{
	__atomic_store_n(&busy, false, __ATOMIC_SEQ_CST);
}

/* One sample: read, turned onto Darwin's axes, into the orientation; a change of it said. */
static bool
sample(void)
{
	uint8_t out[LSM6DSO_OUTPUT_BYTES];
	struct lsm6dso_sample raw;
	struct sensor_sample mg;
	int now;

	if (!regs_read(LSM6DSO_REG_OUTX_L_G, out, sizeof(out))) {
		give_up(failed);
		return false;
	}
	raw = lsm6dso_decode(out);
	mg = sensor_to_darwin(&accel.mount, &accel.scale, (struct sensor_sample){ { raw.accel[0], raw.accel[1], raw.accel[2] } });
	now = sensors_accel_raw(&accel, raw.accel[0], raw.accel[1], raw.accel[2]);
	for (unsigned i = 0; i < 3; i++) {
		shown_mg[i] = mg.v[i];
	}
	samples++;
	if (now != shown_orientation) {
		changes++;
		say("imu: %s (%d %d %d milli-g; the chip's %d %d %d)\n", orientation_names[now], mg.v[0], mg.v[1], mg.v[2],
		    raw.accel[0], raw.accel[1], raw.accel[2]);
		shown_orientation = now;
	}
	return true;
}

static void
imu_thread(__unused void *parameter, __unused wait_result_t result)
{
	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	say("imu: level %u: %s\n", level, level == 1 ? "a look, reading only" : "the accelerometer switched on, the orientation followed");
	if (begin()) {
		bool followed;

		show_pins();
		followed = identified() && level >= 2 && configure();
		done();
		if (followed) {
			say("imu: the orientation is followed from here on (hw.sensors.orientation, hw.sensors.accel), until the run ends\n");
			while (begin()) {
				bool read = sample();

				done();
				if (!read) {
					break;
				}
				wait_ms(POLL_MS);
			}
		}
	}
	say("imu: done\n");
	thread_terminate(current_thread());
}

void
hakios_imu_init(void)
{
	const struct hakios_board *b;
	thread_t thread;

	/* boot-arg sensors=N: the motion sensor -- 1 looks at it, 2 follows the phone's orientation (this file's head) */
	if (!PE_parse_boot_argn("sensors", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (!hakios_board_has(HAKIOS_HAS_IMU_LSM6DSO)) {
		/* the pins and the address are the board's: not on a board that does not say it has the chip on its own pins */
		say("imu: disabled: this board's board.dts lists no \"imu-lsm6dso\"\n");
		level = 0;
		return;
	}
	b = hakios_board();
	if (b->pins.tlmm == 0) {
		say("imu: disabled: the SoC file does not say where the pins are (pins { })\n");
		level = 0;
		return;
	}
	for (unsigned i = 0; i < 3; i++) {
		accel.mount.axis[i] = b->imu.axis[i];
		accel.mount.sign[i] = b->imu.sign[i];
	}
	full_scale = b->imu.full_scale == 2 ? LSM6DSO_FS_2G : b->imu.full_scale == 4 ? LSM6DSO_FS_4G :
	    b->imu.full_scale == 8 ? LSM6DSO_FS_8G : b->imu.full_scale == 16 ? LSM6DSO_FS_16G : 0xff;
	if (b->imu.sda == b->imu.scl || b->imu.address == 0 || full_scale == 0xff || !sensor_mount_ok(&accel.mount)) {
		say("imu: disabled: the board file does not say which pins the chip is on, its address, its range and how it is "
		    "turned (imu { })\n");
		level = 0;
		return;
	}
	accel.scale = (struct sensor_scale){ 1000, lsm6dso_counts_per_g(full_scale) };
	sensors_accel_reset(&accel);
	i2c_bus.sda = PIN_SDA;
	i2c_bus.scl = PIN_SCL;
	if (level > 2) {
		level = 2;
	}
	if (kernel_thread_start(imu_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/*
 * At the run's end and on a panic's way out: the reading stopped, the accelerometer powered down if it was
 * switched on, the pins as they were found.
 */
void
hakios_imu_quiesce(void)
{
	unsigned clocks = 0;
	bool off;

	if (level == 0 || tlmm == 0) {
		return;
	}
	__atomic_store_n(&leaving, true, __ATOMIC_SEQ_CST);
	for (unsigned i = 0; i < 100 && __atomic_load_n(&busy, __ATOMIC_SEQ_CST); i++) {
		wait_ms(1);             /* (the thread finishing what it reads: 100 ms at most, a stopped core's never ends) */
	}
	if (on_bus) {
		/* the thread stopped in the middle of a transfer (a panic, its core stopped): ended, the pins as it found them */
		i2c_gpio_stop(&i2c_bus);
		bus_give_back();
	}
	if (!switched_on) {
		return;
	}
	off = reg_write(LSM6DSO_REG_CTRL1_XL, 0);
	if (!off && i2c_gpio_unwedge(&i2c_bus, &clocks)) {
		/* (a chip cut off in the middle of a byte it was sending holds the data line down until it has sent it) */
		bus_give_back();
		say("imu: at the run's end: the bus came back after %u clocks\n", clocks);
		off = reg_write(LSM6DSO_REG_CTRL1_XL, 0);
	}
	say("imu: at the run's end: %u samples read, the orientation changed %u times; the accelerometer %s\n", samples, changes,
	    off ? "is powered down (CTRL1_XL 0)" : "COULD NOT BE POWERED DOWN");
}
