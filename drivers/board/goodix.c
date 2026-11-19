/*
 * A touch screen on SPI: either a Goodix GT9916 ("Berlin-D") or a FocalTech FT3680, as a source of
 * touches for /dev/hakios-touch (sys/board/input.h). Which serial engine, clocks, pins and supplies
 * is the board's touch { }. Only with boot-arg touchscreen=1; touchscreen=-1 only looks: the clocks', pins' and
 * straps' state as the firmware left them, in the log, and nothing powered, configured or sent.
 *
 * There is no clock, pin, regulator or SPI framework in this kernel, so the driver does all of
 * it itself, in the order the vendor's Linux drivers do:
 *   power    the chip's I/O supply is LDO2 of the PM8350C, switched through RPMh (one request
 *            written into the "always on" processor's command slots); the analog supply is
 *            behind a GPIO
 *   clocks   the serial engine's three clocks, by vote in the global clock controller
 *   pins     four GPIOs to the serial engine, plus reset, interrupt and the analog supply switch
 *   SPI      the serial engine ("GENI") in FIFO mode, polled; or, if the engine cannot be used,
 *            the same four pins driven by hand
 *   panel    GPIO96=1 and GPIO68=0 identify the FocalTech variant; everything else is Goodix,
 *            the same discriminator the vendor sm8350-kernel-OKX uses
 *   chip     Goodix: find where it keeps the touch report, then read it when the IRQ line signals;
 *            FocalTech: read the report at register 0x01 with FTS 5.x SPI framing (cmd 0xC1 for
 *            read, 0xC0 for write, two-byte CRC-16/Kermit)
 * No interrupt is used: a thread looks at the interrupt line 125 times a second (boot-arg
 * touch_poll_us=N: every N microseconds instead of 8000). With touch_lat=1 a line for every report
 * read ("tl: c before US seen US read US": the look before, the look that found the line low, the
 * report read), for measuring how long a touch takes to show (tools/latency.py).
 *
 * Register layouts: Linux drivers/spi/spi-geni-qcom.c, drivers/soc/qcom/{qcom-geni-se,rpmh-rsc,
 * cmd-db}.c, drivers/clk/qcom/gcc-sc7280.c, drivers/pinctrl/qcom/pinctrl-sc7280.c,
 * drivers/input/touchscreen/{goodix_berlin_*,focaltech_touch/focaltech_core}.c, and the phone's
 * own device tree.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/board/input.h>
#include <sys/systm.h>

extern void hakios_tl(const char *format, ...) __printflike(1, 2);

static vm_offset_t tlmm, gcc, se, rsc, cmd_db;

#include "kernel.h"
#include "board.h"
#include "mmio.h"
#include "time.h"

/*
 * What the driver says goes to the loader's log as it is said, and to the kernel's: the kernel's own
 * reaches the loader only when a run is over, and a touch screen that does not answer is looked for
 * while the phone runs.
 */
#include "log.h"
#define printf hakios_say

/*
 * The run's end has the chip (hakios_goodix_quiesce()): the poll thread stops, and what was switched on is switched
 * off. There the other processors are stopped and interrupts are off, so RPMh is asked without its lock, and waited
 * for by spinning, as the other drivers ask it on the way back to the loader.
 */
static bool leaving;
static bool busy;               /* the poll thread is reading the chip: the run's end waits for it, a little */

static void
goodix_wait_us(uint32_t us)
{
	if (__atomic_load_n(&leaving, __ATOMIC_SEQ_CST)) {
		spin_us(us);
	} else {
		sleep_us(us);
	}
}

/* ---- pins (TLMM) ------------------------------------------------------------------------------- */

#define TLMM_PHYS               (hakios_board()->pins.tlmm)     /* the SoC file's pins { } */
#define HAKIOS_TLMM             tlmm
#include "tlmm.h"                    /* a pin's two registers, and the four things done with them */

/* Which pins, which serial engine and which of its clocks: the board file's touch { } (board/board.h). */
#define WIRED                   (&hakios_board()->touch)
#define PIN_MISO                ((unsigned)WIRED->data_in)
#define PIN_MOSI                ((unsigned)WIRED->data_out)
#define PIN_CLK                 ((unsigned)WIRED->clock)
#define PIN_CS                  ((unsigned)WIRED->chip_select)
#define PIN_INT                 ((unsigned)WIRED->interrupt)    /* low = the chip has a report */
#define PIN_RESET               ((unsigned)WIRED->reset)        /* low = in reset */
#define PIN_AVDD                ((unsigned)WIRED->analog_enable)        /* high = analog supply on */
#define PIN_FOCALTECH_ID        ((unsigned)WIRED->vendor_pin[0])        /* FocalTech if this reads 0 ... */
#define PIN_GOODIX_ID           ((unsigned)WIRED->vendor_pin[1])        /* ... and this reads 1 (sm8350-kernel-OKX convention) */
#define PIN_FUNCTION_SPI        1       /* the serial engine's function on its pins */

/* ---- the I/O supply (RPMh) ----------------------------------------------------------------------- */

#define CMD_DB_PHYS             (hakios_board()->rpmh.cmd_db)           /* the SoC file's rpmh { } */
#define CMD_DB_SIZE             (hakios_board()->rpmh.cmd_db_size)
#define RSC_PHYS                (hakios_board()->rpmh.rsc)      /* the application processor's "DRV 2" */
#define RSC_SIZE                0x10000UL
#define TCS(reg)                (rsc + hakios_board()->rpmh.tcs_offset + (reg))           /* command slot set 0, an "active" one */
#define sleep_us goodix_wait_us
#include "rpmh.h"                    /* a slot set's registers, and one request through it */
#undef sleep_us

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
		printf("goodix: the command database is not mapped\n");
		return 0;
	}
	if (!hakios_cmd_db_valid(db)) {
		printf("goodix: no command database (magic %02x %02x %02x %02x)\n", db->magic[0], db->magic[1], db->magic[2],
		    db->magic[3]);
		return 0;
	}
	return hakios_cmd_db_address(db, CMD_DB_SIZE, name);
}

/* Send up to three register writes to RPMh and wait until they are done (board/rpmh.h); NULL, or what went wrong, said. */
static const char *
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	const char *problem;
	bool locked = !__atomic_load_n(&leaving, __ATOMIC_SEQ_CST);

	/* (the slots are shared: the memory's vote, Bluetooth's supplies, the audio DSP's may be sent at the same time) */
	if (locked) {
		hakios_rpmh_lock();
	}
	problem = hakios_rpmh_send_rsc(rsc, hakios_board()->rpmh.tcs_offset, address, data, count);
	if (locked) {
		hakios_rpmh_unlock();
	}

	if (problem != NULL) {
		printf("goodix: RPMh: %s\n", problem);
	}
	return problem;
}

static uint32_t io_supply;      /* the regulator's RPMh address, once the supply has been asked for */
static bool io_voted;           /* ... and the request sent: it is withdrawn when the chip is powered down */

