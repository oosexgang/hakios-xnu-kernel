/*
 * A FocalTech FT3658U touch screen on SPI (Android's fts_ts, running chip ID 0x5652).
 * Written from: OKX sm8350-kernel-OKX 08bbb0c150ea, renoir-sm7350.dtsi,
 * shima-qupv3.dtsi, pinctrl-shima.c and gcc-shima.c. Protocol facts:
 * focaltech_3658u/focaltech_spi.c and focaltech_core.{c,h}.
 * GENI/RPMh FIFO transport is adapted from this project's board/goodix.c.
 * Only with touchscreen=1 on a board that lists "touch-ft3658u"; which serial engine, clocks, pins
 * and supplies is the board file's touch { } (the numbers named in comments are the first phone's).
 * touchscreen=-1 only looks: the clocks' and pins' state as the firmware left them, in the log, and
 * nothing powered, configured or sent.
 * No firmware flashing or bootloader
 * commands; the chip must boot its installed application firmware (0x5652).
 * Polling replaces Linux IRQ delivery; Linux IRQ283 is not a GIC interrupt ID.
 * The panel's size is the board's touch { } coordinates, 1080 x 2400 (the first phone's) where it says none.
 *
 * When bring-up fails, when the polling gives up, and at the run's end (and on a panic's way out) the controller is
 * powered off as far as this driver powered it: held in reset, the votes for its analog and then its I/O supply
 * withdrawn, the clock votes this driver added (only those) dropped, and its pins configured as bring-up found them.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/board/input.h>
#include <sys/systm.h>

static vm_offset_t tlmm, gcc, se, rsc, cmd_db;

/*
 * The thread and the run's end (hakios_fts_quiesce()): the thread holds `busy' around each transaction with the
 * controller and starts none once `leaving' is set; `in_transfer' is set while the chip is selected, so that the run's
 * end knows a transfer was cut off in its middle.
 */
static bool leaving, busy, in_transfer;

#define HAKIOS_MMIO_SYNC
#include "kernel.h"
#include "board.h"
#include "mmio.h"
#include "time.h"

/* On the way out every wait spins: interrupts are off and the other cores stopped (as in board/bt_uart.c). */
static void
fts_wait_us(uint32_t us)
{
	if (__atomic_load_n(&leaving, __ATOMIC_SEQ_CST)) {
		spin_us(us);
	} else {
		sleep_us(us);
	}
}
#define sleep_us fts_wait_us             /* (for RPMh's and the serial engine's waits, included below) */

/*
 * What the driver says goes to the loader's log as it is said, and to the kernel's: the kernel's own
 * reaches the loader only when a run is over, and a touch screen that does not answer is looked for
 * while the phone runs.
 */
#include "log.h"
#define printf hakios_say

static char sleep_event;
static void
fts_sleep_ms(uint32_t ms)
{
    /* Yield the CPU while polling/resetting; do not busy-spin for whole milliseconds. */
    assert_wait_timeout((event_t)&sleep_event, THREAD_UNINT, ms, NSEC_PER_MSEC);
    thread_block(THREAD_CONTINUE_NULL);
}

/* ---- pins (TLMM) ------------------------------------------------------------------------------- */

#define TLMM_PHYS               (hakios_board()->pins.tlmm)     /* the SoC file's pins { } */
#define HAKIOS_TLMM             tlmm
#include "tlmm.h"                    /* a pin's two registers, and the four things done with them */

/* Which pins, which serial engine and which of its clocks: the board file's touch { } (board/board.h). */
#define WIRED                   (&hakios_board()->touch)
#define PIN_DATA0               ((unsigned)WIRED->data_in)
#define PIN_DATA1               ((unsigned)WIRED->data_out)
#define PIN_CLK                 ((unsigned)WIRED->clock)
#define PIN_CS                  ((unsigned)WIRED->chip_select)
#define PIN_INT                 ((unsigned)WIRED->interrupt)    /* low = the chip has a report */
#define PIN_RESET               ((unsigned)WIRED->reset)        /* low = in reset */
#define PIN_FUNCTION_SPI        1       /* the serial engine's function on its pins */

/* ---- the I/O supply (RPMh) ----------------------------------------------------------------------- */

