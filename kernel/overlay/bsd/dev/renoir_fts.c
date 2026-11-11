/*
 * Renoir FT3658U (Android fts_ts, running chip ID 0x5652).
 * Hardware: OKX sm8350-kernel-OKX 08bbb0c150ea, renoir-sm7350.dtsi,
 * shima-qupv3.dtsi, pinctrl-shima.c and gcc-shima.c. Protocol facts:
 * focaltech_3658u/focaltech_spi.c and focaltech_core.{c,h}.
 * GENI/RPMh FIFO transport is adapted from this project's redwood_goodix.c.
 * Only touchscreen=1 on physical board renoir. No firmware flashing or bootloader
 * commands; the chip must boot its installed application firmware (0x5652).
 * Polling replaces Linux IRQ delivery; Linux IRQ283 is not a GIC interrupt ID.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

static vm_offset_t tlmm, gcc, se, rsc, cmd_db;

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static char sleep_event;
static void
sleep_ms(uint32_t ms)
{
    /* Yield the CPU while polling/resetting; do not busy-spin for whole milliseconds. */
    assert_wait_timeout((event_t)&sleep_event, THREAD_UNINT, ms, NSEC_PER_MSEC);
    thread_block(THREAD_CONTINUE_NULL);
}

/* ---- pins (TLMM) ------------------------------------------------------------------------------- */

#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define PULL_NONE               0
#define PULL_UP                 3

#define PIN_DATA0                20
#define PIN_DATA1                21
#define PIN_CLK                 22
#define PIN_CS                  23
#define PIN_INT                 31      /* low = the chip has a report */
#define PIN_RESET               30     /* low = in reset */
#define PIN_FUNCTION_SPI        1       /* "qup10" on gpio20..23; all use function 1 */

static void
gpio_configure(unsigned pin, unsigned function, unsigned pull, unsigned ma, bool output)
{
	wr(GPIO_CFG(pin), pull | function << 2 | (ma / 2 - 1) << 6 | (output ? 1u << 9 : 0));
}

static void
gpio_set(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), high ? 2 : 0);
}

static bool
gpio_get(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
gpio_output(unsigned pin, bool high, unsigned pull, unsigned ma)
{
	gpio_set(pin, high);            /* the level first, so that the pin never drives the wrong one */
	gpio_configure(pin, 0, pull, ma, true);
}

/* ---- the I/O supply (RPMh) ----------------------------------------------------------------------- */

#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x18220000UL    /* the application processor's "DRV 2" */
#define RSC_SIZE                0x10000UL
#define TCS(reg)                (rsc + 0xd00 + (reg))           /* command slot set 0, an "active" one */
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18            /* 0 while busy */
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100

#define VRM_VOLTAGE             0               /* a regulator's registers, from its address: millivolts */
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

/* The RPMh address of a resource by its name in the command database the boot loader left; 0 = none. */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		printf("fts: no command database (magic %02x %02x %02x %02x)\n", db->magic[0], db->magic[1], db->magic[2],
		    db->magic[3]);
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512 ||
		    slave->header_offset + (uint32_t)slave->count * sizeof(*entry) > CMD_DB_SIZE - sizeof(*db)) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static bool