/* The chip's I/O supply: 1.8 V from the regulator the board names (touch { } io-supply: for example "ldoc2", LDO2 of a PM8350C). Nothing else is connected to it. */
static bool
io_supply_on(void)
{
	uint32_t base = rpmh_address(WIRED->io_supply);
	uint32_t address[3] = { base + VRM_VOLTAGE, base + VRM_ENABLE, base + VRM_MODE };
	uint32_t data[3] = { 1800, 1, VRM_MODE_LDO_HPM };
	const char *problem;

	/* addresses of regulators ("VRM") have 4 in bits 19:16; anything else is not what we are looking for */
	if (base == 0 || (base >> 16 & 0xf) != 4) {
		printf("goodix: RPMh: no regulator \"%s\" (address %#x)\n", WIRED->io_supply, base);
		return false;
	}
	printf("goodix: RPMh: %s is at %#x; slot status %#x, control %#x, irq enable %#x status %#x\n", WIRED->io_supply, base,
	    rd(TCS(TCS_STATUS)), rd(TCS(TCS_CONTROL)), rd(TCS(TCS_IRQ_ENABLE)), rd(TCS(TCS_IRQ_STATUS)));
	io_supply = base;
	problem = rpmh_write(address, data, 3);
	/* (a request without an answer was triggered all the same and may be in force: withdrawn like one that was answered, as board/ddr.c does) */
	io_voted = problem == NULL || strcmp(problem, RPMH_SAYS_NO_ANSWER) == 0;
	if (problem != NULL) {
		return false;
	}
	printf("goodix: I/O supply on (1.8 V)\n");
	return true;
}

/* The I/O supply's request withdrawn. True when RPMh has said it is done: then the chip has no supply of ours. */
static bool
io_supply_off(void)
{
	uint32_t address = io_supply + VRM_ENABLE, off = 0;

	if (rpmh_write(&address, &off, 1) != NULL) {
		return false;
	}
	io_voted = false;
	printf("goodix: I/O supply off\n");
	return true;
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
static uint32_t votes_added;    /* the votes clocks_on() found not taken: given back when the chip is powered down */

#define GENI_SPI_NAME           "goodix"
#include "geni_spi.h"               /* the engine's registers, SPI set up, a transfer, a recovery;
                                         * the engine's clock's rate */

/*
 * An AHB clock in hardware gated mode (bit 1) may say it is off while its bus is idle, even with the vote taken
 * (Linux's clk-branch.c does not wait for it either): the engine was given up for that, and its pins driven by hand
 */
static bool
ahb_clock_ready(uint32_t status)
{
	return !(status & CLOCK_OFF) || (status & (1u << 1));
}

static bool
clocks_on(void)
{
	const uint32_t group = VOTE_QUP_CORE_2X | VOTE_QUP_CORE | VOTE_QUP_M_AHB | VOTE_QUP_S_AHB;

	votes_added |= group & ~rd(GCC_VOTE);
	wr(GCC_VOTE, rd(GCC_VOTE) | group);
	if (rd(GCC_SE_DFSR) & 1) {
		/* preset rates: the lowest that is at least 4 MHz */
		for (uint32_t level = 0; level < 8; level++) {
			uint32_t hz = rcg_rate(rd(GCC_SE_PERF_CFG(level)), rd(GCC_SE_PERF_M(level)),
			    rd(GCC_SE_PERF_N(level)));

			if (hz >= 4000000 && (se_source_hz == 0 || hz < se_source_hz)) {
				se_source_hz = hz;
				se_clock_select = level;
			}
		}
		if (se_source_hz == 0) {
			printf("goodix: no usable preset clock rate\n");
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
			printf("goodix: the serial engine's clock does not take its configuration (%#x)\n", rd(GCC_SE_CMD));
			return false;
		}
		se_source_hz = XO_HZ;
		se_clock_select = 0;
	}
	votes_added |= VOTE_SE & ~rd(GCC_VOTE);
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_SE);
	for (int i = 0; i < 1000; i++) {
		if (ahb_clock_ready(rd(GCC_QUP_M_AHB_CBCR)) && ahb_clock_ready(rd(GCC_QUP_S_AHB_CBCR)) &&
		    !(rd(GCC_SE_CBCR) & CLOCK_OFF)) {
			return true;
		}
		sleep_us(10);
	}
	printf("goodix: clocks stay off: %#x %#x %#x\n", rd(GCC_QUP_M_AHB_CBCR), rd(GCC_QUP_S_AHB_CBCR),
	    rd(GCC_SE_CBCR));
	return false;
}

/* ---- SPI through the serial engine (GENI) ---------------------------------------------------------- */

#define SE_PHYS                 ((vm_offset_t)WIRED->serial_engine)
#define SE_SIZE                 0x4000UL

/* ---- SPI by hand, on the same pins ----------------------------------------------------------------- */

static unsigned bitbang_in, bitbang_out;        /* (set before each use: bring_up()) */
static bool bus_taken;          /* the four pins are the engine's or driven by hand: let go when the chip is powered down */

static void
bitbang_init(void)
{
	bus_taken = true;
	gpio_output(PIN_CS, true, PULL_NONE, 6);
	gpio_output(PIN_CLK, false, PULL_NONE, 6);
	/* (2 mA: the second orientation tried makes the chip's own data-out pin ours, and the two drive it against each other) */
	gpio_output(bitbang_out, false, PULL_NONE, 2);
	gpio_configure(bitbang_in, 0, PULL_NONE, 2, false);
}

static bool
bitbang_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	gpio_set(PIN_CS, false);
	for (uint32_t i = 0; i < length; i++) {
		uint8_t out = tx ? tx[i] : 0, in = 0;

		for (int bit = 7; bit >= 0; bit--) {
			gpio_set(bitbang_out, out >> bit & 1);
			gpio_set(PIN_CLK, true);
			in = (uint8_t)(in << 1 | gpio_get(bitbang_in));
			gpio_set(PIN_CLK, false);
		}
		if (rx) {
			rx[i] = in;
		}
	}
	gpio_set(PIN_CS, true);
	return true;
}

static bool (*spi_transfer)(const uint8_t *tx, uint8_t *rx, uint32_t length);

/* ---- the Goodix chip ------------------------------------------------------------------------------- */

#define GOODIX_FW_VERSION       0x10014         /* 28 bytes */
#define GOODIX_IC_INFO          0x10070         /* a 16-bit length, then that many bytes */
#define GOODIX_MAX_READ         1024
#define GOODIX_MAX_TOUCHES      10
#define GOODIX_STATUS_TOUCH     0x80
#define GOODIX_STATUS_REQUEST   0x40
#define GOODIX_REQUEST_RESET    3

static uint8_t spi_tx[8 + GOODIX_MAX_READ], spi_rx[8 + GOODIX_MAX_READ];
static uint32_t touch_address, command_address;
static const char *report_rate = "240";        /* what the status line says (touch_rate=480) */
static uint32_t max_x, max_y;   /* the board's touch { } coordinates; the chip may say otherwise */

static bool
goodix_read(uint32_t address, uint8_t *data, uint32_t length)
{
	if (length > GOODIX_MAX_READ) {
		return false;
	}
	spi_tx[0] = 0xf1;
	spi_tx[1] = (uint8_t)(address >> 24);
	spi_tx[2] = (uint8_t)(address >> 16);
	spi_tx[3] = (uint8_t)(address >> 8);
	spi_tx[4] = (uint8_t)address;
	spi_tx[5] = spi_tx[6] = spi_tx[7] = 0xff;
	memset(spi_tx + 8, 0, length);
	if (!spi_transfer(spi_tx, spi_rx, 8 + length)) {
		return false;
	}
	memcpy(data, spi_rx + 8, length);
	return true;
}

static bool
goodix_write(uint32_t address, const uint8_t *data, uint32_t length)
{
	if (length > GOODIX_MAX_READ) {
		return false;
	}
	spi_tx[0] = 0xf0;
	spi_tx[1] = (uint8_t)(address >> 24);
	spi_tx[2] = (uint8_t)(address >> 16);
	spi_tx[3] = (uint8_t)(address >> 8);
	spi_tx[4] = (uint8_t)address;
	memcpy(spi_tx + 5, data, length);
	return spi_transfer(spi_tx, NULL, 5 + length);
}