#define CMD_DB_PHYS             (hakios_board()->rpmh.cmd_db)           /* the SoC file's rpmh { } */
#define CMD_DB_SIZE             (hakios_board()->rpmh.cmd_db_size)
#define RSC_PHYS                (hakios_board()->rpmh.rsc)      /* the application processor's "DRV 2" */
#define RSC_SIZE                0x10000UL
#define TCS(reg)                (rsc + hakios_board()->rpmh.tcs_offset + (reg))           /* command slot set 0, an "active" one */
#include "rpmh.h"                    /* a slot set's registers, and one request through it */

#define VRM_VOLTAGE             0               /* a regulator's registers, from its address: millivolts */
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7

#include "cmd_db.h"

/* The RPMh address of a resource by its name in the command database the boot loader left; 0 = none. */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;

	if (db == NULL) {
		printf("fts: the command database is not mapped\n");
		return 0;
	}
	if (!hakios_cmd_db_valid(db)) {
		printf("fts: no command database (magic %02x %02x %02x %02x)\n", db->magic[0], db->magic[1], db->magic[2],
		    db->magic[3]);
		return 0;
	}
	return hakios_cmd_db_address(db, CMD_DB_SIZE, name);
}

/* Send up to three register writes to RPMh and wait until they are done (board/rpmh.h); what went wrong is said. */
static bool
rpmh_write_locked(const uint32_t *address, const uint32_t *data, unsigned count)
{
	const char *problem = hakios_rpmh_send_rsc(rsc, hakios_board()->rpmh.tcs_offset, address, data, count);

	if (problem != NULL) {
		printf("fts: RPMh: %s\n", problem);
	}
	return problem == NULL;
}

/* (not on the way out: the other cores are stopped, one of them perhaps holding the lock -- as in board/bt_uart.c) */
static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	bool locking = !__atomic_load_n(&leaving, __ATOMIC_SEQ_CST), ok;

	if (locking) {
		hakios_rpmh_lock();
	}
	ok = rpmh_write_locked(address, data, count);
	if (locking) {
		hakios_rpmh_unlock();
	}
	return ok;
}

/* each supply's RPMh address once it has been asked for, whether the request went through or not; 0: not asked */
static uint32_t io_supply_at, analog_supply_at;

/* The board's two regulators (hakios: L2C = I/O at 1.8 V, L7C = analog at 3.3 V). */
static bool
supply_on(const char *name, uint32_t millivolts, uint32_t *asked)
{
    uint32_t base = rpmh_address(name);
    uint32_t address[3] = { base + VRM_VOLTAGE, base + VRM_ENABLE, base + VRM_MODE };
    uint32_t data[3] = { millivolts, 1, VRM_MODE_LDO_HPM };

    if (base == 0 || (base >> 16 & 0xf) != 4) {
        printf("fts: no RPMh regulator %s (address %#x)\n", name, base);
        return false;
    }
    printf("fts: regulator %s at %#x: %u mV\n", name, base, millivolts);
    *asked = base;
    return rpmh_write(address, data, 3);
}

/* this driver's vote for a supply withdrawn, if it voted (only the enable, as board/bt_uart.c does: another may want it on) */
static void
supply_off(const char *name, uint32_t *asked)
{
    uint32_t address = *asked + VRM_ENABLE, off = 0;

    if (*asked == 0) {
        return;
    }
    *asked = 0;
    printf("fts: regulator %s: vote withdrawn\n", name);
    (void)rpmh_write(&address, &off, 1);
}

/* ---- clocks (GCC) -------------------------------------------------------------------------------- */