rpmh_wait(uint32_t reg, uint32_t mask, uint32_t value, const char *what)
{
	for (int i = 0; i < 2000; i++) {
		if ((rd(TCS(reg)) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	printf("fts: RPMh: timeout waiting for %s (register %#x = %#x)\n", what, reg, rd(TCS(reg)));
	return false;
}

/* Send up to three register writes to RPMh and wait until they are done. */
static bool
rpmh_write_locked(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	bool done;

	/* the slot set must be idle, and switched off the way the hardware wants it: trigger, then enable */
	for (int i = 0; rd(TCS(TCS_STATUS)) == 0; i++) {
		if (i > 2000) {
			printf("fts: RPMh: the command slots stay busy\n");
			return false;
		}
		sleep_us(100);
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(TCS_CMD_MSGID(i)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(TCS_CMD_ADDR(i)), address[i]);
		wr(TCS(TCS_CMD_DATA(i)), data[i]);
	}
	wr(TCS(TCS_WAIT_FOR_COMPLETION), (1u << count) - 1);
	wr(TCS(TCS_CMD_ENABLE), (1u << count) - 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);        /* the status bit follows the enable; the interrupt itself stays masked */
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	done = rpmh_wait(TCS_IRQ_STATUS, 1, 1, "the request");
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	return done;
}

static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	redwood_rpmh_lock();
	bool ok=rpmh_write_locked(address,data,count);
	redwood_rpmh_unlock();return ok;
}

/* renoir DTS: L2C = I/O at 1.8 V, L7C = analog at 3.3 V. */
static bool
supply_on(const char *name, uint32_t millivolts)
{
    uint32_t base = rpmh_address(name);
    uint32_t address[3] = { base + VRM_VOLTAGE, base + VRM_ENABLE, base + VRM_MODE };
    uint32_t data[3] = { millivolts, 1, VRM_MODE_LDO_HPM };

    if (base == 0 || (base >> 16 & 0xf) != 4) {
        printf("fts: no RPMh regulator %s (address %#x)\n", name, base);
        return false;
    }
    printf("fts: regulator %s at %#x: %u mV\n", name, base, millivolts);
    return rpmh_write(address, data, 3);
}

/* ---- clocks (GCC) -------------------------------------------------------------------------------- */

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x60000UL
#define GCC_VOTE                (gcc + 0x52008)         /* the application processor's votes */
#define VOTE_QUP0_CORE_2X       (1u << 9)
#define VOTE_QUP0_CORE          (1u << 8)
#define VOTE_QUP0_M_AHB         (1u << 6)
#define VOTE_QUP0_S_AHB         (1u << 7)
#define VOTE_QUP0_SE2           (1u << 12)             /* SE10 is wrap0 S2, not wrap1 */
#define GCC_QUP0_M_AHB_CBCR     (gcc + 0x17004)         /* bit 31: the clock is off */
#define GCC_QUP0_S_AHB_CBCR     (gcc + 0x17008)
#define GCC_QUP0_SE2_CBCR       (gcc + 0x1726c)
#define GCC_QUP0_SE2_CMD        (gcc + 0x17270)         /* bit 0: take the new configuration */
#define GCC_QUP0_SE2_CFG        (gcc + 0x17274)         /* [4:0] 2*divider-1, [10:8] source, [13:12] mode */
#define GCC_QUP0_SE2_DFSR       (gcc + 0x17284)         /* bit 0: the engine chooses among preset rates itself */
#define GCC_QUP0_SE2_PERF_CFG(l) (gcc + 0x1728c + 4 * (l))
#define GCC_QUP0_SE2_PERF_M(l)  (gcc + 0x172cc + 4 * (l))
#define GCC_QUP0_SE2_PERF_N(l)  (gcc + 0x1730c + 4 * (l))
#define CLOCK_OFF               (1u << 31)
#define XO_HZ                   19200000

static uint32_t se_source_hz, se_clock_select;

/* clk-branch.c skips the halt bit for an AHB branch in hardware gated mode:
 * it may report OFF while idle, even after our GCC vote was accepted.
 */
static bool
fts_ahb_clock_ready(uint32_t status)
{
	return !(status & CLOCK_OFF) || (status & (1u << 1));
}

static uint32_t
rcg_rate(uint32_t cfg, uint32_t m, uint32_t not_n_minus_m)
{
	static const uint32_t parent[8] = { XO_HZ, 600000000, 0, 0, 0, 0, 300000000, 0 };
	uint64_t hz = parent[cfg >> 8 & 7];
	uint32_t divider = cfg & 0x1f, mode = cfg >> 12 & 3;

	if (divider) {
		hz = hz * 2 / (divider + 1);
	}
	if (mode) {
		uint32_t n = (~not_n_minus_m & 0xffff) + (m & 0xffff);

		if (n) {
			hz = hz * (m & 0xffff) / n;
		}
	}
	return (uint32_t)hz;
}

static bool
clocks_on(uint32_t target_hz)
{
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP0_CORE_2X | VOTE_QUP0_CORE | VOTE_QUP0_M_AHB | VOTE_QUP0_S_AHB);
	if (rd(GCC_QUP0_SE2_DFSR) & 1) {
		/* Pick a source no lower than the requested SPI frequency. */
		for (uint32_t level = 0; level < 8; level++) {
			uint32_t hz = rcg_rate(rd(GCC_QUP0_SE2_PERF_CFG(level)), rd(GCC_QUP0_SE2_PERF_M(level)),
			    rd(GCC_QUP0_SE2_PERF_N(level)));

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
		wr(GCC_QUP0_SE2_CFG, (rd(GCC_QUP0_SE2_CFG) & ~0x371fu) | 1);
		wr(GCC_QUP0_SE2_CMD, rd(GCC_QUP0_SE2_CMD) | 1);
		for (int i = 0; i < 500 && (rd(GCC_QUP0_SE2_CMD) & 1); i++) {
			sleep_us(10);
		}
		if (rd(GCC_QUP0_SE2_CMD) & 1) {
			printf("fts: the serial engine's clock does not take its configuration (%#x)\n", rd(GCC_QUP0_SE2_CMD));
			return false;
		}
		se_source_hz = XO_HZ;
		se_clock_select = 0;
	}
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP0_SE2);
	const uint32_t votes = VOTE_QUP0_CORE_2X | VOTE_QUP0_CORE | VOTE_QUP0_M_AHB | VOTE_QUP0_S_AHB | VOTE_QUP0_SE2;
	if ((rd(GCC_VOTE) & votes) != votes) {
		printf("fts: GCC clock votes not accepted (%#x)\n", rd(GCC_VOTE));
		return false;
	}
	for (int i = 0; i < 1000; i++) {
		if (fts_ahb_clock_ready(rd(GCC_QUP0_M_AHB_CBCR)) &&
		    fts_ahb_clock_ready(rd(GCC_QUP0_S_AHB_CBCR)) && !(rd(GCC_QUP0_SE2_CBCR) & CLOCK_OFF)) {
			return true;
		}
		sleep_us(10);
	}
	printf("fts: clocks stay off: %#x %#x %#x\n", rd(GCC_QUP0_M_AHB_CBCR), rd(GCC_QUP0_S_AHB_CBCR),
	    rd(GCC_QUP0_SE2_CBCR));
	return false;
}

/* ---- SPI through the serial engine (GENI) ---------------------------------------------------------- */

#define SE_PHYS                 0x00988000UL
#define SE_SIZE                 0x4000UL
#define GENI_FORCE_DEFAULT      (se + 0x020)
#define GENI_OUTPUT_CTRL        (se + 0x024)
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_STATUS             (se + 0x040)    /* bit 0: a command is running */
#define GENI_SER_M_CLK_CFG      (se + 0x048)    /* bit 0 enable, [15:4] divider */
#define GENI_IF_DISABLE_RO      (se + 0x064)    /* bit 0: no FIFO interface */
#define GENI_FW_REVISION_RO     (se + 0x068)    /* [15:8] protocol: 1 = SPI */
#define GENI_CLK_SEL            (se + 0x07c)
#define SPI_CPHA                (se + 0x224)
#define SPI_LOOPBACK            (se + 0x22c)
#define SPI_CPOL                (se + 0x230)
#define SPI_DEMUX_OUTPUT_INV    (se + 0x24c)
#define SPI_DEMUX_SEL           (se + 0x250)
#define GENI_BYTE_GRAN          (se + 0x254)
#define GENI_DMA_MODE_EN        (se + 0x258)
#define SPI_TRANS_CFG           (se + 0x25c)
#define GENI_TX_PACKING_CFG0    (se + 0x260)
#define GENI_TX_PACKING_CFG1    (se + 0x264)
#define SPI_WORD_LEN            (se + 0x268)
#define SPI_TX_TRANS_LEN        (se + 0x26c)
#define SPI_RX_TRANS_LEN        (se + 0x270)
#define SPI_DELAY_COUNTERS      (se + 0x278)
#define GENI_RX_PACKING_CFG0    (se + 0x284)
#define GENI_RX_PACKING_CFG1    (se + 0x288)
#define GENI_M_CMD0             (se + 0x600)    /* [31:27] operation */
#define GENI_M_CMD_CTRL         (se + 0x604)    /* bit 2 cancel, bit 1 abort */
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_IRQ_CLEAR        (se + 0x648)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_TX_FIFO_STATUS     (se + 0x800)    /* [27:0] words */
#define GENI_RX_FIFO_STATUS     (se + 0x804)    /* bit 31 last, [30:28] bytes of the last word, [24:0] words */
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define SE_DMA_TX_IRQ_CLR       (se + 0xc44)
#define SE_DMA_RX_IRQ_CLR       (se + 0xd44)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)    /* [21:16] FIFO depth in words */
#define SE_DMA_GENERAL_CFG      (se + 0xe30)

#define M_CMD_DONE              (1u << 0)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
#define M_ERRORS                ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))
#define SPI_FULL_DUPLEX         (3u << 27)

static uint32_t fifo_depth;

/* 0 = ready; otherwise why the engine cannot be used */
static const char *
geni_spi_init(uint32_t hz)
{
	uint32_t divider;

	if ((rd(GENI_FW_REVISION_RO) >> 8 & 0xff) != 1) {
		return "the serial engine has no SPI firmware";
	}
	if (rd(GENI_IF_DISABLE_RO) & 1) {
		return "the serial engine's FIFO interface is disabled";
	}
	fifo_depth = rd(SE_HW_PARAM_0) >> 16 & 0x3f;
	if (fifo_depth < 4) {
		return "the serial engine reports no FIFO";
	}
	wr(SE_GSI_EVENT_EN, 0);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(GENI_CGC_CTRL, rd(GENI_CGC_CTRL) | 0x7f);
	wr(SE_DMA_GENERAL_CFG, rd(SE_DMA_GENERAL_CFG) | 0xf);
	wr(GENI_OUTPUT_CTRL, 0x7f);
	wr(GENI_FORCE_DEFAULT, 1);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) & ~1u);
	wr(GENI_RX_WATERMARK, fifo_depth - 3);
	wr(GENI_RX_RFR_WATERMARK, fifo_depth - 2);
	wr(GENI_TX_WATERMARK, 0);
	/* the status bits we poll follow these enables; the interrupt itself stays masked at the GIC */
	wr(SE_IRQ_EN, rd(SE_IRQ_EN) | 0xf);
	wr(GENI_M_IRQ_EN, rd(GENI_M_IRQ_EN) | 0x7fc0007f);

	/* mode 0, chip select 0, 8 bits, most significant first, four bytes in a FIFO word */
	wr(SPI_LOOPBACK, 0);
	wr(SPI_DEMUX_SEL, 0);
	wr(SPI_CPHA, 0);
	wr(SPI_CPOL, 0);
	wr(SPI_DEMUX_OUTPUT_INV, 0);
	wr(SPI_TRANS_CFG, rd(SPI_TRANS_CFG) & ~2u);
	wr(SPI_DELAY_COUNTERS, 0);
	wr(GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_TX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_RX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_BYTE_GRAN, 0);
	wr(SPI_WORD_LEN, 8 - 4);

	divider = (se_source_hz + hz - 1) / hz;
	wr(GENI_CLK_SEL, se_clock_select & 7);
	wr(GENI_SER_M_CLK_CFG, (divider ? divider : 1) << 4 | 1);
	return NULL;
}

