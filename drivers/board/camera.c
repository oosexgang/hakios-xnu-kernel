/*
 * Cameras on an SM7325's camera blocks. How each sensor is wired is the board's camera { } and its
 * sensor@N nodes; where the camera's blocks and clocks are, the SoC file's camera { }; what each sensor
 * chip is (chips[] below), and how the clock controller's PLLs are set up and its clocks divided, this
 * file's. It was written on a phone with four: the macro one, an OmniVision OV02B10 (2 MP,
 * "<phone>_aac_ov02b10_macro"), the wide one, a Samsung S5KHM2SD (108 MP, "<phone>_sunny_s5khm2sd_wide"),
 * the front one and the ultra-wide one, described below as an example. Every number was found on
 * that phone, from Android's own device tree (cam-sensor0 to cam-sensor3 and
 * what they point to), the sensors' module files of Android's camera stack (/vendor/lib64/camera/com.qti.sensormodule.<phone>_*.bin: the
 * slave address, the chip ID and where it is read, the power sequence, the tables) and the drivers
 * of the phone's open kernel (its device package names the tree: cam_sensor_util.c, qcom_pm8008-regulator.c,
 * camcc-yupik.c, gdsc-regulator.c, pinctrl-yupik.c, pinctrl-spmi-gpio.c, cam_csiphy_*), read as
 * data. The macro one (the others' differences are in the board file, chips[] below and capture_sensor()):
 *
 *   the sensor     on CCI 1, master 0 (GPIO 73 data, 74 clock, function cci_i2c, internal pull-ups),
 *                  slave address 0x3c; done by hand here, as the other drivers do their I2C (the
 *                  two pins taken for each transaction and given back as they were found)
 *   chip ID        Android reads it as a 16-bit register at the 16-bit address 0x0203 and wants
 *                  0x002b (mask 0xffff); done the same way here
 *   reset          GPIO 77, high = running
 *   MCLK           camcc's mclk2 (RCG 0xe038, branch 0xe050) at 19.2 MHz on GPIO 66 (function
 *                  cam_mclk, 4 mA); Android takes the 19.2 MHz from camcc's PLL2 (M/N 1/75), here
 *                  it is the crystal's own 19.2 MHz (the RCG's source 0), with no PLL to start.
 *                  Before it Android switches on the camera's top power domain ("cam_clk":
 *                  titan_top_gdsc, camcc 0xc194), and so does this.
 *   VDIG (DOVDD)   PM8008i's LDO6 at 1.8 V; VANA (AVDD) its LDO7 at 2.8 V (the device tree's
 *                  voltages, normal power mode for the load Android asks). The PM8008i is on the
 *                  serial engine 1's I2C (GPIO 4 data, 5 clock; addresses 0x08 for the chip, 0x09
 *                  for the regulators, 16-bit registers), done by hand too; its reset pin is
 *                  PM8350C GPIO 3 (output high, as Android's pinctrl "pm8008i_active" has it)
 *   power-up       (the module file's): reset low, 1 ms; VDIG on; VANA on, 2 ms; MCLK on, 4 ms;
 *                  reset high, 9 ms. Power-down: MCLK off; reset low, 1 ms; VANA off, 7 ms; VDIG off.
 *                  (Both are read from the sensor's table at run time, as the wide one's are.)
 *
 * The wide one: CCI 0 master 1 (GPIO 71/72), slave address 0x20, chip ID 0xfad2 at 0x0000, reset
 * GPIO 21, MCLK1 (RCG 0xe01c, branch 0xe034, GPIO 65); VDIG the PM8008i's LDO1 at 1.0 V, VIO its
 * LDO5 at 1.8 V, VANA a fixed 2.8 V regulator that GPIO 106 switches; three C-PHY trios into
 * CSIPHY 1.
 *
 * The front one, an OmniVision OV16A1Q (16 MP, cam-sensor1): CCI 0 master 0 (GPIO 69/70), slave
 * 0x10, chip ID 0x1641 at 0x300b, reset GPIO 20, MCLK0 (GPIO 64); VANA LDO3 at 2.8 V, VDIG LDO2 at
 * 1.2 V, VIO LDO5 at 1.8 V; four D-PHY lanes into CSIPHY 0.
 *
 * The ultra-wide one, a Samsung S5K4H7 (8 MP, cam-sensor3): CCI 1 master 1 (GPIO 75/76), slave
 * 0x2d, chip ID 0x487b at 0x0000, reset GPIO 78, MCLK3 (GPIO 67) at the 24 MHz its module file
 * asks for, from camcc's PLL2 (Android's 1440 MHz Zonda configuration); VANA LDO4 at 2.8 V, VIO
 * LDO5 at 1.8 V, VDIG a fixed 1.2 V regulator that GPIO 108 switches; four D-PHY lanes into CSIPHY 3.
 *
 * Nothing here is done unless a boot-arg asks for it:
 *
 *   cam=1   a look, reading only: the camera's clocks and power domain as they are, the pins, the
 *           PM8008i's reset pin, and if it is out of reset the PM8008i itself (its chip enable and
 *           the two LDOs)
 *   cam=2   ... and the sensor powered up as Android powers it, its chip ID read, and powered
 *           down again (every write read back)
 *   cam=3   the CSID's test pattern through RDI 0 and IFE-lite 0 into memory, which the loader hands
 *           to the host after the run (the first generation's xl camera): the camera subsystem's rails, PLL, clocks, SMMU
 *           and blocks without the sensor (capture_tpg() below)
 *   cam=4   the sensor's own frames that way, through its CSIPHY (capture_sensor() below)
 *
 * Which sensor it works on is the board's: see sensors_from_board() below.
 *
 * Limits, whatever asks: Android's voltages only (each sensor's supplies as the board's sensor@N gives
 * them, from the device tree; nothing else is ever written to the LDOs' set points); only the registers named above are written (a list each, checked at every
 * write) and each is read back; any write that does not read back, a regulator that is not ready
 * in time or reports over-current, a byte nobody acknowledges, and everything is switched off again
 * in the power-down's order and nothing more is done. At the run's end (and on a panic's way out)
 * whatever was switched on is switched off. Everything that was found switched on is left on.
 * The camera subsystem's own registers (camcc's PLL0 and the clocks named there, the sensor's CSIPHY,
 * CSID-lite 0, IFE-lite 0, one match register and context of the SMMU) are written the same way, read back
 * where they keep what they are given, and taken back in the reverse order.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>  /* xnu-12377: the KMA flags and kmem_alloc moved to the internal header */
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <vm/memory_types.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
#include <sys/board/audio.h>
#include <sys/board/camera.h>
#include <sys/board/input.h>

#include "kernel.h"
#include "smmu.h"
#include "cache.h"
#include "board.h"
#include "log.h"

static uint32_t level;
static bool broken, leaving;

HAKIOS_DEFINE_SAY(!leaving && ml_get_interrupts_enabled())

#include "mmio.h"
#include "time.h"

#define wait_ms(ms)             hakios_wait_ms((ms), leaving)   /* (spun on the way out) */

static void
give_up(const char *why)
{
	if (!broken) {
		broken = true;
		say("camera: %s: nothing more is written in this run\n", why);
	}
}

/* ---- pins ------------------------------------------------------------------------------------------ */

#define TLMM_PHYS               (hakios_board()->pins.tlmm)     /* the SoC file's pins { } */
static vm_offset_t tlmm;
#define HAKIOS_TLMM             tlmm
#include "tlmm.h"                    /* a pin's two registers, and the four things done with them */

/*
 * The sensors come from the board (camera { } and its sensor@N { } in board.dts): where each is wired
 * -- its CCI master's pins (taken by hand), the reset, which MCLK (its pin, function 1, cam_mclk, at 4 mA),
 * the CSIPHY, and how the module file's power sequence switches its supplies (a PM8008 LDO at the device
 * tree's voltage, or a GPIO that switches a fixed regulator). What a sensor is -- its module file, how its
 * exposure registers are laid out, its colour order and gains -- is its chip's, in chips[] below. Boot-arg
 * cam_sensor=NAME picks one by its name in the board (the start of it, up to a dash, will do: "ultra");
 * without it, the board's test-sensor, and for cam=5 its darwin-sensor (the back camera; darwin-front, the front one).
 */
enum {
	REGS_OV_PAGED,                  /* OmniVision, 8-bit registers in pages (0xfd): exposure 0x0e/0x0f, gain 0x22, VTS 0x14/0x15 */
	REGS_SMIA,                      /* SMIA: frame length 0x0340, coarse integration 0x0202, analog gain 0x0204, hold 0x0104 */
	REGS_OV16,                      /* OmniVision, 16-bit addresses: exposure 0x3500.., gain 0x3508, VTS 0x380e, hold 0x3208 */
};
/* the sensor chips this driver knows (tools/boards.py's CAMERA_CHIPS is this list's names) */
static const struct chip {
	const char *    name;           /* as the board names it, and for the frames' header */
	const char *    tab;            /* its module file, in fw/ of the small image (components/camera/tools/camsensor.py) */
	uint8_t         regs;           /* the exposure registers: REGS_OV_PAGED, REGS_SMIA or REGS_OV16 */
	uint8_t         bayer;          /* the picture's order as it comes here (camraw.py's codes: 1 GRBG, 3 BGGR) */
	uint8_t         pattern_bayer;  /* ... and its own colour bars' (its module file's "tpgon") */
	bool            exposure;       /* cam_shutter, cam_gain and cam_vblank have been tried on it */
	uint16_t        gain_unity, gain_max;   /* the analog gain register's 1x, and the most it is given */
} chips[] = {
	{ "OV02B10", "camera-ov02b10.tab", REGS_OV_PAGED, 3, 2, true, 16, 0xf8 },
	{ "S5KHM2SD", "camera-s5khm2sd.tab", REGS_SMIA, 1, 2, true, 32, 0x800 },
	{ "OV16A1Q", "camera-ov16a1q.tab", REGS_OV16, 3, 0, false, 0x100, 0xf80 },
	{ "S5K4H7", "camera-s5k4h7.tab", REGS_SMIA, 1, 2, false, 32, 0x200 },
};
/* a sensor as the rest of this file works on it: the board's wiring, its chip's ways, its clocks' registers */
struct sensor_hw {
	char            name[40];       /* "macro (OV02B10)" */
	const char *    tab;
	char            bus_name[24];   /* "CCI 1 master 0" */
	unsigned        sda, scl, reset;
	uint32_t        mclk_cmd, mclk_cbcr;
	unsigned        mclk_pin;
	struct hakios_camera_supply vana, vdig, vio;
	unsigned        csiphy;
	uint32_t        phy_cbcr, timer_cmd, timer_cbcr;
	uint8_t         regs, bayer, pattern_bayer;
	bool            exposure, focus_motor;
	const char *    chip;           /* for the frames' header */
	uint16_t        gain_unity, gain_max;
};
static struct sensor_hw sensors[4];
static unsigned sensor_count;
static const struct sensor_hw *hw = &sensors[0];

/* The board's sensors, each with its chip's ways; false (and said why) when one of them cannot be worked with. */
static bool
sensors_from_board(void)
{
	const struct hakios_board *board = hakios_board();

	for (unsigned i = 0; i < board->cameras.sensor_count && i < sizeof(sensors) / sizeof(sensors[0]); i++) {
		const struct hakios_camera_sensor *wired = &board->cameras.sensors[i];
		const struct chip *chip = NULL;
		struct sensor_hw *to = &sensors[i];

		for (unsigned c = 0; c < sizeof(chips) / sizeof(chips[0]); c++) {
			chip = strcmp(chips[c].name, wired->chip) == 0 ? &chips[c] : chip;
		}
		if (chip == NULL || wired->mclk >= board->camera.mclk_count || wired->csiphy >= board->camera.csiphy_count) {
			say("camera: disabled: the board's %s camera is %s, which this driver does not know\n", wired->name,
			    chip == NULL ? wired->chip : "on an MCLK or a CSIPHY the SoC has not");
			return false;
		}
		snprintf(to->name, sizeof(to->name), "%s (%s)", wired->name, chip->name);
		snprintf(to->bus_name, sizeof(to->bus_name), "CCI %u master %u", wired->cci, wired->cci_master);
		to->tab = chip->tab;
		to->chip = chip->name;
		to->regs = chip->regs;
		to->bayer = chip->bayer;
		to->pattern_bayer = chip->pattern_bayer;
		to->exposure = chip->exposure;
		to->gain_unity = chip->gain_unity;
		to->gain_max = chip->gain_max;
		to->sda = wired->sda;
		to->scl = wired->scl;
		to->reset = wired->reset;
		to->mclk_pin = wired->mclk_pin;
		to->mclk_cmd = board->camera.mclk_rcg[wired->mclk];
		to->mclk_cbcr = board->camera.mclk_branch[wired->mclk];
		to->csiphy = wired->csiphy;
		to->phy_cbcr = board->camera.csiphy_branch[wired->csiphy];
		to->timer_cmd = board->camera.csiphy_timer_rcg[wired->csiphy];
		to->timer_cbcr = board->camera.csiphy_timer_branch[wired->csiphy];
		to->vana = wired->vana;
		to->vdig = wired->vdig;
		to->vio = wired->vio;
		to->focus_motor = wired->focus_motor != 0;
		sensor_count = i + 1;
	}
	if (sensor_count == 0) {
		say("camera: disabled: the board's camera { } names no sensor\n");
		return false;
	}
	return true;
}
static unsigned hw_index;