#define GCC_PHYS                (hakios_board()->clocks.gcc)    /* the SoC file's clocks { } */
#define GCC_SIZE                0x100000UL     /* (as far as tools/boards.py lets touch { } reach) */
#define GCC_VOTE                (gcc + WIRED->vote_register)    /* the application processor's votes */
/* the clocks of the touch controller's serial engine (SE) and of its group (QUP): which bits and offsets is the board's */
#define VOTE_QUP_CORE_2X        (1u << WIRED->vote[0])
#define VOTE_QUP_CORE           (1u << WIRED->vote[1])
#define VOTE_QUP_M_AHB          (1u << WIRED->vote[2])
#define VOTE_QUP_S_AHB          (1u << WIRED->vote[3])
#define VOTE_SE                 (1u << WIRED->vote[4])
#define GCC_QUP_M_AHB_CBCR      (gcc + WIRED->m_ahb_clock)      /* bit 31: the clock is off */
#define GCC_QUP_S_AHB_CBCR      (gcc + WIRED->s_ahb_clock)
#define GCC_SE_CBCR             (gcc + WIRED->se_clock)
/* the engine's rate generator follows its clock, at the same distances for every engine */
#define GCC_SE_CMD              (gcc + WIRED->se_clock + 0x04)  /* bit 0: take the new configuration */
#define GCC_SE_CFG              (gcc + WIRED->se_clock + 0x08)  /* [4:0] 2*divider-1, [10:8] source, [13:12] mode */
#define GCC_SE_DFSR             (gcc + WIRED->se_clock + 0x18)  /* bit 0: the engine chooses among preset rates itself */
#define GCC_SE_PERF_CFG(l)      (gcc + WIRED->se_clock + 0x20 + 4 * (l))
#define GCC_SE_PERF_M(l)        (gcc + WIRED->se_clock + 0x60 + 4 * (l))
#define GCC_SE_PERF_N(l)        (gcc + WIRED->se_clock + 0xa0 + 4 * (l))
#define CLOCK_OFF               (1u << 31)
#define XO_HZ                   19200000

static uint32_t se_source_hz, se_clock_select;
static uint32_t added_votes;            /* the votes bring-up found off and set: the only ones powering off drops */

/* clk-branch.c skips the halt bit for an AHB branch in hardware gated mode:
 * it may report OFF while idle, even after our GCC vote was accepted.
 */
static bool
fts_ahb_clock_ready(uint32_t status)
{
	return !(status & CLOCK_OFF) || (status & (1u << 1));
}

#define GENI_SPI_NAME           "fts"
#include "geni_spi.h"               /* the engine's registers, SPI set up, a transfer, a recovery;
                                         * the engine's clock's rate */

static bool
clocks_on(uint32_t target_hz)
{
	const uint32_t wrapper_votes = VOTE_QUP_CORE_2X | VOTE_QUP_CORE | VOTE_QUP_M_AHB | VOTE_QUP_S_AHB;

	/* (the wrapper's votes may be another engine's of the same wrapper already: those are left when this driver is done) */
	added_votes |= wrapper_votes & ~rd(GCC_VOTE);
	wr(GCC_VOTE, rd(GCC_VOTE) | wrapper_votes);
	if (rd(GCC_SE_DFSR) & 1) {
		/* Pick a source no lower than the requested SPI frequency. */
		for (uint32_t level = 0; level < 8; level++) {
			uint32_t hz = rcg_rate(rd(GCC_SE_PERF_CFG(level)), rd(GCC_SE_PERF_M(level)),
			    rd(GCC_SE_PERF_N(level)));

			if (hz >= target_hz && (se_source_hz == 0 || hz < se_source_hz)) {
				se_source_hz = hz;
				se_clock_select = level;
			}
		}
		if (se_source_hz == 0) {
			printf("fts: no usable preset clock rate\n");
			return false;
		}
	} else {
		/* the crystal, undivided: nothing else has to be running */
		wr(GCC_SE_CFG, (rd(GCC_SE_CFG) & ~0x371fu) | 1);
		wr(GCC_SE_CMD, rd(GCC_SE_CMD) | 1);
		for (int i = 0; i < 500 && (rd(GCC_SE_CMD) & 1); i++) {
			sleep_us(10);
		}
		if (rd(GCC_SE_CMD) & 1) {
			printf("fts: the serial engine's clock does not take its configuration (%#x)\n", rd(GCC_SE_CMD));
			return false;
		}
		se_source_hz = XO_HZ;
		se_clock_select = 0;
	}
	added_votes |= VOTE_SE & ~rd(GCC_VOTE);
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_SE);
	const uint32_t votes = wrapper_votes | VOTE_SE;
	if ((rd(GCC_VOTE) & votes) != votes) {
		printf("fts: GCC clock votes not accepted (%#x)\n", rd(GCC_VOTE));
		return false;
	}
	for (int i = 0; i < 1000; i++) {
		if (fts_ahb_clock_ready(rd(GCC_QUP_M_AHB_CBCR)) &&
		    fts_ahb_clock_ready(rd(GCC_QUP_S_AHB_CBCR)) && !(rd(GCC_SE_CBCR) & CLOCK_OFF)) {
			return true;
		}
		sleep_us(10);
	}
	printf("fts: clocks stay off: %#x %#x %#x\n", rd(GCC_QUP_M_AHB_CBCR), rd(GCC_QUP_S_AHB_CBCR),
	    rd(GCC_SE_CBCR));
	return false;
}