/* The chip's blocks end in the 16-bit sum of their bytes; all zeros or all ones is no answer at all. */
static bool
goodix_checksum_ok(const uint8_t *data, uint32_t length)
{
	uint32_t sum = 0;
	bool blank = true;

	if (length < 2) {
		return false;
	}
	for (uint32_t i = 0; i < length - 2; i++) {
		sum += data[i];
		blank &= data[i] == 0 || data[i] == 0xff;
	}
	return !blank && (uint16_t)sum == (uint16_t)(data[length - 2] | data[length - 1] << 8);
}

static uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t
le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void
touch_reset(void)
{
	gpio_set(PIN_RESET, false);
	sleep_ms(2);
	gpio_set(PIN_RESET, true);
	sleep_ms(100);
}

/* Is the chip there and running its firmware? Finds where it keeps the touch report. */
static bool
goodix_probe(bool verbose)
{
	static uint8_t info[GOODIX_MAX_READ];
	uint8_t version[28];
	uint32_t length, offset;

	if (!goodix_read(GOODIX_FW_VERSION, version, sizeof(version))) {
		return false;
	}
	if (!goodix_checksum_ok(version, sizeof(version))) {
		if (verbose) {
			printf("goodix: no valid firmware version: %02x %02x %02x %02x %02x %02x %02x %02x ... %02x %02x\n",
			    version[0], version[1], version[2], version[3], version[4], version[5], version[6], version[7],
			    version[26], version[27]);
		}
		return false;
	}
	printf("goodix: chip \"%.6s\", firmware \"%.8s\" %02x.%02x.%02x.%02x, sensor %u\n", (const char *)version,
	    (const char *)version + 10, version[18], version[19], version[20], version[21], version[23]);

	if (!goodix_read(GOODIX_IC_INFO, info, 2)) {
		return false;
	}
	length = le16(info);
	if (length < 40 || length >= GOODIX_MAX_READ || !goodix_read(GOODIX_IC_INFO, info, length) ||
	    !goodix_checksum_ok(info, length)) {
		printf("goodix: bad chip description (length %u)\n", length);
		return false;
	}
	/* the length, the version (16 bytes), the features (10), four counts, then five lists of 16-bit numbers */
	offset = 2 + 16 + 10 + 4;
	for (int i = 0; i < 5; i++) {
		if (offset >= length) {
			return false;
		}
		offset += 1 + 2u * info[offset];
	}
	if (offset + 48 + 2 > length) {
		printf("goodix: chip description too short (%u, lists end at %u)\n", length, offset);
		return false;
	}
	touch_address = le32(info + offset + 44);
	command_address = le32(info + offset + 0);      /* (the "misc" part: the command buffer comes first) */
	if (offset + 122 + 20 + 2 <= length) {
		uint16_t x = le16(info + offset + 122 + 16), y = le16(info + offset + 122 + 18);

		if (x && y) {
			max_x = x;
			max_y = y;
		}
	}
	printf("goodix: touch report at %#x, coordinates up to %u x %u\n", touch_address, max_x, max_y);
	return touch_address != 0;
}

/*
 * A command to the chip (Android's brl_send_cmd(): goodix_brl_hw.c of the phone's kernel): sixteen
 * bytes at the command address -- state, acknowledgement, length, command, its data, and the sum of
 * the bytes from the length on -- then the acknowledgement looked for, 0x80 when done.
 */
#define GOODIX_CMD_HIGH_RATE    0xc0            /* data 1: report 480 times a second instead of 240 (a phone vendor's "game mode") */
#define GOODIX_ACK_OK           0x80
#define GOODIX_ACK_BUSY         0x02

static bool
goodix_command(uint8_t command, uint8_t data)
{
	uint8_t buffer[16], answer[16];
	uint32_t sum;

	if (command_address == 0) {
		return false;
	}
	memset(buffer, 0, sizeof(buffer));
	buffer[2] = 5;
	buffer[3] = command;
	buffer[4] = data;
	sum = buffer[2] + buffer[3] + buffer[4];
	buffer[5] = (uint8_t)sum;
	buffer[6] = (uint8_t)(sum >> 8);
	for (int attempt = 0; attempt < 6; attempt++) {
		if (!goodix_write(command_address, buffer, sizeof(buffer))) {
			return false;
		}
		for (int look = 0; look < 6; look++) {
			if (!goodix_read(command_address, answer, sizeof(answer))) {
				return false;
			}
			if (answer[1] == GOODIX_ACK_OK) {
				return true;
			}
			sleep_ms(1);
			if (answer[1] != GOODIX_ACK_BUSY && answer[1] != 0) {
				break;
			}
		}
	}
	return false;
}

/* What has been seen so far: said once a minute, so that the end of the log always tells. */
static struct {
	uint32_t polls, reports, touches, bad, last_x, last_y;
} seen;

/* Which fingers were down in the last report, and where. */
static struct {
	bool down, seen;
	uint32_t x, y;
} contact[GOODIX_MAX_TOUCHES];
static uint32_t flip;           /* boot-arg touch_flip: 1 = mirror left-right, 2 = mirror top-bottom */
static bool high_rate;          /* touch_rate=480 was asked for: asked again after a reset */

static void focaltech_release(void);

/* Every finger down lifted where it was: the chip reset, or its reports no longer coming (no finger left down for good) */
static void
contacts_release(void)
{
	for (uint32_t id = 0; id < GOODIX_MAX_TOUCHES; id++) {
		if (contact[id].down) {
			contact[id].down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, id, contact[id].x, contact[id].y);
		}
	}
	focaltech_release();
}

/* Reads that failed one after another before the fingers are taken as lifted: a quarter of a second at 125 Hz */
#define READS_FAILED_MOST       32

/*
 * How long fingers that are down are believed without a report: the chip reports at its rate as long as a finger is
 * on it, and one that stops answering is not always a read that fails -- pins driven by hand read whatever the line
 * floats to, all ones (a header not believed) or all zeros (no report), and a transfer of the engine's does not fail
 * either. A quarter of a second, as the failed reads above.
 */
#define SILENT_MOST_US          250000

static uint64_t
micros_now(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

static uint64_t believed_at;    /* when the last report with fingers in it that was believed was read */

static void
goodix_report(const uint8_t *touches, uint32_t count)
{
	static unsigned shown;

	believed_at = micros_now();
	for (int i = 0; i < GOODIX_MAX_TOUCHES; i++) {
		contact[i].seen = false;
	}
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *t = touches + 8 * i;
		uint32_t id = t[0] >> 4, kind = t[0] & 0xf;
		uint32_t x = le16(t + 2), y = le16(t + 4);

		if (id >= GOODIX_MAX_TOUCHES || kind == 1 || kind == 3) {       /* 1 and 3: a stylus */
			continue;
		}
		if (shown < 40) {
			shown++;
			printf("goodix: finger %u at %u, %u (of %u x %u)\n", id, x, y, max_x, max_y);
		}
		seen.touches++;
		seen.last_x = x;
		seen.last_y = y;
		x = x >= max_x ? 65535 : x * 65535 / max_x;
		y = y >= max_y ? 65535 : y * 65535 / max_y;
		if (flip & 1) {
			x = 65535 - x;
		}
		if (flip & 2) {
			y = 65535 - y;
		}
		contact[id].seen = true;
		if (!contact[id].down) {
			hakios_touch_post(HAKIOS_TOUCH_DOWN, id, x, y);
		} else if (contact[id].x != x || contact[id].y != y) {
			hakios_touch_post(HAKIOS_TOUCH_MOVE, id, x, y);
		}
		contact[id].down = true;
		contact[id].x = x;
		contact[id].y = y;
	}
	/* a finger that is no longer in the report has lifted */
	for (uint32_t id = 0; id < GOODIX_MAX_TOUCHES; id++) {
		if (contact[id].down && !contact[id].seen) {
			contact[id].down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, id, contact[id].x, contact[id].y);
		}
	}
}