static void
show_pin(unsigned pin, const char *what)
{
	uint32_t cfg = rd(GPIO_CFG(pin)), io = rd(GPIO_IO(pin));

	say("camera: gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u\n", pin, what, (cfg >> 2) & 15, cfg & 3,
	    (((cfg >> 6) & 7) + 1) * 2, (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") : "input", io & 1);
}

/* The pins written while powered up, as they were found, to be put back: the reset pin, MCLK's, a supply's (supply_on()). */
static uint32_t found_reset_cfg, found_reset_io, found_mclk_cfg;
static bool pins_found;

/* the sensor's reset and MCLK pins as they are now (to be put back when it is powered down) */
static void
find_pins(void)
{
	found_reset_cfg = rd(GPIO_CFG(hw->reset));
	found_reset_io = rd(GPIO_IO(hw->reset));
	found_mclk_cfg = rd(GPIO_CFG(hw->mclk_pin));
	pins_found = true;
}

static void
reset_pin(bool high)
{
	wr(GPIO_IO(hw->reset), high ? 2 : 0);
	wr(GPIO_CFG(hw->reset), CFG_OUTPUT | CFG_DRIVE_MA(2));         /* GPIO, no pull, 2 mA: Android's cam_sensor_active_* */
}

/* ---- I2C by hand (as in board/haptic.c) ---------------------------------------------------------- */

#include "i2c_gpio.h"                /* the two lines, a bit, a byte, the bus taken, given back, and freed of a stuck device */

struct bus {
	const char *    name;
	struct i2c_gpio pins;
};
static struct bus pmic_bus = { .name = "the PM8008i's I2C" };    /* (its pins: the board's camera { } pmic_pins) */
static struct bus cci_bus;              /* the sensor's (from hw) */

static void
i2c_start(struct bus *b)
{
	i2c_gpio_start(&b->pins);
}

static void
i2c_stop(struct bus *b)
{
	i2c_gpio_stop(&b->pins);
}

static bool
i2c_write(struct bus *b, uint8_t byte)
{
	return i2c_gpio_write(&b->pins, byte);
}

static uint8_t
i2c_read(struct bus *b, bool more)
{
	return i2c_gpio_read(&b->pins, more);
}

static void
bus_give_back(struct bus *b)
{
	i2c_gpio_give_back(&b->pins);
}

/*
 * The bus taken for a transaction. A sensor stopped in the middle of a byte -- the camera switched
 * away from it between frames, its digital rail left on because the driver found it on -- goes on
 * holding the data line down: it is clocked out of that byte (board/i2c_gpio.h). This is what kept
 * the wide camera's bus (CCI 0 master 1) for the rest of a session once Camera.app had switched to
 * the front camera and back.
 */
static bool
bus_take(struct bus *b)
{
	unsigned clocks = 0;

	if (i2c_gpio_take(&b->pins)) {
		return true;
	}
	if (i2c_gpio_unwedge(&b->pins, &clocks)) {
		say("camera: %s: the data line was held down and came back after %u clock(s)\n", b->name, clocks);
		return true;
	}
	say("camera: %s is not at rest (gpio%u reads %u, gpio%u reads %u): not touched\n", b->name, b->pins.sda,
	    i2c_gpio_level(b->pins.sda), b->pins.scl, i2c_gpio_level(b->pins.scl));
	return false;
}

/*
 * A transaction: `out' (the register's address, and for a write the data) written, then for a read
 * `in_count' bytes read after a repeated start. False if anybody did not acknowledge.
 */
static bool
i2c_transfer(struct bus *b, uint8_t address, const uint8_t *out, unsigned out_count, uint8_t *in, unsigned in_count)
{
	bool answered;

	if (!bus_take(b)) {
		return false;
	}
	i2c_start(b);
	answered = i2c_write(b, (uint8_t)(address << 1));
	for (unsigned i = 0; answered && i < out_count; i++) {
		answered = i2c_write(b, out[i]);
	}
	if (answered && in_count != 0) {
		i2c_start(b);
		answered = i2c_write(b, (uint8_t)(address << 1 | 1));
		for (unsigned i = 0; answered && i < in_count; i++) {
			in[i] = i2c_read(b, i + 1 < in_count);
		}
	}
	i2c_stop(b);
	bus_give_back(b);
	return answered;
}

/* ---- the PMIC arbiter (versions 5 to 8: board/spmi.h), for the PMIC GPIO of the PM8008i's reset: as in board/torch.c ------ */

/* Where its blocks are: the SoC file's spmi { } (board/board.h). */
#include "spmi.h"

#define PGPIO_ID                (hakios_board()->cameras.pmic_reset)    /* the board's camera { } pmic-reset */
#define PGPIO_TYPE              0x04    /* 0x10: a GPIO */
#define PGPIO_SUBTYPE           0x05
#define PGPIO_RT_STS            0x10    /* [0] what the pin is at */
#define PGPIO_MODE_CTL          0x40    /* LV/MV: [1:0] direction (1 output) */
#define PGPIO_DIG_VIN_CTL       0x41    /* [2:0] which supply: 0 = VIN0 ("power-source 0") */
#define PGPIO_DIG_PULL_CTL      0x42    /* [2:0] 5 = no pull */
#define PGPIO_OUT_SOURCE_CTL    0x44    /* LV/MV: [7] the level (inverted "normal"), [3:0] the function: 0 normal */
#define PGPIO_EN_CTL            0x46    /* [7] enabled */

static struct hakios_spmi arb;
static vm_offset_t pgpio_write;
static uint16_t pgpio_channel;
static bool pgpio_found, pgpio_writable;

static void
lock(void)
{
	if (!leaving) {
		hakios_spmi_lock();
	}
}

static void
unlock(void)
{
	if (!leaving) {
		hakios_spmi_unlock();
	}
}

static const char *
pgpio_read(uint8_t reg, uint8_t *value)
{
	const char *problem = "no channel";

	for (int tries = 0; pgpio_found && tries < 3; tries++) {
		lock();
		problem = hakios_spmi_read(hakios_spmi_observer(&arb, pgpio_channel), reg, value, 1);
		unlock();
		if (problem == NULL || strcmp(problem, SPMI_SAYS_DROPPED) != 0) {
			break;
		}
	}
	return problem;
}

/* One of GPIO 3's five registers above written and read back (when `force', also after something went wrong). */
static bool
pgpio_put(uint8_t reg, uint8_t value, bool force)
{
	const char *problem = NULL;
	uint8_t back = 0xff;

	if ((broken && !force) || !pgpio_writable || pgpio_write == 0 || (reg != PGPIO_MODE_CTL && reg != PGPIO_DIG_VIN_CTL &&
	    reg != PGPIO_DIG_PULL_CTL && reg != PGPIO_OUT_SOURCE_CTL && reg != PGPIO_EN_CTL)) {
		give_up("a write to the PMIC that may not be made");
		return false;
	}
	if (SPMI_ID_OF(hakios_spmi_channel_map(&arb, pgpio_channel)) != PGPIO_ID) {
		give_up("the arbiter's channel is no longer the PM8008i's reset GPIO");
		return false;
	}
	for (int tries = 0; tries < 3; tries++) {
		lock();
		problem = hakios_spmi_write(pgpio_write, reg, value);
		unlock();
		if (problem == NULL || strcmp(problem, SPMI_SAYS_DROPPED) != 0) {
			break;
		}
	}
	if (problem != NULL || (problem = pgpio_read(reg, &back)) != NULL || back != value) {
		say("camera: the PM8008i's reset GPIO 0x%02x <- 0x%02x: %s (reads 0x%02x)\n", reg, value, problem ? problem : "does not read back", back);
		give_up("a write to the PMIC that failed");
		return false;
	}
	return true;
}

static void
pgpio_find(void)
{
	for (uint32_t n = 0; n < arb.channels; n++) {
		uint32_t map = hakios_spmi_channel_map(&arb, n), owner;

		if (map == 0 || SPMI_ID_OF(map) != PGPIO_ID) {
			continue;
		}
		owner = hakios_spmi_channel_owner(&arb, n);
		say("camera: channel %u is the PM8008i's reset GPIO (%#x); written by system %u%s\n", n, map, owner,
		    owner == arb.system ? ", which is us" : "");
		if (!pgpio_found || owner == arb.system) {
			pgpio_found = true;
			pgpio_channel = (uint16_t)n;
			pgpio_writable = owner == arb.system;
		}
	}
	if (pgpio_found && pgpio_writable) {
		pgpio_write = hakios_spmi_map_write(&arb, pgpio_channel);
	}
}

/* GPIO 3 as Android's "pm8008i_active" has it: enabled, an output of the normal function, high, VIN0, no pull. */
static uint8_t pgpio_found_regs[5];             /* DIG_VIN_CTL, DIG_PULL_CTL, MODE_CTL, OUT_SOURCE_CTL, EN_CTL as found */
static bool pgpio_changed;

static bool
pgpio_active(uint8_t *regs)
{
	return (regs[0] & 7) == 0 && (regs[1] & 7) == 5 && (regs[2] & 3) == 1 && regs[3] == 0x80 && (regs[4] & 0x80);
}

static const uint8_t pgpio_which[5] = { PGPIO_DIG_VIN_CTL, PGPIO_DIG_PULL_CTL, PGPIO_MODE_CTL, PGPIO_OUT_SOURCE_CTL, PGPIO_EN_CTL };

static bool
pgpio_read_all(uint8_t *regs)
{
	for (int i = 0; i < 5; i++) {
		if (pgpio_read(pgpio_which[i], &regs[i]) != NULL) {
			return false;
		}
	}
	return true;
}

/* ---- the PM8008i ----------------------------------------------------------------------------------- */

#define PM8008_CHIP             0x08    /* its I2C addresses: the chip ... */
#define PM8008_REGULATORS       0x09    /* ... and its regulators */
#define REVID_BASE              0x0100
#define MISC_CHIP_ENABLE        0x0950  /* [0] */
#define LDO_BASE(n)             (0x4000 + 0x100 * ((n) - 1))
#define LDO_STATUS1             0x08    /* [7] ready, [5] over-current, [1:0] mode: 3 normal, 2 low power */
#define LDO_VSET_LB             0x40    /* the set point in mV, low byte ... */
#define LDO_VSET_UB             0x41    /* ... high byte */
#define LDO_MODE_CTL1           0x45    /* [2:0] 7 normal, 4 low power */
#define LDO_ENABLE              0x46    /* [7] */

/* LDO n is one of the sensor's supplies, at that set point. */
static bool
sensor_ldo(unsigned n, unsigned mv)
{
	const struct hakios_camera_supply *all[3] = { &hw->vana, &hw->vdig, &hw->vio };

	for (int i = 0; i < 3; i++) {
		if (all[i]->kind == HAKIOS_SUPPLY_LDO && all[i]->ldo == n && (mv == 0 || all[i]->mv == mv)) {
			return true;
		}
	}
	return false;
}

/*
 * The PM8008i's I2C is bit-banged, on a bus whose lines the phone's own firmware holds as well, and
 * a transfer now and then goes unanswered -- more often the longer the run, and most often right
 * after the camera has been switched off and on again. Taken for a fault the first time, the camera
 * was gone for the rest of the run (give_up: nothing more is written); asked again, the chip
 * answers. The number of goes that were needed is said at the end of the run.
 */
#define PM8008_GOES             4

static uint64_t pm8008_retries;

static bool
pm8008_get(uint8_t address, uint16_t reg, uint8_t *values, unsigned count)
{
	uint8_t where[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };

	for (unsigned go = 0; go < PM8008_GOES; go++) {
		if (i2c_transfer(&pmic_bus, address, where, 2, values, count)) {
			pm8008_retries += go;
			return true;
		}
		wait_ms(1);
	}
	pm8008_retries += PM8008_GOES;
	return false;
}

/* The registers the PM8008i may be written in: the chip enable, and the set point, mode and enable of the sensor's LDOs. */
static bool
pm8008_put(uint8_t address, uint16_t reg, uint8_t value, bool force)
{
	uint8_t out[3] = { (uint8_t)(reg >> 8), (uint8_t)reg, value }, back = 0xff;
	uint16_t base = reg & 0xff00, offset = reg & 0xff;
	bool allowed = (address == PM8008_CHIP && reg == MISC_CHIP_ENABLE) ||
	    (address == PM8008_REGULATORS && base >= LDO_BASE(1) && base <= LDO_BASE(7) && sensor_ldo((base - LDO_BASE(1)) / 0x100 + 1, 0) &&
	    (offset == LDO_MODE_CTL1 || offset == LDO_ENABLE || offset == LDO_VSET_LB || offset == LDO_VSET_UB));

	if ((broken && !force) || !allowed) {
		give_up(allowed ? "a write to the PM8008i after a fault" : "a PM8008i register that is not to be written");
		return false;
	}
	for (unsigned go = 0; go < PM8008_GOES; go++) {
		if (i2c_transfer(&pmic_bus, address, out, 3, NULL, 0) && pm8008_get(address, reg, &back, 1) && back == value) {
			pm8008_retries += go;
			return true;
		}
		wait_ms(1);
	}
	say("camera: PM8008i 0x%02x:0x%04x <- 0x%02x: %s (reads 0x%02x), %u goes\n", address, reg, value, back == 0xff ? "no answer" :
	    "does not read back", back, PM8008_GOES);
	give_up("a write to the PM8008i that failed");
	return false;
}

static void
show_ldo(unsigned n)
{
	uint8_t status = 0, vset[2] = { 0, 0 }, mode = 0, enable = 0;

	if (pm8008_get(PM8008_REGULATORS, (uint16_t)(LDO_BASE(n) + LDO_STATUS1), &status, 1) &&
	    pm8008_get(PM8008_REGULATORS, (uint16_t)(LDO_BASE(n) + LDO_VSET_LB), vset, 2) &&
	    pm8008_get(PM8008_REGULATORS, (uint16_t)(LDO_BASE(n) + LDO_MODE_CTL1), &mode, 1) &&
	    pm8008_get(PM8008_REGULATORS, (uint16_t)(LDO_BASE(n) + LDO_ENABLE), &enable, 1)) {
		say("camera: PM8008i LDO%u: status 0x%02x (%sready%s, mode %u), set to %u mV, mode control 0x%02x, enable 0x%02x\n", n,
		    status, status & 0x80 ? "" : "not ", status & 0x20 ? ", OVER-CURRENT" : "", status & 3, vset[1] << 8 | vset[0], mode,
		    enable);
	} else {
		say("camera: PM8008i LDO%u: no answer\n", n);
	}
}

/*
 * An LDO on as Android's regulator framework puts it on (set point, load -> normal mode, enable),
 * then ready within 10 tries of the driver's wait, and not over its current. The PM8008i has seven,
 * LDO1 to LDO7: another number (a board's, not to be trusted past what tools/boards.py checks) is
 * refused before anything is read, written or remembered of it.
 */
#define PM8008_LDOS             7
static bool ldo_was_on[PM8008_LDOS + 1], ldo_switched[PM8008_LDOS + 1];      /* (by the LDO's number: [0] is not used) */

static bool
ldo_on(unsigned n, unsigned mv)
{
	uint16_t base = (uint16_t)LDO_BASE(n);
	uint8_t enable = 0, mode = 0, status = 0;

	if (n < 1 || n > PM8008_LDOS) {
		say("camera: PM8008i LDO%u: the chip has LDO1 to LDO%u\n", n, PM8008_LDOS);
		give_up("an LDO the PM8008i has not");
		return false;
	}
	if (!sensor_ldo(n, mv)) {
		give_up("a voltage that is not Android's");
		return false;
	}
	if (!pm8008_get(PM8008_REGULATORS, (uint16_t)(base + LDO_ENABLE), &enable, 1) ||
	    !pm8008_get(PM8008_REGULATORS, (uint16_t)(base + LDO_MODE_CTL1), &mode, 1)) {
		give_up("a PM8008i that does not answer");
		return false;
	}
	ldo_was_on[n] = enable & 0x80;
	if (ldo_was_on[n]) {
		say("camera: PM8008i LDO%u was on already: left as it is\n", n);
		return true;
	}
	if (!pm8008_put(PM8008_REGULATORS, (uint16_t)(base + LDO_VSET_LB), (uint8_t)mv, false) ||
	    !pm8008_put(PM8008_REGULATORS, (uint16_t)(base + LDO_VSET_UB), (uint8_t)(mv >> 8), false) ||
	    !pm8008_put(PM8008_REGULATORS, (uint16_t)(base + LDO_MODE_CTL1), (uint8_t)((mode & ~7) | 7), false)) {
		return false;
	}
	ldo_switched[n] = true;
	if (!pm8008_put(PM8008_REGULATORS, (uint16_t)(base + LDO_ENABLE), (uint8_t)(enable | 0x80), false)) {
		return false;
	}
	for (int tries = 0, allones = 0; tries < 10; tries++) {
		spin_us(20 + mv * 1000 / 38400 + 1);   /* the driver's wait: 20 us and the set point at the stepper's 38.4 mV/us */
		if (!pm8008_get(PM8008_REGULATORS, (uint16_t)(base + LDO_STATUS1), &status, 1)) {
			give_up("a PM8008i that does not answer");
			return false;
		}
		/*
		 * A byte with every bit set is not a status: a good one reads 0x87, and the bits between
		 * "ready" and the mode are not used. It is what this bus reads when the PM8008i has not
		 * answered properly, as it does for a few goes right after the front camera's rails have
		 * gone down -- and it was read as an over-current, which switched the camera off for the
		 * rest of the run just as Camera.app went back to the wide camera. Ask again.
		 */
		if (status == 0xff) {
			if (allones++ == 0) {
				say("camera: PM8008i LDO%u: a status of all ones, which is no answer: asking again\n", n);
			}
			continue;
		}
		if (status & 0x20) {
			uint8_t again = 0;

			/* as healthy() does while it runs: an over-current counts when it is still there */
			wait_ms(2);
			if (pm8008_get(PM8008_REGULATORS, (uint16_t)(base + LDO_STATUS1), &again, 1) && !(again & 0x20)) {
				say("camera: PM8008i LDO%u: status 0x%02x, and 0x%02x when asked again\n", n, status, again);
				status = again;
			} else {
				say("camera: PM8008i LDO%u reports over-current (status 0x%02x)\n", n, status);
				give_up("an LDO over its current");
				return false;
			}
		}
		if (status & 0x80) {
			say("camera: PM8008i LDO%u on at %u mV (status 0x%02x)\n", n, mv, status);
			return true;
		}
	}
	say("camera: PM8008i LDO%u not ready (status 0x%02x)\n", n, status);
	give_up(status == 0xff ? "a PM8008i whose status is all ones" : "an LDO that is not ready");
	return false;
}

static void
ldo_off(unsigned n)
{
	uint8_t enable = 0;

	if (n < 1 || n > PM8008_LDOS || !ldo_switched[n]) {
		return;                 /* (one past the seventh was never switched on: ldo_on() refuses it) */
	}
	if (pm8008_get(PM8008_REGULATORS, (uint16_t)(LDO_BASE(n) + LDO_ENABLE), &enable, 1) &&
	    pm8008_put(PM8008_REGULATORS, (uint16_t)(LDO_BASE(n) + LDO_ENABLE), (uint8_t)(enable & ~0x80), true)) {
		ldo_switched[n] = false;
		say("camera: PM8008i LDO%u off\n", n);
	} else {
		say("camera: PM8008i LDO%u COULD NOT BE SWITCHED OFF\n", n);
	}
}

/* ---- the camera's clocks: camcc --------------------------------------------------------------------- */

#define GCC_PHYS                (hakios_board()->clocks.gcc)    /* the SoC file's clocks { } */
#define CAMERA                  (hakios_board()->camera)        /* the SoC file's camera { } */
#define GCC_CAMERA_AHB_CBCR     (CAMERA.gcc_ahb_branch)         /* always on (Linux keeps it so) */
#define GCC_CAMERA_XO_CBCR      (CAMERA.gcc_xo_branch)
#define CAMCC_PHYS              (CAMERA.camcc)
#define CAMCC_SIZE              0x20000UL
#define MCLK_CMD_RCGR           (hw->mclk_cmd)          /* [0] update, [31] the root is off */
#define MCLK_CFG_RCGR           (hw->mclk_cmd + 4)      /* [4:0] divider (2n - 1), [10:8] source (0: the crystal), [13:12] M/N mode */
#define MCLK_M                  (hw->mclk_cmd + 8)
#define MCLK_N                  (hw->mclk_cmd + 12)
#define MCLK_D                  (hw->mclk_cmd + 16)
#define MCLK_CBCR               (hw->mclk_cbcr)         /* [0] enable, [31] the clock is off */
#define TITAN_TOP_GDSCR         (CAMERA.top_gdsc)       /* [0] collapse, [11] keep the registers, [31] on */
#define CBCR_OFF                (1u << 31)
#define CMD_UPDATE              1u
#define GDSC_COLLAPSE           1u
#define GDSC_ON                 (1u << 31)

static vm_offset_t gcc, camcc;
static uint32_t found_mclk_cfg_rcgr, found_mclk_cbcr, found_gdscr, found_mclk_mnd[3];
static bool gdsc_switched, mclk_switched, pll2_switched;

/*
 * camcc's PLL2 (Zonda), for the 24 MHz MCLK the ultra-wide sensor's module file asks for. Where it is and
 * how it is configured are the SoC file's camera { } (pll2, pll2-config: camcc-CODENAME.c's cam_cc_pll2_config;
 * on an SM7325 its "1440MHz Configuration"); it is set as clk_zonda_pll_configure() and started as
 * clk_zonda_pll_enable() do it. The MCLK RCG takes its early output (camera { } pll2-source) divided by 10 and
 * by M/N 1/n, n what makes 24 MHz of the PLL's rate (6 of 1440 MHz: ftbl_cam_cc_mclk0_clk_src's 24 MHz).
 * Taken down as clk_zonda_pll_disable() does, with the MCLK.
 */
#define PLL2_BASE               (CAMERA.pll2)
#define PLL2_MODE               0x00            /* [0] outputs, [1] out of bypass, [2] out of reset, [20] voted, [29] frequency locked, [31] locked */
#define PLL2_L_VAL              0x04
#define PLL2_USER_CTL           0x0c            /* [3:0] the outputs */
#define PLL2_CONFIG_CTL         0x10
#define PLL2_CONFIG_CTL_U       0x14
#define PLL2_CONFIG_CTL_U1      0x18
#define PLL2_TEST_CTL           0x1c            /* [16] stays in CFA (then the frequency lock is what to wait for) */
#define PLL2_OPMODE             0x28            /* 0 standby, 1 run */
#define PLL2_STATUS             0x38
#define XO_KHZ                  19200           /* the crystal the PLLs multiply */

/* PLL2's rate: L times the crystal (a Zonda PLL here has no fraction) */
static uint32_t
pll2_khz(void)
{
	return CAMERA.pll2_config[0] * XO_KHZ;
}

/* n of the M/N 1/n that makes a 24 MHz MCLK of PLL2 after a divider of 10; 0 (and said why) when none does */
static uint32_t
mclk_24mhz_n(void)
{
	uint32_t n = pll2_khz() / 10 / 24000;

	if (n < 2 || n > 255 || n * 10 * 24000 != pll2_khz()) {
		say("camera: PLL2 at %u kHz (the SoC file's pll2-config) makes no 24 MHz by 10 and a whole 1/n\n", pll2_khz());
		return 0;
	}
	return n;
}

static void
show_clocks(void)
{
	say("camera: GCC: camera AHB %#x, camera XO %#x\n", rd(gcc + GCC_CAMERA_AHB_CBCR), rd(gcc + GCC_CAMERA_XO_CBCR));
	say("camera: camcc: titan top GDSC %#x; the sensor's MCLK RCG cmd %#x cfg %#x m %#x n %#x d %#x, branch %#x\n", rd(camcc + TITAN_TOP_GDSCR),
	    rd(camcc + MCLK_CMD_RCGR), rd(camcc + MCLK_CFG_RCGR), rd(camcc + MCLK_M), rd(camcc + MCLK_N), rd(camcc + MCLK_D),
	    rd(camcc + MCLK_CBCR));
}

/* camcc's registers: only these, each read back (an RCG's update bit and a GDSC's status by waiting for them) */
static bool
camcc_put(uint32_t offset, uint32_t value, uint32_t check_mask)
{
	if (offset != TITAN_TOP_GDSCR && offset != MCLK_CFG_RCGR && offset != MCLK_CMD_RCGR && offset != MCLK_CBCR && offset != MCLK_M &&
	    offset != MCLK_N && offset != MCLK_D && !(offset >= PLL2_BASE && offset <= PLL2_BASE + PLL2_STATUS)) {
		give_up("a camcc register that is not to be written");
		return false;
	}
	wr(camcc + offset, value);
	if ((rd(camcc + offset) & check_mask) != (value & check_mask)) {
		say("camera: camcc %#x <- %#x, but it reads %#x\n", offset, value, rd(camcc + offset));
		give_up("a camcc register that did not read back");
		return false;
	}
	return true;
}

static bool
wait_bits(uint32_t offset, uint32_t mask, uint32_t want, uint32_t us)
{
	for (uint32_t waited = 0; waited <= us; waited += 10) {
		if ((rd(camcc + offset) & mask) == want) {
			return true;
		}
		spin_us(10);
	}
	return false;
}

static bool
gdsc_on(void)
{
	found_gdscr = rd(camcc + TITAN_TOP_GDSCR);
	if (!(found_gdscr & GDSC_COLLAPSE) && (found_gdscr & GDSC_ON)) {
		say("camera: the titan top power domain was on already\n");
		return true;
	}
	if (!camcc_put(TITAN_TOP_GDSCR, found_gdscr & ~GDSC_COLLAPSE, GDSC_COLLAPSE)) {
		return false;
	}
	gdsc_switched = true;
	spin_us(1);
	if (!wait_bits(TITAN_TOP_GDSCR, GDSC_ON, GDSC_ON, 100)) {
		say("camera: the titan top power domain does not come on (%#x)\n", rd(camcc + TITAN_TOP_GDSCR));
		give_up("a power domain that does not come on");
		return false;
	}
	say("camera: the titan top power domain on (%#x)\n", rd(camcc + TITAN_TOP_GDSCR));
	return true;
}

static void
gdsc_off(void)
{
	if (!gdsc_switched) {
		return;
	}
	wr(camcc + TITAN_TOP_GDSCR, rd(camcc + TITAN_TOP_GDSCR) | GDSC_COLLAPSE);
	gdsc_switched = false;
	say("camera: the titan top power domain %s (%#x)\n", wait_bits(TITAN_TOP_GDSCR, GDSC_ON, 0, 100) ? "off" : "NOT OFF",
	    rd(camcc + TITAN_TOP_GDSCR));
}

/* MCLK at 19.2 MHz: the RCG from the crystal, divider 1, no M/N; then the branch; then the pin. */
static bool
pll2_up(void)
{
	uint32_t mode = rd(camcc + PLL2_BASE + PLL2_MODE);

	if (mode & (1u | 1u << 20)) {
		give_up("camcc's PLL2 is running or voted for already (somebody else's)");
		return false;
	}
	pll2_switched = true;
	/* clk_zonda_pll_configure() */
	const uint32_t *config = CAMERA.pll2_config;      /* L, config, config_u, config_u1, user */

	if (!camcc_put(PLL2_BASE + PLL2_L_VAL, config[0], ~0u) || !camcc_put(PLL2_BASE + PLL2_CONFIG_CTL, config[1], ~0u) ||
	    !camcc_put(PLL2_BASE + PLL2_CONFIG_CTL_U, config[2], ~0u) || !camcc_put(PLL2_BASE + PLL2_CONFIG_CTL_U1, config[3], ~0u) ||
	    !camcc_put(PLL2_BASE + PLL2_USER_CTL, config[4], ~0u) ||
	    !camcc_put(PLL2_BASE + PLL2_MODE, rd(camcc + PLL2_BASE + PLL2_MODE) & ~2u, 2) ||
	    !camcc_put(PLL2_BASE + PLL2_MODE, rd(camcc + PLL2_BASE + PLL2_MODE) & ~1u, 1) ||
	    !camcc_put(PLL2_BASE + PLL2_OPMODE, 0, 1) ||
	    !camcc_put(PLL2_BASE + PLL2_MODE, rd(camcc + PLL2_BASE + PLL2_MODE) | 4u, 4)) {
		return false;
	}
	/* clk_zonda_pll_enable(): out of bypass, 1 us, out of reset, run, locked, the outputs on */
	if (!camcc_put(PLL2_BASE + PLL2_MODE, rd(camcc + PLL2_BASE + PLL2_MODE) | 2u, 2)) {
		return false;
	}
	spin_us(1);
	if (!camcc_put(PLL2_BASE + PLL2_MODE, rd(camcc + PLL2_BASE + PLL2_MODE) | 4u, 4) || !camcc_put(PLL2_BASE + PLL2_OPMODE, 1, 1)) {
		return false;
	}
	uint32_t lock = rd(camcc + PLL2_BASE + PLL2_TEST_CTL) & (1u << 16) ? 1u << 29 : 1u << 31;

	if (!wait_bits(PLL2_BASE + PLL2_MODE, lock, lock, 1000)) {
		say("camera: camcc's PLL2 does not lock (mode %#x)\n", rd(camcc + PLL2_BASE + PLL2_MODE));
		give_up("a PLL that does not lock");
		return false;
	}
	if (!camcc_put(PLL2_BASE + PLL2_USER_CTL, rd(camcc + PLL2_BASE + PLL2_USER_CTL) | 0xf, 0xf) ||
	    !camcc_put(PLL2_BASE + PLL2_MODE, rd(camcc + PLL2_BASE + PLL2_MODE) | 1u, 1)) {
		return false;
	}
	say("camera: camcc's PLL2 locked at %u MHz (mode %#x, user_ctl %#x)\n", pll2_khz() / 1000, rd(camcc + PLL2_BASE + PLL2_MODE),
	    rd(camcc + PLL2_BASE + PLL2_USER_CTL));
	return true;
}

static void
pll2_down(void)
{
	if (!pll2_switched) {
		return;
	}
	wr(camcc + PLL2_BASE + PLL2_MODE, rd(camcc + PLL2_BASE + PLL2_MODE) & ~1u);
	wr(camcc + PLL2_BASE + PLL2_USER_CTL, rd(camcc + PLL2_BASE + PLL2_USER_CTL) & ~0xfu);
	wr(camcc + PLL2_BASE + PLL2_MODE, rd(camcc + PLL2_BASE + PLL2_MODE) & ~6u);
	wr(camcc + PLL2_BASE + PLL2_OPMODE, 0);
	pll2_switched = false;
	say("camera: camcc's PLL2 off (mode %#x)\n", rd(camcc + PLL2_BASE + PLL2_MODE));
}

/* MCLK at 19.2 MHz from the crystal itself, or at 24 MHz from PLL2 (khz: the module file's rate) */
static bool
mclk_on(uint32_t khz)
{
	bool pll = khz == 24000;
	uint32_t n = pll ? mclk_24mhz_n() : 0;

	if (pll && n == 0) {
		give_up("an MCLK the PLL cannot make");
		return false;
	}

	found_mclk_cfg_rcgr = rd(camcc + MCLK_CFG_RCGR);
	found_mclk_cbcr = rd(camcc + MCLK_CBCR);
	found_mclk_mnd[0] = rd(camcc + MCLK_M);
	found_mclk_mnd[1] = rd(camcc + MCLK_N);
	found_mclk_mnd[2] = rd(camcc + MCLK_D);
	if (found_mclk_cbcr & 1) {
		give_up("MCLK is on already (somebody else's)");
		return false;
	}
	if (pll) {
		/* __clk_rcg2_configure(): M 1, N as ~(n - m), D as ~2d (2d = n), then PLL2's source, divider 2 * 10 - 1, dual-edge M/N */
		if (!pll2_up() || !camcc_put(MCLK_M, (found_mclk_mnd[0] & ~0xffu) | 1, 0xff) ||
		    !camcc_put(MCLK_N, (found_mclk_mnd[1] & ~0xffu) | (~(n - 1) & 0xff), 0xff) ||
		    !camcc_put(MCLK_D, (found_mclk_mnd[2] & ~0xffu) | (~n & 0xff), 0xff)) {
			mclk_switched = true;
			return false;
		}
	}
	mclk_switched = true;
	if (!camcc_put(MCLK_CFG_RCGR, (found_mclk_cfg_rcgr & ~0x371fu) | (pll ? (uint32_t)CAMERA.pll2_source << 8 | 2u << 12 | 19u : 0u << 8 | 1u), 0x371f) ||
	    !camcc_put(MCLK_CMD_RCGR, rd(camcc + MCLK_CMD_RCGR) | CMD_UPDATE, 0)) {
		return false;
	}
	if (!wait_bits(MCLK_CMD_RCGR, CMD_UPDATE, 0, 500)) {
		say("camera: MCLK's RCG does not take its configuration (cmd %#x)\n", rd(camcc + MCLK_CMD_RCGR));
		give_up("an RCG that does not update");
		return false;
	}
	if (!camcc_put(MCLK_CBCR, found_mclk_cbcr | 1, 1)) {
		return false;
	}
	if (!wait_bits(MCLK_CBCR, CBCR_OFF, 0, 500)) {
		say("camera: MCLK's branch does not come on (%#x)\n", rd(camcc + MCLK_CBCR));
		give_up("a clock that does not come on");
		return false;
	}
	wr(GPIO_CFG(hw->mclk_pin), CFG_FUNCTION(1) | CFG_DRIVE_MA(4));    /* cam_mclk, no pull, 4 mA: cam_macro_mclk_active, cam_wide_mclk_active */
	say("camera: MCLK on at %u.%u MHz (RCG cfg %#x, branch %#x)\n", khz / 1000, khz % 1000 / 100, rd(camcc + MCLK_CFG_RCGR),
	    rd(camcc + MCLK_CBCR));
	return true;
}

static void
mclk_off(void)
{
	if (!mclk_switched) {
		return;
	}
	if (pins_found) {
		wr(GPIO_CFG(hw->mclk_pin), found_mclk_cfg);
	}
	wr(camcc + MCLK_CBCR, rd(camcc + MCLK_CBCR) & ~1u);
	(void)wait_bits(MCLK_CBCR, CBCR_OFF, CBCR_OFF, 500);
	wr(camcc + MCLK_CFG_RCGR, found_mclk_cfg_rcgr);
	wr(camcc + MCLK_CMD_RCGR, rd(camcc + MCLK_CMD_RCGR) | CMD_UPDATE);
	(void)wait_bits(MCLK_CMD_RCGR, CMD_UPDATE, 0, 500);
	if (pll2_switched) {
		wr(camcc + MCLK_M, found_mclk_mnd[0]);
		wr(camcc + MCLK_N, found_mclk_mnd[1]);
		wr(camcc + MCLK_D, found_mclk_mnd[2]);
		pll2_down();
	}
	mclk_switched = false;
	say("camera: MCLK off (branch %#x, RCG cfg %#x as found)\n", rd(camcc + MCLK_CBCR), rd(camcc + MCLK_CFG_RCGR));
}

/* ---- the sensor's tables, its power and its ID ---------------------------------------------------------- */

/*
 * The sensor's tables are Android's, read at run time from fw/ of the small image, where
 * userland/build.sh puts what components/camera/tools/camsensor.py makes of the sensor's module file ("RWCTAB02"):
 * the slave address and the chip ID's read, the lane assignment, the modes, the power sequences
 * (Android's cam_sensor kinds: 0 MCLK, 1 VANA, 2 VDIG, 3 VIO, 8 RESET, with their delays) and the
 * register tables (init, res0.., on, off, ...).
 */
struct ctab_head {
	char            magic[8];       /* "RWCTAB02" */
	uint32_t        slave, address_bytes, data_bytes, id_address, id, id_mask, lane_assign, modes;
};
struct ctab_mode {
	uint32_t        width, height, x, y, lanes, cphy, settle, pixel_clock, line_length, frame_length, fps_milli, data_type;
};
struct ctab_entry {
	uint16_t        address, data;  /* (a power sequence's: kind, value -- MCLK in kHz) */
	uint8_t         address_bytes, data_bytes, operation, zero;
	uint32_t        delay_us;
};

static const struct ctab_head *ctab;
static const struct ctab_mode *ctab_modes;

/* the sensor the rest works on: its descriptor, its CCI master's pins (its tables are loaded afresh: ctab_load()) */
static void
select_sensor(unsigned i)
{
	hw_index = i;
	hw = &sensors[i];
	cci_bus.name = hw->bus_name;
	cci_bus.pins.sda = hw->sda;
	cci_bus.pins.scl = hw->scl;
	ctab = NULL;
}
static const uint8_t *ctab_power, *ctab_tables;
static uint64_t ctab_size;

static bool
ctab_load(void)
{
	const uint8_t *at, *end;

	ctab = (const struct ctab_head *)hakios_wifi_file(hw->tab, &ctab_size);
	if (ctab == NULL || ctab_size < sizeof(*ctab) || memcmp(ctab->magic, "RWCTAB02", 8) != 0 || ctab->modes == 0 ||
	    ctab->modes > 16) {
		say("camera: no %s (RWCTAB02) in fw/ of the small image (made from fw-camera/ when it is built)\n", hw->tab);
		ctab = NULL;
		return false;
	}
	end = (const uint8_t *)ctab + ctab_size;
	ctab_modes = (const struct ctab_mode *)(ctab + 1);
	ctab_power = (const uint8_t *)(ctab_modes + ctab->modes);
	at = ctab_power;
	for (int p = 0; p < 2; p++) {
		if (at + 12 > end) {
			ctab = NULL;
			return false;
		}
		at += 12 + *(const uint32_t *)(at + 8) * sizeof(struct ctab_entry);
	}
	ctab_tables = at;
	if (ctab_tables + 4 > end) {
		ctab = NULL;
		return false;
	}
	say("camera: the %s sensor's tables: slave %#x, chip ID %#x at %#x, lanes %#x, %u modes, %u tables\n", hw->name, ctab->slave,
	    ctab->id, ctab->id_address, ctab->lane_assign, ctab->modes, *(const uint32_t *)ctab_tables);
	for (uint32_t m = 0; m < ctab->modes; m++) {
		const struct ctab_mode *o = &ctab_modes[m];

		say("camera:   mode %u: %ux%u at %u,%u, %u %s, settle %u, pixel clock %u, line %u, frame %u, %u.%03u frames/s\n", m, o->width,
		    o->height, o->x, o->y, o->lanes, o->cphy ? "trio(s), C-PHY" : "lane(s), D-PHY", o->settle, o->pixel_clock,
		    o->line_length, o->frame_length, o->fps_milli / 1000, o->fps_milli % 1000);
	}
	return true;
}

static const struct ctab_entry *
ctab_block(const uint8_t *at, uint32_t blocks, const char *name, uint32_t *count)
{
	const uint8_t *end = (const uint8_t *)ctab + ctab_size;

	for (uint32_t t = 0; t < blocks && at + 12 <= end; t++) {
		uint32_t n = *(const uint32_t *)(at + 8);

		if (strncmp((const char *)at, name, 8) == 0 && at + 12 + n * sizeof(struct ctab_entry) <= end) {
			*count = n;
			return (const struct ctab_entry *)(at + 12);
		}
		at += 12 + n * sizeof(struct ctab_entry);
	}
	*count = 0;
	return NULL;
}

static const struct ctab_entry *
ctab_find(const char *name, uint32_t *count)
{
	return ctab == NULL ? NULL : ctab_block(ctab_tables + 4, *(const uint32_t *)ctab_tables, name, count);
}

static uint8_t
sensor_address(void)
{
	return (uint8_t)(ctab->slave >> 1);    /* (the module file's is the 8-bit one) */
}

static bool chip_enable_switched, powered, reset_driven;

/*
 * The pins of the GPIO-switched supplies as they were found, and whether each was switched: one each of the
 * sensor's VANA, VDIG and VIO (supply_gpio()). A sensor may have more than one so switched (tools/boards.py takes
 * any of the three as a pin, each its own): with one record for all, the second switched on wrote over what was
 * found of the first, the first was put back as the second had been found, and the second was left on.
 */
static uint32_t found_supply_cfg[3], found_supply_io[3];
static bool supply_gpio_switched[3];

static unsigned
supply_gpio(const struct hakios_camera_supply *s)
{
	return s == &hw->vana ? 0 : s == &hw->vdig ? 1 : 2;
}

/* A GPIO-switched supply (the wide sensor's VANA, the ultra-wide's VDIG): the pin a high output (GPIO, no pull, 2 mA: cam_sensor_active_*). */
static bool
supply_on(const struct hakios_camera_supply *s)
{
	if (s->kind == HAKIOS_SUPPLY_LDO) {
		return ldo_on(s->ldo, s->mv);
	}
	if (s->kind == HAKIOS_SUPPLY_GPIO) {
		unsigned which = supply_gpio(s);

		if (s->gpio >= TLMM_SIZE / 0x1000) {
			give_up("a supply's pin past the pins there are");     /* (tools/boards.py says 0 to 255; the table is not trusted past it) */
			return false;
		}
		found_supply_cfg[which] = rd(GPIO_CFG(s->gpio));
		found_supply_io[which] = rd(GPIO_IO(s->gpio));
		supply_gpio_switched[which] = true;
		wr(GPIO_IO(s->gpio), 2);
		wr(GPIO_CFG(s->gpio), CFG_OUTPUT | CFG_DRIVE_MA(2));
		if ((rd(GPIO_CFG(s->gpio)) & 0x3ff) != (CFG_OUTPUT | CFG_DRIVE_MA(2)) || !(rd(GPIO_IO(s->gpio)) & 2)) {
			give_up("a supply's pin that does not read back");
			return false;
		}
		spin_us(s->ramp_us);
		say("camera: gpio%u high: the fixed %u.%u V regulator on\n", s->gpio, s->mv / 1000, s->mv % 1000 / 100);
		return true;
	}
	give_up("a supply this sensor does not have");
	return false;
}

static void
supply_off(const struct hakios_camera_supply *s)
{
	if (s->kind == HAKIOS_SUPPLY_LDO) {
		ldo_off(s->ldo);
	} else if (s->kind == HAKIOS_SUPPLY_GPIO && supply_gpio_switched[supply_gpio(s)]) {
		unsigned which = supply_gpio(s);

		wr(GPIO_IO(s->gpio), 0);
		spin_us(10);
		wr(GPIO_IO(s->gpio), found_supply_io[which] & 2);
		wr(GPIO_CFG(s->gpio), found_supply_cfg[which]);
		supply_gpio_switched[which] = false;
		say("camera: gpio%u back as it was found: the fixed regulator off\n", s->gpio);
	}
}

static void
sensor_delay(uint32_t us)
{
	if (us >= 1000) {
		wait_ms(us / 1000);
		us %= 1000;
	}
	spin_us(us);
}

/* One step of the module file's power sequence (`up': part of the power-up, where a fault stops it). */
static bool
power_step(const struct ctab_entry *e, bool up)
{
	const struct hakios_camera_supply *s = e->address == 1 ? &hw->vana : e->address == 2 ? &hw->vdig : e->address == 3 ? &hw->vio : NULL;

	switch (e->address) {
	case 8:
		reset_driven = true;
		reset_pin(e->data != 0);
		break;
	case 0:
		if (e->data != 0 && e->data != 19200 && e->data != 24000) {
			give_up("an MCLK rate not known here");
			return false;
		}
		if (e->data != 0) {
			if (!up || !gdsc_on() || !mclk_on(e->data)) {
				return false;
			}
		} else {
			mclk_off();
		}
		break;
	case 1: case 2: case 3:
		if (s->kind == HAKIOS_SUPPLY_NONE) {
			give_up("a supply the device tree does not give this sensor");
			return false;
		}
		if (e->data != 0) {
			if (!up || !supply_on(s)) {
				return false;
			}
		} else {
			supply_off(s);
		}
		break;
	default:
		say("camera: a power step of kind %u, which this does not know\n", e->address);
		give_up("a power sequence not understood");
		return false;
	}
	sensor_delay(e->delay_us);
	return true;
}

/* Everything that was switched on, off again: the module file's power-down, then the rest as it was found. */
static void
power_down(void)
{
	bool was = powered || mclk_switched || gdsc_switched || reset_driven || chip_enable_switched || pgpio_changed ||
	    supply_gpio_switched[0] || supply_gpio_switched[1] || supply_gpio_switched[2];
	uint32_t count = 0;
	const struct ctab_entry *e = ctab != NULL ? ctab_block(ctab_power, 2, "pwrdn", &count) : NULL;

	for (unsigned n = 1; n <= PM8008_LDOS; n++) {
		was |= ldo_switched[n];
	}
	if (!was) {
		return;
	}
	say("camera: powering down\n");
	for (uint32_t i = 0; e != NULL && i < count; i++) {
		(void)power_step(&e[i], false);
	}
	/* (whatever the sequence did not reach) */
	mclk_off();
	if (reset_driven) {
		reset_pin(false);
	}
	supply_off(&hw->vio);
	supply_off(&hw->vana);
	supply_off(&hw->vdig);
	gdsc_off();
	/*
	 * The PM8008i itself stays as it is between one use of the camera and the next: out of reset and
	 * enabled, with its LDOs off (they are switched above). Put back into reset it does come out
	 * again -- it answers for its revision at 0x08 -- but its regulators are deaf from then on (a
	 * write to 0x09 reads back 0xff), and the camera is gone for the rest of the run. It is put back
	 * as it was found when the kernel leaves (hakios_camera_quiesce), which is where Android gets
	 * the phone back.
	 */
	if (leaving && chip_enable_switched) {
		uint8_t enable = 0;

		if (pm8008_get(PM8008_CHIP, MISC_CHIP_ENABLE, &enable, 1) &&
		    pm8008_put(PM8008_CHIP, MISC_CHIP_ENABLE, (uint8_t)(enable & ~1), true)) {
			say("camera: the PM8008i's chip enable back off\n");
		} else {
			say("camera: the PM8008i's chip enable COULD NOT BE PUT BACK\n");
		}
		chip_enable_switched = false;
	}
	if (leaving && pgpio_changed) {
		uint8_t now[5] = { 0 };
		bool all = pgpio_read_all(now);

		for (int i = 4; all && i >= 0; i--) {
			if (now[i] != pgpio_found_regs[i]) {
				all &= pgpio_put(pgpio_which[i], pgpio_found_regs[i], true);
			}
		}
		say("camera: the PM8008i's reset GPIO %s\n", all ? "back as it was found" : "COULD NOT BE PUT BACK");
		pgpio_changed = false;
	}
	if (reset_driven && pins_found) {
		wr(GPIO_IO(hw->reset), found_reset_io & 2);
		wr(GPIO_CFG(hw->reset), found_reset_cfg);
		reset_driven = false;
	}
	powered = false;
	if (pm8008_retries != 0) {
		say("camera: powered down (the PM8008i needed %llu more goes on its I2C)\n", pm8008_retries);
	} else {
		say("camera: powered down\n");
	}
}

/* The PM8008i out of reset (its reset pin as Android drives it) and enabled. */
static bool
pm8008_up(void)
{
	uint8_t regs[5], revid[4] = { 0 }, enable = 0;

	if (!pgpio_read_all(regs)) {
		give_up("the PM8008i's reset GPIO cannot be read");
		return false;
	}
	if (!pgpio_changed) {           /* (as Android left it, which is what the run's end puts back) */
		memcpy(pgpio_found_regs, regs, sizeof(regs));
	}
	if (!pgpio_active(regs)) {
		uint8_t want[5] = { (uint8_t)(regs[0] & ~7), (uint8_t)((regs[1] & ~7) | 5), (uint8_t)((regs[2] & ~3) | 1), 0x80,
			            (uint8_t)(regs[4] | 0x80) };

		say("camera: the PM8008i is held in reset (GPIO 3 vin, pull, mode, source, enable: %02x %02x %02x %02x %02x): let out as "
		    "Android lets it out\n", regs[0], regs[1], regs[2], regs[3], regs[4]);
		pgpio_changed = true;
		for (int i = 0; i < 5; i++) {
			/* (only what differs, in the order Android's pinctrl driver writes them: the enable last) */
			if (want[i] != regs[i] && !pgpio_put(pgpio_which[i], want[i], false)) {
				return false;
			}
		}
		wait_ms(5);
	}
	if (!pm8008_get(PM8008_CHIP, REVID_BASE, revid, 4)) {
		give_up("the PM8008i does not answer");
		return false;
	}
	say("camera: the PM8008i answers: revision %02x %02x %02x %02x\n", revid[0], revid[1], revid[2], revid[3]);
	if (!pm8008_get(PM8008_CHIP, MISC_CHIP_ENABLE, &enable, 1)) {
		give_up("the PM8008i does not answer");
		return false;
	}
	if (!(enable & 1)) {
		if (!pm8008_put(PM8008_CHIP, MISC_CHIP_ENABLE, (uint8_t)(enable | 1), false)) {
			return false;
		}
		chip_enable_switched = true;
		say("camera: the PM8008i's chip enabled\n");
	}
	return true;
}

/* The module file's power-up, step by step. */
static bool
power_up(void)
{
	uint32_t count = 0;
	const struct ctab_entry *e = ctab != NULL ? ctab_block(ctab_power, 2, "pwrup", &count) : NULL;

	if (e == NULL) {
		give_up("no power sequence");
		return false;
	}
	powered = true;
	if (!pm8008_up()) {
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		if (!power_step(&e[i], true)) {
			return false;
		}
	}
	say("camera: the %s sensor powered up (%u steps)\n", hw->name, count);
	return true;
}

static bool
read_chip_id(void)
{
	uint8_t where[2] = { (uint8_t)(ctab->id_address >> 8), (uint8_t)ctab->id_address }, id[4] = { 0, 0, 0, 0 };
	unsigned out = ctab->address_bytes == 2 ? 2 : 1, in = ctab->data_bytes >= 1 && ctab->data_bytes <= 4 ? ctab->data_bytes : 2;
	uint32_t chip = 0;

	/*
	 * The first word asked of the sensor after its supplies have come up. Powered up again within a
	 * run -- the capture graph stops and starts the stream as the application reconfigures its
	 * session -- it is not always ready by the time the power sequence's own waits are over, and one
	 * unanswered transfer used to end the camera for the rest of the run. It is asked again, for as
	 * long as the sensor's own data sheet would have it waiting.
	 */
	unsigned go;

	for (go = 0; go < 20; go++) {
		if (i2c_transfer(&cci_bus, sensor_address(), out == 2 ? where : where + 1, out, id, in)) {
			break;
		}
		wait_ms(2);
	}
	if (go == 20) {
		say("camera: the %s sensor does not answer at 0x%02x\n", hw->name, sensor_address());
		return false;
	}
	if (go != 0) {
		say("camera: the %s sensor answered at 0x%02x after %u more goes (%u ms)\n", hw->name, sensor_address(), go, go * 2);
	}
	for (unsigned i = 0; i < in; i++) {
		chip = chip << 8 | id[i];
	}
	say("camera: the %s sensor's chip ID: 0x%04x (%s)\n", hw->name, chip, (chip & ctab->id_mask) == (ctab->id & ctab->id_mask) ?
	    "as Android expects" : "NOT what Android expects");
	return (chip & ctab->id_mask) == (ctab->id & ctab->id_mask);
}

/*
 * While powered up: the LDOs switched on still ready and not over their current. An over-current is
 * asked again 2 ms on before it counts: the lens motor's moves show one on LDO5 for an instant (at a
 * move of a few hundred steps, now and then), and the camera went on as before every time it was not
 * looked at then.
 */
static bool
healthy(void)
{
	for (unsigned n = 1; n <= PM8008_LDOS; n++) {
		uint8_t status = 0;

		if (!ldo_switched[n]) {
			continue;
		}
		for (int look = 0; look < 2; look++) {
			if (!pm8008_get(PM8008_REGULATORS, (uint16_t)(LDO_BASE(n) + LDO_STATUS1), &status, 1) || !(status & 0x80)) {
				break;
			}
			if (!(status & 0x20)) {
				break;
			}
			if (look == 0) {
				say("camera: PM8008i LDO%u: status 0x%02x, asked again\n", n, status);
				wait_ms(2);
			}
		}
		if (!(status & 0x80) || (status & 0x20)) {
			say("camera: PM8008i LDO%u: status 0x%02x\n", n, status);
			give_up("an LDO that is no longer well");
			return false;
		}
	}
	return true;
}

/* ---- the camera subsystem's surroundings, read only: rails, the SMMU, camcc's PLLs ------------------- */

/*
 * What the camera's blocks need beyond the sensor (a look first, nothing written): the CX and MX
 * rails' levels (camcc's clocks at Android's rates want CX at SVS and, for PLL0's 1.2 GHz, MX at
 * SVS_L1), asked of RPMh by level positions from the boot loader's command database; what the
 * application processors' RPMh sequencer (DRV 2) holds for them; the two rails' set points in their
 * regulators (PM8350C S2 and S10); what the SMMU has for the IFEs' streams (dt: 0x800 and 0xc00 with
 * the mask 0x4e0); camcc's PLL0 and PLL6.
 */
#define CMD_DB_PHYS             (hakios_board()->rpmh.cmd_db)           /* the SoC file's rpmh { } */
#define CMD_DB_SIZE             (hakios_board()->rpmh.cmd_db_size)
#define RSC_PHYS                (hakios_board()->rpmh.rsc)      /* DRV 2's (dt: rsc@18200000, drv-id 2) */
#define RSC_TCS_BASE            (hakios_board()->rpmh.tcs_offset)
#define RSC_TCS(n)              (RSC_TCS_BASE + 672 * (n))
#define TCS_CONTROL             0x14
#define TCS_STATUS              0x18
#define TCS_ENABLED             0x1c
#define TCS_COMMAND(c)          (0x30 + 20 * (c))       /* message id, address, value, status */
#define RSC_TCS_COUNT           8               /* dt qcom,tcs-config: 2 active, 3 sleep, 3 wake */

#include "cmd_db.h"

static const struct cmd_db_header *cmd_db;
static vm_offset_t rsc;

/* RPMh's address for a resource, and its levels (as many as `max'); 0: not there. */
static uint32_t
rpmh_resource(const char *name, uint16_t *levels, unsigned max, unsigned *count)
{
	if (cmd_db == NULL) {
		cmd_db = (const struct cmd_db_header *)ml_io_map_unmappable(CMD_DB_PHYS, CMD_DB_SIZE, VM_WIMG_WCOMB);
	}
	return hakios_cmd_db_levels(cmd_db, CMD_DB_SIZE, name, levels, max, count);
}

static void
show_rail(const char *name, uint32_t *address)
{
	uint16_t levels[16];
	unsigned count;
	char line[200];
	int at;

	*address = rpmh_resource(name, levels, 16, &count);
	at = snprintf(line, sizeof(line), "camera: RPMh's %s at %#x, levels:", name, *address);
	for (unsigned l = 0; l < count && at < (int)sizeof(line) - 8; l++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %u", levels[l]);
	}
	say("%s\n", line);
	/* what DRV 2's sequencer holds for it, in any of its TCSs */
	for (unsigned t = 0; *address != 0 && t < RSC_TCS_COUNT; t++) {
		uint32_t enabled = rd(rsc + RSC_TCS(t) + TCS_ENABLED);

		for (unsigned c = 0; c < 16; c++) {
			uint32_t where = rd(rsc + RSC_TCS(t) + TCS_COMMAND(c) + 4);

			if (where == *address) {
				say("camera:   TCS %u command %u: value %u (position), %senabled, message id %#x, status %#x\n", t, c,
				    rd(rsc + RSC_TCS(t) + TCS_COMMAND(c) + 8), (enabled >> c) & 1 ? "" : "not ",
				    rd(rsc + RSC_TCS(t) + TCS_COMMAND(c)), rd(rsc + RSC_TCS(t) + TCS_COMMAND(c) + 12));
			}
		}
	}
}

#define SMMU_PHYS               ((unsigned long)hakios_board()->smmu.apps)      /* the SoC file's smmu { } */
#define IFE_STREAM              (hakios_board()->smmu.camera_stream[0])     /* the SoC file's smmu { } */
#define IFE_STREAM_MASK         (hakios_board()->smmu.camera_stream[1])

static vm_offset_t smmu;

static void
show_smmu(void)
{
	uint32_t scr0 = rd(smmu + SMMU_SCR0), idr0 = rd(smmu + SMMU_IDR0), idr1 = rd(smmu + SMMU_IDR1), free_matches = 0;
	bool used[256] = { false };
	unsigned contexts = idr1 & 0xff, free_contexts = 0;

	for (uint32_t n = 0; n < (idr0 & 0xff) && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(smmu + SMMU_SMR(n)), s2cr = rd(smmu + SMMU_S2CR(n));
		uint32_t stream = smr & 0x7fff, mask = smr >> 16 & 0x7fff;

		if (!(smr >> 31)) {
			free_matches++;
			continue;
		}
		used[s2cr & 0xff] = true;
		for (uint32_t ife = 0; ife < 0x10; ife++) {
			uint32_t sid = (ife < 8 ? 0x800 : 0xc00) + 0x20 * (ife & 7);

			if (((stream ^ sid) & ~mask) == 0) {
				say("camera: SMMU match register %u (stream %#x mask %#x, s2cr %#x) covers the IFEs' stream %#x\n", n, stream,
				    mask, s2cr, sid);
				break;
			}
		}
	}
	for (unsigned c = 0; c < contexts; c++) {
		free_contexts += !used[c];
	}
	say("camera: the SMMU: scr0 %#x (a stream nobody matches %s), %u match registers free, %u of %u contexts unused by them\n", scr0,
	    scr0 & (1u << 10) ? "faults" : "passes", free_matches, free_contexts, contexts);
}

static void
show_plls(void)
{
	for (unsigned p = 0; p <= 6; p += 6) {
		vm_offset_t pll = camcc + 0x1000UL * p;

		say("camera: camcc PLL%u: mode %#x, l %#x, cal_l %#x, user_ctl %#x %#x, config_ctl %#x %#x %#x, opmode %#x, status %#x, "
		    "alpha %#x\n", p, rd(pll + 0x00), rd(pll + 0x04), rd(pll + 0x08), rd(pll + 0x0c), rd(pll + 0x10), rd(pll + 0x18),
		    rd(pll + 0x1c), rd(pll + 0x20), rd(pll + 0x38), rd(pll + 0x30), rd(pll + 0x40));
	}
}

static void
look_around(void)
{
	uint32_t cx = 0, mx = 0;

	if (rsc == 0) {
		rsc = ml_io_map(RSC_PHYS, 0x10000);
		smmu = ml_io_map(SMMU_PHYS, 0x1000);
	}
	show_rail("cx.lvl", &cx);
	show_rail("mx.lvl", &mx);
	show_smmu();
	show_plls();
}

/* ---- the camera subsystem (Titan 165): rails, PLL0, clocks ------------------------------------------- */

/*
 * cam=3: the path a picture takes, tried first with the CSID's own test pattern generator, so that
 * nothing outside the SoC is involved: CSID-lite 0 (0xacc8000, "csid-lite165": the register layout
 * of Android's cam_ife_csid_lite17x.h) makes RAW10 frames of a colour bar and sends them down its
 * RDI 0 path to IFE-lite 0 (0xacc4000, "vfe-lite165": cam_vfe_lite16x.h, the bus of
 * cam_vfe_bus_ver2.c), whose write master 0 writes each frame as it comes (Android's RDI frame mode)
 * to memory through the SMMU, as stream 0x800 with the mask 0x4e0 (dt: msm_cam_smmu_ife), into a
 * context of ours. The memory is one physically contiguous piece that the loader hands to the host
 * after the run ("the first generation's xl camera"; components/camera/tools/camraw.py makes pictures of it).
 *
 * What it takes, in Android's order and with Android's numbers (camcc-yupik.c, gcc-yupik.c, the
 * device tree's clock rates at the "svs" level of csid-lite 0, the sensor's csiphy and cpas):
 *   RPMh   CX at SVS (128) for camcc's clocks at 400 MHz, MX at SVS_L1 (192) for PLL0's 1.2 GHz --
 *          asked through DRV 2's second TCS for requests made at once, as camcc's vdd_cx and vdd_mx
 *          votes are, and given back (position 0) at the end: DRV 2 holds no other vote on them
 *          (cam=1 looks: none of its TCSs has a command for either)
 *   GCC    camera_hf_axi, camera_sf_axi (where each is: the SoC file's camera { }, as all the clocks below)
 *   camcc  PLL0 (Lucid, as the SoC file's pll0-config: 1200 MHz on an SM7325), then the RCGs
 *          slow_ahb 80 MHz, camnoc_axi 150, cphy_rx 400, csiNphytimer 300, ife_lite_0_csid 400,
 *          ife_lite_0 400 (all from PLL0's even (600) or odd (400) output) and the branches
 *          cpas_ahb, camnoc_axi, csiphyN, csiNphytimer, ife_lite_0_csid, ife_lite_0_cphy_rx,
 *          ife_lite_0 (the clocks the device tree gives cpas, the csiphy and csid-lite 0; N is the
 *          sensor's CSIPHY: 2 for the macro one and for the test pattern, 1 for the wide one)
 * Everything is taken back in the reverse order at the end, and on any fault.
 */
#define RSC_IRQ_STATUS          (RSC_TCS_BASE + 0x04)
#define RSC_IRQ_CLEAR           (RSC_TCS_BASE + 0x08)
#define TCS_WAIT_FOR_DONE       0x10
#define TCS_ACTIVE_MODE         (1u << 16)
#define TCS_TRIGGER             (1u << 24)
#define MSGID_WRITE_WAITED      (8u | 1u << 8 | 1u << 16)
#define CAMERA_TCS              1                       /* the second TCS for requests made at once (the audio DSP's driver takes the first) */
#define CX_LEVEL                128                     /* SVS */
#define MX_LEVEL                192                     /* SVS_L1 */

static bool rails_voted;

/* The position of the first level at or above `level' in RPMh's list for that rail; -1: not there. */
static int
rail_position(const char *name, uint32_t level, uint32_t *address)
{
	uint16_t levels[16];
	unsigned count;

	*address = rpmh_resource(name, levels, 16, &count);
	for (unsigned l = 0; *address != 0 && l < count; l++) {
		if (levels[l] >= level) {
			return (int)l;
		}
	}
	return -1;
}

/*
 * What became of a request to RPMh (rpmh_write()). One that was triggered but not answered may well be in force
 * all the same (as board/ddr.c takes it): what sent it marks it voted, and gives it back as it gives back one
 * that was answered.
 */
enum rpmh_sent {
	RPMH_NOT_SENT,          /* the TCS was busy: nothing was sent */
	RPMH_NO_ANSWER,         /* triggered, but RPMh did not say done within 100 ms */
	RPMH_DONE,              /* RPMh has said done */
};

/* Up to three writes to RPMh's resources through the camera's TCS, waited for. */
static enum rpmh_sent
rpmh_write(unsigned n, const uint32_t *addresses, const uint32_t *values)
{
	vm_offset_t tcs = rsc + RSC_TCS(CAMERA_TCS);
	uint32_t done = 0, all = (1u << n) - 1;

	if (rd(tcs + TCS_STATUS) == 0) {
		say("camera: TCS %u of the sequencer is busy (control %#x): nothing sent\n", CAMERA_TCS, rd(tcs + TCS_CONTROL));
		return RPMH_NOT_SENT;
	}
	wr(rsc + RSC_IRQ_CLEAR, 1u << CAMERA_TCS);
	for (unsigned r = 0; r < n; r++) {
		wr(tcs + TCS_COMMAND(r), MSGID_WRITE_WAITED);
		wr(tcs + TCS_COMMAND(r) + 4, addresses[r]);
		wr(tcs + TCS_COMMAND(r) + 8, values[r]);
	}
	wr(tcs + TCS_WAIT_FOR_DONE, all);
	wr(tcs + TCS_ENABLED, all);
	wr(tcs + TCS_CONTROL, rd(tcs + TCS_CONTROL) & ~TCS_TRIGGER);
	wr(tcs + TCS_CONTROL, rd(tcs + TCS_CONTROL) | TCS_ACTIVE_MODE);
	wr(tcs + TCS_CONTROL, rd(tcs + TCS_CONTROL) | TCS_TRIGGER);
	/* (each command's status: [8] issued, [16] completed -- the TCS's interrupt may not be enabled for this one) */
	for (unsigned waited = 0; waited < 100000 && !done; waited += 50) {
		unsigned completed = 0;

		for (unsigned r = 0; r < n; r++) {
			completed += (rd(tcs + TCS_COMMAND(r) + 12) >> 16) & 1;
		}
		done = (rd(rsc + RSC_IRQ_STATUS) & (1u << CAMERA_TCS)) || completed == n;
		if (!done) {
			spin_us(50);
		}
	}
	say("camera: RPMh's TCS %u: interrupt status %#x (enabled %#x), the commands' status %#x %#x %#x\n", CAMERA_TCS,
	    rd(rsc + RSC_IRQ_STATUS), rd(rsc + RSC_TCS_BASE), rd(tcs + TCS_COMMAND(0) + 12), n > 1 ? rd(tcs + TCS_COMMAND(1) + 12) : 0,
	    n > 2 ? rd(tcs + TCS_COMMAND(2) + 12) : 0);
	wr(rsc + RSC_IRQ_CLEAR, 1u << CAMERA_TCS);
	wr(tcs + TCS_CONTROL, rd(tcs + TCS_CONTROL) & ~(TCS_TRIGGER | TCS_ACTIVE_MODE));
	wr(tcs + TCS_ENABLED, 0);
	return done != 0 ? RPMH_DONE : RPMH_NO_ANSWER;
}

/*
 * CX and MX at the camera's levels, or given back (`on' false). True: RPMh has said done. The vote is marked
 * (rails_voted) once it was sent, answered or not, and the mark goes only when RPMh has said done to its
 * withdrawal: one that was not answered is made again (titan_down(), and at the run's end).
 */
static bool
rails(bool on)
{
	uint32_t addresses[2], values[2];
	int cx = rail_position("cx.lvl", CX_LEVEL, &addresses[0]), mx = rail_position("mx.lvl", MX_LEVEL, &addresses[1]);
	enum rpmh_sent sent;
	bool done;

	if (cx < 0 || mx < 0) {
		say("camera: RPMh has no position for CX %u or MX %u\n", CX_LEVEL, MX_LEVEL);
		return false;
	}
	values[0] = on ? (uint32_t)cx : 0;
	values[1] = on ? (uint32_t)mx : 0;
	sent = rpmh_write(2, addresses, values);
	done = sent == RPMH_DONE;
	if (on && sent != RPMH_NOT_SENT) {
		rails_voted = true;
	} else if (!on && done) {
		rails_voted = false;
	}
	say("camera: RPMh %s CX (%#x) position %u and MX (%#x) position %u%s\n", done ? "took" : sent == RPMH_NOT_SENT ? "WAS NOT ASKED FOR" :
	    "DID NOT SAY IT TOOK", addresses[0], values[0], addresses[1], values[1], on ? "" : rails_voted ? " (given back: to be asked again)" :
	    " (given back)");
	return done;
}

/* camcc's PLL0 (Lucid; where it is: the SoC file's camera { } pll0): the registers of clk-alpha-pll.c's Lucid layout */
#define PLL0                    (CAMERA.pll0)
#define PLL_MODE                0x00            /* [0] outputs, [2] out of reset, [23] update bypass, [31] locked */
#define PLL_L_VAL               0x04
#define PLL_CAL_L_VAL           0x08
#define PLL_USER_CTL            0x0c            /* [2:0] the outputs: main, even, odd */
#define PLL_USER_CTL_U          0x10
#define PLL_USER_CTL_U1         0x14
#define PLL_CONFIG_CTL          0x18
#define PLL_CONFIG_CTL_U        0x1c
#define PLL_CONFIG_CTL_U1       0x20
#define PLL_OPMODE              0x38            /* 0 standby, 1 run */
#define PLL_ALPHA_VAL           0x40

static bool pll_on;

/* PLL0's rate: L and the fraction alpha / 2^16 times the crystal (pll0-config's first and third values) */
static uint32_t
pll0_khz(void)
{
	return CAMERA.pll0_config[0] * XO_KHZ + (uint32_t)(((uint64_t)CAMERA.pll0_config[2] * XO_KHZ) >> 16);
}

/*
 * ... and of its even and odd outputs, divided as USER_CTL's [11:8] and [15:12] say (clk-alpha-pll.c's Lucid
 * post-dividers: the even one's field is the divider less one, the odd one's the divider itself; 2 and 3 on
 * an SM7325)
 */
static uint32_t
pll0_output_khz(bool odd)
{
	uint32_t field = (CAMERA.pll0_config[6] >> (odd ? 12 : 8)) & 0xf;

	return pll0_khz() / (odd ? (field ? field : 1) : field + 1);
}

static bool
camcc_any(uint32_t offset, uint32_t value, uint32_t check)
{
	wr(camcc + offset, value);
	if ((rd(camcc + offset) & check) != (value & check)) {
		say("camera: camcc %#x <- %#x, but it reads %#x\n", offset, value, rd(camcc + offset));
		give_up("a camcc register that did not read back");
		return false;
	}
	return true;
}

static bool
pll0_up(void)
{
	/* the SoC file's camera { } pll0-config, in this order (camcc-CODENAME.c's cam_cc_pll0_config) */
	static const uint32_t config[] = { PLL_L_VAL, PLL_CAL_L_VAL, PLL_ALPHA_VAL, PLL_CONFIG_CTL, PLL_CONFIG_CTL_U,
	                                   PLL_CONFIG_CTL_U1, PLL_USER_CTL, PLL_USER_CTL_U, PLL_USER_CTL_U1 };

	_Static_assert(sizeof(config) / sizeof(config[0]) == sizeof(CAMERA.pll0_config) / sizeof(CAMERA.pll0_config[0]),
	    "a value each");

	if (rd(camcc + PLL0 + PLL_MODE) & 1) {
		give_up("camcc's PLL0 is running already (somebody else's)");
		return false;
	}
	for (unsigned i = 0; i < sizeof(config) / sizeof(config[0]); i++) {
		if (!camcc_any(PLL0 + config[i], CAMERA.pll0_config[i], ~0u)) {
			return false;
		}
	}
	pll_on = true;
	/* clk_lucid_pll_configure(): update bypass, outputs off, standby, out of reset */
	if (!camcc_any(PLL0 + PLL_MODE, rd(camcc + PLL0 + PLL_MODE) | 1u << 23, 1u << 23) ||
	    !camcc_any(PLL0 + PLL_MODE, rd(camcc + PLL0 + PLL_MODE) & ~1u, 1) || !camcc_any(PLL0 + PLL_OPMODE, 0, 1) ||
	    !camcc_any(PLL0 + PLL_MODE, rd(camcc + PLL0 + PLL_MODE) | 1u << 2, 1u << 2)) {
		return false;
	}
	/* alpha_pll_lucid_enable(): run, locked, the outputs on */
	if (!camcc_any(PLL0 + PLL_OPMODE, 1, 1)) {
		return false;
	}
	if (!wait_bits(PLL0 + PLL_MODE, 1u << 31, 1u << 31, 1000)) {
		say("camera: camcc's PLL0 does not lock (mode %#x)\n", rd(camcc + PLL0 + PLL_MODE));
		give_up("a PLL that does not lock");
		return false;
	}
	if (!camcc_any(PLL0 + PLL_USER_CTL, rd(camcc + PLL0 + PLL_USER_CTL) | 7, 7) ||
	    !camcc_any(PLL0 + PLL_MODE, rd(camcc + PLL0 + PLL_MODE) | 1, 1)) {
		return false;
	}
	say("camera: camcc's PLL0 locked at %u MHz (mode %#x, user_ctl %#x)\n", pll0_khz() / 1000, rd(camcc + PLL0 + PLL_MODE),
	    rd(camcc + PLL0 + PLL_USER_CTL));
	return true;
}

static void
pll0_down(void)
{
	if (!pll_on) {
		return;
	}
	/* alpha_pll_lucid_disable(), then back into reset as it was found */
	wr(camcc + PLL0 + PLL_MODE, rd(camcc + PLL0 + PLL_MODE) & ~1u);
	wr(camcc + PLL0 + PLL_USER_CTL, rd(camcc + PLL0 + PLL_USER_CTL) & ~7u);
	wr(camcc + PLL0 + PLL_OPMODE, 0);
	wr(camcc + PLL0 + PLL_MODE, rd(camcc + PLL0 + PLL_MODE) & ~(1u << 2));
	pll_on = false;
	say("camera: camcc's PLL0 off (mode %#x)\n", rd(camcc + PLL0 + PLL_MODE));
}

/*
 * An RCG set (source, divider field 2n - 1, no M/N) and updated; then branches on. The rates are the
 * device tree's at the "svs" level; the source is one of PLL0's outputs (camera { } pll0-even-source,
 * pll0-odd-source) and the divider what makes the rate of it, in halves.
 */
struct rcg {
	const char *    name;
	uint32_t        khz;
	bool            odd;            /* from PLL0's odd output, not its even one */
	uint32_t        cmd, source, divider;
	uint32_t        found_cfg;
	bool            set;
};
static struct rcg rcgs[] = {                    /* (where each is: the SoC file's camera { }, in clocks_up()) */
	{ .name = "slow_ahb", .khz = 80000, .odd = false },            /* (an SM7325's PLL0 even, 600 MHz, / 7.5) */
	{ .name = "camnoc_axi", .khz = 150000, .odd = false },         /* / 4 */
	{ .name = "cphy_rx", .khz = 400000, .odd = false },            /* / 1.5 */
	{ .name = "csiNphytimer", .khz = 300000, .odd = false },       /* / 2 (the sensor's CSIPHY's) */
	{ .name = "ife_lite_0_csid", .khz = 400000, .odd = false },    /* / 1.5 */
	{ .name = "ife_lite_0", .khz = 400000, .odd = true },          /* (PLL0 odd, 400 MHz, / 1) */
};

/* each RCG's source and divider, of PLL0's outputs; false (and said which) when a rate is not to be had */
static bool
rcg_plan(void)
{
	for (unsigned i = 0; i < sizeof(rcgs) / sizeof(rcgs[0]); i++) {
		struct rcg *r = &rcgs[i];
		uint32_t parent = pll0_output_khz(r->odd), halves = 2 * parent / r->khz;

		r->source = r->odd ? CAMERA.pll0_odd_source : CAMERA.pll0_even_source;
		if (halves < 2 || halves > 32 || halves * r->khz != 2 * parent) {
			say("camera: %s at %u kHz is no whole number of halves of PLL0's %s output (%u kHz)\n", r->name, r->khz,
			    r->odd ? "odd" : "even", parent);
			return false;
		}
		r->divider = halves - 1;
	}
	return true;
}

struct branch {
	const char *    name;
	bool            gcc;
	uint32_t        cbcr;
	bool            set;
};
static struct branch branches[] = {            /* (the same) */
	{ .name = "gcc camera_hf_axi", .gcc = true }, { .name = "gcc camera_sf_axi", .gcc = true },
	{ .name = "cpas_ahb" }, { .name = "camnoc_axi" }, { .name = "csiphyN" }, { .name = "csiNphytimer" },
	{ .name = "ife_lite_0_csid" }, { .name = "ife_lite_0_cphy_rx" }, { .name = "ife_lite_0" },
};

static bool
clocks_up(void)
{
	const uint32_t rcg_at[] = { CAMERA.slow_ahb_rcg, CAMERA.camnoc_axi_rcg, CAMERA.cphy_rx_rcg, hw->timer_cmd,
	                            CAMERA.ife_csid_rcg, CAMERA.ife_rcg };
	const uint32_t branch_at[] = { CAMERA.gcc_hf_axi_branch, CAMERA.gcc_sf_axi_branch, CAMERA.cpas_ahb_branch,
	                               CAMERA.camnoc_axi_branch, hw->phy_cbcr, hw->timer_cbcr, CAMERA.ife_csid_branch,
	                               CAMERA.ife_cphy_rx_branch, CAMERA.ife_branch };

	_Static_assert(sizeof(rcg_at) / sizeof(rcg_at[0]) == sizeof(rcgs) / sizeof(rcgs[0]), "an RCG's place each");
	_Static_assert(sizeof(branch_at) / sizeof(branch_at[0]) == sizeof(branches) / sizeof(branches[0]), "a branch's each");
	if (!rcg_plan()) {
		give_up("a clock the PLL cannot make");
		return false;
	}
	for (unsigned i = 0; i < sizeof(rcgs) / sizeof(rcgs[0]); i++) {
		rcgs[i].cmd = rcg_at[i];
	}
	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		branches[i].cbcr = branch_at[i];
	}
	for (unsigned i = 0; i < sizeof(rcgs) / sizeof(rcgs[0]); i++) {
		struct rcg *r = &rcgs[i];
		uint32_t cfg = r->cmd + 4;

		r->found_cfg = rd(camcc + cfg);
		if (!camcc_any(cfg, (r->found_cfg & ~0x371fu) | r->source << 8 | r->divider, 0x371f)) {
			return false;
		}
		r->set = true;
		wr(camcc + r->cmd, rd(camcc + r->cmd) | CMD_UPDATE);
		if (!wait_bits(r->cmd, CMD_UPDATE, 0, 500)) {
			say("camera: the RCG %s (%u kHz) does not take its configuration (cmd %#x)\n", r->name, r->khz, rd(camcc + r->cmd));
			give_up("an RCG that does not update");
			return false;
		}
	}
	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		struct branch *b = &branches[i];
		vm_offset_t base = b->gcc ? gcc : camcc;

		if (rd(base + b->cbcr) & 1) {
			continue;               /* (on already: left on at the end too) */
		}
		wr(base + b->cbcr, rd(base + b->cbcr) | 1);
		if (!(rd(base + b->cbcr) & 1)) {
			say("camera: the branch %s does not take its enable (%#x)\n", b->name, rd(base + b->cbcr));
			give_up("a clock that does not read back");
			return false;
		}
		b->set = true;
		for (unsigned waited = 0; rd(base + b->cbcr) & CBCR_OFF; waited += 10) {
			if (waited > 500) {
				say("camera: the branch %s does not come on (%#x)\n", b->name, rd(base + b->cbcr));
				give_up("a clock that does not come on");
				return false;
			}
			spin_us(10);
		}
	}
	say("camera: the camera's clocks on (%u RCGs, %u branches)\n", (unsigned)(sizeof(rcgs) / sizeof(rcgs[0])),
	    (unsigned)(sizeof(branches) / sizeof(branches[0])));
	return true;
}