/* ---- SPI through the serial engine (GENI) ---------------------------------------------------------- */

#define SE_PHYS                 ((vm_offset_t)WIRED->serial_engine)
#define SE_SIZE                 0x4000UL

/*
 * GPIO mode-0 fallback when boot firmware leaves GENI restricted to GPI DMA. Which data pin is which is the board's
 * (bus-pins: data in, the chip's MISO, then data out): the chip's output is only read, never driven against.
 */
#define bitbang_in              PIN_DATA0
#define bitbang_out             PIN_DATA1
static void
fts_bitbang_init(void)
{
    gpio_output(PIN_CS, true, PULL_NONE, 2);
    gpio_output(PIN_CLK, false, PULL_NONE, 2);
    gpio_output(bitbang_out, false, PULL_NONE, 2);
    gpio_configure(bitbang_in, 0, PULL_NONE, 2, false);
}

static bool
fts_bitbang_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
    gpio_set(PIN_CS, false);
    for (uint32_t i = 0; i < length; i++) {
        uint8_t out = tx ? tx[i] : 0, in = 0;
        for (int bit = 7; bit >= 0; bit--) {
            gpio_set(bitbang_out, (out >> bit) & 1);
            sleep_us(1);
            gpio_set(PIN_CLK, true);
            sleep_us(1);
            in = (uint8_t)((in << 1) | gpio_get(bitbang_in));
            gpio_set(PIN_CLK, false);
        }
        if (rx) { rx[i] = in; }
    }
    gpio_set(PIN_CS, true);
    return true;
}

/* ---- FT3658U application protocol --------------------------------------------------------------- */
#include "fts_protocol.h"

static bool (*spi_transfer)(const uint8_t *, uint8_t *, uint32_t) = geni_transfer;
static uint8_t fts_tx[FTS_SPI_BYTES], fts_rx[FTS_SPI_BYTES];
static uint32_t flip, spi_hz = 12000000, trace;
static uint32_t panel_x = FTS_MAX_X, panel_y = FTS_MAX_Y;      /* the largest coordinates: the board's, or the first phone's */
static struct { bool down; uint32_t x, y; } contact[FTS_CONTACTS];
static struct { uint32_t polls, reports, bad, transfers_failed, positions; } seen;

/* Four header bytes, three dummy bytes, data and a two-byte LE CRC.
 * CS must remain asserted for the whole packet and high for >=150 us between attempts.
 */
static bool
fts_read(uint8_t address, uint8_t *data, uint32_t length)
{
    if (length == 0 || length > FTS_MAX_DATA) {
        return false;
    }
    memset(fts_tx, 0, sizeof(fts_tx));
    fts_tx[0] = address;
    fts_tx[1] = 0xa0;
    fts_tx[2] = (uint8_t)(length >> 8);
    fts_tx[3] = (uint8_t)length;
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        if (__atomic_load_n(&leaving, __ATOMIC_SEQ_CST)) {
            return false;               /* (the run's end has the bus) */
        }
        memset(fts_rx, 0, sizeof(fts_rx));
        __atomic_store_n(&in_transfer, true, __ATOMIC_SEQ_CST);
        bool transferred = spi_transfer(fts_tx, fts_rx, FTS_READ_PREFIX + length + 2);
        __atomic_store_n(&in_transfer, false, __ATOMIC_SEQ_CST);
        sleep_us(150);
        if (!transferred || (fts_rx[3] & 0xa0)) {
            continue;
        }
        const uint8_t *payload = fts_rx + FTS_READ_PREFIX;
        uint16_t crc = payload[length] | (uint16_t)payload[length + 1] << 8;
        if (fts_crc(payload, length) != crc) {
            continue;
        }
        memcpy(data, payload, length);
        return true;
    }
    seen.transfers_failed++;
    return false;
}

