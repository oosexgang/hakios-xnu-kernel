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
 *              too only for Darwin's motion stack, below), each write read back. Then its output read about
 *              every 100 ms, turned onto Darwin's axes and into milli-g, each change of orientation said, and the latest in
 *              hw.sensors.orientation (a UIDeviceOrientation: 1 portrait ... 6 face down, 0 not known yet, or
 *              not any more once the chip stops answering) and hw.sensors.accel (milli-g, "x y z" on Darwin's
 *              axes). While it is followed, hw.sensors.hid says 1: backboardd's library then gives the
 *              orientation to Darwin's HID event system, as an iPhone's always-on processor does
 *              (userland/userland/src/hid_orientation.h; after the idea of the OnePlus Nord 2 port's piece 0761).
 *
 *   orientation_hid=0  the orientation is followed and said all the same, but Darwin is not given it:
 *              hw.sensors.hid stays 0, backboardd's library makes no orientation service, and only the log
 *              hears of a turn, as before there was such a service. Nor its motion stack the samples (below).
 *
 *   motion_hid=0  the samples themselves are not given to Darwin's motion stack: hw.sensors.motion stays 0 and
 *              backboardd's library makes no accelerometer service; the orientation is given as ever. Nor the
 *              gyroscope's rates (below): it stays powered down.
 *
 *   gyro_hid=0  the accelerometer's samples are given as ever, but the gyroscope is not switched on: it stays powered
 *              down (CTRL2_G as the reset leaves it, 0), hw.sensors.gyro stays 0, no sample carries rates and
 *              backboardd's library makes no gyroscope service.
 *
 * The samples for Darwin's motion stack (CoreMotion, through locationd): while hw.sensors.hid says 1, and unless
 * the boot-arg above keeps them, hw.sensors.motion says 1 too, and backboardd's library gives each sample of
 * hw.sensors.sample (its number, the time it was read and the three milli-g, read together) to the HID event system
 * as an accelerometer event (userland/userland/src/hid_motion.h; after the idea of the OnePlus Nord 2 port's piece
 * 0760). As often as this file reads the chip and no more: about 10 times a second (POLL_MS), where an iPhone's
 * motion stack is used to fifty or a hundred. Each read is some 135 bits on a bus driven by hand at 100 kHz, about
 * 1.4 ms of a core spun (i2c_gpio.h): 1.4 % of a core at 10 a second, 7 % at 50, 14 % at 100, for as long as the
 * phone is up -- too dear while nothing on the phone has been seen to need more. A program that asks CoreMotion for
 * more gets those 10 a second, dated when they were read.
 *
 * The gyroscope's rates go with them (after the idea of the OnePlus Nord 2 port's piece 0760 too, which gave iOS its
 * hub's gyroscope as well): only while the samples are for the motion stack (hw.sensors.motion 1), and unless the
 * boot-arg gyro_hid above keeps it off, the gyroscope is switched on with the accelerometer (CTRL2_G: 104 Hz, the same
 * rate, at +-2000 dps, where the datasheet's sensitivity is 70 mdps a count: GYRO_FS), read in the same burst -- the
 * twelve output bytes from OUTX_L_G (0x22) to OUTZ_H_A (0x2d) follow each other and IF_INC counts the address up,
 * gyroscope first -- turned
 * onto Darwin's axes as the accelerometer is (the same chip axes; the board's accelerometer mounting with its signs
 * turned back, sensor_axes_core.h: sensor_gyro_mount()) and into milli-rad/s, and put out with the accelerometer's
 * sample under the same lock: hw.sensors.sample says "NUMBER TIME X Y Z GX GY GZ" for a read that has the
 * gyroscope's rates, "NUMBER TIME X Y Z" as before for one that has not (a reader of the five reads what it read).
 * backboardd's library sends them from a gyroscope service of their own (userland/userland/src/hid_gyro.h).
 *
 * What it costs: the bus time is no more than before -- this file has always read the twelve bytes in one go, the
 * gyroscope's six with the accelerometer's (a read of the accelerometer's six alone, from OUTX_L_A, would be some 81
 * bits, 0.8 ms, against the 135 bits, 1.4 ms, of the twelve: about 0.55 ms a read, 0.55 % of a core at 10 a second, is what
 * the gyroscope's half of the burst costs, and it was already being paid for six bytes of zeros) -- and one more
 * register written and read back at the start and at the run's end. What it does cost is current: the gyroscope is
 * the chip's dearer half to keep running (the datasheet puts the two together at about 0.55 mA at full performance),
 * hence powered down whenever nothing of Darwin's is to have its rates. And at 10 reads a second each rate is the
 * one the chip had at that moment, not an average over the tenth of a second: a rotation is caught as it is when
 * read, which is coarse for whatever integrates the rates into an attitude.
 *
 * Never a made-up rate: the gyroscope's output is not taken for its first GYRO_SETTLE reads after it is switched on,
 * then not until two of its reads have been seen to differ (a gyroscope at rest still wobbles by a count or so; one
 * that is powered down, or does not answer, reads the same zeros each time), and once it reads the very same rates on
 * all three axes for GYRO_STUCK reads in a row it is held stuck: hw.sensors.gyro goes back to 0, the samples carry no
 * rates any more, and the gyroscope is powered down for the rest of the run.
 *
 * Limits, whatever asks: another chip's identity, a byte nobody acknowledges, a write that does not read
 * back or a bus that is not at rest, and nothing more is done for the rest of the run, with a line that
 * says why. At the run's end (and on a panic's way out) the reading stops and the accelerometer is
 * powered down (CTRL1_XL 0), if it was switched on, a bus the chip holds down (cut off in a byte it was
 * sending) is clocked free, at a look too, and the pins are as they were found. The gyroscope, if it was switched
 * on, is powered down too (CTRL2_G 0), before the accelerometer. Nothing is written anywhere but to the chip's three
 * control registers and the bus's two pins.
 */
#include <kern/clock.h>
#include <kern/locks.h>
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
static bool gyro_switched_on;           /* ... and the gyroscope */
static bool leaving;                    /* the run's end has the chip (hakios_imu_quiesce()) */
static bool busy;                       /* the thread is using the bus: the run's end waits for it */
static bool on_bus;                     /* ... from just before it takes the pins until it gives them back */

/* every line to the loader's log as well (and to the console when that may be written) */
HAKIOS_DEFINE_SAY(!__atomic_load_n(&leaving, __ATOMIC_SEQ_CST) && ml_get_interrupts_enabled())

#include "mmio.h"
#include "time.h"

#define wait_ms(ms)             hakios_wait_ms((ms), __atomic_load_n(&leaving, __ATOMIC_SEQ_CST))   /* (spun on the way out) */

#define POLL_MS                 100     /* how often the output is read: at most 10 samples a second (this file's head) */
#define ODR_104_HZ              4       /* CTRL1_XL's output data rate code for 104 Hz (lsm6dso_core.h: lsm6dso_ctrl1_xl()); CTRL2_G's too */
#define GYRO_FS                 LSM6DSO_FS_G_2000DPS    /* the gyroscope's range: +-2000 dps, 70 mdps a count (lsm6dso_core.h) */
#define GYRO_SETTLE             3       /* reads after the gyroscope is switched on whose rates are not taken (this file's head) */
#define GYRO_STUCK              100     /* reads in a row (10 s) of the very same rates that say the gyroscope is stuck */

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

/*
 * The two pins as the run's first transaction found them, which is what every transaction and the run's end give
 * back. Not what each take finds: a take after one that was cut off half way (its core stopped at the run's end)
 * would find the pins as plain GPIOs and hand them back so.
 */
static uint32_t pins_found_cfg[2], pins_found_io[2];
static bool pins_found;

static void
pins_record(void)
{
	if (__atomic_load_n(&pins_found, __ATOMIC_SEQ_CST)) {
		return;
	}
	pins_found_cfg[0] = rd(GPIO_CFG(i2c_bus.sda));
	pins_found_cfg[1] = rd(GPIO_CFG(i2c_bus.scl));
	pins_found_io[0] = rd(GPIO_IO(i2c_bus.sda));
	pins_found_io[1] = rd(GPIO_IO(i2c_bus.scl));
	__atomic_store_n(&pins_found, true, __ATOMIC_SEQ_CST);
}

static void
bus_give_back(void)
{
	for (unsigned i = 0; i < 2; i++) {
		i2c_bus.found_cfg[i] = pins_found_cfg[i];
		i2c_bus.found_io[i] = pins_found_io[i];
	}
	i2c_gpio_give_back(&i2c_bus);
	__atomic_store_n(&on_bus, false, __ATOMIC_SEQ_CST);
}

static bool
bus_take(void)
{
	pins_record();
	/* (on the bus before the pins are touched: a thread stopped inside the take leaves them to the run's end to give back) */
	__atomic_store_n(&on_bus, true, __ATOMIC_SEQ_CST);
	if (i2c_gpio_take(&i2c_bus)) {
		return true;
	}
	bus_give_back();
	say("imu: the I2C bus is not at rest (gpio%u reads %u, gpio%u reads %u): not touched\n", PIN_SDA,
	    i2c_gpio_level(PIN_SDA), PIN_SCL, i2c_gpio_level(PIN_SCL));
	failed = not_at_rest;
	return false;
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
static bool gyro_wanted;                /* the gyroscope to be switched on: its rates for Darwin's motion stack (this file's head) */

/* reset, whole samples read in one go, then the accelerometer on at 104 Hz and the board's range */
static bool
configure(void)
{
	uint8_t control = LSM6DSO_CTRL3_SW_RESET;

	say("imu: the chip is reset and its accelerometer switched on: 104 Hz, +-%u g%s\n", hakios_board()->imu.full_scale,
	    gyro_wanted ? "; its gyroscope too, for Darwin's motion stack: 104 Hz, +-2000 dps" : "");
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
	if (!put(LSM6DSO_REG_CTRL1_XL, lsm6dso_ctrl1_xl(ODR_104_HZ, full_scale))) {
		return false;
	}
	if (!gyro_wanted) {
		return true;            /* (the gyroscope stays as the reset left it: powered down) */
	}
	gyro_switched_on = true;
	return put(LSM6DSO_REG_CTRL2_G, lsm6dso_ctrl2_g(ODR_104_HZ, GYRO_FS));
}

/* ---- the orientation, for Darwin: hw.sensors ----------------------------------------------------- */

static struct sensors_accel accel;      /* the board's mounting and scale, and the orientation detector */
static volatile int32_t shown_orientation;      /* hw.sensors.orientation: ORIENT_* */
static int32_t shown_mg[3];                     /* hw.sensors.accel: the latest sample, milli-g on Darwin's axes */
static uint32_t shown_number;                   /* hw.sensors.sample: its number (the first is 1; 0: none read yet) ... */
static uint64_t shown_time;                     /* ... and when it was read (mach_absolute_time) */
static LCK_GRP_DECLARE(imu_lock_group, "imu");
/* the three of a sample and its orientation, put out together: a reader never sees x of one sample and y of the next */
static LCK_SPIN_DECLARE(shown_lock, &imu_lock_group);
static volatile uint32_t samples;               /* hw.sensors.samples: how many were read */
static unsigned changes;
static uint32_t give_darwin = 1;                /* boot-arg orientation_hid (this file's head): 0 keeps the orientation to the log */
static volatile int32_t followed_for_darwin;    /* hw.sensors.hid: 1 while the orientation is followed and Darwin is to have it */
static uint32_t give_motion = 1;                /* boot-arg motion_hid (this file's head): 0 keeps the samples from Darwin's motion stack */
static volatile int32_t motion_for_darwin;      /* hw.sensors.motion: 1 while hw.sensors.hid is and the samples are for the motion stack */
static uint32_t give_gyro = 1;                  /* boot-arg gyro_hid (this file's head): 0 keeps the gyroscope powered down */
static volatile int32_t gyro_for_darwin;        /* hw.sensors.gyro: 1 while hw.sensors.motion is and the samples carry the gyroscope's rates */
static struct sensor_mount gyro_mount;          /* the gyroscope's turn onto Darwin's axes: the accelerometer's, signs back (sensor_gyro_mount()) */
static struct sensor_scale gyro_scale;          /* its counts -> milli-rad/s */
static int32_t shown_rate[3];                   /* the latest sample's rates, milli-rad/s on Darwin's axes ... */
static bool shown_rate_ok;                      /* ... if it has them */
static unsigned gyro_reads, gyro_same;          /* the gyroscope's reads since it was switched on, and how many in a row said the same */
static int16_t gyro_last[3];                    /* what the last one said, in counts */
static bool gyro_live, gyro_stuck;              /* seen to answer; held stuck (this file's head) */

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
	int32_t mg[3];

	lck_spin_lock(&shown_lock);
	for (unsigned i = 0; i < 3; i++) {
		mg[i] = shown_mg[i];
	}
	lck_spin_unlock(&shown_lock);
	snprintf(text, sizeof(text), "%d %d %d", mg[0], mg[1], mg[2]);
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}

/*
 * hw.sensors.sample: "NUMBER TIME X Y Z", one sample's, read together under the lock (TIME in mach_absolute_time), and
 * " GX GY GZ" after them when that sample has the gyroscope's rates (milli-rad/s on Darwin's axes)
 */
static int
sysctl_sample SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char text[128];
	int32_t mg[3], rate[3];
	uint32_t number;
	uint64_t time;
	bool rated;
	int length;

	lck_spin_lock(&shown_lock);
	number = shown_number;
	time = shown_time;
	rated = shown_rate_ok;
	for (unsigned i = 0; i < 3; i++) {
		mg[i] = shown_mg[i];
		rate[i] = shown_rate[i];
	}
	lck_spin_unlock(&shown_lock);
	length = snprintf(text, sizeof(text), "%u %llu %d %d %d", number, (unsigned long long)time, mg[0], mg[1], mg[2]);
	if (rated && length > 0 && (size_t)length < sizeof(text)) {
		snprintf(text + length, sizeof(text) - (size_t)length, " %d %d %d", rate[0], rate[1], rate[2]);
	}
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}