/* Read the report if there is one, and tell the chip it has been read. */
static void
goodix_read_report(void)
{
	static uint8_t report[8 + 8 * GOODIX_MAX_TOUCHES + 2];
	static unsigned bad, failed;
	const uint8_t zero = 0;
	const uint32_t first = 8 + 8 * 2 + 2;           /* the header and two fingers: all there is, most of the time */
	uint32_t count;

	seen.polls++;
	if (!goodix_read(touch_address, report, first)) {
		if (++failed == READS_FAILED_MOST) {
			printf("goodix: %u reads failed one after another: the fingers taken as lifted\n", failed);
			contacts_release();
		}
		return;
	}
	failed = 0;
	if (report[0] == 0) {
		return;
	}
	seen.reports++;
	count = report[2] & 0xf;
	if ((report[0] & GOODIX_STATUS_TOUCH) && count > 2 && count <= GOODIX_MAX_TOUCHES &&
	    !goodix_read(touch_address + 8, report + 8, 8 * count + 2)) {
		return;
	}
	goodix_write(touch_address, &zero, 1);
	if (!goodix_checksum_ok(report, 8)) {
		seen.bad++;
		if (bad++ < 8) {
			printf("goodix: bad report header: %02x %02x %02x %02x %02x %02x %02x %02x\n", report[0], report[1],
			    report[2], report[3], report[4], report[5], report[6], report[7]);
		}
		return;
	}
	if (report[0] & GOODIX_STATUS_REQUEST) {
		if ((report[2] & 0xf) == GOODIX_REQUEST_RESET || report[2] == GOODIX_REQUEST_RESET) {
			printf("goodix: the chip asks to be reset\n");
			/* (the reports after a reset do not say the fingers that were down have lifted) */
			contacts_release();
			touch_reset();
			if (high_rate && !goodix_command(GOODIX_CMD_HIGH_RATE, 1)) {
				report_rate = "240 (480 NOT ACKNOWLEDGED after a reset)";
				printf("goodix: 480 reports a second asked again after the reset: NOT ACKNOWLEDGED\n");
			}
		}
		return;
	}
	if (!(report[0] & GOODIX_STATUS_TOUCH)) {
		return;
	}
	if (count > GOODIX_MAX_TOUCHES || (count && !goodix_checksum_ok(report + 8, count * 8 + 2))) {
		seen.bad++;
		if (bad++ < 8) {
			printf("goodix: bad report (%u touches)\n", count);
		}
		return;
	}
	goodix_report(report + 8, count);
}

static bool
contacts_down(void)
{
	for (uint32_t id = 0; id < GOODIX_MAX_TOUCHES; id++) {
		if (contact[id].down) {
			return true;
		}
	}
	return false;
}

/* A look at the chip; fingers left down while it has said nothing believable for a while are lifted. */
static void
goodix_poll(void)
{
	static unsigned said;

	goodix_read_report();
	if (contacts_down() && micros_now() - believed_at >= SILENT_MOST_US) {
		if (said++ < 8) {
			printf("goodix: no report believed for %u ms with fingers down: the fingers taken as lifted\n",
			    SILENT_MOST_US / 1000);
		}
		contacts_release();
	}
}

/* ---- the FocalTech FT3680 chip --------------------------------------------------------------------- */

/*
 * FTS 5.x SPI framing from sm8350-kernel-OKX drivers/input/touchscreen/focaltech_3680/
 * focaltech_spi.c (verbatim):
 *   TX[0] = register address (writebuf[0] / cmd[0])
 *   TX[1] = FOCALTECH_READ_CTRL (0xA0 = READ_CMD, CRC_EN set) or FOCALTECH_WRITE_CTRL (0x00)
 *   TX[2..3] = data length BE (for read: datalen; for write: writelen - 1)
 *   TX[4..6] = three dummy bytes (SPI_DUMMY_BYTE)
 *   TX[7..7+len-1] = write data (empty on read; chip drives MISO here)
 *   TX[last-1..last] = CRC-16/Kermit of the data bytes, little-endian (only when READ_CMD or
 *                     when a vendor WRITE op needs CRC; vendor WRITE_CMD = 0x00 has no CRC)
 * SPI_HEADER_LENGTH = 4 + SPI_DUMMY_BYTE = 7.
 */
#define FOCALTECH_READ_CTRL         0xa0		/* READ_CMD = 0x80 | DATA_CRC_EN(0x20) */
#define FOCALTECH_WRITE_CTRL        0x00		/* WRITE_CMD, no CRC on TX */
#define FOCALTECH_CHIP_ID_H         0xa3
#define FOCALTECH_CHIP_ID_L         0x9f
#define FOCALTECH_TOUCH_DATA        0x01		/* the touch report lives at this address */
#define FOCALTECH_MAX_TOUCHES       10
#define FOCALTECH_TOUCH_REPORT_LEN  (2 + 6 * FOCALTECH_MAX_TOUCHES)
#define FOCALTECH_MAX_TRANSFER      (7 + FOCALTECH_TOUCH_REPORT_LEN + 2)

static uint8_t focaltech_tx[FOCALTECH_MAX_TRANSFER], focaltech_rx[FOCALTECH_MAX_TRANSFER];

/* CRC-16/Kermit (polynomial 0x8408 reflected, initial 0xffff) -- same as vendor rckit_crc16(). */
static uint16_t
focaltech_crc(const uint8_t *data, uint32_t length)
{
	uint16_t crc = 0xffff;

	for (uint32_t i = 0; i < length; i++) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; bit++) {
			crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
		}
	}
	return crc;
}

static bool
focaltech_read(uint8_t address, uint8_t *data, uint32_t length)
{
	const uint32_t data_offset = 7;
	uint32_t transfer_length = data_offset + length + 2;

	if (length == 0 || transfer_length > sizeof(focaltech_tx)) {
		return false;
	}
	for (int attempt = 0; attempt < 3; attempt++) {
		memset(focaltech_tx, 0, transfer_length);
		memset(focaltech_rx, 0, transfer_length);
		focaltech_tx[0] = address;		/* vendor: TX[0] = cmd[0] = reg address */
		focaltech_tx[1] = FOCALTECH_READ_CTRL;	/* vendor: TX[1] = ctrl = 0xA0 */
		focaltech_tx[2] = (uint8_t)(length >> 8);
		focaltech_tx[3] = (uint8_t)length;
		if (!spi_transfer(focaltech_tx, focaltech_rx, transfer_length)) {
			sleep_us(150);
			continue;
		}
		/* vendor: bit 7 of status (rx[3]) is BUSY, bit 5 is CRC error */
		if (!(focaltech_rx[3] & 0xa0)) {
			uint16_t expected = focaltech_crc(focaltech_rx + data_offset, length);
			uint16_t received = (uint16_t)(focaltech_rx[data_offset + length] |
			    focaltech_rx[data_offset + length + 1] << 8);
			if (expected == received) {
				memcpy(data, focaltech_rx + data_offset, length);
				sleep_us(150);		/* minimum CS-high time in the vendor driver */
				return true;
			}
		}
		sleep_us(150);
	}
	return false;
}

/*
 * Vendor fts_write: no CRC on TX. For a 1-byte "command" (length == 0) the frame is just the
 * four header bytes { addr, 0x00, 0x00, 0x00 } with no dummy and no data. For length > 0 the
 * three dummy bytes precede the data: { addr, 0x00, lenH, lenL, 0, 0, 0, data... }. The chip
 * drives the "status" byte on rx[3] -- bits 7 and 5 set mean busy or CRC error (vendor retries
 * SPI_RETRY_NUMBER = 3 times on failure before giving up).
 */