static void
fts_report(const struct fts_frame *frame)
{
    for (uint32_t id = 0; id < FTS_CONTACTS; id++) {
        if (!(frame->active & (1u << id))) {
            if (contact[id].down) {
                hakios_touch_post(HAKIOS_TOUCH_UP, id, contact[id].x, contact[id].y);
                contact[id].down = false;
            }
            continue;
        }
        uint32_t x = (uint32_t)frame->x[id] * 65535 / panel_x;
        uint32_t y = (uint32_t)frame->y[id] * 65535 / panel_y;
        if (flip & 1) { x = 65535 - x; }
        if (flip & 2) { y = 65535 - y; }
        if (!contact[id].down) {
            if (trace && seen.positions < 40) {
                printf("fts: finger %u DOWN at %u,%u\n", id, frame->x[id], frame->y[id]);
            }
            hakios_touch_post(HAKIOS_TOUCH_DOWN, id, x, y);
        } else if (contact[id].x != x || contact[id].y != y) {
            hakios_touch_post(HAKIOS_TOUCH_MOVE, id, x, y);
        }
        contact[id].down = true;
        contact[id].x = x;
        contact[id].y = y;
        seen.positions++;
    }
}

static void
fts_release(void)
{
    const struct fts_frame empty = { 0 };
    fts_report(&empty);
}

static void
fts_reset(void)
{
    gpio_set(PIN_RESET, false);
    fts_sleep_ms(1);
    gpio_set(PIN_RESET, true);
    fts_sleep_ms(200);
}

static bool
fts_probe(void)
{
    uint8_t high = 0, low = 0, version = 0, mode = 0xff;
    for (unsigned attempt = 0; attempt < 10 && !__atomic_load_n(&leaving, __ATOMIC_SEQ_CST); attempt++) {
        if (fts_read(0xa3, &high, 1) && fts_read(0x9f, &low, 1) && high == 0x56 && low == 0x52) {
            if (!fts_read(0x00, &mode, 1)) {
                /* (one read that failed is not the chip's mode: asked again as the ID was) */
                fts_sleep_ms(100);
                continue;
            }
            if ((mode & 0x70) != 0) {
                printf("fts: chip %#x%02x is not in application work mode (%#x)\n", high, low, mode);
                return false;
            }
            if (fts_read(0xa6, &version, 1)) {
                printf("fts: FT3658U ID 0x%02x%02x, firmware %#x, SPI mode 0, %u Hz requested\n",
                    high, low, version, spi_hz);
            }
            return true;
        }
        fts_sleep_ms(100);
    }
    printf("fts: no application ID 0x5652 (last 0x%02x%02x); no firmware is flashed\n", high, low);
    return false;
}

/* The six pins as bring-up found them, before it changed any: what powering off configures them back to. */
#define PINS                    6
static unsigned pin_number[PINS];
static uint32_t pin_found_cfg[PINS], pin_found_io[PINS];
static bool pins_found;

static void
pins_record(void)
{
    const unsigned pins[PINS] = { PIN_DATA0, PIN_DATA1, PIN_CLK, PIN_CS, PIN_INT, PIN_RESET };

    for (unsigned i = 0; i < PINS; i++) {
        pin_number[i] = pins[i];
        pin_found_cfg[i] = rd(GPIO_CFG(pins[i]));
        pin_found_io[i] = rd(GPIO_IO(pins[i]));
    }
    __atomic_store_n(&pins_found, true, __ATOMIC_SEQ_CST);
}

/*
 * The controller powered off, as far as this driver powered it (this file's head): bounded, and without sleeping on
 * the way out. The reset pin keeps its level low under the configuration it is given back.
 */
static void
fts_power_off(void)
{
    uint32_t dropped = added_votes;

    if (!__atomic_load_n(&pins_found, __ATOMIC_SEQ_CST)) {
        return;                         /* (nothing was touched, or it has been done already) */
    }
    gpio_output(PIN_RESET, false, PULL_UP, 8);
    supply_off(WIRED->analog_supply, &analog_supply_at);
    supply_off(WIRED->io_supply, &io_supply_at);
    if (dropped != 0) {
        wr(GCC_VOTE, rd(GCC_VOTE) & ~dropped);
        added_votes = 0;
    }
    for (unsigned i = 0; i < PINS; i++) {
        if (pin_number[i] != PIN_RESET) {
            wr(GPIO_IO(pin_number[i]), pin_found_io[i] & 2);    /* (the level first, as gpio_output()) */
        }
        wr(GPIO_CFG(pin_number[i]), pin_found_cfg[i]);
    }
    __atomic_store_n(&pins_found, false, __ATOMIC_SEQ_CST);
    printf("fts: powered off: in reset, clock votes %#x dropped, pins as found\n", dropped);
}