SYSCTL_INT(_hw_sensors, OID_AUTO, orientation, CTLFLAG_RD | CTLFLAG_LOCKED, __DEVOLATILE(int32_t *, &shown_orientation), 0,
    "the phone's orientation (UIDeviceOrientation): 0 not known, 1 portrait, 2 upside down, 3 landscape left, 4 right, 5 face up, 6 down");
SYSCTL_PROC(_hw_sensors, OID_AUTO, accel, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_accel, "A",
    "the latest accelerometer sample: x y z in milli-g, on Darwin's axes");
SYSCTL_UINT(_hw_sensors, OID_AUTO, samples, CTLFLAG_RD | CTLFLAG_LOCKED, __DEVOLATILE(uint32_t *, &samples), 0,
    "accelerometer samples read");
/*
 * The latest sample with its number and the time it was read, the five together: what backboardd's library gives
 * Darwin's motion stack (userland/userland/src/hid_motion.h), which must not take one sample twice nor date it by
 * when it happened to look. "0 0 0 0 0" until the first is read.
 */
SYSCTL_PROC(_hw_sensors, OID_AUTO, sample, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_sample, "A",
    "the latest sample: its number, the time it was read (mach_absolute_time), x y z in milli-g on Darwin's axes, "
    "and the gyroscope's x y z in milli-rad/s when it has them");