static void
geni_drain(uint8_t *rx, uint32_t length, uint32_t *received)
{
	uint32_t status = rd(GENI_RX_FIFO_STATUS), words = status & 0x01ffffff, bytes = words * 4;

	if (status & 0x80000000u) {
		uint32_t valid = status >> 28 & 7;

		if (valid && valid < 4) {
			bytes -= 4 - valid;
		}
	}
	for (uint32_t i = 0; i < words; i++) {
		uint32_t word = rd(GENI_RX_FIFO);

		for (int j = 0; j < 4 && bytes; j++, bytes--) {
			if (*received < length) {
				if (rx) {
					rx[*received] = (uint8_t)(word >> (8 * j));
				}
				(*received)++;
			}
		}
	}
}

static void
geni_recover(void)
{
	wr(GENI_M_CMD_CTRL, 1u << 2);
	for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL); i++) {
		sleep_us(100);
	}
	if (!(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL)) {
		wr(GENI_M_CMD_CTRL, 1u << 1);
		for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_ABORT); i++) {
			sleep_us(100);
		}
	}
	for (int i = 0; i < 64 && (rd(GENI_RX_FIFO_STATUS) & 0x01ffffff); i++) {
		(void)rd(GENI_RX_FIFO);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
}

/* One transfer with the chip selected throughout: `length` bytes out (zeros without tx) and as many in. */
static bool
geni_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	static unsigned complaints;
	uint32_t sent = 0, received = 0, status = 0;
	uint64_t deadline;

	for (int i = 0; rd(GENI_STATUS) & 1; i++) {
		if (i > 2000) {
			geni_recover();
			break;
		}
		sleep_us(100);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(SPI_TX_TRANS_LEN, length);
	wr(SPI_RX_TRANS_LEN, length);
	wr(GENI_M_CMD0, SPI_FULL_DUPLEX);
	clock_interval_to_deadline(200, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		while (sent < length && (rd(GENI_TX_FIFO_STATUS) & 0x0fffffff) + 1 < fifo_depth) {
			uint32_t word = 0;

			for (int j = 0; j < 4 && sent < length; j++, sent++) {
				word |= (uint32_t)(tx ? tx[sent] : 0) << (8 * j);
			}
			wr(GENI_TX_FIFO, word);
		}
		status = rd(GENI_M_IRQ_STATUS);
		geni_drain(rx, length, &received);
		if (status & M_ERRORS) {
			break;
		}
		if (status & M_CMD_DONE) {
			geni_drain(rx, length, &received);
			wr(GENI_M_IRQ_CLEAR, status);
			if (received == length) {
				return true;
			}
			break;
		}
	}
	if (complaints++ < 8) {
		printf("fts: SPI transfer of %u bytes failed: status %#x, sent %u, received %u\n", length, status, sent,
		    received);
	}
	geni_recover();
	return false;
}