static bool
fts_bring_up(void)
{
    printf("fts: serial engine at %#x; reset gpio%u, IRQ gpio%u; supplies %s and %s\n", WIRED->serial_engine, PIN_RESET, PIN_INT,
        WIRED->io_supply, WIRED->analog_supply);
    pins_record();
    gpio_output(PIN_RESET, false, PULL_UP, 8);
    gpio_configure(PIN_INT, 0, PULL_UP, 8, false);
    fts_sleep_ms(1);
    if (!supply_on(WIRED->io_supply, 1800, &io_supply_at)) { return false; }
    fts_sleep_ms(1);
    if (!supply_on(WIRED->analog_supply, 3300, &analog_supply_at)) { return false; }
    fts_sleep_ms(15);
    const char *problem = "SPI clocks unavailable";
    if (clocks_on(spi_hz)) {
        printf("fts: GENI firmware %#x, interface %#x, parameters %#x\n",
            rd(GENI_FW_REVISION_RO), rd(GENI_IF_DISABLE_RO), rd(SE_HW_PARAM_0));
        problem = geni_spi_init(spi_hz);
    }
    if (problem == NULL) {
        gpio_configure(PIN_DATA0, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
        gpio_configure(PIN_DATA1, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
        gpio_configure(PIN_CLK, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
        gpio_configure(PIN_CS, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
        spi_transfer = geni_transfer;
        uint32_t divider = (rd(GENI_SER_M_CLK_CFG) >> 4) & 0xfff;
        printf("fts: source %u Hz, select %u, divider %u, actual SPI %u Hz\n",
            se_source_hz, se_clock_select, divider, divider ? se_source_hz / divider : 0);
        fts_reset();
        if (fts_probe()) { return true; }
    } else {
        printf("fts: %s; trying GPIO SPI mode 0\n", problem);
    }
    spi_transfer = fts_bitbang_transfer;
    /* CS stays high while the pins change; use the minimum 2 mA drive. */
    fts_bitbang_init();
    fts_reset();
    if (fts_probe()) {
        printf("fts: GPIO SPI ready: MISO gpio%u, MOSI gpio%u, CLK gpio%u, CS gpio%u\n",
            bitbang_in, bitbang_out, PIN_CLK, PIN_CS);
        return true;
    }
    gpio_set(PIN_CS, true);
    return false;
}

static bool looking_only;              /* touchscreen=-1 */

/* The level that only looks: what the firmware left -- the votes, the engine's clocks, the pins -- read from the clock
 * controller and the pin controller, which are always reachable; the serial engine, behind its clocks, is not read */
static void
fts_look(void)
{
    printf("fts: as found (touchscreen=-1: looking only): votes %#x, clocks %#x %#x %#x, rate cfg %#x dfs %#x; pins"
        " cs/clk/data0/data1 cfg %#x %#x %#x %#x, reset %#x/%u, int %#x/%u\n", rd(GCC_VOTE), rd(GCC_QUP_M_AHB_CBCR),
        rd(GCC_QUP_S_AHB_CBCR), rd(GCC_SE_CBCR), rd(GCC_SE_CFG), rd(GCC_SE_DFSR), rd(GPIO_CFG(PIN_CS)), rd(GPIO_CFG(PIN_CLK)),
        rd(GPIO_CFG(PIN_DATA0)), rd(GPIO_CFG(PIN_DATA1)), rd(GPIO_CFG(PIN_RESET)), rd(GPIO_IO(PIN_RESET)),
        rd(GPIO_CFG(PIN_INT)), rd(GPIO_IO(PIN_INT)));
    printf("fts: touchscreen=-1: nothing powered, configured or sent\n");
}

/* The controller is the thread's from here until it calls fts_done(); false once the run's end has it. */
static bool
fts_begin(void)
{
    __atomic_store_n(&busy, true, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&leaving, __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&busy, false, __ATOMIC_SEQ_CST);
        return false;
    }
    return true;
}

static void
fts_done(void)
{
    __atomic_store_n(&busy, false, __ATOMIC_SEQ_CST);
}

/*
 * A finger is lifted once the controller has said nothing believable for FTS_LIFT_US, however many polls failed in that
 * time: counted in polls, a transfer that fails slowly held it down for seconds to minutes. The resets counted are
 * forgotten after FTS_TROUBLE_FORGOTTEN_US without a failed poll: an idle phone reads few reports, and a count kept
 * until enough of them arrived would end the polling over troubles of hours before.
 */
#define FTS_LIFT_US             250000
#define FTS_TROUBLE_FORGOTTEN_US (60 * 1000000ull)

/* consecutive bad polls, recoveries made, and when the controller last said something believable and last failed */
static struct { unsigned failures, resets; uint64_t last_good_us, last_failure_us; } trouble;

static void
fts_heard(uint64_t now)
{
    trouble.failures = 0;
    trouble.last_good_us = now;
    if (trouble.resets != 0 && now - trouble.last_failure_us >= FTS_TROUBLE_FORGOTTEN_US) {
        trouble.resets = 0;
    }
}

/* One look at the controller; false: it is given up. */
static bool
fts_poll(unsigned tick)
{
    uint8_t report[FTS_REPORT_BYTES];
    struct fts_frame frame;
    /* Falling-edge IRQ can be a brief pulse: read periodically even if GPIO is high. */
    bool pending = !gpio_get(PIN_INT), active = false;
    for (unsigned id = 0; id < FTS_CONTACTS; id++) { active |= contact[id].down; }
    if (!pending && !active && tick % 10 != 0) {
        return true;
    }
    seen.polls++;
    bool read_ok = fts_read(0x01, report, sizeof(report));
    uint64_t now = now_us();
    /* This chip returns a CRC-valid all-ff frame while idle. Android reads
     * only on IRQ; our periodic fallback must not reset an idle controller.
     * (With a finger down it is decoded below: no finger, its lift posted.) */
    if (read_ok && !pending && !active && fts_report_empty(report, sizeof(report))) {
        fts_heard(now);
        return true;
    }
    if (read_ok && fts_decode(report, sizeof(report), panel_x, panel_y, &frame)) {
        fts_heard(now);
        seen.reports++;
        fts_report(&frame);
    } else {
        seen.bad++;
        trouble.last_failure_us = now;
        if (seen.bad <= 4) {
            if (read_ok) {
                printf("fts: rejected report: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                    report[0], report[1], report[2], report[3], report[4], report[5], report[6], report[7]);
            } else {
                printf("fts: report SPI/status/CRC failure\n");
            }
        }
        if (active && now - trouble.last_good_us >= FTS_LIFT_US) {
            printf("fts: nothing believable for %u ms with a finger down: lifted\n",
                (unsigned)((now - trouble.last_good_us) / 1000));
            fts_release();
        }
        if (++trouble.failures >= 125) {
            fts_release();
            printf("fts: %u consecutive invalid reports (%u SPI failures); previous recoveries %u/3\n",
                trouble.failures, seen.transfers_failed, trouble.resets);
            if (trouble.resets++ >= 3) { return false; }
            fts_reset();
            if (!fts_probe()) { return false; }
            trouble.failures = 0;
        }
    }
    if (tick % (60 * 125) == 0) {
        printf("fts: polls %u, reports %u, invalid %u, failed SPI reads %u, positions %u, IRQ %s\n",
            seen.polls, seen.reports, seen.bad, seen.transfers_failed, seen.positions,
            gpio_get(PIN_INT) ? "high" : "low");
    }
    return true;
}

static void
fts_thread(__unused void *parameter, __unused wait_result_t result)
{
    tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
    gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
    if (looking_only) {
        if (tlmm && gcc) { fts_look(); }
        thread_terminate(current_thread());
        return;
    }
    se = ml_io_map(SE_PHYS, SE_SIZE);
    rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
    cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
    if (!fts_begin()) {
        thread_terminate(current_thread());
        return;
    }
    if (!tlmm || !gcc || !se || !rsc || !cmd_db || !fts_bring_up()) {
        printf("fts: touchscreen unavailable\n");
        fts_power_off();
        fts_done();
        thread_terminate(current_thread());
        return;
    }
    fts_done();
    printf("fts: touchscreen ready, %ux%u, ten contacts; polling at 125 Hz\n", panel_x, panel_y);
    trouble.last_good_us = now_us();
    for (unsigned tick = 1; fts_begin(); tick++) {
        if (!fts_poll(tick)) {
            fts_release();
            printf("fts: stopped after repeated controller errors\n");
            fts_power_off();
            fts_done();
            break;
        }
        fts_done();
        fts_sleep_ms(8);
    }
    /* (or the run's end has the controller: hakios_fts_quiesce() powers it off) */
    thread_terminate(current_thread());
}

void
hakios_fts_init(void)
{
    int32_t enabled = 0;
    thread_t thread;
    /* boot-arg touchscreen=1: the controller started, its touches posted to /dev/hakios-touch; -1: only looked at */
    if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) { return; }
    looking_only = enabled == -1;
    /* (called only on a board that lists "touch-ft3658u": bsd/dev/board/drivers.c) */
    if (hakios_board()->pins.tlmm == 0 || hakios_board()->clocks.gcc == 0 || hakios_board()->rpmh.cmd_db == 0) {
        printf("fts: disabled: the SoC file does not say where the pins, the clock controller and RPMh are (pins { }, clocks { }, rpmh { })\n");
        return;
    }
    if (WIRED->serial_engine == 0 || WIRED->io_supply == NULL || WIRED->analog_supply == NULL) {
        printf("fts: disabled: the board file does not say how the controller is wired (touch { }, with analog_supply)\n");
        return;
    }
    /* (a report carries 12 bits of a coordinate: a panel larger than that is one the board file has wrong) */
    panel_x = WIRED->max_x != 0 ? WIRED->max_x : FTS_MAX_X;
    panel_y = WIRED->max_y != 0 ? WIRED->max_y : FTS_MAX_Y;
    if (panel_x > FTS_COORDINATE_MAX || panel_y > FTS_COORDINATE_MAX) {
        printf("fts: disabled: touch { } coordinates %u x %u are more than a report can say (%u)\n", panel_x, panel_y,
            FTS_COORDINATE_MAX);
        return;
    }
    /*
     * touch_flip=N: bit 0 mirrors the touches left to right, bit 1 top to bottom.
     * fts_trace=1: the first 40 touches' positions as the controller gives them, in the log.
     * fts_spi_hz=N: the SPI clock, 1 to 12 MHz (default 12 MHz).
     */
    PE_parse_boot_argn("touch_flip", &flip, sizeof(flip));
    PE_parse_boot_argn("fts_trace", &trace, sizeof(trace));
    PE_parse_boot_argn("fts_spi_hz", &spi_hz, sizeof(spi_hz));
    if (spi_hz < 1000000 || spi_hz > 12000000) {
        printf("fts: invalid fts_spi_hz=%u (allowed 1..12 MHz)\n", spi_hz);
        return;
    }
    if (kernel_thread_start(fts_thread, NULL, &thread) == KERN_SUCCESS) {
        thread_deallocate(thread);
    } else {
        printf("fts: cannot start touchscreen thread\n");
    }
}

/*
 * At the run's end and on a panic's way out (osfmk/arm64/loader_return.c): no more polling, a transfer cut off in its
 * middle ended, and the controller powered off (this file's head). Spins, with interrupts off, a bounded time.
 */
void
hakios_fts_quiesce(void)
{
    __atomic_store_n(&leaving, true, __ATOMIC_SEQ_CST);
    for (unsigned i = 0; i < 100 && __atomic_load_n(&busy, __ATOMIC_SEQ_CST); i++) {
        spin_us(1000);                  /* (the thread finishing a transaction: 100 ms at most, a stopped core's never ends) */
    }
    if (!__atomic_load_n(&pins_found, __ATOMIC_SEQ_CST)) {
        return;
    }
    if (__atomic_load_n(&in_transfer, __ATOMIC_SEQ_CST)) {
        if (spi_transfer == geni_transfer) {
            printf("fts: at the run's end: a transfer cut off: the serial engine's command cancelled\n");
            geni_recover();
        } else {
            printf("fts: at the run's end: a transfer cut off: chip select raised\n");
            gpio_set(PIN_CS, true);
        }
    }
    fts_power_off();
}