/*
 * Whether backboardd's library is to give the orientation to Darwin (userland/userland/src/hid_orientation.h): 1 from
 * the moment the orientation is followed until the reading stops -- the chip lost, the run's end -- and never with
 * orientation_hid=0. The library looks at it before it makes its service and at every look after, and lets the service
 * fall silent when it goes back to 0: what it said last stands, as on a phone whose sensor has gone quiet.
 */
SYSCTL_INT(_hw_sensors, OID_AUTO, hid, CTLFLAG_RD | CTLFLAG_LOCKED, __DEVOLATILE(int32_t *, &followed_for_darwin), 0,
    "1 while the orientation is followed and Darwin's HID event system is to be given it (boot-arg orientation_hid=0: never)");
/*
 * Whether backboardd's library is to give the samples themselves to Darwin's motion stack (hid_motion.h): 1 only
 * while hw.sensors.hid is 1, and never with motion_hid=0. Set before hw.sensors.hid and let go of after it.
 */
SYSCTL_INT(_hw_sensors, OID_AUTO, motion, CTLFLAG_RD | CTLFLAG_LOCKED, __DEVOLATILE(int32_t *, &motion_for_darwin), 0,
    "1 while hw.sensors.hid is 1 and Darwin's motion stack is to be given the samples (boot-arg motion_hid=0: never)");