static bool
focaltech_write(uint8_t address, const uint8_t *data, uint32_t length)
{
	uint32_t transfer_length;

	if (length == 0) {
		transfer_length = 4;
	} else {
		transfer_length = 7 + length;
	}
	if (transfer_length > sizeof(focaltech_tx)) {
		return false;
	}
	memset(focaltech_tx, 0, transfer_length);
	focaltech_tx[0] = address;
	focaltech_tx[1] = FOCALTECH_WRITE_CTRL;
	focaltech_tx[2] = (uint8_t)(length >> 8);
	focaltech_tx[3] = (uint8_t)length;
	if (data && length > 0) {
		memcpy(focaltech_tx + 7, data, length);
	}
	for (int attempt = 0; attempt < 3; attempt++) {
		if (spi_transfer(focaltech_tx, focaltech_rx, transfer_length) &&
		    !(focaltech_rx[3] & 0xa0)) {
			sleep_us(150);
			return true;
		}
		sleep_us(150);
	}
	return false;
}

/*
 * Vendor fts_fwupg_get_boot_state() reads the chip's "boot ID" via the FT boot-ROM protocol:
 *   TX = { 0x55, 0xAA }                 (FTS_CMD_START1, FTS_CMD_START2 -- enter boot)
 *   wait FTS_CMD_START_DELAY = 12 ms
 *   TX = { 0x90 }, RX = 2 bytes         (FTS_CMD_READ_ID, incell cmd length)
 * If the two RX bytes are the chip's boot ID (0x56 0x62 for FT3680), firmware is present and
 * valid in flash but has not yet been started; the host must issue FTS_CMD_FIRMWARE_START
 * (0x08) to make the boot ROM jump to it.
 */
#define FTS_CMD_START1              0x55
#define FTS_CMD_START2              0xaa
#define FTS_CMD_START_DELAY         12		/* ms, between enter-boot and first command */
#define FTS_CMD_READ_BOOT_ID        0x90

/* Boot-ROM commands for firmware (pramboot) upload, from focaltech_3658u/focaltech_flash.h. */
#define FTS_ROMBOOT_CMD_SET_PRAM_ADDR   0xad
#define FTS_ROMBOOT_CMD_WRITE           0xae
#define FTS_ROMBOOT_CMD_START_APP       0x08
#define FTS_FLASH_PACKET_LENGTH         32		/* bytes per PRAM write */

#include "focaltech_fw.h"

/*
 * Vendor fts_read_bootid(): send { 0x55, 0xAA } as a 2-byte framed write (treated as a write of
 * one data byte 0xAA to register 0x55), wait 12 ms, then do a framed read of 2 bytes at
 * register 0x90. The chip answers with its ROM ID (0x56 0x62 for FT3680) if it is sitting in
 * the boot ROM waiting for a firmware download.
 */
static bool
focaltech_read_boot_id(uint8_t *high, uint8_t *low)
{
	uint8_t aa = FTS_CMD_START2;
	uint8_t out[2] = { 0 };
	bool ok;

	if (!focaltech_write(FTS_CMD_START1, &aa, 1)) {
		return false;
	}
	sleep_ms(12);			/* FTS_CMD_START_DELAY */
	ok = focaltech_read(FTS_CMD_READ_BOOT_ID, out, 2);
	*high = out[0];
	*low  = out[1];
	return ok;
}

/* Vendor fts_wait_tp_to_valid: poll chip ID for up to TIMEOUT_READ_REG (1 s) at 200 ms steps. */
static bool
focaltech_wait_valid(uint8_t *high, uint8_t *low)
{
	for (int attempt = 0; attempt < 5; attempt++) {
		if (focaltech_read(FOCALTECH_CHIP_ID_H, high, 1) &&
		    focaltech_read(FOCALTECH_CHIP_ID_L, low, 1) &&
		    *high == 0x56 && *low == 0x62) {
			printf("focaltech: FT3680 runtime ID valid after %d attempt(s)\n", attempt + 1);
			return true;
		}
		sleep_ms(200);			/* vendor INTERVAL_READ_REG */
	}
	return false;
}

/*
 * Reset the chip and send 0x55/0xAA to put it in boot ROM, as vendor fts_get_ic_information
 * does. Try up to 3 times with escalating settle delays (12, 20, 28 ms). Returns true if the
 * chip is sitting in its boot ROM, with { 0x56, 0x62 } read back from the boot-ID register.
 */
static bool
focaltech_enter_boot_rom(void)
{
	uint8_t boot_high = 0, boot_low = 0;

	for (int cnt = 0; cnt < 3; cnt++) {
		gpio_set(PIN_RESET, false);
		sleep_ms(1);
		gpio_set(PIN_RESET, true);
		sleep_ms(FTS_CMD_START_DELAY + cnt * 8);
		if (focaltech_read_boot_id(&boot_high, &boot_low) &&
		    boot_high == 0x56 && boot_low == 0x62) {
			printf("focaltech: in boot ROM after reset %d\n", cnt);
			return true;
		}
		printf("focaltech: boot ID attempt %d: %02x %02x (not 56 62)\n", cnt, boot_high, boot_low);
	}
	return false;
}

/*
 * Download the embedded firmware (pramboot + application) into the chip's PRAM via the boot
 * ROM's SPI_V2 upload protocol. Each 32-byte packet is sent as two framed writes: first set
 * the PRAM destination address (SET_PRAM_ADDR + 3 BE address bytes), then WRITE + data bytes.
 */
static bool
focaltech_write_pram(const uint8_t *buf, uint32_t len)
{
	uint8_t addr_bytes[3];

	for (uint32_t offset = 0; offset < len; offset += FTS_FLASH_PACKET_LENGTH) {
		uint32_t chunk = FTS_FLASH_PACKET_LENGTH;
		if (offset + chunk > len) {
			chunk = len - offset;
		}
		addr_bytes[0] = (uint8_t)(offset >> 16);
		addr_bytes[1] = (uint8_t)(offset >> 8);
		addr_bytes[2] = (uint8_t)offset;
		if (!focaltech_write(FTS_ROMBOOT_CMD_SET_PRAM_ADDR, addr_bytes, 3)) {
			printf("focaltech: PRAM address write failed at offset %u\n", offset);
			return false;
		}
		if (!focaltech_write(FTS_ROMBOOT_CMD_WRITE, buf + offset, chunk)) {
			printf("focaltech: PRAM data write failed at offset %u\n", offset);
			return false;
		}
	}
	return true;
}

static bool
focaltech_start_app(void)
{
	uint8_t cmd = FTS_ROMBOOT_CMD_START_APP;

	return focaltech_write(cmd, NULL, 0);
}

/*
 * Full chip bring-up: look for an already-running firmware; if none, reset to boot ROM and
 * download the embedded firmware into PRAM; then start it and wait for runtime to come up.
 */
static bool
focaltech_probe(bool verbose)
{
	uint8_t high = 0, low = 0;

	/*
	 * Maybe firmware is already running (previous session, warm boot) -- which a chip still held in reset cannot say:
	 * power_on() leaves the reset line low, and the look below found nothing every time
	 */
	touch_reset();
	if (focaltech_wait_valid(&high, &low)) {
		return true;
	}
	if (FOCALTECH_FW_LENGTH == 0) {
		/* (the build's stand-in focaltech_fw.h: started, the boot ROM would jump into empty PRAM) */
		printf("focaltech: no firmware built in (focaltech_fw.h is empty): the chip is not started\n");
		return false;
	}

	if (!focaltech_enter_boot_rom()) {
		if (verbose) {
			printf("focaltech: chip not in boot ROM; last read runtime %02x %02x\n", high, low);
		}
		return false;
	}

	printf("focaltech: downloading %u bytes of firmware to PRAM...\n", FOCALTECH_FW_LENGTH);
	if (!focaltech_write_pram(focaltech_fw, FOCALTECH_FW_LENGTH)) {
		printf("focaltech: PRAM download failed\n");
		return false;
	}
	printf("focaltech: PRAM download complete; starting application\n");
	if (!focaltech_start_app()) {
		printf("focaltech: start-app command failed\n");
		return false;
	}
	sleep_ms(300);			/* firmware needs time to initialize all peripherals */

	if (focaltech_wait_valid(&high, &low)) {
		return true;
	}
	if (verbose) {
		printf("focaltech: firmware did not come up (last runtime read %02x %02x)\n", high, low);
	}
	return false;
}