/* GPIO mode-0 fallback when boot firmware leaves GENI restricted to GPI DMA. */
static unsigned bitbang_in, bitbang_out;
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
#include "renoir_fts_protocol.h"

static bool (*spi_transfer)(const uint8_t *, uint8_t *, uint32_t) = geni_transfer;
static uint8_t fts_tx[FTS_SPI_BYTES], fts_rx[FTS_SPI_BYTES];
static uint32_t flip, spi_hz = 12000000, trace;
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
        memset(fts_rx, 0, sizeof(fts_rx));
        bool transferred = spi_transfer(fts_tx, fts_rx, FTS_READ_PREFIX + length + 2);
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
                redwood_touch_post(REDWOOD_TOUCH_UP, id, contact[id].x, contact[id].y);
                contact[id].down = false;
            }
            continue;
        }
        uint32_t x = (uint32_t)frame->x[id] * 65535 / FTS_MAX_X;
        uint32_t y = (uint32_t)frame->y[id] * 65535 / FTS_MAX_Y;
        if (flip & 1) { x = 65535 - x; }
        if (flip & 2) { y = 65535 - y; }
        if (!contact[id].down) {
            if (trace && seen.positions < 40) {
                printf("fts: finger %u DOWN at %u,%u\n", id, frame->x[id], frame->y[id]);
            }
            redwood_touch_post(REDWOOD_TOUCH_DOWN, id, x, y);
        } else if (contact[id].x != x || contact[id].y != y) {
            redwood_touch_post(REDWOOD_TOUCH_MOVE, id, x, y);
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
    sleep_ms(1);
    gpio_set(PIN_RESET, true);
    sleep_ms(200);
}