/*
 * Whether backboardd's library is to give the gyroscope's rates to Darwin's motion stack (hid_gyro.h): 1 only while
 * hw.sensors.motion is 1, from the read that shows the gyroscope answers, until it is held stuck or the reading stops,
 * and never with gyro_hid=0. While it says 1 the samples of hw.sensors.sample carry rates; set after hw.sensors.motion
 * and let go of before it.
 */
SYSCTL_INT(_hw_sensors, OID_AUTO, gyro, CTLFLAG_RD | CTLFLAG_LOCKED, __DEVOLATILE(int32_t *, &gyro_for_darwin), 0,
    "1 while hw.sensors.motion is 1 and the samples carry the gyroscope's rates (boot-arg gyro_hid=0: never)");

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

/*
 * Whether the gyroscope's rates of this read (counts) are its own, to be given (this file's head): not in its first
 * GYRO_SETTLE reads, nor before two reads after those have been seen to differ, nor once GYRO_STUCK in a row have said
 * the very same -- when it is held stuck, said, and powered down for the rest of the run.
 */
static bool
gyro_taken(const int16_t raw[3])
{
	bool same = raw[0] == gyro_last[0] && raw[1] == gyro_last[1] && raw[2] == gyro_last[2];

	for (unsigned i = 0; i < 3; i++) {
		gyro_last[i] = raw[i];
	}
	if (gyro_stuck || ++gyro_reads <= GYRO_SETTLE + 1) {
		return false;           /* (the first after the settling ones is only what the next is held against) */
	}
	gyro_same = same ? gyro_same + 1 : 0;
	if (gyro_same + 1 >= GYRO_STUCK) {     /* (the first of them and the GYRO_STUCK - 1 after it that said the same) */
		gyro_stuck = true;
		__atomic_store_n(&gyro_for_darwin, 0, __ATOMIC_SEQ_CST);
		say("imu: the gyroscope has read %d %d %d (counts) %u times in a row: %s; it is powered down (CTRL2_G 0%s)\n", raw[0],
		    raw[1], raw[2], gyro_same + 1, gyro_live ? "stuck, its rates NOT GIVEN ANY MORE" :
		    "it does not answer, its rates NOT GIVEN", reg_write(LSM6DSO_REG_CTRL2_G, 0) ? "" : ": NOT WRITTEN");
		return false;
	}
	if (!gyro_live && !same) {
		gyro_live = true;       /* (said by the caller, which has the rates on Darwin's axes) */
	}
	return gyro_live;
}