/* ---- FocalTech touch reports --------------------------------------------------------------------- */

/*
 * Per-chip report layout for FT3680 (vendor focaltech_core.c, FTS_TOUCH_OFF_* offsets):
 *   report[0] = gesture (0 outside gesture mode)
 *   report[1] = (chip_type << 4) | finger_count
 *   report[2 + 6*i + 0] = E_XH:  bits 7..6 = event (0 = down, 1 = up, 2 = contact, 3 = none)
 *                                bits 3..0 = x bits 14..11
 *   report[2 + 6*i + 1] = XL:    x bits 10..3
 *   report[2 + 6*i + 2] = ID_YH: bits 7..4 = finger id, bits 3..0 = y bits 14..11
 *   report[2 + 6*i + 3] = YL:    y bits 10..3
 *   report[2 + 6*i + 4] = PRE:   bits 7..5 = x bits 2..0, bits 4..2 = y bits 2..0
 *                                (also the pressure byte -- the lowest bits of each 15-bit axis
 *                                 are packed in with it)
 *   report[2 + 6*i + 5] = AREA:  touch area / major
 * Coordinates are 15-bit "super-high resolution" sub-pixel values; the touch panel's actual
 * extent is the board's touch { } coordinates (the device tree's focaltech,display-coords).
 */
#define FOCALTECH_EVENT_DOWN        0
#define FOCALTECH_EVENT_UP          1
#define FOCALTECH_EVENT_CONTACT     2

static struct {
	bool down, seen;
	uint32_t x, y;
} focaltech_contact[FOCALTECH_MAX_TOUCHES];

static void
focaltech_contact_update(uint32_t id, uint32_t event, uint32_t x, uint32_t y,
    uint32_t max_report_x, uint32_t max_report_y)
{
	static unsigned shown;

	if (id >= FOCALTECH_MAX_TOUCHES || max_report_x < 2 || max_report_y < 2) {
		return;
	}
	/* (only down and contact are a finger on the glass, as the vendor's EVENT_DOWN() has it: up and "no event" are not) */
	if (event != FOCALTECH_EVENT_DOWN && event != FOCALTECH_EVENT_CONTACT) {
		focaltech_contact[id].seen = true;
		if (focaltech_contact[id].down) {
			focaltech_contact[id].down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, id, focaltech_contact[id].x, focaltech_contact[id].y);
		}
		return;
	}
	seen.touches++;
	/* (15 bits from the chip, more than the panel has: kept on it, so that a mirror does not wrap past 0) */
	x = x < max_report_x ? x : max_report_x - 1;
	y = y < max_report_y ? y : max_report_y - 1;
	switch (flip) {
	case 1: x = max_report_x - 1 - x; break;
	case 2: y = max_report_y - 1 - y; break;
	case 3: x = max_report_x - 1 - x; y = max_report_y - 1 - y; break;
	}
	x = (uint32_t)(((uint64_t)x * 65535) / (max_report_x - 1));
	y = (uint32_t)(((uint64_t)y * 65535) / (max_report_y - 1));
	seen.last_x = x;
	seen.last_y = y;
	focaltech_contact[id].seen = true;
	if (!focaltech_contact[id].down) {
		focaltech_contact[id].down = true;
		hakios_touch_post(HAKIOS_TOUCH_DOWN, id, x, y);
	} else if (focaltech_contact[id].x != x || focaltech_contact[id].y != y) {
		hakios_touch_post(HAKIOS_TOUCH_MOVE, id, x, y);
	}
	focaltech_contact[id].x = x;
	focaltech_contact[id].y = y;
	if (shown++ < 8) {
		printf("focaltech: finger %u %s at %u, %u\n", id,
		    event == FOCALTECH_EVENT_DOWN ? "down" : "move", x, y);
	}
}

static void
focaltech_release(void)
{
	for (uint32_t i = 0; i < FOCALTECH_MAX_TOUCHES; i++) {
		if (focaltech_contact[i].down) {
			focaltech_contact[i].down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, i, focaltech_contact[i].x, focaltech_contact[i].y);
		}
	}
}

/* Panel size in the FT3680 super-high-resolution reports: the board's touch { } coordinates. */
static uint32_t focaltech_max_x, focaltech_max_y;

static void
focaltech_report(const uint8_t *report)
{
	uint32_t count = report[1] & 0x0f;

	/*
	 * A report that says no finger is on the glass and has one down or in contact in a slot says two things: not
	 * believed, and the fingers left as the last report had them
	 */
	for (uint32_t i = 0; count == 0 && i < FOCALTECH_MAX_TOUCHES; i++) {
		const uint8_t *touch = report + 2 + 6 * i;
		uint32_t event = (touch[0] >> 6) & 0x3;

		if (((touch[2] >> 4) & 0x0f) >= FOCALTECH_MAX_TOUCHES) {
			break;
		}
		if (event == FOCALTECH_EVENT_DOWN || event == FOCALTECH_EVENT_CONTACT) {
			seen.bad++;
			return;
		}
	}
	believed_at = micros_now();
	for (uint32_t i = 0; i < FOCALTECH_MAX_TOUCHES; i++) {
		focaltech_contact[i].seen = false;
	}
	for (uint32_t i = 0; i < FOCALTECH_MAX_TOUCHES; i++) {
		const uint8_t *touch = report + 2 + 6 * i;
		uint32_t id = (touch[2] >> 4) & 0x0f;
		uint32_t event, x, y;

		if (id >= FOCALTECH_MAX_TOUCHES) {
			break;			/* no more valid slots in this report */
		}
		event = (touch[0] >> 6) & 0x3;
		x = ((uint32_t)(touch[0] & 0x0f) << 11) | ((uint32_t)touch[1] << 3) |
		    ((uint32_t)touch[4] >> 5);
		y = ((uint32_t)(touch[2] & 0x0f) << 11) | ((uint32_t)touch[3] << 3) |
		    (((uint32_t)touch[4] & 0x1c) >> 2);
		focaltech_contact_update(id, event, x, y, focaltech_max_x, focaltech_max_y);
	}
	/* Fingers not in this report are up (vendor driver uses a bitmap; we remember explicitly). */
	for (uint32_t i = 0; i < FOCALTECH_MAX_TOUCHES; i++) {
		if (!focaltech_contact[i].seen && focaltech_contact[i].down) {
			focaltech_contact[i].down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, i, focaltech_contact[i].x, focaltech_contact[i].y);
		}
	}
}

static bool
focaltech_down(void)
{
	for (uint32_t i = 0; i < FOCALTECH_MAX_TOUCHES; i++) {
		if (focaltech_contact[i].down) {
			return true;
		}
	}
	return false;
}