static bool
fts_probe(void)
{
    uint8_t high = 0, low = 0, version = 0, mode = 0xff;
    for (unsigned attempt = 0; attempt < 10; attempt++) {
        if (fts_read(0xa3, &high, 1) && fts_read(0x9f, &low, 1) && high == 0x56 && low == 0x52) {
            if (!fts_read(0x00, &mode, 1) || (mode & 0x70) != 0) {
                printf("fts: chip %#x%02x is not in application work mode (%#x)\n", high, low, mode);
                return false;
            }
            if (fts_read(0xa6, &version, 1)) {
                printf("fts: FT3658U ID 0x%02x%02x, firmware %#x, SPI mode 0, %u Hz requested\n",
                    high, low, version, spi_hz);
            }
            return true;
        }
        sleep_ms(100);
    }
    printf("fts: no application ID 0x5652 (last 0x%02x%02x); no firmware is flashed\n", high, low);
    return false;
}

static bool
fts_bring_up(void)
{
    /* DTS: SPI SE10 is wrap0 S2; gpio20..23 are qup10, GPIO30 reset, GPIO31 IRQ. */
    printf("fts: renoir SE10 0x988000; reset gpio30, IRQ gpio31; L2C/L7C\n");
    gpio_output(PIN_RESET, false, PULL_UP, 8);
    gpio_configure(PIN_INT, 0, PULL_UP, 8, false);
    sleep_ms(1);
    if (!supply_on("ldoc2", 1800)) { return false; }
    sleep_ms(1);
    if (!supply_on("ldoc7", 3300)) { return false; }
    sleep_ms(15);
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
    /* As in redwood's fallback, accept a data-pin orientation only after ID+CRC.
     * CS stays high while directions change; use the minimum 2 mA drive. */
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        bitbang_in = attempt == 0 ? PIN_DATA0 : PIN_DATA1;
        bitbang_out = attempt == 0 ? PIN_DATA1 : PIN_DATA0;
        fts_bitbang_init();
        fts_reset();
        if (fts_probe()) {
            printf("fts: GPIO SPI ready: MISO gpio%u, MOSI gpio%u, CLK gpio22, CS gpio23\n",
                bitbang_in, bitbang_out);
            return true;
        }
    }
    gpio_set(PIN_CS, true);
    return false;
}