/* One sample: read, turned onto Darwin's axes, into the orientation; a change of it said. */
static bool
sample(void)
{
	uint8_t out[LSM6DSO_OUTPUT_BYTES];
	struct lsm6dso_sample raw;
	struct sensor_sample mg, rate = { { 0, 0, 0 } };
	bool rated;
	int now;

	if (!regs_read(LSM6DSO_REG_OUTX_L_G, out, sizeof(out))) {
		give_up(failed);
		/* nothing is read from here on: the orientation is not known any more, not the last one kept as if it were */
		lck_spin_lock(&shown_lock);
		shown_orientation = ORIENT_UNKNOWN;
		lck_spin_unlock(&shown_lock);
		return false;
	}
	raw = lsm6dso_decode(out);
	mg = sensor_to_darwin(&accel.mount, &accel.scale, (struct sensor_sample){ { raw.accel[0], raw.accel[1], raw.accel[2] } });
	rated = gyro_switched_on && gyro_taken(raw.gyro);
	if (rated) {
		rate = sensor_to_darwin(&gyro_mount, &gyro_scale, (struct sensor_sample){ { raw.gyro[0], raw.gyro[1], raw.gyro[2] } });
	}
	now = sensors_accel_raw(&accel, raw.accel[0], raw.accel[1], raw.accel[2]);
	if (now != shown_orientation) {
		changes++;
		say("imu: %s (%d %d %d milli-g; the chip's %d %d %d)\n", orientation_names[now], mg.v[0], mg.v[1], mg.v[2],
		    raw.accel[0], raw.accel[1], raw.accel[2]);
	}
	lck_spin_lock(&shown_lock);
	for (unsigned i = 0; i < 3; i++) {
		shown_mg[i] = mg.v[i];
		shown_rate[i] = rate.v[i];
	}
	shown_rate_ok = rated;
	shown_orientation = now;
	shown_time = mach_absolute_time();
	shown_number = ++samples;
	lck_spin_unlock(&shown_lock);
	if (rated && !__atomic_load_n(&gyro_for_darwin, __ATOMIC_SEQ_CST) && !__atomic_load_n(&leaving, __ATOMIC_SEQ_CST)) {
		/* (once: only stuck takes it back, for good) */
		say("imu: the gyroscope answers at its read %u (%d %d %d milli-rad/s; the chip's %d %d %d): its rates are given to Darwin's "
		    "motion stack (hw.sensors.gyro 1)\n", gyro_reads, rate.v[0], rate.v[1], rate.v[2], raw.gyro[0], raw.gyro[1], raw.gyro[2]);
		__atomic_store_n(&gyro_for_darwin, 1, __ATOMIC_SEQ_CST);
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
			say("imu: the orientation is followed from here on (hw.sensors.orientation, hw.sensors.accel), until the run ends; "
			    "%s%s%s\n", give_darwin ? "Darwin is given it (hw.sensors.hid 1)" : "Darwin is not given it (orientation_hid=0)",
			    !give_darwin ? "" : give_motion ? ", and its motion stack the samples (hw.sensors.motion 1)" :
			    ", not its motion stack the samples (motion_hid=0)", !give_darwin || !give_motion ? "" :
			    gyro_wanted ? ", with the gyroscope's rates once it is seen to answer (hw.sensors.gyro)" :
			    !give_gyro ? ", without the gyroscope's rates (gyro_hid=0)" : ", without the gyroscope's rates (the board's mount)");
			/* (the motion first: the library looks at it once hw.sensors.hid says 1) */
			__atomic_store_n(&motion_for_darwin, give_darwin && give_motion ? 1 : 0, __ATOMIC_SEQ_CST);
			__atomic_store_n(&followed_for_darwin, give_darwin ? 1 : 0, __ATOMIC_SEQ_CST);
			while (begin()) {
				bool read = sample();

				done();
				if (!read) {
					break;
				}
				wait_ms(POLL_MS);
			}
			/* (nothing more is read: the library's services fall silent, and keep what they said last) */
			__atomic_store_n(&gyro_for_darwin, 0, __ATOMIC_SEQ_CST);
			__atomic_store_n(&followed_for_darwin, 0, __ATOMIC_SEQ_CST);
			__atomic_store_n(&motion_for_darwin, 0, __ATOMIC_SEQ_CST);
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
	/* boot-arg orientation_hid=0: followed and said, not given to Darwin (this file's head) */
	PE_parse_boot_argn("orientation_hid", &give_darwin, sizeof(give_darwin));
	/* boot-arg motion_hid=0: the samples not given to Darwin's motion stack (this file's head) */
	PE_parse_boot_argn("motion_hid", &give_motion, sizeof(give_motion));
	/* boot-arg gyro_hid=0: the gyroscope not switched on, its rates not given (this file's head) */
	PE_parse_boot_argn("gyro_hid", &give_gyro, sizeof(give_gyro));
	gyro_wanted = level >= 2 && give_darwin && give_motion && give_gyro;
	if (gyro_wanted && !sensor_gyro_mount(&accel.mount, &gyro_mount)) {
		say("imu: the gyroscope is not switched on: the board's mount (imu { }) is not a turn a chip's axes can have "
		    "(the accelerometer's, which turns its +1 g up into Darwin's -1 g, must be a mirror), so its rates could not be "
		    "put the right way round on Darwin's axes\n");
		gyro_wanted = false;
	}
	gyro_scale = sensor_gyro_scale(lsm6dso_gyro_udps_per_count(GYRO_FS));
	if (kernel_thread_start(imu_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/*
 * At the run's end and on a panic's way out: the reading stopped, the bus at rest, the gyroscope and the accelerometer
 * powered down if they were switched on, the pins as they were found.
 */
void
hakios_imu_quiesce(void)
{
	unsigned clocks = 0;
	bool off, came_back;

	if (level == 0 || tlmm == 0) {
		return;
	}
	__atomic_store_n(&leaving, true, __ATOMIC_SEQ_CST);
	__atomic_store_n(&gyro_for_darwin, 0, __ATOMIC_SEQ_CST);      /* (whether its thread gets that far or not) */
	__atomic_store_n(&followed_for_darwin, 0, __ATOMIC_SEQ_CST);
	__atomic_store_n(&motion_for_darwin, 0, __ATOMIC_SEQ_CST);
	for (unsigned i = 0; i < 100 && __atomic_load_n(&busy, __ATOMIC_SEQ_CST); i++) {
		wait_ms(1);             /* (the thread finishing what it reads: 100 ms at most, a stopped core's never ends) */
	}
	__atomic_store_n(&gyro_for_darwin, 0, __ATOMIC_SEQ_CST);      /* (again: a read that ended after the first might have said 1) */
	if (__atomic_load_n(&on_bus, __ATOMIC_SEQ_CST)) {
		/*
		 * the thread stopped in the middle of a transfer, or of taking the pins (a panic, its core stopped): ended, the
		 * pins as the run found them
		 */
		i2c_gpio_stop(&i2c_bus);
		bus_give_back();
	}
	if (!i2c_gpio_level(PIN_SDA) || !i2c_gpio_level(PIN_SCL)) {
		/*
		 * A chip cut off in the middle of a byte it was sending holds the data line down until it has sent it. Clocked
		 * free whether or not there is anything to power down: a look leaves the bus as it found it too, and whoever
		 * has the pins next finds it at rest.
		 */
		pins_record();
		came_back = i2c_gpio_unwedge(&i2c_bus, &clocks);
		bus_give_back();
		if (came_back) {
			say("imu: at the run's end: the bus came back after %u clocks\n", clocks);
		} else {
			say("imu: at the run's end: THE I2C BUS IS NOT AT REST (gpio%u reads %u, gpio%u reads %u)\n", PIN_SDA,
			    i2c_gpio_level(PIN_SDA), PIN_SCL, i2c_gpio_level(PIN_SCL));
		}
	}
	if (!switched_on) {
		return;
	}
	if (gyro_switched_on) {
		off = reg_write(LSM6DSO_REG_CTRL2_G, 0);
		say("imu: at the run's end: the gyroscope %s\n", off ? "is powered down (CTRL2_G 0)" : "COULD NOT BE POWERED DOWN");
	}
	off = reg_write(LSM6DSO_REG_CTRL1_XL, 0);
	say("imu: at the run's end: %u samples read, the orientation changed %u times; the accelerometer %s\n", samples, changes,
	    off ? "is powered down (CTRL1_XL 0)" : "COULD NOT BE POWERED DOWN");
}