static void
focaltech_poll(void)
{
	static uint8_t report[FOCALTECH_TOUCH_REPORT_LEN];
	static unsigned bad, failed, said;

	seen.polls++;
	if (!focaltech_read(FOCALTECH_TOUCH_DATA, report, sizeof(report))) {
		seen.bad++;
		if (bad++ < 8) {
			printf("focaltech: SPI read failed for touch report\n");
		}
		if (++failed == READS_FAILED_MOST) {
			printf("focaltech: %u reads failed one after another: the fingers taken as lifted\n", failed);
			focaltech_release();
		}
	} else {
		failed = 0;
		seen.reports++;
		focaltech_report(report);
	}
	/*
	 * As goodix_poll(): fingers left down while the chip has said nothing believable for a while are lifted. Counted
	 * in reads alone, a chip that falls silent with its interrupt line high (looked at every 80 ms) held them down for
	 * two and a half seconds -- long enough for five of them to end the run (input.c).
	 */
	if (focaltech_down() && micros_now() - believed_at >= SILENT_MOST_US) {
		if (said++ < 8) {
			printf("focaltech: no report believed for %u ms with fingers down: the fingers taken as lifted\n",
			    SILENT_MOST_US / 1000);
		}
		focaltech_release();
	}
}

static bool looking_only;              /* touchscreen=-1 */

/* Which touch chip is on this board. */
enum touch_chip { TOUCH_GOODIX, TOUCH_FOCALTECH };
static enum touch_chip touch_chip;

static enum touch_chip
touch_chip_detect(void)
{
	gpio_configure(PIN_FOCALTECH_ID, 0, PULL_NONE, 2, false);
	gpio_configure(PIN_GOODIX_ID, 0, PULL_NONE, 2, false);
	if (gpio_get(PIN_GOODIX_ID) && !gpio_get(PIN_FOCALTECH_ID)) {
		return TOUCH_FOCALTECH;
	}
	return TOUCH_GOODIX;
}

static bool
chip_probe(bool verbose)
{
	if (touch_chip == TOUCH_FOCALTECH) {
		return focaltech_probe(verbose);
	}
	touch_reset();
	return goodix_probe(verbose);
}

static void
chip_poll(void)
{
	if (touch_chip == TOUCH_FOCALTECH) {
		focaltech_poll();
	} else {
		goodix_poll();
	}
}

/* A look at the chip from the poll thread; false, and nothing read, once the run's end has the chip. */
static bool
chip_look(void)
{
	__atomic_store_n(&busy, true, __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&leaving, __ATOMIC_SEQ_CST)) {
		__atomic_store_n(&busy, false, __ATOMIC_SEQ_CST);
		return false;
	}
	chip_poll();
	__atomic_store_n(&busy, false, __ATOMIC_SEQ_CST);
	return true;
}

/* ---- all of it together ---------------------------------------------------------------------------- */

static void
show_state(const char *when)
{
	printf("goodix: %s: votes %#x, clocks %#x %#x %#x, rate cfg %#x dfs %#x; pins cs/clk/mosi/miso cfg %#x %#x %#x %#x,"
	    " reset %#x/%u, int %#x/%u, avdd %#x/%u, straps %u %u\n", when, rd(GCC_VOTE), rd(GCC_QUP_M_AHB_CBCR),
	    rd(GCC_QUP_S_AHB_CBCR), rd(GCC_SE_CBCR), rd(GCC_SE_CFG), rd(GCC_SE_DFSR), rd(GPIO_CFG(PIN_CS)),
	    rd(GPIO_CFG(PIN_CLK)), rd(GPIO_CFG(PIN_MOSI)), rd(GPIO_CFG(PIN_MISO)), rd(GPIO_CFG(PIN_RESET)),
	    rd(GPIO_IO(PIN_RESET)), rd(GPIO_CFG(PIN_INT)), rd(GPIO_IO(PIN_INT)), rd(GPIO_CFG(PIN_AVDD)), rd(GPIO_IO(PIN_AVDD)),
	    gpio_get(PIN_FOCALTECH_ID), gpio_get(PIN_GOODIX_ID));
}

static bool reset_driven, int_pulled, avdd_on;    /* what power_on() did, for power_down() */

/*
 * Power the chip up from nothing, in the vendor driver's order. False when its I/O supply did not come up: then nothing
 * more is driven, neither the analog supply nor a pin the chip would be powered through without it -- the interrupt
 * line's pull-up is such a pin, and is switched on only once the supply is.
 */
static bool
power_on(void)
{
	gpio_output(PIN_RESET, false, PULL_UP, 8);
	reset_driven = true;
	if (!io_supply_on()) {
		return false;
	}
	gpio_configure(PIN_INT, 0, PULL_UP, 8, false);
	int_pulled = true;
	sleep_ms(3);
	gpio_output(PIN_AVDD, true, PULL_NONE, 2);
	avdd_on = true;
	sleep_ms(15);
	return true;
}

/*
 * Everything power_on(), clocks_on() and the bus did, undone, in the vendor driver's order (goodix_berlin_power_off():
 * reset, then the analog supply, then the I/O supply): the chip deselected while it still has its supply, held in
 * reset, its analog supply switched off, the clocks' votes this driver took given back, and the I/O supply's
 * request withdrawn. Once RPMh says the supply is off, nothing of ours drives the chip or pulls one of its lines up:
 * the bus pins are let go. Without an answer the chip may still be powered, and chip select stays high. Neither
 * sleeps nor takes a lock at the run's end (leaving): bounded by RPMh's own wait.
 */
static void
power_down(void)
{
	if (bus_taken && io_voted) {
		gpio_output(PIN_CS, true, PULL_NONE, 2);        /* (also the end of a transfer that was cut off) */
	}
	if (reset_driven) {
		gpio_output(PIN_RESET, false, PULL_NONE, 2);
	}
	if (avdd_on) {
		gpio_output(PIN_AVDD, false, PULL_NONE, 2);
		avdd_on = false;
	}
	if (int_pulled) {
		gpio_configure(PIN_INT, 0, PULL_NONE, 2, false);
		int_pulled = false;
	}
	if (votes_added != 0) {
		wr(GCC_VOTE, rd(GCC_VOTE) & ~votes_added);
		votes_added = 0;
	}
	if (io_voted) {
		(void)io_supply_off();
	}
	if (bus_taken && !io_voted) {
		gpio_configure(PIN_CS, 0, PULL_NONE, 2, false);
		gpio_configure(PIN_CLK, 0, PULL_NONE, 2, false);
		gpio_configure(PIN_MOSI, 0, PULL_NONE, 2, false);
		gpio_configure(PIN_MISO, 0, PULL_NONE, 2, false);
		bus_taken = false;
	}
}