static void
fts_thread(__unused void *parameter, __unused wait_result_t result)
{
    uint8_t report[FTS_REPORT_BYTES];
    unsigned failures = 0, resets = 0;
    tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
    gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
    se = ml_io_map(SE_PHYS, SE_SIZE);
    rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
    cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
    if (!tlmm || !gcc || !se || !rsc || !cmd_db || !fts_bring_up()) {
        printf("fts: touchscreen unavailable\n");
        thread_terminate(current_thread());
        return;
    }
    printf("fts: touchscreen ready, 1080x2400, ten contacts; polling at 125 Hz\n");
    for (unsigned tick = 1;; tick++) {
        /* Falling-edge IRQ can be a brief pulse: read periodically even if GPIO is high. */
        bool pending = !gpio_get(PIN_INT), active = false;
        for (unsigned id = 0; id < FTS_CONTACTS; id++) { active |= contact[id].down; }
        if (!pending && !active && tick % 10 != 0) {
            sleep_ms(8);
            continue;
        }
        seen.polls++;
        struct fts_frame frame;
        bool read_ok = fts_read(0x01, report, sizeof(report));
        /* This chip returns a CRC-valid all-ff frame while idle. Android reads
         * only on IRQ; our periodic fallback must not reset an idle controller. */
        if (read_ok && !pending && !active && fts_report_empty(report, sizeof(report))) {
            failures = 0;
            sleep_ms(8);
            continue;
        }
        if (read_ok && fts_decode(report, sizeof(report), &frame)) {
            failures = 0;
            seen.reports++;
            fts_report(&frame);
        } else {
            seen.bad++;
            if (seen.bad <= 4) {
                if (read_ok) {
                    printf("fts: rejected report: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                        report[0], report[1], report[2], report[3], report[4], report[5], report[6], report[7]);
                } else {
                    printf("fts: report SPI/status/CRC failure\n");
                }
            }
            if (++failures >= 125) {
                fts_release();
                printf("fts: %u consecutive invalid reports (%u SPI failures); previous recoveries %u/3\n",
                    failures, seen.transfers_failed, resets);
                if (resets++ >= 3) { break; }
                fts_reset();
                if (!fts_probe()) { break; }
                failures = 0;
            }
        }
        if (tick % (60 * 125) == 0) {
            printf("fts: polls %u, reports %u, invalid %u, failed SPI reads %u, positions %u, IRQ %s\n",
                seen.polls, seen.reports, seen.bad, seen.transfers_failed, seen.positions,
                gpio_get(PIN_INT) ? "high" : "low");
        }
        sleep_ms(8);
    }
    fts_release();
    printf("fts: stopped after repeated controller errors\n");
    thread_terminate(current_thread());
}

void
renoir_fts_init(void)
{
    uint32_t enabled = 0;
    thread_t thread;
    if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) { return; }
    if (!redwood_board_is("renoir")) {
        printf("fts: disabled: FT3658U wiring is for renoir only\n");
        return;
    }
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