static void
clocks_down(void)
{
	for (int i = (int)(sizeof(branches) / sizeof(branches[0])) - 1; i >= 0; i--) {
		struct branch *b = &branches[i];
		vm_offset_t base = b->gcc ? gcc : camcc;

		if (b->set) {
			wr(base + b->cbcr, rd(base + b->cbcr) & ~1u);
			b->set = false;
		}
	}
	for (int i = (int)(sizeof(rcgs) / sizeof(rcgs[0])) - 1; i >= 0; i--) {
		struct rcg *r = &rcgs[i];

		if (r->set) {
			wr(camcc + r->cmd + 4, r->found_cfg);
			wr(camcc + r->cmd, rd(camcc + r->cmd) | CMD_UPDATE);
			(void)wait_bits(r->cmd, CMD_UPDATE, 0, 500);
			r->set = false;
		}
	}
	say("camera: the camera's clocks off\n");
}

/* ---- the frames' memory, and the SMMU ---------------------------------------------------------------- */

#define FRAME_ADDRESSES         0x40000000ull           /* the IFE's addresses for it (in dt's iova pool 0x100000-0xfff00000) */
#define CAM_HEADER_BYTES        4096
#define CAM_FRAMES              2
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define CAM_PTE                 (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)       /* as board/sound.c's */