static void
use_engine_pins(void)
{
	bus_taken = true;
	gpio_configure(PIN_MISO, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	gpio_configure(PIN_MOSI, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	gpio_configure(PIN_CLK, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	gpio_configure(PIN_CS, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
}

static const char *
chip_name(void)
{
	return touch_chip == TOUCH_FOCALTECH ? "focaltech" : "goodix";
}

static bool
bring_up(void)
{
	const char *problem = "its clocks cannot be turned on";

	show_state("as found");
	touch_chip = touch_chip_detect();
	printf("goodix: ID gpio%u=%u gpio%u=%u: selected %s\n", PIN_GOODIX_ID, gpio_get(PIN_GOODIX_ID), PIN_FOCALTECH_ID,
	    gpio_get(PIN_FOCALTECH_ID), chip_name());
	if (!power_on()) {
		printf("goodix: the chip's I/O supply is not on: nothing more powered or driven\n");
		return false;
	}

	if (clocks_on()) {
		printf("goodix: serial engine: firmware %#x, interface %#x, parameters %#x, source clock %u Hz (select %u)\n",
		    rd(GENI_FW_REVISION_RO), rd(GENI_IF_DISABLE_RO), rd(SE_HW_PARAM_0), se_source_hz, se_clock_select);
		uint32_t spi_hz = 1000000;

		/* boot-arg touch_spi_hz=N: the bus at N Hz (Android's device tree gives the chip 5 MHz) */
		PE_parse_boot_argn("touch_spi_hz", &spi_hz, sizeof(spi_hz));
		if (spi_hz < 100000 || spi_hz > 10000000) {
			spi_hz = 1000000;
		}
		printf("goodix: the bus at %u Hz\n", spi_hz);
		problem = geni_spi_init(spi_hz);
	}
	if (problem == NULL) {
		use_engine_pins();
		spi_transfer = geni_transfer;
		show_state("powered, on the serial engine");
		if (chip_probe(true)) {
			return true;
		}
		printf("goodix: %s did not answer through the serial engine; trying the pins by hand\n", chip_name());
	} else {
		printf("goodix: %s; driving the pins by hand\n", problem);
	}

	/* by hand; which of the two data pins is the input is not written down anywhere, so try both */
	spi_transfer = bitbang_transfer;
	for (int attempt = 0; attempt < 2; attempt++) {
		bitbang_in = attempt == 0 ? PIN_MISO : PIN_MOSI;
		bitbang_out = attempt == 0 ? PIN_MOSI : PIN_MISO;
		bitbang_init();
		if (chip_probe(true)) {
			printf("goodix: %s answers on pins driven by hand (data in on gpio%u)\n", chip_name(), bitbang_in);
			return true;
		}
	}
	show_state("no answer");
	return false;
}

static void
goodix_thread(__unused void *parameter, __unused wait_result_t result)
{
	unsigned idle = 0, idle_looks;
	uint32_t poll_us = 8000, latency = 0, reads = 0;
	uint64_t before = 0, reading = 0;

	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	PE_parse_boot_argn("touch_lat", &latency, sizeof(latency));
	if (poll_us < 250 || poll_us > 100000) {
		poll_us = 8000;
	}
	idle_looks = 80000 / poll_us;   /* a report read every 80 ms even with the line high, in case it is missed */

	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	if (!tlmm || !gcc) {
		printf("goodix: the pins or the clock controller could not be mapped\n");
		thread_terminate(current_thread());
		return;
	}
	if (looking_only) {
		/* (the clock controller and the pins are always reachable; the serial engine, behind its clocks, is not read) */
		show_state("as found (touchscreen=-1: looking only)");
		printf("goodix: touchscreen=-1: the straps read gpio%u=%u gpio%u=%u (%s); nothing powered, configured or sent\n",
		    PIN_GOODIX_ID, gpio_get(PIN_GOODIX_ID), PIN_FOCALTECH_ID, gpio_get(PIN_FOCALTECH_ID),
		    gpio_get(PIN_GOODIX_ID) && !gpio_get(PIN_FOCALTECH_ID) ? "FocalTech" : "Goodix");
		thread_terminate(current_thread());
		return;
	}
	se = ml_io_map(SE_PHYS, SE_SIZE);
	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);      /* memory, not registers: no alignment rules */

	if (!se || !rsc || !cmd_db) {
		printf("goodix: the serial engine, RPMh or its command database could not be mapped\n");
		thread_terminate(current_thread());
		return;
	}
	if (!bring_up()) {
		printf("goodix: no %s touch screen; what was switched on for it is off again\n", chip_name());
		power_down();
		thread_terminate(current_thread());
		return;
	}
	printf("goodix: %s touch screen ready; the interrupt line is looked at every %u us\n", chip_name(), poll_us);
	{
		uint32_t rate = 0;

		/* boot-arg touch_rate=480: the chip reports twice as often (a phone vendor's "game mode"; 240 otherwise) */
		if (PE_parse_boot_argn("touch_rate", &rate, sizeof(rate)) && rate == 480 && touch_chip == TOUCH_GOODIX) {
			bool done = goodix_command(GOODIX_CMD_HIGH_RATE, 1);

			high_rate = true;
			report_rate = done ? "480 (acknowledged)" : "240 (480 NOT ACKNOWLEDGED)";
			printf("goodix: reports 480 times a second (command %#x at %#x): %s\n", GOODIX_CMD_HIGH_RATE, command_address,
			    done ? "acknowledged" : "NOT ACKNOWLEDGED");
		}
	}
	for (unsigned tick = 1; !__atomic_load_n(&leaving, __ATOMIC_SEQ_CST); tick++) {
		uint64_t looked = micros_now(), read_at;

		/* the interrupt line is low while a report waits; look anyway every 80 ms in case it is missed */
		if (!gpio_get(PIN_INT)) {
			idle = 0;
			if (!chip_look()) {
				break;
			}
			read_at = micros_now();
			reads++;
			reading += read_at - looked;
			if (latency) {
				hakios_tl("tl: c before %llu seen %llu read %llu\n", before, looked, read_at);
			}
		} else if (++idle >= idle_looks) {
			idle = 0;
			if (!chip_look()) {
				break;
			}
			reads++;
			reading += micros_now() - looked;
		}
		before = looked;
		if (tick % (60 * (1000000 / poll_us)) == 0) {
			printf("%s: so far %u reports in %u looks (%u bad), %u finger positions, the last at %u, %u; "
			    "interrupt line %s; a report takes %llu us to read; the line looked at every %u us, reports %s a second\n",
			    chip_name(), seen.reports, seen.polls, seen.bad, seen.touches, seen.last_x, seen.last_y,
			    gpio_get(PIN_INT) ? "high" : "low", reads ? reading / reads : 0, poll_us, report_rate);
		}
		sleep_us(poll_us);
	}
	thread_terminate(current_thread());     /* (the run's end has the chip) */
}

/*
 * At the run's end and on a panic's way out (osfmk/arm64/loader_return.c): the poll thread stopped, and the chip
 * powered down if it was powered up -- chip select high while it has its supply, reset low, the analog supply off,
 * and the I/O supply's request withdrawn without RPMh's lock, as the other drivers withdraw theirs there.
 */
void
hakios_goodix_quiesce(void)
{
	__atomic_store_n(&leaving, true, __ATOMIC_SEQ_CST);
	/* (nothing switched on, or all of it off again already: a chip that was not found) */
	if (tlmm == 0 || !(io_voted || avdd_on || int_pulled || bus_taken || votes_added != 0)) {
		return;
	}
	for (unsigned i = 0; i < 100 && __atomic_load_n(&busy, __ATOMIC_SEQ_CST); i++) {
		spin_us(1000);          /* (the thread finishing the report it reads: 100 ms at most, a stopped core's never ends) */
	}
	power_down();
	printf("goodix: at the run's end: %u reports in %u looks; the chip held in reset, its analog supply off, its I/O supply %s\n",
	    seen.reports, seen.polls, io_voted ? "STILL ASKED FOR" : "off");
}

void
hakios_goodix_init(void)
{
	int32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	looking_only = enabled == -1;
	/* (called only on a board that lists "touch-goodix": bsd/dev/board/drivers.c) */
	if (hakios_board()->pins.tlmm == 0 || hakios_board()->clocks.gcc == 0 || hakios_board()->rpmh.cmd_db == 0) {
		printf("touch: disabled: the SoC file does not say where the pins, the clock controller and RPMh are (pins { }, clocks { }, rpmh { })\n");
		return;
	}
	if (WIRED->serial_engine == 0 || WIRED->io_supply == NULL || WIRED->analog_enable < 0 || WIRED->vendor_pin[0] < 0 ||
	    WIRED->vendor_pin[1] < 0 || WIRED->max_x < 2 || WIRED->max_y < 2) {
		printf("touch: disabled: the board file does not say how the controller is wired (touch { }, with analog-enable, "
		    "vendor-pins and coordinates)\n");
		return;
	}
	max_x = focaltech_max_x = WIRED->max_x;
	max_y = focaltech_max_y = WIRED->max_y;
	PE_parse_boot_argn("touch_flip", &flip, sizeof(flip));
	if (kernel_thread_start(goodix_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