/* What the loader hands to the host (components/camera/tools/camraw.py reads it). */
struct cam_header {
	char            magic[8];       /* "RWCAM001" */
	uint32_t        width, height;  /* pixels */
	uint32_t        stride;         /* bytes from one line to the next */
	uint32_t        format;         /* 1: MIPI RAW10, packed (four pixels in five bytes) */
	uint32_t        bayer;          /* 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR */
	uint32_t        frames;         /* how many were written whole */
	uint32_t        frame_bytes;    /* each, from CAM_HEADER_BYTES on */
	uint32_t        source;         /* 0: the CSID's test pattern, 1: the sensor */
	char            sensor[16];
	uint32_t        last;           /* which of the frames' places holds the last one written */
};

extern uint64_t loader_recording[3];
extern lck_mtx_t hakios_smmu_lock;

static vm_offset_t frames, smmu_second, smmu_context, page_tables;
static vm_offset_t keep;                /* memory in one piece, taken at boot (hakios_camera_init()) */
static uint64_t keep_bytes;
static uint64_t frames_bytes;
static uint32_t smmu_match = ~0u, smmu_context_n, smmu_old_smr, smmu_old_s2cr, smmu_pages_size, smmu_pages;
static bool smmu_on;
/*
 * The match register found free was written (smmu_up()): only then is it ours to put back. One found and not
 * taken -- no context left, no memory for the tables -- may be another driver's by the time the session ends
 * (the SMMU's lock is let go in between), and "putting it back" wrote the free register's old value over that
 * driver's stream.
 */
static bool smmu_match_taken;

static bool
frames_alloc(uint32_t width, uint32_t height, uint32_t stride, uint32_t source)
{
	struct cam_header *h;
	uint32_t frame_bytes = stride * height;

	frames_bytes = round_page(CAM_HEADER_BYTES + (uint64_t)CAM_FRAMES * frame_bytes);
	if (keep != 0 && keep_bytes >= frames_bytes) {
		frames = keep;                  /* (the piece taken at boot: hakios_camera_init()) */
		bzero((void *)frames, CAM_HEADER_BYTES);
	} else if (kmem_alloc_contig(kernel_map, &frames, frames_bytes, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("camera: no memory for %u frames (%llu KB)\n", CAM_FRAMES, frames_bytes >> 10);
		return false;
	}
	h = (struct cam_header *)frames;
	memcpy(h->magic, "RWCAM001", 8);
	h->width = width;
	h->height = height;
	h->stride = stride;
	h->format = 1;
	h->bayer = 0;                   /* (RGGB: the CSID's test pattern; a sensor's picture: capture_sensor()) */
	h->frame_bytes = frame_bytes;
	h->source = source;
	strlcpy(h->sensor, !source ? "CSID TPG" : hw->chip, sizeof(h->sensor));
	/* (what the processor wrote out of its caches; the IFE writes past them) */
	hakios_cache_flush(frames, frames + frames_bytes);
	loader_recording[0] = kvtophys(frames);
	loader_recording[1] = frames_bytes;
	loader_recording[2] = (uint64_t)frames;
	say("camera: %llu KB of memory for %u frames at %#llx, handed to the loader for the host\n", frames_bytes >> 10, CAM_FRAMES,
	    loader_recording[0]);
	return true;
}

static bool
smmu_find(void)
{
	bool used[256] = { false };
	uint32_t idr0 = rd(smmu + SMMU_IDR0), idr1 = rd(smmu + SMMU_IDR1);

	smmu_pages_size = idr1 >> 31 ? 0x10000 : 0x1000;
	smmu_pages = 2u << (idr1 >> 28 & 7);
	if (2 * smmu_pages * smmu_pages_size != 0x100000) {
		say("camera: the SMMU's layout is not the one known here\n");
		return false;
	}
	for (uint32_t n = 0; n < (idr0 & 0xff) && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(smmu + SMMU_SMR(n)), s2cr = rd(smmu + SMMU_S2CR(n));

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
			if ((((smr & 0x7fff) ^ IFE_STREAM) & ~((smr >> 16 & 0x7fff) | IFE_STREAM_MASK)) == 0) {
				say("camera: SMMU match register %u (%#x) is somebody's for the IFEs' streams: left alone\n", n, smr);
				return false;
			}
		} else if (smmu_match == ~0u) {
			smmu_match = n;
			smmu_old_smr = smr;
			smmu_old_s2cr = s2cr;
		}
	}
	for (smmu_context_n = 0; smmu_context_n < (idr1 & 0xff) && used[smmu_context_n]; smmu_context_n++) {
	}
	if (smmu_match == ~0u || smmu_context_n >= (idr1 & 0xff)) {
		say("camera: the SMMU has no match register or context left\n");
		return false;
	}
	/* (mapped once a context number: a session after every second of idleness mapped them anew, and nothing unmapped them) */
	{
		static uint32_t mapped_context = ~0u;
		static vm_offset_t second_page, context_page;

		if (second_page == 0) {
			second_page = ml_io_map(SMMU_PHYS + smmu_pages_size, 0x1000);
		}
		if (mapped_context != smmu_context_n || context_page == 0) {
			context_page = ml_io_map(SMMU_PHYS + (uint64_t)(smmu_pages + smmu_context_n) * smmu_pages_size, 0x1000);
			mapped_context = smmu_context_n;
		}
		smmu_second = second_page;
		smmu_context = context_page;
	}
	if (rd(smmu_context + SMMU_CB_SCTLR) & 1) {
		say("camera: SMMU context %u is switched on although no stream uses it: somebody's\n", smmu_context_n);
		return false;
	}
	return true;
}

static bool
smmu_put(vm_offset_t base, uint32_t offset, uint32_t value, uint32_t check)
{
	wr(base + offset, value);
	if ((rd(base + offset) & check) != (value & check)) {
		say("camera: SMMU %#x <- %#x, but it reads %#x\n", offset, value, rd(base + offset));
		give_up("an SMMU register that did not read back");
		return false;
	}
	return true;
}

/* The frames' memory behind FRAME_ADDRESSES for the IFEs' streams, through a free context with tables of ours. */
static bool
smmu_up(void)
{
	uint64_t *root, *second, table_root;
	uint32_t thirds = (uint32_t)((frames_bytes + 0x1fffff) >> 21), pages = (uint32_t)(frames_bytes >> 12);

	lck_mtx_lock(&hakios_smmu_lock);
	if (!smmu_find()) {
		lck_mtx_unlock(&hakios_smmu_lock);
		give_up("no place in the SMMU");
		return false;
	}
	if (kmem_alloc(kernel_map, &page_tables, (2 + thirds) * 4096, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		lck_mtx_unlock(&hakios_smmu_lock);
		give_up("no memory for page tables");
		return false;
	}
	root = (uint64_t *)page_tables;
	second = (uint64_t *)(page_tables + 4096);
	root[FRAME_ADDRESSES >> 30 & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
	for (uint32_t t = 0; t < thirds; t++) {
		uint64_t *third = (uint64_t *)(page_tables + (2 + t) * 4096);

		second[(FRAME_ADDRESSES >> 21 & 0x1ff) + t] = kvtophys((vm_offset_t)third) | 3;
		for (uint32_t p = 0; p < 512 && t * 512 + p < pages; p++) {
			third[p] = kvtophys(frames + 4096ull * (t * 512 + p)) | CAM_PTE;
		}
	}
	hakios_cache_flush(page_tables, page_tables + (2 + thirds) * 4096);
	table_root = kvtophys(page_tables) | (uint64_t)smmu_context_n << 48;
	say("camera: SMMU: context %u gets our tables for %llu KB at %#llx; match register %u sends streams %#x/%#x through it\n",
	    smmu_context_n, frames_bytes >> 10, FRAME_ADDRESSES, smmu_match, IFE_STREAM, IFE_STREAM_MASK);
	if (!smmu_put(smmu_context, SMMU_CB_SCTLR, 0, ~0u) || !smmu_put(smmu_second, SMMU_CBA2R(smmu_context_n), 1, 1) ||
	    !smmu_put(smmu_second, SMMU_CBAR(smmu_context_n), SMMU_CBAR_STAGE_1, 0x30000) ||
	    !smmu_put(smmu_context, SMMU_CB_TCR2, SMMU_TCR2, 0) || !smmu_put(smmu_context, SMMU_CB_TCR, SMMU_TCR_39_BITS, ~0u) ||
	    !smmu_put(smmu_context, SMMU_CB_TTBR0, (uint32_t)table_root, ~0u) ||
	    !smmu_put(smmu_context, SMMU_CB_TTBR0 + 4, (uint32_t)(table_root >> 32), ~0u) ||
	    !smmu_put(smmu_context, SMMU_CB_TTBR1, 0, ~0u) || !smmu_put(smmu_context, SMMU_CB_TTBR1 + 4, 0, ~0u) ||
	    !smmu_put(smmu_context, SMMU_CB_MAIR0, SMMU_MAIR0, ~0u) || !smmu_put(smmu_context, SMMU_CB_MAIR1, 0, ~0u)) {
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	wr(smmu_context + SMMU_CB_FSR, SMMU_FSR_ALL);
	wr(smmu_context + SMMU_CB_TLBIASID, smmu_context_n);
	wr(smmu_context + SMMU_CB_TLBSYNC, 1);
	for (unsigned waited = 0; rd(smmu_context + SMMU_CB_TLBSTATUS) & 1; waited++) {
		if (waited > 1000) {
			lck_mtx_unlock(&hakios_smmu_lock);
			give_up("the SMMU does not finish forgetting");
			return false;
		}
		spin_us(10);
	}
	if (!smmu_put(smmu_context, SMMU_CB_SCTLR, SMMU_SCTLR_ON, 1)) {
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	smmu_on = true;
	smmu_match_taken = true;
	if (!smmu_put(smmu, SMMU_S2CR(smmu_match), smmu_context_n, 0x300ff) ||
	    !smmu_put(smmu, SMMU_SMR(smmu_match), 1u << 31 | (uint32_t)IFE_STREAM_MASK << 16 | IFE_STREAM, ~0u)) {
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	return true;
}

static void
smmu_faults(void)
{
	uint32_t status, global;

	if (!smmu_on) {
		return;
	}
	status = rd(smmu_context + SMMU_CB_FSR);
	global = rd(smmu + SMMU_SGFSR);
	if ((status & SMMU_FSR_ALL) == 0 && global == 0) {
		say("camera: the SMMU has refused the IFE nothing (context %u)\n", smmu_context_n);
		return;
	}
	say("camera: THE SMMU HAS REFUSED A REQUEST: context %u status %#x, address %#llx, syndrome %#x; global status %#x, stream %#x\n",
	    smmu_context_n, status, (uint64_t)rd(smmu_context + SMMU_CB_FAR + 4) << 32 | rd(smmu_context + SMMU_CB_FAR),
	    rd(smmu_context + SMMU_CB_FSYNR0), global, rd(smmu + SMMU_SGFSYNR1));
}

static void
smmu_down(void)
{
	if (!smmu_on && !smmu_match_taken) {
		smmu_match = ~0u;       /* (found, not taken: not ours to put back) */
		return;
	}
	smmu_faults();
	if (!leaving) {
		lck_mtx_lock(&hakios_smmu_lock);
	}
	if (smmu_match_taken) {
		wr(smmu + SMMU_SMR(smmu_match), smmu_old_smr);
		wr(smmu + SMMU_S2CR(smmu_match), smmu_old_s2cr);
	}
	if (smmu_on) {
		wr(smmu_context + SMMU_CB_SCTLR, 0);
		wr(smmu_context + SMMU_CB_TLBIASID, smmu_context_n);
		wr(smmu_context + SMMU_CB_TLBSYNC, 1);
	}
	if (!leaving) {
		lck_mtx_unlock(&hakios_smmu_lock);
	}
	smmu_on = false;
	smmu_match_taken = false;
	smmu_match = ~0u;
	say("camera: the SMMU's match register and context put back as they were found\n");
}

/* ---- CSID-lite 0 and IFE-lite 0 ----------------------------------------------------------------------- */

#define CSID_PHYS               (hakios_board()->camera.csid)   /* the SoC file's camera { } */
#define IFE_PHYS                (hakios_board()->camera.ife)
#define CSIPHY_PHYS(n)          (hakios_board()->camera.csiphy + 0x2000UL * (n))

#define CSID_HW_VERSION         0x000
#define CSID_RST_STROBES        0x010
#define CSID_CSI2_RX_IRQ_STATUS 0x020
#define CSID_CSI2_RX_IRQ_MASK   0x024
#define CSID_CSI2_RX_IRQ_CLEAR  0x028
#define CSID_RDI0_IRQ_STATUS    0x030
#define CSID_RDI0_IRQ_MASK      0x034
#define CSID_RDI0_IRQ_CLEAR     0x038
#define CSID_TOP_IRQ_STATUS     0x070
#define CSID_TOP_IRQ_MASK       0x074
#define CSID_TOP_IRQ_CLEAR      0x078
#define CSID_IRQ_CMD            0x080
#define CSID_CSI2_RX_CFG0       0x100
#define CSID_CSI2_RX_CFG1       0x104
#define CSID_CSI2_RX_PKTS       0x160
#define CSID_CSI2_RX_ECC        0x164
#define CSID_CSI2_RX_CRC        0x168
#define CSID_RDI0_CFG0          0x200
#define CSID_RDI0_CFG1          0x204
#define CSID_RDI0_CTRL          0x208
#define CSID_RDI0_FRM_DROP_PATTERN 0x20c
#define CSID_RDI0_FRM_DROP_PERIOD  0x210
#define CSID_RDI0_IRQ_SUB_PATTERN  0x214
#define CSID_RDI0_IRQ_SUB_PERIOD   0x218
#define CSID_RDI0_PIX_DROP_PATTERN 0x224
#define CSID_RDI0_PIX_DROP_PERIOD  0x228
#define CSID_RDI0_LINE_DROP_PATTERN 0x22c
#define CSID_RDI0_LINE_DROP_PERIOD  0x230
#define CSID_RDI0_STATUS        0x250
#define CSID_RDI0_BYTES_PING    0x2e0
#define CSID_RDI0_BYTES_PONG    0x2e4
#define CSID_TPG_CTRL           0x600
#define CSID_TPG_VC_CFG0        0x604
#define CSID_TPG_VC_CFG1        0x608
#define CSID_TPG_LFSR_SEED      0x60c
#define CSID_TPG_DT0_CFG0       0x610
#define CSID_TPG_DT0_CFG1       0x614
#define CSID_TPG_DT0_CFG2       0x618
#define CSID_TPG_COLOR_BARS_CFG 0x640
#define CSID_TPG_COMMON_GEN_CFG 0x648
#define TPG_VC                  0xa
#define RAW10_DT                0x2b
/*
 * A line's bytes as the write master lays them out: RAW10 (four pixels in five bytes), the line
 * rounded up to 16 bytes, the bus's width (a 4000-pixel line takes 5008, not 5000).
 */
#define RAW10_STRIDE(width)     (((width) * 10 / 8 + 15) & ~15u)

#define IFE_HW_VERSION          0x0000
#define IFE_GLOBAL_RESET        0x0018
#define IFE_IRQ_MASK0           0x005c
#define IFE_IRQ_MASK1           0x0060
#define IFE_IRQ_CLEAR0          0x0064
#define IFE_IRQ_CLEAR1          0x0068
#define IFE_IRQ_STATUS0         0x006c
#define IFE_IRQ_STATUS1         0x0070
#define IFE_IRQ_GLOBAL_CLEAR    0x0058
#define IFE_VIOLATION_STATUS    0x007c
#define IFE_REG_UPDATE          0x04ac
#define BUS_HW_VERSION          0x2000
#define BUS_CGC_OVD             0x200c
#define BUS_IRQ_MASK1           0x2048
#define BUS_IRQ_CLEAR0          0x2050
#define BUS_IRQ_CLEAR1          0x2054
#define BUS_IRQ_CLEAR2          0x2058
#define BUS_IRQ_STATUS0         0x205c
#define BUS_IRQ_STATUS1         0x2060
#define BUS_IRQ_STATUS2         0x2064
#define BUS_IRQ_GLOBAL_CLEAR    0x2068
#define BUS_COMP_ERROR          0x206c
#define BUS_ADDR_SYNC_FRAME_HDR 0x2080
#define BUS_ADDR_SYNC_NO_SYNC   0x2084
#define BUS_TEST_CTRL           0x211c
#define WM0_STATUS0             0x2200
#define WM0_STATUS1             0x2204
#define WM0_CFG                 0x2208
#define WM0_IMAGE_ADDR          0x2214
#define WM0_IMAGE_ADDR_OFFSET   0x2218
#define WM0_WIDTH               0x221c
#define WM0_HEIGHT              0x2220
#define WM0_PACKER              0x2224
#define WM0_STRIDE              0x2228
#define WM0_IRQ_SUB_PERIOD      0x2248
#define WM0_IRQ_SUB_PATTERN     0x224c
#define WM0_FRAMEDROP_PERIOD    0x2250
#define WM0_FRAMEDROP_PATTERN   0x2254
#define WM0_FRAME_INC           0x2258
#define WM0_BURST_LIMIT         0x225c

static vm_offset_t csid, ife, csiphy;
static bool csid_running, ife_running, tpg_running;

/* A register of the three camera blocks: written, and read back where it is one that keeps what it is given. */
static bool
blk_put(vm_offset_t base, const char *what, uint32_t offset, uint32_t value, bool check)
{
	wr(base + offset, value);
	hakios_barrier();
	if (check && rd(base + offset) != value) {
		say("camera: %s %#x <- %#x, but it reads %#x\n", what, offset, value, rd(base + offset));
		give_up("a camera register that did not read back");
		return false;
	}
	return true;
}

static bool
wait_reg(vm_offset_t base, uint32_t offset, uint32_t mask, uint32_t want, uint32_t us)
{
	for (uint32_t waited = 0; waited <= us; waited += 10) {
		if ((rd(base + offset) & mask) == want) {
			return true;
		}
		spin_us(10);
	}
	return false;
}

/* cam_ife_csid_global_reset() and reset_regs(): interrupts masked and cleared, then the registers, then the hardware. */
static bool
csid_reset(void)
{
	blk_put(csid, "CSID", CSID_CSI2_RX_IRQ_MASK, 0, false);
	for (unsigned r = 0; r < 4; r++) {
		blk_put(csid, "CSID", CSID_RDI0_IRQ_MASK + 0x10 * r, 0, false);
	}
	blk_put(csid, "CSID", CSID_TOP_IRQ_CLEAR, 1, false);
	blk_put(csid, "CSID", CSID_CSI2_RX_IRQ_CLEAR, 0xfffffff, false);
	for (unsigned r = 0; r < 4; r++) {
		blk_put(csid, "CSID", CSID_RDI0_IRQ_CLEAR + 0x10 * r, 0x7fff, false);
	}
	blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
	blk_put(csid, "CSID", CSID_CSI2_RX_CFG1, 0x80, false);
	for (unsigned r = 0; r < 4; r++) {
		blk_put(csid, "CSID", CSID_RDI0_CFG0 + 0x100 * r, 2, false);
	}
	for (unsigned hw = 0; hw < 2; hw++) {
		blk_put(csid, "CSID", CSID_TOP_IRQ_CLEAR, 1, false);
		blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
		if (hw) {
			blk_put(csid, "CSID", CSID_TOP_IRQ_MASK, 1, false);
			blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
		}
		blk_put(csid, "CSID", CSID_RST_STROBES, hw ? 0x1e : 0x1, false);
		if (!hw) {
			blk_put(csid, "CSID", CSID_TOP_IRQ_MASK, 1, false);
			blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
		}
		if (!wait_reg(csid, CSID_TOP_IRQ_STATUS, 1, 1, 100000)) {
			say("camera: the CSID's %s reset does not finish (top irq %#x)\n", hw ? "hardware" : "registers'",
			    rd(csid + CSID_TOP_IRQ_STATUS));
			give_up("a CSID that does not reset");
			return false;
		}
		blk_put(csid, "CSID", CSID_TOP_IRQ_CLEAR, 1, false);
		blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
	}
	say("camera: CSID-lite 0 reset (version %#x)\n", rd(csid + CSID_HW_VERSION));
	return true;
}

/* The RX (from a PHY, or the test pattern generator: `phy' -1) and RDI 0, for RAW10 of vc/dt, written as it comes (packed). */
static bool
csid_config(int phy, unsigned lanes, uint32_t lane_cfg, uint32_t vc, uint32_t width, uint32_t height, bool cphy)
{
	uint32_t cfg0 = vc << 22 | RAW10_DT << 16 | 0u << 27 | 0xfu << 12 | 0u << 10 | 1u << 2 | 3u;

	if (!blk_put(csid, "CSID", CSID_CSI2_RX_CFG0, (lanes - 1) | lane_cfg << 4 | (uint32_t)cphy << 24 |
	    (uint32_t)(phy < 0 ? 0 : phy & 3) << 20,
	    true) || !blk_put(csid, "CSID", CSID_CSI2_RX_CFG1, 1u << 6 | 1u, true)) {
		return false;
	}
	if (phy < 0) {
		/* cam_ife_csid_config_tpg(): one data type, frames without end, blanking 0x3ff lines and 0x740 pixels, RAW10, colour bars */
		if (!blk_put(csid, "CSID", CSID_TPG_VC_CFG0, 0u << 16 | 1u << 10 | vc, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_VC_CFG1, 0x3ffu << 12 | 0x740, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_LFSR_SEED, 0x12345678, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_DT0_CFG0, width << 16 | height, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_DT0_CFG1, RAW10_DT, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_DT0_CFG2, (3u - 1) << 16 | 0x8, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_COLOR_BARS_CFG, 1u << 5, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_COMMON_GEN_CFG, 0, true)) {
			return false;
		}
	}
	/* cam_ife_csid_init_config_rdi_path() */
	if (!blk_put(csid, "CSID", CSID_RDI0_CFG0, cfg0, true) || !blk_put(csid, "CSID", CSID_RDI0_CFG1, 2, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_FRM_DROP_PERIOD, 1, true) || !blk_put(csid, "CSID", CSID_RDI0_FRM_DROP_PATTERN, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_IRQ_SUB_PERIOD, 1, true) || !blk_put(csid, "CSID", CSID_RDI0_IRQ_SUB_PATTERN, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_PIX_DROP_PATTERN, 0, true) || !blk_put(csid, "CSID", CSID_RDI0_PIX_DROP_PERIOD, 1, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_LINE_DROP_PATTERN, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_LINE_DROP_PERIOD, 1, true) || !blk_put(csid, "CSID", CSID_RDI0_CTRL, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_CFG0, cfg0 | 1u << 31, true)) {
		return false;
	}
	csid_running = true;
	/* cam_ife_csid_enable_rdi_path(): resume at the frame's boundary; the reset, overflow and count errors seen */
	return blk_put(csid, "CSID", CSID_RDI0_CTRL, 1, true) &&
	       blk_put(csid, "CSID", CSID_RDI0_IRQ_MASK, 1u << 1 | 1u << 2 | 1u << 3 | 1u << 4 | 1u << 5 | 1u << 6, true);
}

static bool
tpg_start(void)
{
	/* cam_ife_csid_tpg_start(): four lanes' worth */
	if (!blk_put(csid, "CSID", CSID_TPG_CTRL, 4u << 20 | 0x80u << 8 | 3u << 4 | 7u, true)) {
		return false;
	}
	tpg_running = true;
	return true;
}

/* cam_vfe_top_reset(), cam_vfe_bus_init_hw(), cam_vfe_bus_start_wm() and update_wm() for RDI 0 in frame mode. */
static bool
ife_config(uint64_t address, uint32_t frame_bytes, uint32_t frame_inc)
{
	blk_put(ife, "IFE", IFE_IRQ_MASK0, 1u << 31, false);
	blk_put(ife, "IFE", IFE_GLOBAL_RESET, 0x3f9f, false);
	if (!wait_reg(ife, IFE_IRQ_STATUS0, 1u << 31, 1u << 31, 100000)) {
		say("camera: the IFE does not reset (irq %#x)\n", rd(ife + IFE_IRQ_STATUS0));
		give_up("an IFE that does not reset");
		return false;
	}
	blk_put(ife, "IFE", IFE_IRQ_CLEAR0, 0xffffffff, false);
	blk_put(ife, "IFE", IFE_IRQ_CLEAR1, 0xffffffff, false);
	blk_put(ife, "IFE", IFE_IRQ_GLOBAL_CLEAR, 1, false);
	say("camera: IFE-lite 0 reset (version %#x, bus %#x)\n", rd(ife + IFE_HW_VERSION), rd(ife + BUS_HW_VERSION));
	if (!blk_put(ife, "IFE", BUS_ADDR_SYNC_FRAME_HDR, 0, true) || !blk_put(ife, "IFE", BUS_CGC_OVD, 0xfffff, false) ||
	    !blk_put(ife, "IFE", BUS_TEST_CTRL, 0, false) || !blk_put(ife, "IFE", BUS_ADDR_SYNC_NO_SYNC, 0xffffff, false) ||
	    !blk_put(ife, "IFE", WM0_BURST_LIMIT, 0xf, true) || !blk_put(ife, "IFE", WM0_WIDTH, 0xff01, true) ||
	    !blk_put(ife, "IFE", WM0_HEIGHT, 0, true) || !blk_put(ife, "IFE", WM0_PACKER, 0, true) ||
	    !blk_put(ife, "IFE", WM0_STRIDE, 0xff01, true) || !blk_put(ife, "IFE", WM0_FRAMEDROP_PERIOD, 0, true) ||
	    !blk_put(ife, "IFE", WM0_FRAMEDROP_PATTERN, 1, true) || !blk_put(ife, "IFE", WM0_IRQ_SUB_PERIOD, 0, true) ||
	    !blk_put(ife, "IFE", WM0_IRQ_SUB_PATTERN, 1, true) || !blk_put(ife, "IFE", BUS_IRQ_MASK1, 1, false) ||
	    !blk_put(ife, "IFE", WM0_IMAGE_ADDR, (uint32_t)address, false) || !blk_put(ife, "IFE", WM0_FRAME_INC, frame_inc, true) ||
	    !blk_put(ife, "IFE", WM0_CFG, 3, true)) {
		return false;
	}
	ife_running = true;
	/* the RDI 0 resource's start: its register update */
	return blk_put(ife, "IFE", IFE_REG_UPDATE, 0x2, false);
}

static void
show_paths(const char *when)
{
	say("camera: %s: CSID rx irq %#x, packets %u, ecc %#x, crc %u; RDI0 irq %#x status %#x, bytes %u/%u; IFE irq %#x %#x, "
	    "violation %#x; bus irq %#x %#x %#x, comp error %#x; WM0 status %#x %#x\n", when, rd(csid + CSID_CSI2_RX_IRQ_STATUS),
	    rd(csid + CSID_CSI2_RX_PKTS), rd(csid + CSID_CSI2_RX_ECC), rd(csid + CSID_CSI2_RX_CRC), rd(csid + CSID_RDI0_IRQ_STATUS),
	    rd(csid + CSID_RDI0_STATUS), rd(csid + CSID_RDI0_BYTES_PING), rd(csid + CSID_RDI0_BYTES_PONG), rd(ife + IFE_IRQ_STATUS0),
	    rd(ife + IFE_IRQ_STATUS1), rd(ife + IFE_VIOLATION_STATUS), rd(ife + BUS_IRQ_STATUS0), rd(ife + BUS_IRQ_STATUS1),
	    rd(ife + BUS_IRQ_STATUS2), rd(ife + BUS_COMP_ERROR), rd(ife + WM0_STATUS0), rd(ife + WM0_STATUS1));
}

static void
paths_down(void)
{
	if (tpg_running) {
		wr(csid + CSID_TPG_CTRL, 0);
		tpg_running = false;
	}
	if (csid_running) {
		/* cam_ife_csid_disable_rdi_path(): halted at once, then the path off */
		wr(csid + CSID_RDI0_IRQ_MASK, 0);
		wr(csid + CSID_RDI0_CTRL, (rd(csid + CSID_RDI0_CTRL) & ~3u) | 2);
		wr(csid + CSID_RDI0_CFG0, rd(csid + CSID_RDI0_CFG0) & ~(1u << 31));
		wr(csid + CSID_CSI2_RX_CFG0, 0);
		csid_running = false;
	}
	if (ife_running) {
		wr(ife + WM0_CFG, 0);
		wr(ife + IFE_REG_UPDATE, 0x2);
		spin_us(1000);
		ife_running = false;
	}
}

/* The camera's blocks up to their clocks; false (and everything taken back by titan_down()) on any fault. */
static bool
titan_up(void)
{
	/* (a mapping each: as many CSIPHYs as camera { } may list, and sensors_from_board() takes none past its count) */
	static vm_offset_t csiphys[HAKIOS_CAMERA_CLOCKS];

	if (csid == 0) {
		csid = ml_io_map(CSID_PHYS, 0x1000);
		ife = ml_io_map(IFE_PHYS, 0x4000);
	}
	if (csiphys[hw->csiphy] == 0) {
		csiphys[hw->csiphy] = ml_io_map(CSIPHY_PHYS(hw->csiphy), 0x2000);
	}
	csiphy = csiphys[hw->csiphy];   /* (the sensor's: another one after a change of sensor) */
	if (!(rd(camcc + TITAN_TOP_GDSCR) & GDSC_ON) && !gdsc_on()) {
		return false;
	}
	if (!rails(true)) {
		give_up("RPMh did not take the camera's rails");
		return false;
	}
	return pll0_up() && clocks_up();
}

static void
titan_down(void)
{
	paths_down();
	smmu_down();
	clocks_down();
	pll0_down();
	if (rails_voted) {
		(void)rails(false);     /* (still marked when RPMh did not say done: asked again next time, at the latest at the run's end) */
	}
}

/* cam=3: a few frames of the CSID's test pattern through RDI 0 and IFE-lite 0 into memory. */
static void
capture_tpg(void)
{
	const uint32_t width = 1600, height = 1200, stride = width * 10 / 8;
	struct cam_header *h;

	if (!frames_alloc(width, height, stride, 0) || !titan_up() || !smmu_up() || !csid_reset() ||
	    !csid_config(-1, 4, 0x3210, TPG_VC, width, height, false) || !ife_config(FRAME_ADDRESSES + CAM_HEADER_BYTES, stride * height, stride * height)) {
		titan_down();
		return;
	}
	show_paths("configured");
	if (!tpg_start()) {
		titan_down();
		return;
	}
	h = (struct cam_header *)frames;
	for (unsigned ms = 0; ms < 2000; ms += 100) {
		wait_ms(100);
		if (rd(ife + BUS_IRQ_STATUS1) & 1) {
			h->frames = 1;
			say("camera: write master 0 says its frame is done after %u ms\n", ms + 100);
			break;
		}
	}
	show_paths("after the frames");
	smmu_faults();
	titan_down();
	/* (what the IFE wrote, a look: the first bytes of the frame, past the processor's caches) */
	hakios_cache_flush(frames, frames + CAM_HEADER_BYTES + 256);
	const uint8_t *p = (const uint8_t *)(frames + CAM_HEADER_BYTES);
	say("camera: the frame's first bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x ... (frames done: %u)\n", p[0], p[1], p[2],
	    p[3], p[4], p[5], p[6], p[7], p[8], p[9], h->frames);
	hakios_cache_clean(frames, frames + CAM_HEADER_BYTES);
}

/* ---- cam=4: the sensor's own frames ------------------------------------------------------------------ */

/*
 * The sensor's tables are Android's, read at run time: components/camera/tools/camsensor.py makes camera-ov02b10.tab
 * and camera-s5khm2sd.tab out of the module files (userland/build.sh puts them into fw/ of the small
 * image). The sequence is Android's: power up, the chip ID, the initialisation ("init": the macro's
 * 108 writes, a soft reset and then its registers page by page; the wide one's 1441), the mode's own
 * table ("resN": boot-arg cam_mode=N, or the mode with the fewest pixels: the macro's 1600x1200 RAW10
 * on one lane at 665.6 Mbit/s, 30 frames a second; the wide one's 1920x1080 RAW10 on three C-PHY
 * trios at 3.11 Gbit/s each, 120 frames a second); the sensor's CSIPHY, CSID-lite 0's RX from that
 * PHY and RDI 0, IFE-lite 0's write master as for cam=3; then "on", the frames, "off" (and its 30 ms),
 * and everything down again. The sensor's writes are not read back (some of its registers start
 * something and clear themselves, a soft reset among them); each byte must be acknowledged.
 *
 * The CSIPHY ("qcom,csiphy-v1.2.1": Android's cam_csiphy_1_2_1_hwreg.h, cam_csiphy_core.c): its reset,
 * the common registers with the lanes to enable (from the module file's lane assignment: the macro's
 * data lane 0 and the clock lane; the wide one's three trios), then the two-phase (D-PHY) settings of
 * every lane position or the three-phase (C-PHY) ones of every trio with the row of the data rate's
 * table that covers the rate, the settle count among them, and its interrupts (status only, nothing
 * handled). The settle count is the module file's for the mode (the macro's 20, the wide one's 34,
 * in periods of the 300 MHz timer). Boot-arg cam_settle=N sets it instead.
 */
/* One of the sensor's tables written in order, as Android's sensor driver writes it. */
static bool
sensor_table(const char *name)
{
	uint32_t count;
	const struct ctab_entry *e = ctab_find(name, &count);

	if (e == NULL) {
		say("camera: the tables have no \"%s\"\n", name);
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		uint8_t out[4];
		unsigned n = 0;

		if (e[i].operation != 0 || e[i].address_bytes < 1 || e[i].address_bytes > 2 || e[i].data_bytes < 1 || e[i].data_bytes > 2) {
			say("camera: \"%s\" %u: an operation not known here (%u, %u/%u bytes)\n", name, i, e[i].operation, e[i].address_bytes,
			    e[i].data_bytes);
			give_up("a sensor table not understood");
			return false;
		}
		if (e[i].address_bytes == 2) {
			out[n++] = (uint8_t)(e[i].address >> 8);
		}
		out[n++] = (uint8_t)e[i].address;
		if (e[i].data_bytes == 2) {
			out[n++] = (uint8_t)(e[i].data >> 8);
		}
		out[n++] = (uint8_t)e[i].data;
		if (broken || !i2c_transfer(&cci_bus, sensor_address(), out, n, NULL, 0)) {
			say("camera: \"%s\" %u: %#x <- %#x not acknowledged\n", name, i, e[i].address, e[i].data);
			give_up("a sensor write nobody acknowledged");
			return false;
		}
		if (e[i].delay_us != 0) {
			spin_us(e[i].delay_us > 100000 ? 100000 : e[i].delay_us);
		}
	}
	say("camera: the sensor's \"%s\": %u writes\n", name, count);
	return true;
}

static bool csiphy_running;

/* The common part of cam_csiphy_config_dev(): the lanes to enable, then the registers of the PHY's kind. */
static bool
csiphy_common(uint32_t lane_enable, bool cphy)
{
	if (!blk_put(csiphy, "CSIPHY", 0x814, lane_enable, true) || !blk_put(csiphy, "CSIPHY", 0x818, 0x01, true) ||
	    !blk_put(csiphy, "CSIPHY", 0x81c, cphy ? 0x52 : 0x02, true)) {
		return false;
	}
	wr(csiphy + 0x800, 0x03);
	spin_us(1);
	wr(csiphy + 0x800, cphy ? 0x0e : 0x02);
	return true;
}

/* D-PHY: csiphy_2ph_v1_2_1_reg, every lane position (data lanes 0..3 and the clock lane) */
static bool
csiphy_dphy(unsigned settle)
{
	static const uint32_t lanes[5][2] = {           /* each lane position's two blocks: data lane 0, the clock lane, data lanes 1, 2, 3 */
		{ 0x000, 0x900 }, { 0x700, 0xc80 }, { 0x200, 0xa00 }, { 0x400, 0xb00 }, { 0x600, 0xc00 },
	};

	for (unsigned l = 0; l < 5; l++) {
		uint32_t b = lanes[l][0], m = lanes[l][1];
		bool clock = l == 1;

		if (!blk_put(csiphy, "CSIPHY", b + 0x30, 0x00, true) || !blk_put(csiphy, "CSIPHY", m + 0x0, 0x05, true) ||
		    !blk_put(csiphy, "CSIPHY", m + 0x8, 0x10, true) || !blk_put(csiphy, "CSIPHY", m + 0x4, 0x00, true) ||
		    !blk_put(csiphy, "CSIPHY", m + 0x4, 0x07, true) || !blk_put(csiphy, "CSIPHY", b + 0x04, 0x0c, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x2c, 0x01, true) || !blk_put(csiphy, "CSIPHY", b + 0x34, 0x07, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x10, 0x02, true) || !blk_put(csiphy, "CSIPHY", b + 0x1c, 0x08, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x3c, 0xb8, true) || !blk_put(csiphy, "CSIPHY", b + 0x08, settle & 0xff, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x00, clock ? 0x80 : 0x8d, true) ||
		    (clock && !blk_put(csiphy, "CSIPHY", b + 0x0c, 0xa5, true)) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x38, clock ? 0x1f : 0xfe, true) || !blk_put(csiphy, "CSIPHY", b + 0x14, 0x60, true) ||
		    (clock && !blk_put(csiphy, "CSIPHY", b + 0x28, 0x04, true)) || !blk_put(csiphy, "CSIPHY", 0x800, 0x02, true) ||
		    !blk_put(csiphy, "CSIPHY", 0x884, 0x01, true)) {
			return false;
		}
	}
	return true;
}

/*
 * C-PHY: csiphy_3ph_v1_2_1_reg, every trio position (three), then cam_csiphy_cphy_data_rate_config()'s
 * deltas of data_rate_delta_table_1_2_1 for the first rate that covers this one (its rows go up to
 * 2.5, 3.5 and 4.5 Gsym/s a trio, 2.28 bits a symbol).
 */
static bool
csiphy_cphy(unsigned settle, uint64_t bits_per_trio)
{
	static const uint32_t trios[3][2] = { { 0x100, 0x980 }, { 0x300, 0xa80 }, { 0x500, 0xb80 } };
	static const struct {
		uint64_t        bandwidth;
		uint8_t         r144, r164, r16c, rb4;  /* (rb4: 0 for none) */
	} rates[3] = {
		{ 5700000000ull, 0x32, 0x0b, 0x25, 0 }, { 7980000000ull, 0xb2, 0x33, 0x1d, 0x03 }, { 10260000000ull, 0xb2, 0x33, 0x25, 0x02 },
	};
	unsigned r = 0;

	for (unsigned t = 0; t < 3; t++) {
		uint32_t b = trios[t][0], m = trios[t][1];

		if (!blk_put(csiphy, "CSIPHY", m + 0x10, 0x00, true) || !blk_put(csiphy, "CSIPHY", m + 0x14, 0x00, true) ||
		    !blk_put(csiphy, "CSIPHY", m + 0x18, 0x00, true) || !blk_put(csiphy, "CSIPHY", m + 0x10, 0x08, true) ||
		    !blk_put(csiphy, "CSIPHY", m + 0x14, 0x08, true) || !blk_put(csiphy, "CSIPHY", m + 0x18, 0x1a, true) ||
		    !blk_put(csiphy, "CSIPHY", m + 0x0c, 0xaf, true)) {
			return false;
		}
		spin_us(1);
		if (!blk_put(csiphy, "CSIPHY", b + 0x5c, 0x46, true) || !blk_put(csiphy, "CSIPHY", b + 0x68, 0xa0, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x04, 0x06, true) || !blk_put(csiphy, "CSIPHY", b + 0x0c, settle & 0xff, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x08, (settle >> 8) & 0xff, true) || !blk_put(csiphy, "CSIPHY", b + 0x14, 0x20, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x50, 0x00, true) || !blk_put(csiphy, "CSIPHY", b + 0x88, 0x00, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x8c, 0x7f, true) || !blk_put(csiphy, "CSIPHY", b + 0x90, 0x7f, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x18, 0x3e, true) || !blk_put(csiphy, "CSIPHY", b + 0x1c, 0x41, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x20, 0x41, true) || !blk_put(csiphy, "CSIPHY", b + 0x24, 0x7f, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x28, 0x00, true) || !blk_put(csiphy, "CSIPHY", b + 0x2c, 0x00, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x60, 0x02, true) || !blk_put(csiphy, "CSIPHY", b + 0xcc, 0x41, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0xdc, 0x50, true) || !blk_put(csiphy, "CSIPHY", m + 0x04, 0x20, true) ||
		    !blk_put(csiphy, "CSIPHY", m + 0x08, 0x05, true) || !blk_put(csiphy, "CSIPHY", m + 0x00, 0x60, true) ||
		    !blk_put(csiphy, "CSIPHY", m + 0x2c, 0x35, true) || !blk_put(csiphy, "CSIPHY", m + 0x30, 0x01, true) ||
		    !blk_put(csiphy, "CSIPHY", 0x800, 0x0e, true) || !blk_put(csiphy, "CSIPHY", 0x884, 0x01, true)) {
			return false;
		}
	}
	while (r < 2 && bits_per_trio > rates[r].bandwidth) {
		r++;
	}
	for (unsigned t = 0; t < 3; t++) {
		uint32_t b = trios[t][0], m = trios[t][1];

		if (!blk_put(csiphy, "CSIPHY", b + 0x44, rates[r].r144, true) || !blk_put(csiphy, "CSIPHY", b + 0x64, rates[r].r164, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x6c, rates[r].r16c, true) ||
		    (rates[r].rb4 != 0 && !blk_put(csiphy, "CSIPHY", m + 0x34, rates[r].rb4, true))) {
			return false;
		}
		if (rates[r].rb4 != 0) {
			spin_us(1);
		}
	}
	say("camera: CSIPHY %u: C-PHY rate row %u (up to %llu bit/s a trio) for %llu\n", hw->csiphy, r, rates[r].bandwidth, bits_per_trio);
	return true;
}

/* cam_csiphy_reset(), then the configuration for `lanes' lanes (trios) of the lane assignment, and the interrupt masks. */
static bool
csiphy_up(unsigned lanes, bool cphy, uint32_t lane_assign, unsigned settle, uint64_t bits_per_lane)
{
	static const uint8_t irq_masks[11] = { 0xff, 0xff, 0xfb, 0xff, 0x7f, 0xff, 0xff, 0xef, 0xff, 0xff, 0xff };
	uint32_t lane_enable = 0;

	/* cam_csiphy_get_lane_enable(): D-PHY data lane n at bit 2n, the clock lane at 7; C-PHY trio n at bit 2n + 1 */
	for (unsigned l = 0; l < lanes; l++, lane_assign >>= 4) {
		unsigned n = lane_assign & 0xf;

		if (n > (cphy ? 2u : 3u)) {
			give_up("a lane assignment not known here");
			return false;
		}
		lane_enable |= cphy ? 1u << (2 * n + 1) : 1u << (2 * n) | 0x80u;
	}
	wr(csiphy + 0x814, 0x00);
	spin_us(5);
	wr(csiphy + 0x818, 0x00);
	wr(csiphy + 0x81c, 0x00);
	wr(csiphy + 0x800, 0x01);
	spin_us(1);
	wr(csiphy + 0x800, 0x00);
	csiphy_running = true;
	if (!csiphy_common(lane_enable, cphy) || !(cphy ? csiphy_cphy(settle, bits_per_lane) : csiphy_dphy(settle))) {
		return false;
	}
	for (unsigned i = 0; i < 11; i++) {
		wr(csiphy + 0x82c + 4 * i, irq_masks[i]);
		wr(csiphy + 0x858 + 4 * i, 0xff);
	}
	wr(csiphy + 0x828, 0x1);
	wr(csiphy + 0x828, 0x0);
	say("camera: CSIPHY %u configured: %u %s (lanes enabled %#x), settle count %u\n", hw->csiphy, lanes, cphy ? "trio(s), C-PHY" :
	    "lane(s), D-PHY", lane_enable, settle);
	return true;
}

static void
show_csiphy(const char *when)
{
	char line[200];
	int at = snprintf(line, sizeof(line), "camera: CSIPHY %u %s, interrupt status:", hw->csiphy, when);

	for (unsigned i = 0; i < 11 && at < (int)sizeof(line) - 6; i++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %02x", rd(csiphy + 0x8b0 + 4 * i) & 0xff);
	}
	say("%s\n", line);
}

static void
csiphy_down(void)
{
	if (!csiphy_running) {
		return;
	}
	wr(csiphy + 0x814, 0x00);
	spin_us(5);
	wr(csiphy + 0x818, 0x00);
	wr(csiphy + 0x81c, 0x00);
	wr(csiphy + 0x800, 0x01);
	spin_us(1);
	wr(csiphy + 0x800, 0x00);
	csiphy_running = false;
}

/*
 * The exposure. The OV02B10's module file has a table that sets it (page 1: analog gain 0x22, the
 * frame's extra lines 0x14/0x15, the exposure in lines 0x0e/0x0f, 0x9b, then 0xfe = 2 to apply it;
 * "holdon" here), written as Android's camera stack writes its first one; the S5KHM2SD's starts as
 * its tables leave it. Boot-args for a look in other light, written the sensor's way: cam_shutter=
 * LINES, cam_gain=N (OV02B10: 0x22, 16 is 1x; S5KHM2SD: 0x0204, 32 is 1x), cam_vblank=LINES (the
 * frame that much longer: OV02B10 0x14/0x15; S5KHM2SD 0x0340, the frame length, by as many lines),
 * the S5KHM2SD's between its group hold on and off (0x0104, its "holdon"/"holdoff" tables). The
 * other two sensors (the S5K4H7 has the SMIA registers too) start as their tables leave them; the
 * boot-args are for the macro and the wide one.
 */
static bool
sensor_write(uint16_t address, uint16_t value, bool sixteen)
{
	uint8_t out[4] = { (uint8_t)(address >> 8), (uint8_t)address, (uint8_t)(value >> 8), (uint8_t)value };

	if (hw->regs == REGS_OV_PAGED) {
		out[0] = (uint8_t)address;
		out[1] = (uint8_t)value;
		return i2c_transfer(&cci_bus, sensor_address(), out, 2, NULL, 0);
	}
	return sixteen ? i2c_transfer(&cci_bus, sensor_address(), out, 4, NULL, 0) :
	       i2c_transfer(&cci_bus, sensor_address(), (uint8_t[]){ out[0], out[1], out[3] }, 3, NULL, 0);
}

/*
 * The wide camera's lens: a voice-coil motor driven by a Dongwoon DW9800 on the sensor's own I2C at
 * 0x0c (the module file's actuatorDriver, "<phone>_sunny_s5khm2sd_dw9800_wide"), powered by the
 * PM8350C's L9 (the device tree's actuator0: cam_vaf at 2.8 V, a regulator the SD card's slot has as
 * its vdd too; RPMh's "ldoc9"). The module file's initialisation, as it is: 0x02 = 0x01 (power down),
 * 0x02 = 0x00 and 1 ms, 0x02 = 0x02 (ring control), 0x06 = 0x40 (SAC3), 0x07 = 0x07 (its timing), the
 * position 0x190 and 20 ms, then 0x12c. The position is ten bits, 0x03 (the high two) and 0x04 written
 * together (its registerParam). Those registers only are written. A sensor without focus-motor has a
 * fixed lens. When the motor does not answer, its supply (the sensor node's focus-supply) is switched
 * on (and back off at the end) and it is asked again; at the end the lens is let down to 0 and the chip
 * powered down.
 */
#define AF_ADDRESS              0x0c
#define AF_CONTROL              0x02            /* [0] power down, [1] ring control */
#define AF_POSITION             0x03            /* and 0x04: ten bits */
#define AF_MODE                 0x06
#define AF_TIMING               0x07
#define VRM_VOLTAGE             0x0             /* an RPMh VRM resource's registers (rpmh-regulator.c) */
#define VRM_ENABLE              0x4
#define VRM_MODE                0x8
#define VRM_MODE_HPM            7               /* a PMIC5 LDO's high power mode */

static bool vaf_voted;                  /* a vote of ours on the lens motor's supply was sent: to be given back */
static uint32_t vaf_at;                 /* ... to that resource (the sensor's then: another may be chosen before it is given back) */
static const char *vaf_name;
static uint32_t af_position = HAKIOS_CAMERA_NO_FOCUS;

/* (a look, for now: what the LDOs say after the motor does something; said only when not all is well) */
static void
af_look(const char *when)
{
	for (unsigned n = 1; n <= PM8008_LDOS; n++) {
		uint8_t status = 0;

		if (ldo_switched[n] && pm8008_get(PM8008_REGULATORS, (uint16_t)(LDO_BASE(n) + LDO_STATUS1), &status, 1) &&
		    (status & 0xb8) != 0x80) {
			say("camera: %s: PM8008i LDO%u status 0x%02x\n", when, n, status);
		}
	}
}

static bool
af_write(uint8_t reg, uint16_t value)
{
	uint8_t out[3] = { reg, (uint8_t)value, 0 };

	if (reg != AF_CONTROL && reg != AF_POSITION && reg != AF_MODE && reg != AF_TIMING) {
		return false;
	}
	if (reg == AF_POSITION) {
		out[1] = (uint8_t)(value >> 8 & 3);
		out[2] = (uint8_t)value;
	}
	return i2c_transfer(&cci_bus, AF_ADDRESS, out, reg == AF_POSITION ? 3 : 2, NULL, 0);
}

/*
 * The lens motor's supply (the sensor node's focus-supply) on in high power mode, or given back: its enable,
 * mode and voltage each back to 0, no request of ours, as the rails are given back. True: RPMh has said done.
 * The vote is marked (vaf_voted) once it was sent, answered or not, and the mark goes only when RPMh has said
 * done to its withdrawal: af_down() asks again, at the latest at the run's end.
 */
static bool
vaf(bool on)
{
	const struct hakios_camera_sensor *wired = &hakios_board()->cameras.sensors[hw_index];
	uint16_t levels[1];
	unsigned count;
	uint32_t base, addresses[3], values[3];
	enum rpmh_sent sent;

	if (!on) {
		if (!vaf_voted) {
			return true;    /* (nothing of ours to give back) */
		}
		base = vaf_at;
		addresses[0] = base + VRM_ENABLE;
		addresses[1] = base + VRM_MODE;
		addresses[2] = base + VRM_VOLTAGE;
		values[0] = values[1] = values[2] = 0;
	} else {
		if (wired->focus_supply[0] == 0) {
			say("camera: the board names no supply for the lens motor (focus-supply)\n");
			return false;
		}
		if ((base = rpmh_resource(wired->focus_supply, levels, 1, &count)) == 0) {
			say("camera: RPMh has no %s (the lens motor's supply)\n", wired->focus_supply);
			return false;
		}
		addresses[0] = base + VRM_VOLTAGE;
		values[0] = wired->focus_mv;
		addresses[1] = base + VRM_MODE;
		values[1] = VRM_MODE_HPM;
		addresses[2] = base + VRM_ENABLE;
		values[2] = 1;
	}
	sent = rpmh_write(3, addresses, values);
	if (on && sent != RPMH_NOT_SENT) {
		vaf_voted = true;
		vaf_at = base;
		vaf_name = wired->focus_supply;
	} else if (!on && sent == RPMH_DONE) {
		vaf_voted = false;
	}
	if (on) {
		say("camera: RPMh %s %s (%#x) on, %u mV\n", sent == RPMH_DONE ? "took" : sent == RPMH_NOT_SENT ? "WAS NOT ASKED FOR" :
		    "DID NOT SAY IT TOOK", wired->focus_supply, base, wired->focus_mv);
	} else {
		say("camera: RPMh %s %s (%#x) given back%s\n", sent == RPMH_DONE ? "took" : sent == RPMH_NOT_SENT ? "WAS NOT ASKED FOR" :
		    "DID NOT SAY IT TOOK", vaf_name, base, vaf_voted ? ": to be asked again" : "");
	}
	return sent == RPMH_DONE;
}

static void
af_up(bool sensor_frames)
{
	af_position = HAKIOS_CAMERA_NO_FOCUS;
	if (!hw->focus_motor || !sensor_frames) {
		return;
	}
	if (!af_write(AF_CONTROL, 0x01)) {
		say("camera: the lens motor (DW9800 at 0x0c) does not answer: its supply on\n");
		if (!vaf(true)) {
			(void)vaf(false);       /* (sent but not answered, it may be in force: given back; nothing when it was not sent) */
			return;
		}
		wait_ms(2);
		af_look("the lens motor's supply on");
		if (!af_write(AF_CONTROL, 0x01)) {
			say("camera: the lens motor does not answer with its supply on either: no focus in this session\n");
			(void)vaf(false);
			return;
		}
	}
	if (!af_write(AF_CONTROL, 0x00)) {
		goto failed;
	}
	wait_ms(1);
	if (!af_write(AF_CONTROL, 0x02) || !af_write(AF_MODE, 0x40) || !af_write(AF_TIMING, 0x07) || !af_write(AF_POSITION, 0x190)) {
		goto failed;
	}
	wait_ms(20);
	if (!af_write(AF_POSITION, 0x12c)) {
		goto failed;
	}
	af_position = 0x12c;
	af_look("the lens motor initialised");
	say("camera: the lens motor (DW9800) is up, the lens at %u\n", af_position);
	return;
failed:
	say("camera: the lens motor stopped answering during its initialisation: no focus in this session\n");
}

static void
af_move(uint32_t position)
{
	if (af_position == HAKIOS_CAMERA_NO_FOCUS) {
		return;
	}
	position = position > 1023 ? 1023 : position;
	if (position != af_position && af_write(AF_POSITION, (uint16_t)position)) {
		char when[48];

		snprintf(when, sizeof(when), "the lens from %u to %u", af_position, position);
		af_position = position;
		af_look(when);
	}
}

static void
af_down(void)
{
	if (af_position != HAKIOS_CAMERA_NO_FOCUS) {
		/* down in steps, not to knock the lens against its stop */
		while (af_position > 0) {
			uint32_t was = af_position;

			af_move(af_position > 48 ? af_position - 48 : 0);
			if (af_position == was) {
				break;
			}
			wait_ms(10);
		}
		(void)af_write(AF_CONTROL, 0x01);
		af_position = HAKIOS_CAMERA_NO_FOCUS;
	}
	if (vaf_voted) {
		(void)vaf(false);
	}
}

static bool
exposure(const struct ctab_mode *mode)
{
	uint32_t shutter = 0, gain = 0, vblank = 0;
	bool ok;

	(void)PE_parse_boot_argn("cam_shutter", &shutter, sizeof(shutter));
	(void)PE_parse_boot_argn("cam_gain", &gain, sizeof(gain));
	(void)PE_parse_boot_argn("cam_vblank", &vblank, sizeof(vblank));
	if (hw->regs == REGS_OV_PAGED && !sensor_table("holdon")) {
		return false;
	}
	if (shutter == 0 && gain == 0 && vblank == 0) {
		return true;
	}
	if (!hw->exposure) {
		say("camera: cam_shutter, cam_gain and cam_vblank have been tried on the OV02B10 and the S5KHM2SD: the %s one's "
		    "exposure is its tables'\n", hw->name);
		return true;
	}
	vblank = vblank > 0x3fff ? 0x3fff : vblank;
	if (shutter > mode->frame_length + vblank - 40) {
		shutter = mode->frame_length + vblank - 40;     /* (within the frame's lines, as the sensors want) */
	}
	if (hw->regs == REGS_OV_PAGED) {
		gain = gain > 0xf8 ? 0xf8 : gain;
		ok = sensor_write(0xfd, 0x01, false) && (vblank == 0 || (sensor_write(0x14, (uint16_t)(vblank >> 8), false) &&
		    sensor_write(0x15, (uint16_t)(vblank & 0xff), false))) && (shutter == 0 ||
		    (sensor_write(0x0e, (uint16_t)(shutter >> 8), false) && sensor_write(0x0f, (uint16_t)(shutter & 0xff), false))) &&
		    (gain == 0 || sensor_write(0x22, (uint16_t)gain, false)) && sensor_write(0xfe, 0x02, false) &&
		    sensor_write(0xfd, 0x00, false);
	} else {
		gain = gain > 0x1000 ? 0x1000 : gain;
		ok = sensor_table("holdon") && (vblank == 0 || sensor_write(0x0340, (uint16_t)(mode->frame_length + vblank), true)) &&
		    (shutter == 0 || sensor_write(0x0202, (uint16_t)shutter, true)) && (gain == 0 || sensor_write(0x0204, (uint16_t)gain, true)) &&
		    sensor_table("holdoff");
	}
	if (!ok) {
		give_up("a sensor write nobody acknowledged");
		return false;
	}
	say("camera: the exposure set to %u lines, gain %#x, %u extra lines a frame\n", shutter, gain, vblank);
	return true;
}

static void
capture_sensor(void)
{
	uint32_t width, height, stride, settle = 0, pattern = 0, wanted = 8, mode_n = ~0u;
	const struct ctab_mode *mode;
	struct cam_header *h;
	char res[8];
	uint64_t rate;

	if (!ctab_load()) {
		return;
	}
	/* the mode: cam_mode=N, or the one with the fewest pixels */
	if (!PE_parse_boot_argn("cam_mode", &mode_n, sizeof(mode_n)) || mode_n >= ctab->modes) {
		mode_n = 0;
		for (uint32_t m = 1; m < ctab->modes; m++) {
			if ((uint64_t)ctab_modes[m].width * ctab_modes[m].height <
			    (uint64_t)ctab_modes[mode_n].width * ctab_modes[mode_n].height) {
				mode_n = m;
			}
		}
	}
	mode = &ctab_modes[mode_n];
	snprintf(res, sizeof(res), "res%u", mode_n);
	width = mode->width;
	height = mode->height;
	stride = RAW10_STRIDE(width);
	rate = (uint64_t)mode->pixel_clock * 10 / (mode->lanes ? mode->lanes : 1);
	/* boot-arg cam_frames=N: how many frames are taken, 1 to 300 (default 8) */
	(void)PE_parse_boot_argn("cam_frames", &wanted, sizeof(wanted));
	wanted = wanted < 1 ? 1 : wanted > 300 ? 300 : wanted;
	if (!PE_parse_boot_argn("cam_settle", &settle, sizeof(settle)) || settle == 0) {
		settle = mode->settle;  /* (the module file's, for this mode) */
	}
	if (mode->lanes < 1 || mode->lanes > (mode->cphy ? 3u : 4u) || mode->data_type != RAW10_DT || width % 4 != 0) {
		say("camera: mode %u: %u lanes, data type %#x: not what this is written for\n", mode_n, mode->lanes, mode->data_type);
		return;
	}
	say("camera: mode %u: %ux%u, %u %s at %llu bit/s each, settle count %u\n", mode_n, width, height, mode->lanes,
	    mode->cphy ? "trio(s)" : "lane(s)", rate, settle);
	if (!frames_alloc(width, height, stride, 1)) {
		return;
	}
	if (!power_up() || !healthy() || !read_chip_id() || !sensor_table("init") || !sensor_table(res) || !healthy() ||
	    !titan_up() || !smmu_up() || !csid_reset() ||
	    !csid_config((int)hw->csiphy, mode->lanes, ctab->lane_assign & ((1u << (4 * mode->lanes)) - 1), 0, width, height,
	    mode->cphy != 0) || !ife_config(FRAME_ADDRESSES + CAM_HEADER_BYTES, stride * height, stride * height) ||
	    !csiphy_up(mode->lanes, mode->cphy != 0, ctab->lane_assign, settle, rate)) {
		csiphy_down();
		titan_down();
		return;
	}
	show_paths("configured");
	h = (struct cam_header *)frames;
	(void)PE_parse_boot_argn("cam_pattern", &pattern, sizeof(pattern));
	/*
	 * The picture's order is the one each sensor's module file gives its modes (resolutionData's
	 * colorFilterArrangement, in CamX's order: 0 BGGR, 1 GBRG, 2 GRBG, 3 RGGB -- camera.qcom.so's
	 * TranslateColorFilterArrangement turns it into Android's by the table 3, 2, 1, 0): the OV02B10's
	 * and the OV16A1Q's BGGR, the S5KHM2SD's and the S5K4H7's GRBG (components/camera/tools/camsensor.py lists it).
	 * Their greens are where a picture of a room has them (the sites of a cell that track each
	 * other); red against blue had been guessed from raw levels under a warm light, the other way
	 * round, and every picture had the two swapped. The OV02B10's own colour bars (cam_pattern=1)
	 * are still taken for GBRG, as before: the module file says nothing of them. A sensor whose
	 * module file has no colour bars (the wide one's and the ultra-wide's have none) gives its
	 * picture instead.
	 */
	uint32_t tpg_count = 0;
	const struct ctab_entry *tpgon = ctab_find("tpgon", &tpg_count);

	if (pattern && (tpgon == NULL || tpg_count == 0 || tpgon[0].address == 0)) {
		say("camera: cam_pattern=1: the %s sensor's module file has no colour bars of its own: its picture\n", hw->name);
		pattern = 0;
	}
	h->bayer = pattern ? hw->pattern_bayer : hw->bayer;
	if ((pattern == 0 || sensor_table("tpgon")) && sensor_table("on") && exposure(mode)) {
		/* frames one after the other into the two places, the last of `wanted' kept (the first ones come before the exposure) */
		for (unsigned ms = 0, done = 0; ms < 5000 && done < wanted; ms += 2) {
			sensor_delay(2000);
			if (!(rd(ife + BUS_IRQ_STATUS1) & 1)) {
				continue;
			}
			done++;
			if (done <= 3 || done == wanted) {
				say("camera: frame %u written, %u ms after the sensor was started\n", done, ms + 2);
			}
			blk_put(ife, "IFE", BUS_IRQ_CLEAR1, 1, false);
			blk_put(ife, "IFE", BUS_IRQ_GLOBAL_CLEAR, 1, false);
			if (done < wanted) {
				blk_put(ife, "IFE", WM0_IMAGE_ADDR, (uint32_t)(FRAME_ADDRESSES + CAM_HEADER_BYTES + (uint64_t)(done & 1) *
				    stride * height), false);
				blk_put(ife, "IFE", IFE_REG_UPDATE, 0x2, false);
			}
			h->frames = done < CAM_FRAMES ? done : CAM_FRAMES;
			h->last = (done - 1) & 1;
			if ((done & 7) == 0 && !healthy()) {
				break;
			}
		}
		show_paths("after the frames");
		show_csiphy("after the frames");
		(void)sensor_table("off");
	}
	smmu_faults();
	csiphy_down();
	titan_down();
	hakios_cache_flush(frames, frames + CAM_HEADER_BYTES + 256);
	const uint8_t *p = (const uint8_t *)(frames + CAM_HEADER_BYTES);
	say("camera: the frame's first bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x ... (frames done: %u)\n", p[0], p[1], p[2],
	    p[3], p[4], p[5], p[6], p[7], p[8], p[9], h->frames);
	hakios_cache_clean(frames, frames + CAM_HEADER_BYTES);
}

/* ---- cam=5: the wide camera for Darwin (/dev/hakios-camera) ------------------------------------------- */

/*
 * The sensor's frames for a program (Darwin's camera stack in mediaserverd, libhakios-media.dylib), for
 * as long as it holds /dev/hakios-camera open (sys/board/camera.h): the sensor powered up and
 * streaming at the first read after the open, everything down again at the last close, after
 * IDLE_MS without a read, and on any fault (and then for the rest of the run). The sequence is
 * cam=4's. The frames go into three places in turn (kernel memory, mapped page by page through the
 * SMMU): one the IFE writes, one a read may be copying, one ready.
 *
 * Which place the IFE writes: at each start of frame (RDI 0's SOF, polled every millisecond) the
 * address of the place for the frame after it goes to the write master, which takes it at the next
 * SOF (the register update is latched there): a whole frame's time to give it in, not the blanking
 * between two frames. Should no address be waiting at an SOF (the write master's frame increment is
 * 0 here: it then writes the frame where the last one went, not past it), that frame is counted as
 * written over -- and the one before it, done while no address was waiting, is dropped rather than
 * given to a read that would copy it while it is written over (place_written()). The exposure the
 * program asks for is written between two frames, after the address, in the sensor's group hold; it
 * counts from the second frame after.
 *
 * A picture for the host (HAKIOS_CAMERA_KEEP: what the program made of a frame) goes into one
 * piece of memory the loader hands over after the run, as cam=3 and cam=4's frames do (the first generation's xl camera).
 */
#define RING                    3
#define PLACE_SPARE             (256u << 10)    /* room after each frame: some come longer than the mode's lines */
#define IDLE_MS                 1000
#define FRAME_TIMEOUT_MS        1500
#define EXPOSURE_MARGIN         40
#define GAIN_UNITY              (hw->gain_unity)
#define GAIN_MAX                (hw->gain_max)
#define FRAME_LINES_MAX         0xfff0
#define RDI0_SOF                (1u << 27)      /* IFE_IRQ_STATUS0 (cam_vfe_lite16x.h: sof_irq_mask of RDI 0) */
#define WM0_DONE                1u              /* BUS_IRQ_STATUS1 */

/* QUEUED: the address given for the next frame, which the IFE takes at the next SOF -- not a reader's to take meanwhile */
enum { PLACE_FREE, PLACE_WRITING, PLACE_READY, PLACE_READING, PLACE_QUEUED };

static LCK_GRP_DECLARE(darwin_lck_grp, "hakios-camera");
static LCK_MTX_DECLARE(darwin_lock, &darwin_lck_grp);
static struct {
	uint8_t         state;
	uint64_t        sequence, time;
	uint32_t        exposure_lines, gain, frame_lines;
} place[RING];
static vm_offset_t ring;
static uint64_t ring_bytes, ring_tables_bytes, ring_place_bytes;
static uint32_t ring_frame_bytes, darwin_odd_frames;
static const struct ctab_mode *darwin_mode;
static uint32_t darwin_mode_n, darwin_line_ns;
static volatile bool darwin_open, darwin_streaming, darwin_wanted;  /* open; frames coming; a read wants them */
#define DARWIN_OPEN_CHANNEL        ((void *)(uintptr_t)&darwin_open)  /* what a wait on darwin_open sleeps on: its address */
static uint64_t darwin_last_read, darwin_given, darwin_frames_done, darwin_writes_late;
/*
 * Reads copying out of the ring now (camdev_read, which copies with darwin_lock let go): the ring is not freed
 * under them. Darwin 27's cameracaptured, asking for another sensor while a frame was being read, had the ring
 * taken away mid-copy and the kernel stopped on the cache maintenance of a page no longer there.
 */
static uint32_t darwin_readers;
static struct { uint32_t lines, gain, frame_lines; bool pending; uint32_t focus; bool focus_pending; } darwin_ask;
static struct { uint32_t lines, gain, frame_lines; uint64_t from; } darwin_now, darwin_next;
/*
 * The longest frame the sensor may be making now: the length in force, or a longer one written since and not yet
 * seen to have taken over -- what "no frame in its time" is measured by. darwin_now changes at the second frame
 * after a write (when the exposure shows in the pictures), but a sensor given a longer frame in a frame's blanking
 * may stretch that very blanking: the frame of 0xfff0 lines any process may ask for (1.7 s in a mode of 1300 lines
 * at 30 frames a second) was measured by the 33 ms one before it, given up on after 1.5 s, and the camera was gone
 * for the rest of the run.
 */
static uint32_t darwin_longest_lines;
/*
 * What the frames are (boot-arg cam_pattern): 0 the sensor's picture; 1 colour bars -- the sensor's own,
 * where its module file has a table for them (Android's "testPatternData"), else the CSID's; 2 the
 * CSID's colour bars (no sensor at all). The wide camera's module file has no colour bars of the
 * sensor's (its one test pattern table is empty), so cam_pattern=1 gives it the CSID's.
 */
enum { SOURCE_SENSOR, SOURCE_SENSOR_PATTERN, SOURCE_CSID_TPG };
static int darwin_source = SOURCE_SENSOR;
/* HAKIOS_CAMERA_SENSOR: the sensor (and mode) to change to, taken between sessions */
static volatile unsigned darwin_switch_to;         /* 1 + the sensor's index; 0: none asked for */
static uint32_t darwin_switch_mode;

static uint64_t
darwin_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

/* exposure_lines, gain, frame_lines within what the sensor takes (frame_lines 0: the shortest that holds the exposure) */
static void
exposure_limits(uint32_t *lines, uint32_t *gain, uint32_t *frame_lines)
{
	uint32_t shortest = darwin_mode->frame_length;

	*gain = *gain < GAIN_UNITY ? GAIN_UNITY : *gain > GAIN_MAX ? GAIN_MAX : *gain;
	*lines = *lines < 4 ? 4 : *lines > FRAME_LINES_MAX - EXPOSURE_MARGIN ? FRAME_LINES_MAX - EXPOSURE_MARGIN : *lines;
	if (*frame_lines == 0) {
		*frame_lines = *lines + EXPOSURE_MARGIN > shortest ? *lines + EXPOSURE_MARGIN : shortest;
	}
	*frame_lines = *frame_lines < shortest ? shortest : *frame_lines > FRAME_LINES_MAX ? FRAME_LINES_MAX : *frame_lines;
	if (*lines > *frame_lines - EXPOSURE_MARGIN) {
		*lines = *frame_lines - EXPOSURE_MARGIN;
	}
}

/*
 * The exposure, gain and frame length, written the sensor's way in its group hold (its module file's
 * "holdon" and "holdoff" tables; the OV02B10's page 1 and its 0xfe = 2):
 *   SMIA      0x0340 frame length, 0x0202 coarse integration (lines), 0x0204 analog gain (32 is 1x)
 *   OV16A1Q   0x380e/0x380f frame length (VTS), 0x3500..0x3502 exposure (lines, 20 bits), 0x3508/0x3509
 *             gain (0x100 is 1x) -- the registers its tables set the first exposure in
 *   OV02B10   0x14/0x15 the frame's extra lines, 0x0e/0x0f exposure (lines), 0x22 gain (16 is 1x)
 */
static bool
exposure_write(uint32_t lines, uint32_t gain, uint32_t frame_lines)
{
	bool ok;

	switch (hw->regs) {
	case REGS_SMIA:
		ok = sensor_table("holdon") && sensor_write(0x0340, (uint16_t)frame_lines, true) && sensor_write(0x0202, (uint16_t)lines, true) &&
		    sensor_write(0x0204, (uint16_t)gain, true) && sensor_table("holdoff");
		break;
	case REGS_OV16:
		ok = sensor_table("holdon") && sensor_write(0x380e, (uint16_t)(frame_lines >> 8), false) &&
		    sensor_write(0x380f, (uint16_t)(frame_lines & 0xff), false) && sensor_write(0x3500, (uint16_t)(lines >> 16 & 0xf), false) &&
		    sensor_write(0x3501, (uint16_t)(lines >> 8 & 0xff), false) && sensor_write(0x3502, (uint16_t)(lines & 0xff), false) &&
		    sensor_write(0x3508, (uint16_t)(gain >> 8 & 0x1f), false) && sensor_write(0x3509, (uint16_t)(gain & 0xff), false) &&
		    sensor_table("holdoff");
		break;
	default: {
		uint32_t extra = frame_lines - darwin_mode->frame_length;

		ok = sensor_write(0xfd, 0x01, false) && sensor_write(0x14, (uint16_t)(extra >> 8), false) &&
		    sensor_write(0x15, (uint16_t)(extra & 0xff), false) && sensor_write(0x0e, (uint16_t)(lines >> 8), false) &&
		    sensor_write(0x0f, (uint16_t)(lines & 0xff), false) && sensor_write(0x22, (uint16_t)gain, false) &&
		    sensor_write(0xfe, 0x02, false) && sensor_write(0xfd, 0x00, false);
		break;
	}
	}
	if (!ok) {
		give_up("a sensor write nobody acknowledged");
		return false;
	}
	return true;
}

static void
ring_free(void)
{
	lck_mtx_lock(&darwin_lock);
	while (darwin_readers > 0) {
		(void)msleep(&darwin_readers, &darwin_lock, PZERO, "hakios-camera-readers", NULL);
	}
	lck_mtx_unlock(&darwin_lock);
	if (page_tables != 0) {
		kmem_free(kernel_map, page_tables, ring_tables_bytes);
		page_tables = 0;
	}
	if (ring != 0) {
		kmem_free(kernel_map, ring, ring_bytes);
		ring = 0;
	}
	frames = 0;
	frames_bytes = 0;
}

static bool
ring_alloc(uint32_t frame_bytes)
{
	ring_frame_bytes = frame_bytes;
	ring_place_bytes = round_page(frame_bytes) + PLACE_SPARE;
	ring_bytes = RING * ring_place_bytes;
	if (kmem_alloc(kernel_map, &ring, ring_bytes, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("camera: no memory for %u frames (%llu KB)\n", RING, ring_bytes >> 10);
		ring = 0;
		return false;
	}
	hakios_cache_flush(ring, ring + ring_bytes);
	frames = ring;                  /* (what smmu_up() maps) */
	frames_bytes = ring_bytes;
	ring_tables_bytes = (2 + ((ring_bytes + 0x1fffff) >> 21)) * 4096;
	return true;
}

/* the sensor was sent its "on" in this session: its "off" is due at the end (not to a sensor a start left before it) */
static bool darwin_sensor_on;

/* The session's end, whatever ended it: the sensor's "off", the blocks, the supplies, the memory. */
static void
darwin_stop(const char *why)
{
	lck_mtx_lock(&darwin_lock);
	darwin_streaming = false;
	for (int i = 0; i < RING; i++) {
		place[i].state = PLACE_FREE;
	}
	lck_mtx_unlock(&darwin_lock);
	wakeup(&darwin_frames_done);
	if (!broken && darwin_sensor_on && darwin_source != SOURCE_CSID_TPG) {
		(void)sensor_table("off");
	}
	darwin_sensor_on = false;
	af_down();
	/* (only when the paths were set up: a start that failed before them -- a PMIC that did not answer -- has no CSID mapped) */
	if (csid_running || ife_running) {
		show_paths("at the end");
	}
	smmu_faults();
	csiphy_down();
	titan_down();
	power_down();
	ring_free();
	say("camera: the frames for Darwin stopped (%s): %llu done, %llu given, %llu written over\n", why, darwin_frames_done, darwin_given,
	    darwin_writes_late);
}

/*
 * The places at the IFE's end of a frame and at its start of one (darwin_lock held). `writing' is the place
 * the IFE writes in; `next' the one whose address it was given at the last SOF for the frame after (-1: none was
 * free, every other place a reader's). Without one the IFE writes the next frame where it wrote this one: so a
 * frame done then is dropped, not given to a reader who would copy it while it is written over, and a place a
 * reader holds is never taken back from it.
 */
static bool
place_written(int writing, int next)
{
	if (next < 0 || place[writing].state != PLACE_WRITING) {
		return false;
	}
	place[writing].state = PLACE_READY;
	place[writing].sequence = darwin_frames_done;
	place[writing].time = mach_absolute_time();
	place[writing].exposure_lines = darwin_now.lines;
	place[writing].gain = darwin_now.gain;
	place[writing].frame_lines = darwin_now.frame_lines;
	return true;
}

static void
place_started(int *writing, int *next)
{
	if (*next >= 0) {
		*writing = *next;       /* (the address given at the last SOF, taken at this one) */
		*next = -1;
	} else {
		darwin_writes_late++;   /* no address waiting: this frame goes where the last one went */
	}
	if (place[*writing].state != PLACE_READING) {
		place[*writing].state = PLACE_WRITING;
	}
	/* the address for the frame after this one, taken at the next SOF: a whole frame's time to give it in */
	for (int i = 0; i < RING; i++) {
		int c = (*writing + 1 + i) % RING;

		if (c != *writing && place[c].state != PLACE_READING && (*next < 0 || place[c].sequence < place[*next].sequence)) {
			*next = c;
		}
	}
	if (*next >= 0) {
		place[*next].state = PLACE_QUEUED;      /* (a reader that came now would read what the IFE writes over) */
	}
}

static const char *darwin_res(void);

static bool
darwin_start(void)
{
	uint32_t lines, gain, frame_lines;

	darwin_frames_done = darwin_given = darwin_writes_late = 0;
	darwin_odd_frames = 0;
	if (!ring_alloc(RAW10_STRIDE(darwin_mode->width) * darwin_mode->height)) {
		return false;
	}
	if (darwin_source == SOURCE_CSID_TPG) {
		/* the CSID's colour bars in the mode's size, as cam=3 makes them: no sensor, no CSIPHY */
		if (!titan_up() || !smmu_up() || !csid_reset() ||
		    !csid_config(-1, 4, 0x3210, TPG_VC, darwin_mode->width, darwin_mode->height, false) ||
		    !ife_config(FRAME_ADDRESSES, ring_frame_bytes, 0) || !tpg_start()) {
			return false;
		}
		lck_mtx_lock(&darwin_lock);
		for (int i = 0; i < RING; i++) {
			place[i].state = PLACE_FREE;
			place[i].sequence = 0;            /* (numbered afresh: see the SOF's choice of the next place) */
		}
		place[0].state = PLACE_WRITING;
		lck_mtx_unlock(&darwin_lock);
		darwin_now.lines = darwin_next.lines = darwin_mode->frame_length - EXPOSURE_MARGIN;
		darwin_now.gain = darwin_next.gain = GAIN_UNITY;
		darwin_now.frame_lines = darwin_next.frame_lines = darwin_mode->frame_length;
		darwin_next.from = 0;
		say("camera: the frames for Darwin: the CSID's colour bars, %ux%u (no sensor)\n", darwin_mode->width, darwin_mode->height);
		return true;
	}
	if (!power_up() || !healthy() || !read_chip_id() || !sensor_table("init") || !sensor_table(darwin_res()) || !healthy() ||
	    !titan_up() || !smmu_up() || !csid_reset() ||
	    !csid_config((int)hw->csiphy, darwin_mode->lanes, ctab->lane_assign & ((1u << (4 * darwin_mode->lanes)) - 1), 0, darwin_mode->width,
	    darwin_mode->height, darwin_mode->cphy != 0) || !ife_config(FRAME_ADDRESSES, ring_frame_bytes, 0) ||
	    !csiphy_up(darwin_mode->lanes, darwin_mode->cphy != 0, ctab->lane_assign, darwin_mode->settle,
	    (uint64_t)darwin_mode->pixel_clock * 10 / darwin_mode->lanes)) {
		return false;
	}
	if (darwin_source == SOURCE_SENSOR_PATTERN && !sensor_table("tpgon")) {
		return false;
	}
	lck_mtx_lock(&darwin_lock);
	for (int i = 0; i < RING; i++) {
		place[i].state = PLACE_FREE;
		place[i].sequence = 0;            /* (numbered afresh: see the SOF's choice of the next place) */
	}
	place[0].state = PLACE_WRITING;
	lines = darwin_ask.lines;
	gain = darwin_ask.gain;
	frame_lines = darwin_ask.frame_lines;
	darwin_ask.pending = false;
	lck_mtx_unlock(&darwin_lock);
	exposure_limits(&lines, &gain, &frame_lines);
	darwin_sensor_on = true;
	if (!sensor_table("on") || !exposure_write(lines, gain, frame_lines)) {
		return false;
	}
	af_up(darwin_source == SOURCE_SENSOR);
	darwin_now.lines = darwin_next.lines = lines;
	darwin_now.gain = darwin_next.gain = gain;
	darwin_now.frame_lines = darwin_next.frame_lines = frame_lines;
	darwin_next.from = 0;
	say("camera: the frames for Darwin: mode %u, %ux%u, %u lines of %u ns a frame at the shortest; exposure %u lines, gain %#x, "
	    "frame %u lines\n", darwin_mode_n, darwin_mode->width, darwin_mode->height, darwin_mode->frame_length, darwin_line_ns, lines, gain, frame_lines);
	return true;
}

/*
 * The sensor and mode of the last session that started: where serve_darwin() goes back to when another does not
 * start (with no fault: no memory for its frames, a table it has not), rather than give the camera up for the run.
 */
static bool darwin_started;
static unsigned darwin_started_sensor;
static uint32_t darwin_started_mode;

/* A session, until it ends; false when it did not start without a fault (everything is down again, nothing given up). */
static bool
darwin_session(void)
{
	int writing = 0, next = 0;      /* (place 0's address is the write master's from ife_config(): taken at the first SOF) */
	uint64_t last_frame;

	if (!darwin_start()) {
		darwin_stop("it did not start");
		return broken;
	}
	darwin_started = true;
	darwin_started_sensor = hw_index;
	darwin_started_mode = darwin_mode_n;
	darwin_longest_lines = darwin_now.frame_lines;
	last_frame = darwin_ms();
	blk_put(ife, "IFE", IFE_IRQ_CLEAR0, 0xffffffff, false);
	blk_put(ife, "IFE", IFE_IRQ_GLOBAL_CLEAR, 1, false);
	lck_mtx_lock(&darwin_lock);
	darwin_streaming = true;
	darwin_last_read = darwin_ms();
	lck_mtx_unlock(&darwin_lock);
	while (!broken && darwin_open && darwin_switch_to == 0) {
		uint32_t top, bus;
		uint64_t now;

		wait_ms(1);
		now = darwin_ms();
		top = rd(ife + IFE_IRQ_STATUS0);
		bus = rd(ife + BUS_IRQ_STATUS1);
		if (!(top & RDI0_SOF) && !(bus & WM0_DONE)) {
			/*
			 * (no frame for 1.5 s, or for two frames' time where Darwin asked for frames that long: a frame length
			 * any process may ask for is not to give the camera up for the rest of the run)
			 */
			if (now - last_frame > MAX(FRAME_TIMEOUT_MS,
			    2 * (uint64_t)darwin_longest_lines * darwin_line_ns / 1000000)) {
				show_paths("no frame");
				give_up("no frame in its time");
				break;
			}
			if (now - darwin_last_read > IDLE_MS) {
				break;
			}
			continue;
		}
		blk_put(ife, "IFE", IFE_IRQ_CLEAR0, top & RDI0_SOF, false);
		blk_put(ife, "IFE", IFE_IRQ_GLOBAL_CLEAR, 1, false);
		blk_put(ife, "IFE", BUS_IRQ_CLEAR1, bus & WM0_DONE, false);
		blk_put(ife, "IFE", BUS_IRQ_GLOBAL_CLEAR, 1, false);
		lck_mtx_lock(&darwin_lock);
		if (bus & WM0_DONE) {
			uint32_t ping = rd(csid + CSID_RDI0_BYTES_PING), pong = rd(csid + CSID_RDI0_BYTES_PONG);
			uint32_t data = darwin_mode->width * 10 / 8 * darwin_mode->height;

			last_frame = now;
			darwin_frames_done++;
			if ((ping != data || (pong != data && darwin_frames_done > 1)) && darwin_odd_frames++ < 8) {
				say("camera: frame %llu: the CSID counted %u and %u bytes (the mode's frame is %u)\n", darwin_frames_done, ping, pong,
				    data);
			}
			if (darwin_frames_done >= darwin_next.from && darwin_next.from != 0) {
				darwin_now = darwin_next;
				darwin_next.from = 0;
				darwin_longest_lines = darwin_now.frame_lines;  /* (the last written has taken over: none other is to come) */
			}
			(void)place_written(writing, next);
		}
		if (top & RDI0_SOF) {
			place_started(&writing, &next);
			if (next >= 0) {
				blk_put(ife, "IFE", WM0_IMAGE_ADDR, (uint32_t)(FRAME_ADDRESSES + (uint64_t)next * ring_place_bytes), false);
				blk_put(ife, "IFE", IFE_REG_UPDATE, 0x2, false);
			}
		}
		bool ask = (bus & WM0_DONE) && darwin_ask.pending, focus = (bus & WM0_DONE) && darwin_ask.focus_pending;
		uint32_t lines = darwin_ask.lines, gain = darwin_ask.gain, frame_lines = darwin_ask.frame_lines, position = darwin_ask.focus;

		darwin_ask.pending = darwin_ask.pending && !ask;
		darwin_ask.focus_pending = darwin_ask.focus_pending && !focus;
		lck_mtx_unlock(&darwin_lock);
		if (focus) {
			af_move(position);
		}
		if (bus & WM0_DONE) {
			wakeup(&darwin_frames_done);
		}
		if (ask && darwin_source != SOURCE_CSID_TPG) {
			exposure_limits(&lines, &gain, &frame_lines);
			if (lines != darwin_now.lines || gain != darwin_now.gain || frame_lines != darwin_now.frame_lines) {
				if (!exposure_write(lines, gain, frame_lines)) {
					break;
				}
				darwin_next.lines = lines;
				darwin_next.gain = gain;
				darwin_next.frame_lines = frame_lines;
				darwin_next.from = darwin_frames_done + 2;
				darwin_longest_lines = MAX(darwin_longest_lines, frame_lines);
			}
		}
		if ((bus & WM0_DONE) && darwin_frames_done % 30 == 0) {
			uint32_t fsr = rd(smmu_context + SMMU_CB_FSR);

			if (!healthy()) {
				break;
			}
			if (fsr & SMMU_FSR_ALL) {
				smmu_faults();
				give_up("the SMMU refused the IFE");
				break;
			}
			if (darwin_frames_done == 30 || darwin_frames_done % 900 == 0) {
				show_paths("streaming");
			}
		}
	}
	darwin_stop(broken ? "a fault" : !darwin_open ? "closed" : darwin_switch_to != 0 ? "another sensor asked for" : "nobody reads");
	return true;
}

/*
 * cam_fake=1 (with cam=5; for QEMU): /dev/hakios-camera without the phone's camera. The sessions,
 * sensors, modes and frame headers are cam=5's, from the same tables in fw/, but nothing of the phone
 * is touched: the frames are a colour chart made here -- eight bars (white, beige, red, yellow, green,
 * cyan, blue, 18 % grey) over a grey ramp -- in RAW10 in the sensor's own Bayer order, as bright as the
 * exposure asked for makes them, so that everything above the kernel (the ISP of camera_test and of
 * the media library, the capture graph, Camera.app) runs as it does on the phone and has a known
 * picture to show. cam_fake_fps: frames a second (default 5: QEMU's processor is slow).
 */
static bool fake;

/* the bars' linear reflectance, R, G, B in thousandths */
static const uint16_t fake_bars[8][3] = {
	{ 900, 900, 900 }, { 730, 550, 300 }, { 600, 50, 50 }, { 750, 650, 50 },
	{ 80, 450, 100 }, { 50, 500, 600 }, { 50, 100, 550 }, { 180, 180, 180 },
};

/* one line of the chart, RAW10: `ramp' the grey ramp's, else the bars'; `odd' the second line of the cells */
static void
fake_line(uint8_t *out, uint32_t width, bool odd, bool ramp, uint64_t light)
{
	static const int red[4] = { 0, 1, 2, 3 }, blue[4] = { 3, 2, 1, 0 };
	int r = red[hw->bayer & 3], b = blue[hw->bayer & 3];
	uint32_t v[4];

	for (uint32_t x = 0; x < width; x += 4) {
		for (int i = 0; i < 4; i++) {
			uint32_t px = x + (uint32_t)i, site = (odd ? 2u : 0u) + (px & 1), c = site == (uint32_t)r ? 0 : site == (uint32_t)b ? 2 : 1;
			uint64_t refl = ramp ? (uint64_t)px * 1000 / width : fake_bars[px * 8 / width][c];
			/* (2/15 of a 10-bit step per thousandth of reflectance and line at 1x: 18 % grey at AE's 15 % of white near 6000) */
			uint64_t level = 64 + refl * light * 2 / 15000;

			v[i] = level > 1023 ? 1023 : (uint32_t)level;
		}
		uint8_t *g = out + (x / 4) * 5;

		g[0] = (uint8_t)(v[0] >> 2);
		g[1] = (uint8_t)(v[1] >> 2);
		g[2] = (uint8_t)(v[2] >> 2);
		g[3] = (uint8_t)(v[3] >> 2);
		g[4] = (uint8_t)((v[0] & 3) | (v[1] & 3) << 2 | (v[2] & 3) << 4 | (v[3] & 3) << 6);
	}
}

/* place p filled with the chart as an exposure of `lines' at `gain' shows it */
static void
fake_fill(int p, uint32_t lines, uint32_t gain)
{
	uint8_t *at = (uint8_t *)(ring + (uint64_t)p * ring_place_bytes);
	uint32_t width = darwin_mode->width, height = darwin_mode->height, stride = RAW10_STRIDE(width), ramp_from = height * 3 / 4 & ~1u;
	uint64_t light = (uint64_t)lines * gain / GAIN_UNITY;

	fake_line(at, width, false, false, light);
	fake_line(at + stride, width, true, false, light);
	fake_line(at + (uint64_t)ramp_from * stride, width, false, true, light);
	fake_line(at + (uint64_t)(ramp_from + 1) * stride, width, true, true, light);
	for (uint32_t y = 2; y < height; y++) {
		if (y != ramp_from && y != ramp_from + 1) {
			memcpy(at + (uint64_t)y * stride, at + (uint64_t)((y & 1) + (y >= ramp_from ? ramp_from : 0)) * stride, stride);
		}
	}
}

/* a session of cam_fake: darwin_session()'s, with the chart for the sensor and a timer for its frames */
static void
fake_session(void)
{
	uint32_t fps = 5, lines, gain, frame_lines, filled_lines[RING] = { 0 }, filled_gain[RING] = { 0 };

	(void)PE_parse_boot_argn("cam_fake_fps", &fps, sizeof(fps));
	fps = fps < 1 ? 1 : fps > 30 ? 30 : fps;
	darwin_frames_done = darwin_given = darwin_writes_late = 0;
	if (!ring_alloc(RAW10_STRIDE(darwin_mode->width) * darwin_mode->height)) {
		return;
	}
	lck_mtx_lock(&darwin_lock);
	for (int i = 0; i < RING; i++) {
		place[i].state = PLACE_FREE;
		place[i].sequence = 0;
	}
	lines = darwin_ask.lines;
	gain = darwin_ask.gain;
	frame_lines = darwin_ask.frame_lines;
	darwin_ask.pending = false;
	exposure_limits(&lines, &gain, &frame_lines);
	darwin_now.lines = lines;
	darwin_now.gain = gain;
	darwin_now.frame_lines = frame_lines;
	darwin_streaming = true;
	darwin_last_read = darwin_ms();
	lck_mtx_unlock(&darwin_lock);
	say("camera: the frames for Darwin: cam_fake's colour chart, %ux%u in %s's order, %u a second\n", darwin_mode->width, darwin_mode->height,
	    hw->chip, fps);
	while (darwin_open && darwin_switch_to == 0) {
		int p = -1;

		wait_ms(1000 / fps);
		lck_mtx_lock(&darwin_lock);
		if (darwin_ms() - darwin_last_read > IDLE_MS) {
			lck_mtx_unlock(&darwin_lock);
			break;
		}
		if (darwin_ask.pending) {
			lines = darwin_ask.lines;
			gain = darwin_ask.gain;
			frame_lines = darwin_ask.frame_lines;
			darwin_ask.pending = false;
			exposure_limits(&lines, &gain, &frame_lines);
			darwin_now.lines = lines;
			darwin_now.gain = gain;
			darwin_now.frame_lines = frame_lines;
		}
		for (int i = 0; i < RING; i++) {
			if (place[i].state != PLACE_READING && (p < 0 || place[i].sequence < place[p].sequence)) {
				p = i;
			}
		}
		if (p >= 0) {
			place[p].state = PLACE_WRITING;
		}
		lck_mtx_unlock(&darwin_lock);
		if (p < 0) {
			darwin_writes_late++;
			continue;
		}
		if (filled_lines[p] != darwin_now.lines || filled_gain[p] != darwin_now.gain) {
			fake_fill(p, darwin_now.lines, darwin_now.gain);
			filled_lines[p] = darwin_now.lines;
			filled_gain[p] = darwin_now.gain;
		}
		lck_mtx_lock(&darwin_lock);
		place[p].state = PLACE_READY;
		place[p].sequence = ++darwin_frames_done;
		place[p].time = mach_absolute_time();
		place[p].exposure_lines = darwin_now.lines;
		place[p].gain = darwin_now.gain;
		place[p].frame_lines = darwin_now.frame_lines;
		lck_mtx_unlock(&darwin_lock);
		wakeup(&darwin_frames_done);
	}
	lck_mtx_lock(&darwin_lock);
	darwin_streaming = false;
	for (int i = 0; i < RING; i++) {
		place[i].state = PLACE_FREE;
	}
	lck_mtx_unlock(&darwin_lock);
	wakeup(&darwin_frames_done);
	ring_free();
	say("camera: the frames for Darwin stopped (%s): %llu done, %llu given\n", !darwin_open ? "closed" : darwin_switch_to != 0 ?
	    "another sensor asked for" : "nobody reads", darwin_frames_done, darwin_given);
}

static const char *
darwin_res(void)
{
	static char res[8];

	snprintf(res, sizeof(res), "res%u", darwin_mode_n);
	return res;
}

static int
camdev_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	if (broken) {
		return EIO;
	}
	darwin_open = true;
	return 0;
}

/* (the last close) */
static int
camdev_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	darwin_open = false;
	darwin_wanted = false;
	wakeup(DARWIN_OPEN_CHANNEL);
	return 0;
}

static int
camdev_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hakios_camera_frame f;
	uint64_t since = darwin_ms();
	int b = -1, error = 0;
	vm_offset_t at;
	uint32_t frame_bytes;

	lck_mtx_lock(&darwin_lock);
	darwin_last_read = darwin_ms();
	if (!darwin_streaming && !darwin_wanted) {
		darwin_wanted = true;
		wakeup(DARWIN_OPEN_CHANNEL);
	}
	for (;;) {
		if (broken) {
			lck_mtx_unlock(&darwin_lock);
			return EIO;
		}
		b = -1;
		for (int i = 0; i < RING; i++) {
			if (place[i].state == PLACE_READY && place[i].sequence > darwin_given && (b < 0 || place[i].sequence > place[b].sequence)) {
				b = i;
			}
		}
		if (b >= 0) {
			break;
		}
		if (darwin_ms() - since > (darwin_streaming ? 3000u : 15000u)) {      /* (the sensor's tables take seconds) */
			lck_mtx_unlock(&darwin_lock);
			return EAGAIN;
		}
		if (!darwin_streaming && !darwin_wanted) {
			darwin_wanted = true;      /* (the frames stopped meanwhile: another sensor) */
			wakeup(DARWIN_OPEN_CHANNEL);
		}
		struct timespec ts = { 0, 100 * NSEC_PER_MSEC };
		error = msleep(&darwin_frames_done, &darwin_lock, PZERO | PCATCH, "hakios-camera", &ts);
		if (error != 0 && error != EWOULDBLOCK) {
			lck_mtx_unlock(&darwin_lock);
			return error;
		}
		darwin_last_read = darwin_ms();
	}
	place[b].state = PLACE_READING;
	darwin_given = place[b].sequence;
	darwin_readers++;
	at = ring + (uint64_t)b * ring_place_bytes;    /* (taken while the ring cannot change) */
	frame_bytes = ring_frame_bytes;
	bzero(&f, sizeof(f));
	memcpy(f.magic, HAKIOS_CAMERA_MAGIC, 8);
	f.header_bytes = sizeof(f);
	f.width = darwin_mode->width;
	f.height = darwin_mode->height;
	f.stride = RAW10_STRIDE(darwin_mode->width);
	f.format = 1;
	f.bayer = darwin_source == SOURCE_CSID_TPG ? 0 : darwin_source == SOURCE_SENSOR_PATTERN ? hw->pattern_bayer : hw->bayer;
	f.black = 64;
	f.sequence = place[b].sequence;
	f.time = place[b].time;
	f.exposure_lines = place[b].exposure_lines;
	f.line_ns = darwin_line_ns;
	f.gain = place[b].gain;
	f.gain_unity = GAIN_UNITY;
	f.gain_max = GAIN_MAX;
	f.frame_lines = place[b].frame_lines;
	f.frame_lines_min = darwin_mode->frame_length;
	f.frame_lines_max = FRAME_LINES_MAX;
	f.margin = EXPOSURE_MARGIN;
	f.sensor = hw_index;
	f.focus = af_position;
	strlcpy(f.chip, hw->chip, sizeof(f.chip));
	f.upside_down = hakios_board()->cameras.sensors[hw_index].upside_down;
	f.darwin_back = hakios_board()->cameras.darwin_sensor;
	f.darwin_front = hakios_board()->cameras.darwin_front;
	lck_mtx_unlock(&darwin_lock);

	error = uiomove((caddr_t)&f, (int)MIN(sizeof(f), (size_t)uio_resid(uio)), uio);
	if (error == 0 && uio_resid(uio) > 0) {
		uint32_t n = (uint32_t)MIN((uint64_t)frame_bytes, (uint64_t)uio_resid(uio));

		uint64_t t0 = mach_absolute_time(), t1, t2;

		/* (what the IFE wrote went past the processor's caches) */
		hakios_cache_flush(at, at + n);
		t1 = mach_absolute_time();
		error = uiomove((caddr_t)at, (int)n, uio);
		t2 = mach_absolute_time();
		if (f.sequence <= 3 || f.sequence % 300 == 0) {
			uint64_t a, b;

			absolutetime_to_nanoseconds(t1 - t0, &a);
			absolutetime_to_nanoseconds(t2 - t1, &b);
			say("camera: frame %llu read: %u bytes, the caches' lines forgotten in %llu us, copied in %llu us\n", f.sequence, n,
			    a / 1000, b / 1000);
		}
	}
	lck_mtx_lock(&darwin_lock);
	if (place[b].state == PLACE_READING) {
		place[b].state = PLACE_READY;
	}
	darwin_last_read = darwin_ms();
	if (--darwin_readers == 0) {
		wakeup(&darwin_readers);
	}
	lck_mtx_unlock(&darwin_lock);
	return error;
}

/* one picture kept at a time: two writers at once would leave one's header over the other's pixels */
static LCK_MTX_DECLARE(keep_lock, &darwin_lck_grp);

/* HAKIOS_CAMERA_KEEP: a picture for the host, in one piece of memory the loader hands over. */
static int
keep_picture(const struct hakios_camera_command *c, struct uio *uio)
{
	struct cam_header *h;
	uint64_t want = round_page(CAM_HEADER_BYTES + (uint64_t)c->bytes);
	int error;

	if (c->bytes == 0 || c->d < 1 || c->d > 3 || (uint64_t)c->c * c->b > (uint64_t)c->bytes * 2) {
		return EINVAL;
	}
	if (keep == 0 || keep_bytes < want) {
		return ENOSPC;                  /* (the piece is made when the device is, while there is memory in one piece) */
	}
	lck_mtx_lock(&keep_lock);
	h = (struct cam_header *)keep;
	bzero(h, CAM_HEADER_BYTES);
	memcpy(h->magic, "RWCAM001", 8);
	h->width = c->a;
	h->height = c->b;
	h->stride = c->c;
	h->format = c->d;
	h->bayer = c->d == 1 ? hw->bayer : 0;
	h->frame_bytes = c->bytes;
	h->source = 2;
	strlcpy(h->sensor, "Darwin", sizeof(h->sensor));
	error = uiomove((caddr_t)(keep + CAM_HEADER_BYTES), (int)MIN((uint64_t)c->bytes, (uint64_t)uio_resid(uio)), uio);
	h->frames = error == 0 ? 1 : 0;
	hakios_cache_clean(keep, keep + want);
	loader_recording[0] = kvtophys(keep);
	loader_recording[1] = want;
	loader_recording[2] = (uint64_t)keep;
	lck_mtx_unlock(&keep_lock);
	say("camera: a picture from Darwin for the host: %ux%u, format %u, %u bytes\n", c->a, c->b, c->d, c->bytes);
	return error;
}

static int
camdev_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hakios_camera_command c;
	int error;

	if (uio_resid(uio) < (user_ssize_t)sizeof(c)) {
		return EINVAL;
	}
	if ((error = uiomove((caddr_t)&c, sizeof(c), uio)) != 0) {
		return error;           /* (the writer's memory could not be read: its own error, not "invalid") */
	}
	if (c.command == HAKIOS_CAMERA_EXPOSURE) {
		lck_mtx_lock(&darwin_lock);
		darwin_ask.lines = c.a;
		darwin_ask.gain = c.b;
		darwin_ask.frame_lines = c.c;
		darwin_ask.pending = true;
		lck_mtx_unlock(&darwin_lock);
		return 0;
	}
	if (c.command == HAKIOS_CAMERA_KEEP) {
		return keep_picture(&c, uio);
	}
	if (c.command == HAKIOS_CAMERA_FOCUS) {
		lck_mtx_lock(&darwin_lock);
		darwin_ask.focus = c.a;
		darwin_ask.focus_pending = true;
		lck_mtx_unlock(&darwin_lock);
		return 0;
	}
	if (c.command == HAKIOS_CAMERA_SENSOR) {
		if (c.a >= sensor_count) {
			return EINVAL;
		}
		lck_mtx_lock(&darwin_lock);
		darwin_switch_mode = c.b;
		darwin_switch_to = c.a + 1;
		lck_mtx_unlock(&darwin_lock);
		wakeup(DARWIN_OPEN_CHANNEL);
		return 0;
	}
	return EINVAL;
}

static const struct cdevsw camdev_cdevsw = {
	.d_open = camdev_open, .d_close = camdev_close, .d_read = camdev_read, .d_write = camdev_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/*
 * The device node, made as the thread starts (before mediaserverd does: its capture stack looks for
 * cameras once, when it starts); reads wait until serve_darwin() is there to answer them.
 */
static bool camdev_made;

static void
camdev_make(void)
{
	int major = cdevsw_add(-1, &camdev_cdevsw);

	camdev_made = major >= 0 && devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-camera") != NULL;
}

/*
 * A mode the sessions take: at most 13 megapixels and 30 frames a second (31 with what a table rounds). The
 * ring is three of its frames, wired; a mode past that, asked for by any process that opens the node, would
 * take hundreds of megabytes of the phone's memory.
 */
#define DARWIN_MOST_PIXELS      13000000ull
#define DARWIN_MOST_FPS_MILLI   31000u

static bool
darwin_mode_fits(const struct ctab_mode *o)
{
	return (uint64_t)o->width * o->height <= DARWIN_MOST_PIXELS && o->fps_milli <= DARWIN_MOST_FPS_MILLI;
}

/*
 * The sensor (and its mode) the sessions take: its tables, the mode asked for or else the one with
 * the most pixels of those at 4:3 that darwin_mode_fits() (the wide one's 4000x3000, the front one's
 * 2304x1728, the ultra-wide's 3264x2448, the macro's 1600x1200), the colour bars' source (cam_pattern),
 * the first exposure. False if the sensor cannot be used, or the mode asked for is past those limits.
 */
static bool
darwin_select(unsigned index, uint32_t mode_n)
{
	select_sensor(index);
	if (!fake) {
		find_pins();
	}
	if (!ctab_load()) {
		return false;
	}
	if (mode_n >= ctab->modes) {
		uint64_t best = 0;

		mode_n = 0;
		for (uint32_t m = 0; m < ctab->modes; m++) {
			const struct ctab_mode *o = &ctab_modes[m];
			uint64_t pixels = (uint64_t)o->width * o->height;

			if (o->width * 3 == o->height * 4 && darwin_mode_fits(o) && pixels > best) {
				best = pixels;
				mode_n = m;
			}
		}
	}
	if (!darwin_mode_fits(&ctab_modes[mode_n])) {
		say("camera: mode %u (%ux%u, %u.%03u frames/s) is more than the sessions take (%llu pixels, %u.%03u frames/s at most)\n", mode_n,
		    ctab_modes[mode_n].width, ctab_modes[mode_n].height, ctab_modes[mode_n].fps_milli / 1000, ctab_modes[mode_n].fps_milli % 1000,
		    DARWIN_MOST_PIXELS, DARWIN_MOST_FPS_MILLI / 1000, DARWIN_MOST_FPS_MILLI % 1000);
		return false;
	}
	darwin_mode_n = mode_n;
	darwin_mode = &ctab_modes[mode_n];
	if (darwin_mode->lanes < 1 || darwin_mode->lanes > (darwin_mode->cphy ? 3u : 4u) || darwin_mode->data_type != RAW10_DT ||
	    darwin_mode->width % 4 != 0 || darwin_mode->fps_milli == 0 || darwin_mode->frame_length == 0) {
		say("camera: mode %u is not what this is written for\n", mode_n);
		return false;
	}
	darwin_line_ns = (uint32_t)(1000000000000ull / ((uint64_t)darwin_mode->fps_milli * darwin_mode->frame_length));
	uint32_t pattern = 0, count = 0;
	const struct ctab_entry *tpgon;

	(void)PE_parse_boot_argn("cam_pattern", &pattern, sizeof(pattern));
	tpgon = ctab_find("tpgon", &count);
	darwin_source = pattern == 0 ? SOURCE_SENSOR : pattern == 1 && tpgon != NULL && count > 0 && tpgon[0].address != 0 ?
	    SOURCE_SENSOR_PATTERN : SOURCE_CSID_TPG;
	if (pattern != 0) {
		say("camera: cam_pattern=%u: %s\n", pattern, darwin_source == SOURCE_SENSOR_PATTERN ? "the sensor's colour bars (its tpgon table)" :
		    "the CSID's colour bars");
	}
	/* the first exposure, until the program asks for its own: boot-args as cam=4's, else a room's */
	darwin_ask.lines = darwin_mode->frame_length - EXPOSURE_MARGIN;
	darwin_ask.gain = 8 * GAIN_UNITY;
	darwin_ask.frame_lines = 0;
	(void)PE_parse_boot_argn("cam_shutter", &darwin_ask.lines, sizeof(darwin_ask.lines));
	(void)PE_parse_boot_argn("cam_gain", &darwin_ask.gain, sizeof(darwin_ask.gain));
	say("camera: /dev/hakios-camera: the %s camera's frames (mode %u: %ux%u, %u.%03u frames/s at the most), whenever it is "
	    "read\n", hw->name, mode_n, darwin_mode->width, darwin_mode->height, darwin_mode->fps_milli / 1000, darwin_mode->fps_milli % 1000);
	return true;
}

/* cam=5: a session whenever a read wants frames, another sensor whenever one is asked for, until a fault. */
static void
serve_darwin(void)
{
	uint32_t mode_n = ~0u;

	if (!camdev_made) {
		say("camera: /dev/hakios-camera cannot be made\n");
		return;
	}
	(void)PE_parse_boot_argn("cam_mode", &mode_n, sizeof(mode_n));
	if (!darwin_select(hw_index, mode_n) && (mode_n == ~0u || !darwin_select(hw_index, ~0u))) {
		/*
		 * (a cam_mode that cannot be used: the usual mode instead.) Neither: no frames in this run, and said so
		 * to whoever opens the node -- it stays there, and was opened and read as if the camera were coming, each
		 * read waiting its 15 s for frames nothing would make, and each HAKIOS_CAMERA_SENSOR taken and never done.
		 */
		give_up("the sensor's tables or modes cannot be used");
		say("camera: /dev/hakios-camera: no frames in this run\n");
		return;
	}
	while (!broken) {
		lck_mtx_lock(&darwin_lock);
		while (!broken && !(darwin_open && darwin_wanted) && darwin_switch_to == 0) {
			(void)msleep(DARWIN_OPEN_CHANNEL, &darwin_lock, PZERO, "hakios-camera-idle", NULL);
		}
		unsigned to = darwin_switch_to;

		darwin_switch_to = 0;
		lck_mtx_unlock(&darwin_lock);
		if (broken) {
			break;
		}
		if (to != 0) {
			/* (the last session is over: everything is down) */
			unsigned was_sensor = hw_index;
			uint32_t was_mode = darwin_mode_n;

			if (!darwin_select(to - 1, darwin_switch_mode)) {
				/*
				 * A sensor or a mode that cannot be used, asked for by any process that opens the node: the one before
				 * is taken again, rather than the camera given up for the rest of the run.
				 */
				say("camera: sensor %u, mode %u cannot be used: back to sensor %u, mode %u\n", to - 1, darwin_switch_mode,
				    was_sensor, was_mode);
				if (!darwin_select(was_sensor, was_mode)) {
					give_up("neither the sensor asked for nor the one before can be used");
					break;
				}
			}
			continue;
		}
		lck_mtx_lock(&darwin_lock);
		darwin_wanted = false;
		lck_mtx_unlock(&darwin_lock);
		if (fake) {
			fake_session();
		} else if (!darwin_session()) {
			/*
			 * It did not start, with no fault, and everything is down again: back to the sensor and mode that
			 * last started, if that was another. Else (the first session, or that one itself) given up as before.
			 */
			unsigned was_sensor = hw_index;
			uint32_t was_mode = darwin_mode_n;

			if (darwin_started && (darwin_started_sensor != was_sensor || darwin_started_mode != was_mode)) {
				say("camera: sensor %u, mode %u did not start: back to sensor %u, mode %u\n", was_sensor, was_mode,
				    darwin_started_sensor, darwin_started_mode);
				if (darwin_select(darwin_started_sensor, darwin_started_mode)) {
					continue;
				}
			}
			give_up("the camera did not start");
			break;
		}
	}
	say("camera: /dev/hakios-camera: no more frames in this run\n");
}

/* ---- the look, and the thread ----------------------------------------------------------------------- */

static void
show_ldos(void)
{
	const struct hakios_camera_supply *all[3] = { &hw->vana, &hw->vdig, &hw->vio };

	for (int i = 0; i < 3; i++) {
		if (all[i]->kind == HAKIOS_SUPPLY_LDO) {
			show_ldo(all[i]->ldo);
		}
	}
}

static void
look(void)
{
	uint8_t regs[5], type = 0, subtype = 0, sts = 0, revid[4] = { 0 }, enable = 0;

	show_clocks();
	show_pin(pmic_bus.pins.sda, "the PM8008i's I2C data");
	show_pin(pmic_bus.pins.scl, "the PM8008i's I2C clock");
	show_pin(hw->mclk_pin, "the sensor's MCLK");
	show_pin(hw->sda, "the sensor's CCI data");
	show_pin(hw->scl, "the sensor's CCI clock");
	show_pin(hw->reset, "the sensor's reset");
	if (hw->vana.kind == HAKIOS_SUPPLY_GPIO) {
		show_pin(hw->vana.gpio, "the sensor's VANA switch");
	}
	if (!pgpio_found || pgpio_read(PGPIO_TYPE, &type) != NULL || pgpio_read(PGPIO_SUBTYPE, &subtype) != NULL ||
	    pgpio_read(PGPIO_RT_STS, &sts) != NULL || !pgpio_read_all(regs)) {
		say("camera: the PM8008i's reset GPIO cannot be read\n");
		return;
	}
	say("camera: the PM8008i's reset GPIO: type 0x%02x subtype 0x%02x, at %u; vin 0x%02x pull 0x%02x mode 0x%02x "
	    "source 0x%02x enable 0x%02x: %s\n", type, subtype, sts & 1, regs[0], regs[1], regs[2], regs[3], regs[4],
	    pgpio_active(regs) ? "as Android's pm8008i_active (out of reset)" : "not as Android's pm8008i_active");
	if (!pm8008_get(PM8008_CHIP, REVID_BASE, revid, 4)) {
		say("camera: the PM8008i does not answer (in reset, or its bus is not at rest)\n");
		return;
	}
	(void)pm8008_get(PM8008_CHIP, MISC_CHIP_ENABLE, &enable, 1);
	say("camera: the PM8008i: revision %02x %02x %02x %02x, chip enable 0x%02x\n", revid[0], revid[1], revid[2], revid[3], enable);
	show_ldos();
}

static void
camera_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t secs = 20;
	char which[16] = "";

	/* boot-arg cam_after=N: how many seconds after the start the camera is looked at (default 20: the system up first) */
	(void)PE_parse_boot_argn("cam_after", &secs, sizeof(secs));
	if (level == 5) {
		camdev_make();
	}
	select_sensor(level == 5 ? hakios_board()->cameras.darwin_sensor : hakios_board()->cameras.test_sensor);
	if (PE_parse_boot_arg_str("cam_sensor", which, sizeof(which))) {
		for (unsigned i = 0; i < sensor_count; i++) {
			if (strncmp(which, sensors[i].name, strlen(which)) == 0 && (sensors[i].name[strlen(which)] == ' ' ||
			    sensors[i].name[strlen(which)] == '-')) {
				select_sensor(i);
			}
		}
	}
	if (fake) {
		say("camera: cam=5 cam_fake=1: the %s camera's frames for Darwin are a chart made here; nothing of the phone is touched\n",
		    hw->name);
		serve_darwin();
		say("camera: done\n");
		thread_terminate(current_thread());
	}
	wait_ms(secs * 1000);           /* (the system up first: nothing else is on these buses then) */
	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	gcc = ml_io_map(GCC_PHYS, 0x30000);
	camcc = ml_io_map(CAMCC_PHYS, CAMCC_SIZE);
	if (!hakios_spmi_map(&arb)) {
		say("camera: PMIC arbiter version %#x, not 5, 7 or 8: the PM8008i's reset GPIO is left alone\n", arb.version);
		arb.channels = 0;       /* (no channel is looked for, so nothing of a PMIC is read or written) */
	}
	say("camera: cam=%u: the %s camera%s\n", level, hw->name, level == 5 ? ": its frames for Darwin" :
	    level == 4 ? ": its frames through its CSIPHY and RDI 0 into memory" :
	    level == 3 ? ": the CSID's test pattern through RDI 0 into memory" :
	    level == 2 ? ": powered up, its chip ID read, powered down" : ": a look, reading only");
	pgpio_find();
	find_pins();
	look();
	look_around();
	if (level == 3) {
		capture_tpg();
		power_down();
	} else if (level == 5) {
		serve_darwin();
		power_down();
	} else if (level == 4) {
		capture_sensor();
		power_down();
	} else if (level >= 2) {
		if (ctab_load() && power_up() && healthy()) {
			(void)read_chip_id();
			(void)healthy();
			show_ldos();
		}
		power_down();
		look();
	}
	say("camera: done\n");
	thread_terminate(current_thread());
}

void
hakios_camera_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("cam", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (!hakios_board_has(HAKIOS_HAS_CAMERA_SM7325)) {
		/* the camera blocks are an SM7325's, the sensors' supplies a PM8008's: not on a board that does not say so */
		say("camera: disabled: this board's board.dts lists no \"camera-sm7325\"\n");
		level = 0;
		return;
	}
	if (!sensors_from_board()) {
		level = 0;
		return;
	}
	pmic_bus.pins.sda = hakios_board()->cameras.pmic_sda;
	pmic_bus.pins.scl = hakios_board()->cameras.pmic_scl;
	if (hakios_board()->spmi.core == 0 || hakios_board()->spmi.channels == 0) {
		say("camera: disabled: the SoC file's spmi { } does not say where the arbiter and its channels are\n");
		level = 0;
		return;
	}
	if (hakios_board()->pins.tlmm == 0 || hakios_board()->clocks.gcc == 0 || hakios_board()->rpmh.cmd_db == 0 ||
	    hakios_board()->smmu.apps == 0 || hakios_board()->camera.camcc == 0 || hakios_board()->camera.ife == 0 ||
	    hakios_board()->camera.csid == 0 || hakios_board()->camera.csiphy == 0) {
		say("camera: disabled: the SoC file does not say where the pins, the clock controller, RPMh, the SMMU and the "
		    "camera subsystem's blocks are (pins { }, clocks { }, rpmh { }, smmu { }, camera { })\n");
		level = 0;
		return;
	}
	level = level > 5 ? 5 : level;
	uint32_t fake_n = 0;

	fake = level == 5 && PE_parse_boot_argn("cam_fake", &fake_n, sizeof(fake_n)) && fake_n != 0;
	if (level >= 3) {
		/*
		 * Memory in one piece for the host (cam_keep_mb, default 24: a 12-megapixel NV12 picture of cam=5,
		 * or two frames of cam=3 and cam=4 up to 12 MB each), taken now, while the system's memory still
		 * is in large pieces.
		 */
		uint32_t keep_mb = 24;

		(void)PE_parse_boot_argn("cam_keep_mb", &keep_mb, sizeof(keep_mb));
		keep_bytes = (uint64_t)(keep_mb > 64 ? 64 : keep_mb) << 20;
		if (keep_bytes != 0 && kmem_alloc_contig(kernel_map, &keep, keep_bytes, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("camera: no memory in one piece for a picture for the host (%u MB)\n", keep_mb);
			keep = 0;
			keep_bytes = 0;
		}
	}
	if (kernel_thread_start(camera_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end and on a panic's way out: whatever was switched on, off. */
void
hakios_camera_quiesce(void)
{
	if (level < 2 || tlmm == 0) {
		return;
	}
	leaving = true;
	broken = false;                 /* (one last time, whatever went before) */
	csiphy_down();
	titan_down();
	af_down();                      /* (the lens motor's supply vote too: it stayed on after the kernel had left) */
	power_down();
}
