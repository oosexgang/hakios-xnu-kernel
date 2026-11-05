/*
 * The touch screen of the Xiaomi Mi 8 (dipper): STMicroelectronics FingerTipS
 * controller (st_fts / FTS521, address 0x49) on I2C serial engine 14 (QUPV3 wrap 1 SE6 at 0xa98000).
 *
 * Hardware connections on dipper:
 *   I2C       qupv3_se14_i2c at 0xa98000 (0x4000 bytes)
 *   clocks    GCC at 0x100000: vote 0x5200c (SE6 bit 28, AHB bits 20, 21), SE6 clock 0x18738
 *   pins      TLMM (North Tile at 0x3900000, South Tile at 0x3d00000):
 *             GPIO 31: interrupt (active low while controller has pending events)
 *             GPIO 32: reset (active low)
 *             GPIO 33: I2C SDA (qup14, function 2)
 *             GPIO 34: I2C SCL (qup14, function 2)
 *             GPIO 23: VDDIO power rail (disp_vddio_vreg, active high)
 *
 * Events feed /dev/hakios-touch (sys/hakios.input.h).
 * Diagnostics & testing interface exposed via /dev/hakios-i2c (sys/hakios.i2c.h).
 */

#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/hakios.input.h>
#include <sys/hakios.i2c.h>
#include <sys/systm.h>
#include <sys/uio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */
extern void hakios_tl(const char *format, ...) __printflike(1, 2);

static vm_offset_t tlmm_north, tlmm_south, gcc, se;
static lck_mtx_t i2c_mutex;
static lck_grp_t *i2c_lck_grp;

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- pins (TLMM SDM845) ------------------------------------------------------------------------ */

/*
 * On SDM845, TLMM registers are grouped in tiles:
 *   North Tile: 0x03400000 + 0x00500000 = 0x03900000 (pins 31-34)
 *   South Tile: 0x03400000 + 0x00900000 = 0x03d00000 (pin 23)
 * Mapping only the needed ranges avoids overflowing the kernel's io_submap (8 MB total).
 */
#define TLMM_NORTH_PHYS         0x03900000UL
#define TLMM_NORTH_SIZE         0x00030000UL    /* covers pins up to 48 */

#define TLMM_SOUTH_PHYS         0x03d00000UL
#define TLMM_SOUTH_SIZE         0x00020000UL    /* covers pin 23 */

#define PIN_VDDIO               23      /* South tile */
#define PIN_INT                 31      /* North tile, low = pending events */
#define PIN_RESET               32      /* North tile, low = in reset */
#define PIN_SDA                 33      /* North tile */
#define PIN_SCL                 34      /* North tile */
#define PIN_FUNC_GPIO           0
#define PIN_FUNC_QUP14          2

#define PULL_NONE               0
#define PULL_DOWN               1
#define PULL_UP                 3

static inline vm_offset_t
gpio_cfg_reg(unsigned pin)
{
	if (pin == PIN_VDDIO) {
		return tlmm_south + 0x1000UL * pin;
	}
	return tlmm_north + 0x1000UL * pin;
}

static inline vm_offset_t
gpio_io_reg(unsigned pin)
{
	return gpio_cfg_reg(pin) + 4;
}

static void
gpio_configure(unsigned pin, unsigned function, unsigned pull, unsigned ma, bool output)
{
	unsigned drv = (ma >= 2) ? ((ma / 2 - 1) & 7) : 0;
	wr(gpio_cfg_reg(pin), (pull & 3) | ((function & 0xf) << 2) | (drv << 6) | (output ? (1u << 9) : 0));
}

static void
gpio_set(unsigned pin, bool high)
{
	uint32_t val = rd(gpio_io_reg(pin));
	if (high) {
		val |= (1u << 1);
	} else {
		val &= ~(1u << 1);
	}
	wr(gpio_io_reg(pin), val);
}

static bool
gpio_get(unsigned pin)
{
	return (rd(gpio_io_reg(pin)) & 1) != 0;
}

/* ---- PMIC / RPMh VRM (PM8998 LDO 28 / 3.3V for ST FTS) ---------------------------------------- */

#define APSS_RSC_PHYS           0x179e0000UL
#define APSS_RSC_SIZE           0x00004000UL

#define RSC_DRV_IRQ_STATUS      0x04
#define RSC_DRV_IRQ_CLEAR       0x08
#define RSC_DRV_CMD_WAIT_FOR_CMPL 0x10
#define RSC_DRV_CONTROL         0x14
#define RSC_DRV_STATUS          0x18
#define RSC_DRV_CMD_ENABLE      0x1C
#define RSC_DRV_CMD_MSGID(n)    (0x30 + 0x14 * (n))
#define RSC_DRV_CMD_ADDR(n)     (0x34 + 0x14 * (n))
#define RSC_DRV_CMD_DATA(n)     (0x38 + 0x14 * (n))

#define TCS_AMC_MODE_ENABLE     (1u << 16)
#define TCS_AMC_MODE_TRIGGER    (1u << 24)

#define CMD_MSGID_LEN           8u
#define CMD_MSGID_RESP_REQ      (1u << 8)
#define CMD_MSGID_WRITE         (1u << 16)

#define RPMH_VRM_LDOA28_VOLT    0x42100UL
#define RPMH_VRM_LDOA28_EN      0x42104UL

static void
rsc_enable_ldoa28(void)
{
	vm_offset_t rsc_map = ml_io_map(APSS_RSC_PHYS, APSS_RSC_SIZE);
	if (!rsc_map) {
		printf("fts: failed to map APSS RSC\n");
		return;
	}
	vm_offset_t base = rsc_map + 0xd00;

	/* Wait up to 1ms for TCS 0 to be IDLE */
	for (int i = 0; i < 100; i++) {
		if (rd(base + RSC_DRV_STATUS) != 0) {
			break;
		}
		sleep_us(10);
	}

	uint32_t msgid = CMD_MSGID_LEN | CMD_MSGID_RESP_REQ | CMD_MSGID_WRITE;

	/* CMD 0: set voltage to 3300 mV */
	wr(base + RSC_DRV_CMD_MSGID(0), msgid);
	wr(base + RSC_DRV_CMD_ADDR(0), RPMH_VRM_LDOA28_VOLT);
	wr(base + RSC_DRV_CMD_DATA(0), 3300);

	/* CMD 1: set enable to 1 */
	wr(base + RSC_DRV_CMD_MSGID(1), msgid);
	wr(base + RSC_DRV_CMD_ADDR(1), RPMH_VRM_LDOA28_EN);
	wr(base + RSC_DRV_CMD_DATA(1), 1);

	/* Both commands must complete */
	wr(base + RSC_DRV_CMD_WAIT_FOR_CMPL, 0x3);
	wr(base + RSC_DRV_CMD_ENABLE, 0x3);

	/* Trigger AMC on TCS 0 */
	uint32_t ctl = rd(base + RSC_DRV_CONTROL);
	ctl &= ~TCS_AMC_MODE_TRIGGER;
	wr(base + RSC_DRV_CONTROL, ctl);
	ctl &= ~TCS_AMC_MODE_ENABLE;
	wr(base + RSC_DRV_CONTROL, ctl);

	ctl = TCS_AMC_MODE_ENABLE;
	wr(base + RSC_DRV_CONTROL, ctl);
	ctl |= TCS_AMC_MODE_TRIGGER;
	wr(base + RSC_DRV_CONTROL, ctl);

	/* Wait up to 10ms for TCS 0 IRQ completion */
	for (int i = 0; i < 1000; i++) {
		if (rd(base + RSC_DRV_IRQ_STATUS) & 1) {
			break;
		}
		sleep_us(10);
	}

	/* Clear IRQ and disable AMC mode */
	wr(base + RSC_DRV_IRQ_CLEAR, 1);
	wr(base + RSC_DRV_CONTROL, 0);

	printf("fts: PM8998 L28 (3.3V) enabled via RPMh RSC\n");
}

static uint32_t pin_pull_mode = PULL_UP;

static void
pins_init(void)
{
	/* Hold in reset first */
	gpio_configure(PIN_RESET, PIN_FUNC_GPIO, PULL_UP, 16, true);
	gpio_set(PIN_RESET, false);

	/* Power on VDDIO (disp_vddio_vreg, GPIO 23) */
	gpio_configure(PIN_VDDIO, PIN_FUNC_GPIO, PULL_UP, 2, true);
	gpio_set(PIN_VDDIO, true);

	/* Power on AVDD (PM8998 LDO 28 / 3.3V) */
	rsc_enable_ldoa28();

	sleep_ms(30);
	gpio_set(PIN_RESET, true);
	sleep_ms(120);

	/* Interrupt pin: input with pull-up */
	gpio_configure(PIN_INT, PIN_FUNC_GPIO, PULL_UP, 16, false);

	/* I2C pins: function QUP14 with pull-up enabled */
	gpio_configure(PIN_SDA, PIN_FUNC_QUP14, pin_pull_mode, 2, false);
	gpio_configure(PIN_SCL, PIN_FUNC_QUP14, pin_pull_mode, 2, false);

	printf("fts: pins_init: INT line (GPIO %u) is %s, VDDIO=HIGH, RESET=HIGH\n",
	    PIN_INT, gpio_get(PIN_INT) ? "high" : "low");
}

/* ---- clocks (GCC SDM845) ----------------------------------------------------------------------- */

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x00060000UL

#define GCC_VOTE_WRAP1          (gcc + 0x5200c)
#define VOTE_WRAP1_M_AHB        (1u << 20)
#define VOTE_WRAP1_S_AHB        (1u << 21)
#define VOTE_WRAP1_SE6          (1u << 28)

#define GCC_QUPV3_WRAP1_SE6_CMD (gcc + 0x18738)
#define GCC_QUPV3_WRAP1_SE6_CFG (gcc + 0x1873c)
#define GCC_QUPV3_WRAP1_SE6_CBCR (gcc + 0x18734)
#define GCC_QUPV3_WRAP1_M_AHB_CBCR (gcc + 0x1800c)
#define GCC_QUPV3_WRAP1_S_AHB_CBCR (gcc + 0x18010)
#define CLOCK_OFF               (1u << 31)

static bool
clocks_on(void)
{
	/* 1. Root clock: set CMD_ROOT_EN (bit 1) and CMD_UPDATE (bit 0) */
	wr(GCC_QUPV3_WRAP1_SE6_CFG, 0); /* 19.2 MHz source undivided */
	wr(GCC_QUPV3_WRAP1_SE6_CMD, rd(GCC_QUPV3_WRAP1_SE6_CMD) | (1u << 1) | 1u);
	for (int i = 0; i < 500 && (rd(GCC_QUPV3_WRAP1_SE6_CMD) & 1); i++) {
		sleep_us(10);
	}
	/* Wait for root clock to turn ON (CMD_ROOT_OFF, bit 31, must be 0) */
	for (int i = 0; i < 500 && (rd(GCC_QUPV3_WRAP1_SE6_CMD) & (1u << 31)); i++) {
		sleep_us(10);
	}

	/* 2. Direct branch enable for SE6 CBCR, M_AHB, S_AHB */
	wr(GCC_QUPV3_WRAP1_SE6_CBCR, rd(GCC_QUPV3_WRAP1_SE6_CBCR) | 1u);
	wr(GCC_QUPV3_WRAP1_M_AHB_CBCR, rd(GCC_QUPV3_WRAP1_M_AHB_CBCR) | 1u);
	wr(GCC_QUPV3_WRAP1_S_AHB_CBCR, rd(GCC_QUPV3_WRAP1_S_AHB_CBCR) | 1u);

	/* 3. Vote for AHB and SE6 clocks in GCC_VOTE_WRAP1 */
	wr(GCC_VOTE_WRAP1, rd(GCC_VOTE_WRAP1) | VOTE_WRAP1_M_AHB | VOTE_WRAP1_S_AHB | VOTE_WRAP1_SE6);
	for (int i = 0; i < 1000; i++) {
		if (!((rd(GCC_QUPV3_WRAP1_M_AHB_CBCR) | rd(GCC_QUPV3_WRAP1_S_AHB_CBCR) |
		    rd(GCC_QUPV3_WRAP1_SE6_CBCR)) & CLOCK_OFF)) {
			return true;
		}
		sleep_us(10);
	}

	printf("fts: warning: branch clocks report off: m_ahb=%#x s_ahb=%#x se6=%#x rcg=%#x\n",
	    rd(GCC_QUPV3_WRAP1_M_AHB_CBCR), rd(GCC_QUPV3_WRAP1_S_AHB_CBCR),
	    rd(GCC_QUPV3_WRAP1_SE6_CBCR), rd(GCC_QUPV3_WRAP1_SE6_CMD));
	return true;
}

/* ---- I2C through QUPv3 GENI -------------------------------------------------------------------- */

#define SE_PHYS                 0x00a98000UL
#define SE_SIZE                 0x4000UL

#define GENI_FORCE_DEFAULT      (se + 0x020)
#define GENI_OUTPUT_CTRL        (se + 0x024)
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_STATUS             (se + 0x040)
#define GENI_SER_M_CLK_CFG      (se + 0x048)
#define GENI_IF_DISABLE_RO      (se + 0x064)
#define GENI_FW_REVISION_RO     (se + 0x068)
#define GENI_CLK_SEL            (se + 0x07c)
#define GENI_BYTE_GRAN          (se + 0x254)
#define GENI_DMA_MODE_EN        (se + 0x258)
#define GENI_TX_PACKING_CFG0    (se + 0x260)
#define GENI_TX_PACKING_CFG1    (se + 0x264)
#define SE_I2C_TX_TRANS_LEN     (se + 0x26c)
#define SE_I2C_RX_TRANS_LEN     (se + 0x270)
#define SE_I2C_SCL_COUNTERS     (se + 0x278)
#define GENI_RX_PACKING_CFG0    (se + 0x284)
#define GENI_RX_PACKING_CFG1    (se + 0x288)
#define GENI_M_CMD0             (se + 0x600)
#define GENI_M_CMD_CTRL         (se + 0x604)
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_IRQ_EN           (se + 0x644)
#define GENI_S_IRQ_CLEAR        (se + 0x648)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_TX_FIFO_STATUS     (se + 0x800)
#define GENI_RX_FIFO_STATUS     (se + 0x804)
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define SE_GENI_M_GP_LENGTH     (se + 0x910)
#define SE_GENI_IOS             (se + 0x908)
#define SE_DMA_TX_IRQ_CLR       (se + 0xc44)
#define SE_DMA_RX_IRQ_CLR       (se + 0xd44)
#define SE_DMA_TX_PTR_L         (se + 0xc30)
#define SE_DMA_TX_PTR_H         (se + 0xc34)
#define SE_DMA_TX_ATTR          (se + 0xc38)
#define SE_DMA_TX_LEN           (se + 0xc3c)
#define SE_DMA_TX_IRQ_STAT      (se + 0xc40)
#define SE_DMA_TX_IRQ_EN_SET    (se + 0xc4c)
#define SE_DMA_TX_LEN_IN        (se + 0xc54)
#define SE_DMA_TX_FSM_RST       (se + 0xc58)
#define SE_DMA_RX_PTR_L         (se + 0xd30)
#define SE_DMA_RX_PTR_H         (se + 0xd34)
#define SE_DMA_RX_ATTR          (se + 0xd38)
#define SE_DMA_RX_LEN           (se + 0xd3c)
#define SE_DMA_RX_IRQ_STAT      (se + 0xd40)
#define SE_DMA_RX_IRQ_EN_SET    (se + 0xd4c)
#define SE_DMA_RX_LEN_IN        (se + 0xd54)
#define SE_DMA_RX_FSM_RST       (se + 0xd58)

#define TX_DMA_DONE             (1u << 0)
#define TX_SBE                  (1u << 2)
#define RX_DMA_DONE             (1u << 0)
#define RX_SBE                  (1u << 2)
/* GP error bits in RX DMA status (GP_IRQ1/3/4 shifted by 5) */
#define RX_DMA_GP_ERRS          (((1u << 1) | (1u << 3) | (1u << 4)) << 5)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)
#define SE_DMA_GENERAL_CFG      (se + 0xe30)

#define I2C_OPCODE_WRITE        (1u << 27)
#define I2C_OPCODE_READ         (2u << 27)
#define I2C_STOP_STRETCH        (1u << 2)

#define M_CMD_DONE              (1u << 0)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
#define M_TX_FIFO_WATERMARK     (1u << 30)
/* FIFO-mode IRQ mask, exactly like geni_se_select_fifo_mode() in
 * drivers/platform/msm/qcom-geni-se.c (M_COMMON_GENI_M_IRQ_EN |
 * M_CMD_DONE | M_TX/RX_FIFO_WATERMARK | M_RX_FIFO_LAST) */
#define FIFO_M_IRQ_EN_MASK      ((1u << 0) | (1u << 26) | (1u << 27) | (1u << 30) | \
				 (0x7e) | (1u << 22) | (1u << 23) | (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))
#define M_ERRORS                ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 9) | (1u << 10) | \
				 (1u << 12) | (1u << 13) | (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))

static uint32_t fts_i2c_addr = 0x49;
static uint32_t fifo_depth = 16;
/* On SDM845 QUPv3 GENI, DFS mode index 0 is default 19.2MHz root */
static uint32_t current_clk_idx = 0;
static uint32_t current_speed_khz = 100;

static struct {
	uint32_t polls;
	uint32_t events;
	uint32_t touches;
	uint32_t last_x;
	uint32_t last_y;
	uint32_t last_m_irq;
	uint8_t  last_ev[8];
	bool     online;
	uint8_t  int_pin;
	uint8_t  rst_pin;
	uint8_t  vddio_pin;
	uint8_t  sda_pin;
	uint8_t  scl_pin;
	uint32_t probe_count;
	uint32_t rd_ok;
	uint32_t stage;
	uint32_t snap[4];
	uint32_t err_irq;
	uint32_t rd_fail;
	uint32_t wm_hits;
	uint32_t txf_pre;
	uint32_t wr_words;
	uint32_t txf_max;
	uint32_t gp_len;
	uint32_t loop_iter;
	uint32_t rb_if;
	uint32_t rb_dma;
	uint32_t rb_irqen;
	uint32_t rb_wmk;
	uint32_t rb_pack0;
	uint32_t rb_gran;
	uint32_t rb_2004;
	uint32_t rb_2008;
	uint32_t probe_byte;
} stats;

static void
capture_readbacks(void)
{
	stats.rb_if = rd(GENI_IF_DISABLE_RO);
	stats.rb_dma = rd(GENI_DMA_MODE_EN);
	stats.rb_irqen = rd(GENI_M_IRQ_EN);
	stats.rb_wmk = rd(GENI_TX_WATERMARK);
	stats.rb_pack0 = rd(GENI_TX_PACKING_CFG0);
	stats.rb_gran = rd(GENI_BYTE_GRAN);
	/* NOTE: se+0x2004/0x2008 (SE_DMA_IF_EN / SE_FIFO_IF_DISABLE from
	 * newer kernels) must NOT be touched on SDM845: that range is not
	 * decoded and the read aborts the kernel thread before the test
	 * even starts. */
}

/* select FIFO (non-DMA) mode before every transfer, like Linux does in
 * geni_i2c_xfer() via geni_se_select_mode(FIFO_MODE). Without this the
 * IRQ enables / DMA-mode bits can be in a stale state and the TX
 * watermark request never reaches the sequencer. */
static void
geni_select_fifo_mode(void)
{
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(SE_IRQ_EN, 0xffffffff);
	wr(GENI_M_IRQ_EN, FIFO_M_IRQ_EN_MASK);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) & ~1u);
	wr(SE_GSI_EVENT_EN, 0);
}

/* ---- SE DMA (the SE's internal DMA engine) -------------------------------
 * The CPU FIFO interface is hardware-disabled on this SE
 * (GENI_IF_DISABLE_RO reads 1), so data cannot be fed through
 * GENI_TX_FIFO at all. The SE DMA engine is a separate interface:
 * program SE_DMA_TX/RX_PTR + LEN + ATTR, set GENI_DMA_MODE_EN and
 * let the sequencer pull/push data directly from/to RAM, exactly
 * like geni_se_tx_dma_prep()/geni_se_rx_dma_prep() in
 * drivers/platform/msm/qcom-geni-se.c. */
#define DMA_BUF_SIZE            256
static uint8_t dma_tx_buf[DMA_BUF_SIZE] __attribute__((aligned(256)));
static uint8_t dma_rx_buf[DMA_BUF_SIZE] __attribute__((aligned(256)));

static inline void
cache_clean_range(void *p, size_t n)
{
	uintptr_t a = (uintptr_t)p;
	uintptr_t end = a + n;
	a &= ~(uintptr_t)63;
	for (; a < end; a += 64) {
		__asm__ volatile("dc cvau, %0" ::"r"(a));
	}
	__asm__ volatile("dmb sy");
}

static inline void
cache_invalidate_range(void *p, size_t n)
{
	uintptr_t a = (uintptr_t)p;
	uintptr_t end = a + n;
	a &= ~(uintptr_t)63;
	for (; a < end; a += 64) {
		__asm__ volatile("dc ivac, %0" ::"r"(a));
	}
	__asm__ volatile("dmb sy");
}

/* like geni_se_select_dma_mode() in qcom-geni-se.c */
static void
geni_select_dma_mode(void)
{
	wr(SE_GSI_EVENT_EN, 0);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(SE_IRQ_EN, 0xffffffff);
	/* keep DONE + error IRQs enabled so we can poll M_IRQ_STATUS for
	 * NACK/bus errors in addition to the DMA done bits */
	wr(GENI_M_IRQ_EN, FIFO_M_IRQ_EN_MASK & ~(M_TX_FIFO_WATERMARK | (1u << 26) | (1u << 27)));
	wr(GENI_S_IRQ_EN, 0);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) | 1u);
}

static void
geni_recover(void)
{
	wr(GENI_TX_WATERMARK, 0);
	/* reset the DMA FSMs so a wedged DMA does not poison the next xfer */
	wr(SE_DMA_TX_FSM_RST, 1);
	wr(SE_DMA_RX_FSM_RST, 1);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(GENI_M_CMD_CTRL, 1u << 2);   /* cancel */
	for (int i = 0; i < 500 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL); i++) {
		sleep_us(50);
	}
	if (!(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL)) {
		wr(GENI_M_CMD_CTRL, 1u << 1);   /* abort */
		for (int i = 0; i < 500 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_ABORT); i++) {
			sleep_us(50);
		}
	}
	for (int i = 0; i < 64 && (rd(GENI_RX_FIFO_STATUS) & 0x01ffffff); i++) {
		(void)rd(GENI_RX_FIFO);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
}

static bool
geni_wait_idle(void)
{
	for (int i = 0; i < 1000; i++) {
		if (!(rd(GENI_STATUS) & 1)) {
			return true;
		}
		sleep_us(10);
	}
	return false;
}

static bool
geni_i2c_apply_clock(uint32_t clk_idx, uint32_t speed_khz)
{
	current_clk_idx = clk_idx;
	current_speed_khz = speed_khz;

	wr(GENI_CLK_SEL, clk_idx);

	if (speed_khz >= 400) {
		/* Fast mode 400 kHz: clk_div=2, t_high=5, t_low=12, t_cycle=24 */
		wr(GENI_SER_M_CLK_CFG, (2 << 4) | 1);
		wr(SE_I2C_SCL_COUNTERS, (5u << 20) | (12u << 10) | 24);
	} else {
		/* Standard mode 100 kHz: clk_div=7, t_high=10, t_low=11, t_cycle=26 (Linux matching) */
		wr(GENI_SER_M_CLK_CFG, (7 << 4) | 1);
		wr(SE_I2C_SCL_COUNTERS, (10u << 20) | (11u << 10) | 26);
	}

	printf("fts: clock configured: clk_sel=%u, speed=%ukHz, scl_counters=%#x, ser_cfg=%#x\n",
	    clk_idx, speed_khz, rd(SE_I2C_SCL_COUNTERS), rd(GENI_SER_M_CLK_CFG));
	return true;
}

static bool
geni_i2c_init(void)
{
	uint32_t proto = (rd(GENI_FW_REVISION_RO) >> 8) & 0xff;
	if (proto != 3) {
		printf("fts: serial engine protocol is not I2C (%u, expected 3)\n", proto);
		return false;
	}
	uint32_t hw0 = rd(SE_HW_PARAM_0);
	fifo_depth = (hw0 >> 16) & 0x3f;
	if (fifo_depth < 2 || fifo_depth > 64) {
		fifo_depth = 16;
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
	wr(GENI_RX_WATERMARK, fifo_depth - 1);
	wr(GENI_RX_RFR_WATERMARK, fifo_depth);
	wr(GENI_TX_WATERMARK, 0);
	geni_select_fifo_mode();

	/* 4 bytes per FIFO word, 8-bit MSB first */
	wr(GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_TX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_RX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_BYTE_GRAN, 0);

	geni_i2c_apply_clock(current_clk_idx, current_speed_khz);
	capture_readbacks();

	printf("fts: QUPv3 GENI I2C ready (FIFO depth %u, hw0=%#x, clk_sel %u, ios=%#x)\n",
	    fifo_depth, hw0, current_clk_idx, rd(SE_GENI_IOS));
	return true;
}

/* ---- low-level bus transfers ------------------------------------------------------------------- */

/* one DMA write phase; DMA mode must already be selected.
 * returns true on TX_DMA_DONE, false on error/timeout (no recover here,
 * caller decides). */
static bool
dma_write_phase(uint8_t addr, const uint8_t *data, uint32_t length, bool hold)
{
	uint32_t status = 0;
	uint64_t deadline;
	uint32_t params = (addr & 0x7f) << 9;
	uint64_t pa;

	if (length == 0 || length > DMA_BUF_SIZE) {
		return false;
	}
	memcpy(dma_tx_buf, data, length);
	cache_clean_range(dma_tx_buf, length);
	pa = kvtophys((vm_offset_t)dma_tx_buf);
	stats.wr_words++;

	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_I2C_TX_TRANS_LEN, length);
	wr(GENI_M_CMD0, I2C_OPCODE_WRITE | params | (hold ? I2C_STOP_STRETCH : 0));
	/* geni_se_tx_dma_prep(): enable DMA IRQs, program pointer/len/attr */
	wr(SE_DMA_TX_IRQ_EN_SET, 7);
	wr(SE_DMA_TX_PTR_L, (uint32_t)pa);
	wr(SE_DMA_TX_PTR_H, (uint32_t)(pa >> 32));
	wr(SE_DMA_TX_ATTR, 1);      /* EOT bit */
	wr(SE_DMA_TX_LEN, length);
	__builtin_arm_dmb(15);

	clock_interval_to_deadline(2000, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		uint32_t txst = rd(SE_DMA_TX_IRQ_STAT);
		status = rd(GENI_M_IRQ_STATUS);
		if (status) {
			stats.last_m_irq = status;
		}
		if ((status & (M_ERRORS | M_CMD_ABORT)) || (txst & TX_SBE)) {
			stats.err_irq = status | txst;
			return false;
		}
		if (txst & TX_DMA_DONE) {
			wr(SE_DMA_TX_IRQ_CLR, txst);
			if (status) {
				wr(GENI_M_IRQ_CLEAR, status);
			}
			return true;
		}
		sleep_us(50);
	}
	stats.snap[0] = rd(GENI_STATUS);
	stats.snap[1] = rd(SE_DMA_TX_IRQ_STAT);
	stats.snap[2] = rd(SE_DMA_TX_LEN_IN);
	stats.snap[3] = rd(GENI_M_CMD0);
	stats.gp_len = rd(SE_GENI_M_GP_LENGTH);
	capture_readbacks();
	return false;
}

static bool bb_write(uint8_t addr, const uint8_t *data, uint32_t length, bool hold);
static bool bb_read(uint8_t addr, uint8_t *data, uint32_t length, bool repeated);

static bool
i2c_write(uint8_t addr, const uint8_t *data, uint32_t length, bool hold)
{
	/* hardware GENI data paths are disabled on this SE: bit-bang */
	return bb_write(addr, data, length, hold);
}

/* one DMA read phase; DMA mode must already be selected */
static bool
dma_read_phase(uint8_t addr, uint8_t *data, uint32_t length)
{
	uint32_t status = 0;
	uint64_t deadline;
	uint32_t params = (addr & 0x7f) << 9;
	uint64_t pa;

	if (length == 0 || length > DMA_BUF_SIZE) {
		return false;
	}
	cache_invalidate_range(dma_rx_buf, length);
	pa = kvtophys((vm_offset_t)dma_rx_buf);

	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(SE_I2C_RX_TRANS_LEN, length);
	wr(GENI_M_CMD0, I2C_OPCODE_READ | params);
	/* geni_se_rx_dma_prep(): enable DMA IRQs, program pointer/len/attr */
	wr(SE_DMA_RX_IRQ_EN_SET, 7);
	wr(SE_DMA_RX_PTR_L, (uint32_t)pa);
	wr(SE_DMA_RX_PTR_H, (uint32_t)(pa >> 32));
	wr(SE_DMA_RX_ATTR, 0);      /* RX has no EOT bit */
	wr(SE_DMA_RX_LEN, length);
	__builtin_arm_dmb(15);

	clock_interval_to_deadline(2000, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		uint32_t rxst = rd(SE_DMA_RX_IRQ_STAT);
		status = rd(GENI_M_IRQ_STATUS);
		if (status) {
			stats.last_m_irq = status;
		}
		if ((status & (M_ERRORS | M_CMD_ABORT)) || (rxst & (RX_SBE | RX_DMA_GP_ERRS))) {
			stats.err_irq = status | rxst;
			return false;
		}
		if (rxst & RX_DMA_DONE) {
			wr(SE_DMA_RX_IRQ_CLR, rxst);
			if (status) {
				wr(GENI_M_IRQ_CLEAR, status);
			}
			cache_invalidate_range(dma_rx_buf, length);
			memcpy(data, dma_rx_buf, length);
			return true;
		}
		sleep_us(50);
	}
	stats.snap[0] = rd(GENI_STATUS);
	stats.snap[1] = rd(SE_DMA_RX_IRQ_STAT);
	stats.snap[2] = rd(SE_DMA_RX_LEN_IN);
	stats.snap[3] = rd(GENI_M_CMD0);
	stats.gp_len = rd(SE_GENI_M_GP_LENGTH);
	capture_readbacks();
	return false;
}

static bool
i2c_read(uint8_t addr, uint8_t *data, uint32_t length)
{
	return bb_read(addr, data, length, false);
}

static bool
i2c_write_read(uint8_t addr, const uint8_t *tx_data, uint32_t tx_len, uint8_t *rx_data, uint32_t rx_len)
{
	if (tx_len == 0 || rx_len == 0 || tx_len > DMA_BUF_SIZE || rx_len > DMA_BUF_SIZE) {
		return false;
	}
	/* Method 1: bit-bang write held for repeated start, then read */
	stats.last_m_irq = 0;
	stats.stage = 0;
	if (!bb_write(addr, tx_data, tx_len, true)) {
		stats.stage = 1;
		stats.err_irq = stats.last_m_irq;
	} else if (!bb_read(addr, rx_data, rx_len, true)) {
		stats.stage = 4;	/* read phase failed */
		stats.err_irq = stats.last_m_irq;
	} else {
		return true;
	}
	/* Method 2 fallback: write with STOP, then standalone read */
	uint32_t m1_stage = stats.stage;
	stats.last_m_irq = 0;
	if (!bb_write(addr, tx_data, tx_len, false)) {
		stats.stage = 10 + m1_stage;	/* 1x: fallback write failed */
		stats.err_irq = stats.last_m_irq;
		return false;
	}
	stats.last_m_irq = 0;
	if (!bb_read(addr, rx_data, rx_len, false)) {
		stats.stage = 20 + m1_stage;	/* 2x: fallback read failed */
		stats.err_irq = stats.last_m_irq;
		return false;
	}
	return true;
}

/* ---- software (bit-bang) I2C over GPIO 33/34 ------------------------------------------------
 * The GENI CPU-FIFO interface is hardware-disabled on this SE
 * (GENI_IF_DISABLE_RO=1) and SE-DMA commands are rejected with
 * M_ILLEGAL_CMD, so the hardware data paths are unusable. The
 * touchscreen pins are plain GPIOs though: emulate open-drain I2C
 * by toggling the TLMM output-enable bit (release = pulled high,
 * output-enable + value 0 = driven low). 100 kHz is plenty for
 * the FTS and the master being slow is legal I2C. */
static uint32_t bb_ops;

static inline void
bb_sda_low(void)
{
	wr(gpio_io_reg(PIN_SDA), rd(gpio_io_reg(PIN_SDA)) & ~(1u << 1));
	wr(gpio_cfg_reg(PIN_SDA), rd(gpio_cfg_reg(PIN_SDA)) | (1u << 9));
}

static inline void
bb_sda_release(void)
{
	wr(gpio_cfg_reg(PIN_SDA), rd(gpio_cfg_reg(PIN_SDA)) & ~(1u << 9));
}

static inline void
bb_scl_low(void)
{
	wr(gpio_io_reg(PIN_SCL), rd(gpio_io_reg(PIN_SCL)) & ~(1u << 1));
	wr(gpio_cfg_reg(PIN_SCL), rd(gpio_cfg_reg(PIN_SCL)) | (1u << 9));
}

static inline void
bb_scl_release(void)
{
	wr(gpio_cfg_reg(PIN_SCL), rd(gpio_cfg_reg(PIN_SCL)) & ~(1u << 9));
}

static inline bool
bb_sda_get(void)
{
	return (rd(gpio_io_reg(PIN_SDA)) & 1) != 0;
}

static inline bool
bb_scl_get(void)
{
	return (rd(gpio_io_reg(PIN_SCL)) & 1) != 0;
}

static inline void
bb_half(void)
{
	delay_for_interval(3, NSEC_PER_USEC);   /* ~3us half period */
}

/* release SCL and wait for the slave to let go (clock stretching).
 * returns false on timeout. */
static bool
bb_scl_high(void)
{
	bb_scl_release();
	for (int i = 0; i < 2000; i++) {
		if (bb_scl_get()) {
			return true;
		}
		bb_half();
	}
	return false;
}

static void
bb_start(void)
{
	bb_sda_release();
	if (!bb_scl_high()) {
		return;
	}
	bb_half();
	bb_sda_low();
	bb_half();
	bb_scl_low();
}

static void
bb_repeated_start(void)
{
	/* SCL is low here (end of previous phase): raise SDA, SCL, drop SDA */
	bb_sda_release();
	(void)bb_scl_high();
	bb_half();
	bb_sda_low();
	bb_half();
	bb_scl_low();
}

static void
bb_stop(void)
{
	bb_sda_low();
	bb_half();
	(void)bb_scl_high();
	bb_half();
	bb_sda_release();
	bb_half();
}

/* write one byte, return slave ACK */
static bool
bb_write_byte(uint8_t b)
{
	for (int i = 7; i >= 0; i--) {
		if (b & (1u << i)) {
			bb_sda_release();
		} else {
			bb_sda_low();
		}
		bb_half();
		if (!bb_scl_high()) {
			return false;
		}
		bb_half();
		bb_scl_low();
	}
	/* ACK phase */
	bb_sda_release();
	bb_half();
	if (!bb_scl_high()) {
		return false;
	}
	bool ack = !bb_sda_get();
	bb_half();
	bb_scl_low();
	return ack;
}

/* read one byte; ack=true sends ACK after the byte */
static bool
bb_read_byte(uint8_t *out, bool ack)
{
	uint8_t b = 0;
	bb_sda_release();
	for (int i = 7; i >= 0; i--) {
		bb_half();
		if (!bb_scl_high()) {
			return false;
		}
		bb_half();
		if (bb_sda_get()) {
			b |= (uint8_t)(1u << i);
		}
		bb_scl_low();
	}
	if (ack) {
		bb_sda_low();
	} else {
		bb_sda_release();
	}
	bb_half();
	if (!bb_scl_high()) {
		return false;
	}
	bb_half();
	bb_scl_low();
	*out = b;
	return true;
}

static void
bb_init(void)
{
	/* reconfigure the QUP pins as plain GPIOs, open-drain style */
	gpio_configure(PIN_SDA, PIN_FUNC_GPIO, PULL_UP, 16, true);
	gpio_configure(PIN_SCL, PIN_FUNC_GPIO, PULL_UP, 16, true);
	bb_sda_release();
	bb_scl_release();
	bb_half();
	printf("fts: bit-bang i2c up: sda=%d scl=%d\n", bb_sda_get() ? 1 : 0, bb_scl_get() ? 1 : 0);
}

/* bit-bang write; hold=true ends without STOP (repeated start follows).
 * SCL is left low in that case. */
static bool
bb_write(uint8_t addr, const uint8_t *data, uint32_t length, bool hold)
{
	bb_ops++;
	bb_start();
	if (!bb_write_byte((uint8_t)(addr << 1))) {
		stats.err_irq = 0xa0000000 | addr;
		bb_stop();
		return false;
	}
	for (uint32_t i = 0; i < length; i++) {
		if (!bb_write_byte(data[i])) {
			stats.err_irq = 0xd0000000 | data[i];
			bb_stop();
			return false;
		}
	}
	if (!hold) {
		bb_stop();
	}
	return true;
}

static bool
bb_read(uint8_t addr, uint8_t *data, uint32_t length, bool repeated)
{
	bb_ops++;
	if (repeated) {
		bb_repeated_start();
	} else {
		bb_start();
	}
	if (!bb_write_byte((uint8_t)((addr << 1) | 1))) {
		stats.err_irq = 0xa1000000 | addr;
		bb_stop();
		return false;
	}
	for (uint32_t i = 0; i < length; i++) {
		bool last = (i + 1 == length);
		if (!bb_read_byte(&data[i], !last)) {
			stats.err_irq = 0xe0000000 | i;
			bb_stop();
			return false;
		}
	}
	bb_stop();
	return true;
}

/* ---- bus scanner & diagnostics ----------------------------------------------------------------- */

static void
i2c_scan_bus(struct hakios_i2c_scan *scan)
{
	printf("fts: === I2C BUS SCAN (0x08..0x77, clk_sel=%u, %ukHz) ===\n",
	    current_clk_idx, current_speed_khz);
	printf("fts:      0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");

	if (scan) {
		memset(scan, 0, sizeof(*scan));
	}

	for (uint8_t row = 0; row < 128; row += 16) {
		char line[80];
		int pos = snprintf(line, sizeof(line), "fts: %02x: ", row);

		for (uint8_t col = 0; col < 16; col++) {
			uint8_t a = row + col;
			if (a < 0x08 || a > 0x77) {
				pos += snprintf(line + pos, sizeof(line) - pos, "   ");
				continue;
			}

			uint8_t dummy = 0;
			bool ack = i2c_read(a, &dummy, 1);
			if (ack) {
				pos += snprintf(line + pos, sizeof(line) - pos, "%02x ", a);
				if (scan) {
					scan->found[a] = 1;
					scan->count++;
				}
			} else {
				pos += snprintf(line + pos, sizeof(line) - pos, "-- ");
			}
		}
		printf("%s\n", line);
	}
	if (scan) {
		printf("fts: scan complete: %u device(s) found\n", scan->count);
	}
}

static void
i2c_get_status(struct hakios_i2c_status *st)
{
	if (!st) return;
	st->geni_status = rd(GENI_STATUS);
	st->geni_ios = rd(SE_GENI_IOS);
	st->m_irq_status = rd(GENI_M_IRQ_STATUS);
	st->clk_sel = rd(GENI_CLK_SEL);
	st->scl_counters = rd(SE_I2C_SCL_COUNTERS);
	st->ser_m_clk_cfg = rd(GENI_SER_M_CLK_CFG);
	st->pin_vddio = gpio_get(PIN_VDDIO) ? 1 : 0;
	st->pin_int = gpio_get(PIN_INT) ? 1 : 0;
	st->pin_reset = gpio_get(PIN_RESET) ? 1 : 0;
	st->pin_sda = gpio_get(PIN_SDA) ? 1 : 0;
	st->pin_scl = gpio_get(PIN_SCL) ? 1 : 0;
	st->gcc_m_ahb = rd(GCC_QUPV3_WRAP1_M_AHB_CBCR);
	st->gcc_s_ahb = rd(GCC_QUPV3_WRAP1_S_AHB_CBCR);
	st->gcc_se6 = rd(GCC_QUPV3_WRAP1_SE6_CBCR);
}

static void
fts_hw_reset(void)
{
	gpio_set(PIN_RESET, false);
	sleep_ms(20);
	gpio_set(PIN_RESET, true);
	sleep_ms(100);
}

/* ---- STMicroelectronics FTS / Atmel Protocol --------------------------------------------------- */

#define STMFTS_CMD_READ_INFO            0x80
#define STMFTS_CMD_READ_ONE_EVENT       0x85
#define STMFTS_CMD_READ_ALL_EVENTS      0x86
#define STMFTS_CMD_SLEEP_OUT            0x91
#define STMFTS_CMD_SENSE_ON             0x93
#define STMFTS_CMD_SYSTEM_RESET         0xA0
#define STMFTS_CMD_CLEAR_STACK          0xA1

#define STMFTS_EV_NO_EVENT              0x00
#define STMFTS_EV_CONTROLLER_READY      0x03
#define STMFTS_EV_TOUCH_ENTER           0x03
#define STMFTS_EV_TOUCH_MOTION          0x05
#define STMFTS_EV_TOUCH_LEAVE           0x04

#include <console/video_console.h>

extern struct vc_info vinfo;
extern unsigned char iso_font[256 * 16];

#define STMFTS_EVENT_SIZE               8
#define STMFTS_MAX_EVENTS               32
#define STMFTS_MAX_FINGERS              10

static uint32_t max_x = 1080, max_y = 2248;
static uint32_t flip = 0;
static uint32_t fts_test_mode = 0;

#define GRID_COLS 10
#define GRID_ROWS 20
static uint8_t grid_touched[GRID_ROWS][GRID_COLS];

static inline void
fb_draw_pixel(uint32_t x, uint32_t y, uint32_t color)
{
	if (vinfo.v_baseaddr == 0 || vinfo.v_depth != 32) return;
	if (x >= vinfo.v_width || y >= vinfo.v_height) return;
	volatile uint32_t *p = (volatile uint32_t *)(vinfo.v_baseaddr + y * vinfo.v_rowbytes + x * 4);
	*p = color;
}

static void
fb_fill_rect(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t color)
{
	if (vinfo.v_baseaddr == 0 || vinfo.v_depth != 32) return;
	uint32_t x_end = (x0 + w > vinfo.v_width) ? vinfo.v_width : (x0 + w);
	uint32_t y_end = (y0 + h > vinfo.v_height) ? vinfo.v_height : (y0 + h);
	for (uint32_t y = y0; y < y_end; y++) {
		volatile uint32_t *row = (volatile uint32_t *)(vinfo.v_baseaddr + y * vinfo.v_rowbytes);
		for (uint32_t x = x0; x < x_end; x++) {
			row[x] = color;
		}
	}
}

static void
fb_draw_char(uint32_t x0, uint32_t y0, char c, uint32_t fg, uint32_t bg)
{
	unsigned char uc = (unsigned char)c;
	const unsigned char *glyph = iso_font + (uc * 16);
	for (int row = 0; row < 16; row++) {
		unsigned char bits = glyph[row];
		for (int col = 0; col < 8; col++) {
			if (bits & (1 << col)) {
				fb_draw_pixel(x0 + col, y0 + row, fg);
			} else if (bg != 0xffffffff) {
				fb_draw_pixel(x0 + col, y0 + row, bg);
			}
		}
	}
}

static void
fb_draw_char_2x(uint32_t x0, uint32_t y0, char c, uint32_t fg, uint32_t bg)
{
	unsigned char uc = (unsigned char)c;
	const unsigned char *glyph = iso_font + (uc * 16);
	for (int row = 0; row < 16; row++) {
		unsigned char bits = glyph[row];
		for (int col = 0; col < 8; col++) {
			uint32_t color = (bits & (1 << col)) ? fg : bg;
			if (color != 0xffffffff) {
				fb_draw_pixel(x0 + col * 2,     y0 + row * 2,     color);
				fb_draw_pixel(x0 + col * 2 + 1, y0 + row * 2,     color);
				fb_draw_pixel(x0 + col * 2,     y0 + row * 2 + 1, color);
				fb_draw_pixel(x0 + col * 2 + 1, y0 + row * 2 + 1, color);
			}
		}
	}
}

static void
fb_draw_string(uint32_t x0, uint32_t y0, const char *s, uint32_t fg, uint32_t bg)
{
	uint32_t cur_x = x0;
	while (*s) {
		fb_draw_char(cur_x, y0, *s, fg, bg);
		cur_x += 8;
		s++;
	}
}

static void
fb_draw_string_2x(uint32_t x0, uint32_t y0, const char *s, uint32_t fg, uint32_t bg)
{
	uint32_t cur_x = x0;
	while (*s) {
		fb_draw_char_2x(cur_x, y0, *s, fg, bg);
		cur_x += 16;
		s++;
	}
}

static void
fb_draw_circle(int cx, int cy, int radius, uint32_t color)
{
	for (int y = -radius; y <= radius; y++) {
		for (int x = -radius; x <= radius; x++) {
			if (x * x + y * y <= radius * radius) {
				int px = cx + x;
				int py = cy + y;
				if (px >= 0 && (uint32_t)px < vinfo.v_width &&
				    py >= 0 && (uint32_t)py < vinfo.v_height) {
					fb_draw_pixel((uint32_t)px, (uint32_t)py, color);
				}
			}
		}
	}
}

static void
fts_test_draw_diag(void)
{
	if (!fts_test_mode || vinfo.v_baseaddr == 0 || vinfo.v_depth != 32) return;

	char line1[80];
	char line2[80];
	char line3[80];
	char line4[80];
	char line5[80];

	stats.int_pin = gpio_get(PIN_INT) ? 1 : 0;
	stats.rst_pin = gpio_get(PIN_RESET) ? 1 : 0;
	stats.vddio_pin = gpio_get(PIN_VDDIO) ? 1 : 0;

	int touched_count = 0;
	for (int r = 3; r < GRID_ROWS; r++) {
		for (int c = 0; c < GRID_COLS; c++) {
			if (grid_touched[r][c]) touched_count++;
		}
	}

	snprintf(line1, sizeof(line1), "HAKIOS TOUCH: %s",
	    stats.online ? "ONLINE (0x49)" : "SEARCHING (0x49)...");
	snprintf(line2, sizeof(line2), "PINS: INT=%c RST=%c VDD=%c | POLL:%u PRB:%u",
	    stats.int_pin ? 'H' : 'L', stats.rst_pin ? 'H' : 'L', stats.vddio_pin ? 'H' : 'L',
	    stats.polls, stats.probe_count);
	snprintf(line3, sizeof(line3), "S:%u E:%08x IOS:%x ST:%x",
	    stats.stage, stats.err_irq, rd(SE_GENI_IOS), rd(GENI_STATUS));
	snprintf(line4, sizeof(line4), "SDA:%u SCL:%u BB:%u PR:%02x",
	    bb_sda_get() ? 1 : 0, bb_scl_get() ? 1 : 0, bb_ops, stats.probe_byte);
	snprintf(line5, sizeof(line5), "OK:%u FAIL:%u EV:%u T:%d/%d",
	    stats.rd_ok, stats.rd_fail, stats.events,
	    touched_count, (GRID_ROWS - 3) * GRID_COLS);

	/* Clear diagnostic header box below notch (y=150 to y=330) */
	fb_fill_rect(10, 150, vinfo.v_width - 20, 180, 0x00080808);
	fb_fill_rect(10, 150, vinfo.v_width - 20, 2, 0x00444444);
	fb_fill_rect(10, 328, vinfo.v_width - 20, 2, 0x00444444);

	uint32_t col_title = stats.online ? 0x0000ff00 : 0x00ff4444;
	fb_draw_string_2x(24, 154, line1, col_title, 0x00080808);
	fb_draw_string_2x(24, 188, line2, 0x00ffff00, 0x00080808);
	fb_draw_string_2x(24, 222, line3, 0x0000ffff, 0x00080808);
	fb_draw_string_2x(24, 256, line4, 0x00ffaa00, 0x00080808);
	fb_draw_string_2x(24, 290, line5, 0x00ffffff, 0x00080808);
}

static void
fts_test_grid_init(void)
{
	if (vinfo.v_baseaddr == 0 || vinfo.v_depth != 32) return;
	memset(grid_touched, 0, sizeof(grid_touched));

	fb_fill_rect(0, 0, vinfo.v_width, vinfo.v_height, 0x00101010);

	uint32_t col_w = vinfo.v_width / GRID_COLS;
	uint32_t row_h = vinfo.v_height / GRID_ROWS;

	/* Rows 3..19 are active touch test tiles */
	for (int r = 3; r < GRID_ROWS; r++) {
		for (int c = 0; c < GRID_COLS; c++) {
			uint32_t x = c * col_w;
			uint32_t y = r * row_h;
			fb_fill_rect(x + 1, y + 1, col_w - 2, row_h - 2, 0x00222222);
		}
	}

	fts_test_draw_diag();
}

static void
fts_test_on_touch(uint32_t raw_x, uint32_t raw_y, uint8_t slot, uint8_t ev_type)
{
	(void)ev_type;
	if (!fts_test_mode || vinfo.v_baseaddr == 0) return;

	uint32_t col_w = vinfo.v_width / GRID_COLS;
	uint32_t row_h = vinfo.v_height / GRID_ROWS;

	int c = raw_x / col_w;
	int r = raw_y / row_h;

	if (c >= 0 && c < GRID_COLS && r >= 3 && r < GRID_ROWS) {
		if (!grid_touched[r][c]) {
			grid_touched[r][c] = 1;
			uint32_t x = c * col_w;
			uint32_t y = r * row_h;
			fb_fill_rect(x + 1, y + 1, col_w - 2, row_h - 2, 0x0000bb33);
		}
	}

	uint32_t colors[5] = { 0x00ff0055, 0x0000ccff, 0x00ffcc00, 0x00ff00ff, 0x0000ffcc };
	uint32_t col = colors[slot % 5];
	fb_draw_circle((int)raw_x, (int)raw_y, 10, col);

	fts_test_draw_diag();
}

static bool
fts_probe_chip(uint8_t addr)
{
	uint8_t dummy = 0;
	bool ack = false;

	/* 1. Try reading 1 byte (slave address ACK check) */
	if (i2c_read(addr, &dummy, 1)) {
		ack = true;
		stats.probe_byte = dummy;
		printf("fts: probe 0x%02x: 1-byte read answered (0x%02x)\n", addr, dummy);
	} else {
		/* 2. Try reading FIFO event (0x86) */
		uint8_t ev[STMFTS_EVENT_SIZE] = { 0 };
		const uint8_t cmd_fifo = STMFTS_CMD_READ_ALL_EVENTS;
		if (i2c_write_read(addr, &cmd_fifo, 1, ev, sizeof(ev))) {
			ack = true;
			memcpy(stats.last_ev, ev, 8);
			printf("fts: probe 0x%02x: FIFO answered: [%02x %02x %02x %02x %02x %02x %02x %02x]\n",
			    addr, ev[0], ev[1], ev[2], ev[3], ev[4], ev[5], ev[6], ev[7]);
		} else {
			/* 3. Try writing scan mode active (0xA0 0x00 0x01) */
			const uint8_t cmd_scan[3] = { 0xA0, 0x00, 0x01 };
			if (i2c_write(addr, cmd_scan, sizeof(cmd_scan), false)) {
				ack = true;
				printf("fts: probe 0x%02x: ACKed scan mode write\n", addr);
			}
		}
	}

	if (ack) {
		fts_i2c_addr = addr;
		stats.online = true;
		return true;
	}

	return false;
}

static bool
fts_bringup(void)
{
	pins_init();

	if (!clocks_on()) {
		printf("fts: failed to enable clocks\n");
		return false;
	}

	if (!geni_i2c_init()) {
		printf("fts: failed to initialize GENI I2C\n");
		return false;
	}
	/* GENI data paths are hardware-disabled on this SE, so the bus
	 * runs bit-banged over the same pins (reconfigured as GPIOs). */
	bb_init();

	/* Reset chip */
	fts_hw_reset();

	bool found = false;
	const uint8_t addrs[] = { 0x49, 0x48, 0x4a };
	for (size_t a = 0; a < sizeof(addrs) / sizeof(addrs[0]) && !found; a++) {
		for (int i = 0; i < 3; i++) {
			if (fts_probe_chip(addrs[a])) {
				found = true;
				break;
			}
			sleep_ms(30);
			fts_hw_reset();
		}
	}

	if (!found) {
		printf("fts: touch controller did not answer initial probe (will keep searching)\n");
		return false;
	}

	/* Turn on multi-touch sensing: FTS521 SCAN_MODE_ACTIVE (0xA0 0x00 0x01) */
	const uint8_t cmd_scan_off[3] = { 0xA0, 0x00, 0x00 };
	i2c_write(fts_i2c_addr, cmd_scan_off, sizeof(cmd_scan_off), false);
	sleep_ms(20);

	const uint8_t cmd_scan_on[3] = { 0xA0, 0x00, 0x01 };
	i2c_write(fts_i2c_addr, cmd_scan_on, sizeof(cmd_scan_on), false);
	sleep_ms(20);

	printf("fts: touchscreen active at 0x%02x (scan mode 0xA0 0x00 0x01 sent), INT line is %s\n",
	    fts_i2c_addr, gpio_get(PIN_INT) ? "high" : "low");
	return true;
}

/* ---- touch report dispatch --------------------------------------------------------------------- */

static struct {
	bool down;
	bool seen;
	uint32_t x;
	uint32_t y;
} slots[STMFTS_MAX_FINGERS];

static void
fts_handle_contact(uint32_t id, uint32_t kind, uint32_t x, uint32_t y)
{
	if (id >= STMFTS_MAX_FINGERS) {
		return;
	}

	if (kind == STMFTS_EV_TOUCH_LEAVE) {
		slots[id].seen = true;
		if (slots[id].down) {
			slots[id].down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, id, slots[id].x, slots[id].y);
		}
		return;
	}

	if (max_x == 0 || max_y == 0) {
		return;
	}
	if (x >= max_x) {
		x = max_x - 1;
	}
	if (y >= max_y) {
		y = max_y - 1;
	}
	if (flip & 1) {
		x = max_x - 1 - x;
	}
	if (flip & 2) {
		y = max_y - 1 - y;
	}

	/* Normalize coordinates to [0, 65535] */
	uint32_t norm_x = (uint32_t)((uint64_t)x * 65535 / (max_x - 1));
	uint32_t norm_y = (uint32_t)((uint64_t)y * 65535 / (max_y - 1));

	slots[id].seen = true;
	stats.touches++;
	stats.last_x = norm_x;
	stats.last_y = norm_y;

	if (!slots[id].down) {
		slots[id].down = true;
		slots[id].x = norm_x;
		slots[id].y = norm_y;
		hakios_touch_post(HAKIOS_TOUCH_DOWN, id, norm_x, norm_y);
	} else if (slots[id].x != norm_x || slots[id].y != norm_y) {
		slots[id].x = norm_x;
		slots[id].y = norm_y;
		hakios_touch_post(HAKIOS_TOUCH_MOVE, id, norm_x, norm_y);
	}
}

static void
fts_poll_events(void)
{
	static uint32_t touch_log_count = 0;
	for (uint32_t i = 0; i < STMFTS_MAX_FINGERS; i++) {
		slots[i].seen = false;
	}

	const uint8_t cmd_fifo = STMFTS_CMD_READ_ALL_EVENTS;
	for (unsigned n = 0; n < STMFTS_MAX_EVENTS; n++) {
		uint8_t ev[STMFTS_EVENT_SIZE] = { 0 };

		lck_mtx_lock(&i2c_mutex);
		bool ok = i2c_write_read(fts_i2c_addr, &cmd_fifo, 1, ev, sizeof(ev));
		lck_mtx_unlock(&i2c_mutex);

		if (!ok) {
			stats.rd_fail++;
			break;
		}
		stats.rd_ok++;

		memcpy(stats.last_ev, ev, 8);

		if (ev[0] == 0x00) {
			break;
		}

		stats.events++;

		/* FTS521 event protocol (Xiaomi Mi 8) */
		if (ev[0] == 0x13 || ev[0] == 0x23 || ev[0] == 0x33) {
			uint8_t slot_id = (ev[1] >> 4) & 0x0f;
			uint32_t x = (((uint32_t)ev[3] & 0x0f) << 8) | (uint32_t)ev[2];
			uint32_t y = ((uint32_t)ev[4] << 4) | (((uint32_t)ev[3] >> 4) & 0x0f);

			if (touch_log_count < 30) {
				touch_log_count++;
				printf("fts: touch #%u: slot=%u kind=0x%02x (%u, %u)\n",
				    touch_log_count, slot_id, ev[0], x, y);
			}

			if (ev[0] == 0x33) {
				fts_handle_contact(slot_id, STMFTS_EV_TOUCH_LEAVE, 0, 0);
			} else {
				fts_handle_contact(slot_id, STMFTS_EV_TOUCH_ENTER, x, y);
				if (fts_test_mode) {
					uint32_t scr_x = (flip & 1) ? (max_x - 1 - x) : x;
					uint32_t scr_y = (flip & 2) ? (max_y - 1 - y) : y;
					fts_test_on_touch(scr_x, scr_y, slot_id, ev[0]);
				}
			}
		} else if (ev[0] == 0x03) {
			printf("fts: controller ready event (0x03) received, enabling scan mode!\n");
			const uint8_t cmd_scan_on[3] = { 0xA0, 0x00, 0x01 };
			lck_mtx_lock(&i2c_mutex);
			i2c_write(fts_i2c_addr, cmd_scan_on, sizeof(cmd_scan_on), false);
			lck_mtx_unlock(&i2c_mutex);
		} else if ((ev[0] & 0x0f) == 0x03 || (ev[0] & 0x0f) == 0x04 || (ev[0] & 0x0f) == 0x05) {
			/* Legacy STMFTS event protocol */
			uint8_t ev_id = ev[0] & 0x0f;
			uint8_t slot_id = (ev[0] >> 4) & 0x0f;
			uint32_t x = (uint32_t)ev[1] | (((uint32_t)ev[2] & 0x0f) << 8);
			uint32_t y = ((uint32_t)ev[2] >> 4) | ((uint32_t)ev[3] << 4);

			if (touch_log_count < 30) {
				touch_log_count++;
				printf("fts: legacy touch #%u: slot=%u kind=0x%02x (%u, %u)\n",
				    touch_log_count, slot_id, ev_id, x, y);
			}

			fts_handle_contact(slot_id, ev_id, x, y);
			if (fts_test_mode && ev_id != STMFTS_EV_TOUCH_LEAVE) {
				uint32_t scr_x = (flip & 1) ? (max_x - 1 - x) : x;
				uint32_t scr_y = (flip & 2) ? (max_y - 1 - y) : y;
				fts_test_on_touch(scr_x, scr_y, slot_id, ev[0]);
			}
		}
	}

	/* Lift any fingers not seen in this cycle */
	for (uint32_t id = 0; id < STMFTS_MAX_FINGERS; id++) {
		if (slots[id].down && !slots[id].seen) {
			slots[id].down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, id, slots[id].x, slots[id].y);
		}
	}
}

/* ---- character device (/dev/hakios-i2c) -------------------------------------------------------- */

static int
i2c_dev_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
i2c_dev_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
i2c_dev_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flags, __unused struct proc *p)
{
	int ret = 0;

	lck_mtx_lock(&i2c_mutex);

	switch (cmd) {
	case HAKIOS_I2C_IOC_XFER: {
		struct hakios_i2c_msg *m = (struct hakios_i2c_msg *)data;
		if (!m || m->addr > 0x7f) {
			ret = EINVAL;
			break;
		}

		if (m->write_len > 0 && m->read_len > 0) {
			/* Write followed by Read */
			if (!i2c_write_read((uint8_t)m->addr, m->write_buf, m->write_len, m->read_buf, m->read_len)) {
				ret = EIO;
			}
		} else if (m->write_len > 0) {
			/* Write only */
			bool hold = (m->flags & 2) != 0;
			if (!i2c_write((uint8_t)m->addr, m->write_buf, m->write_len, hold)) {
				ret = EIO;
			}
		} else if (m->read_len > 0) {
			/* Read only */
			if (!i2c_read((uint8_t)m->addr, m->read_buf, m->read_len)) {
				ret = EIO;
			}
		}
		break;
	}

	case HAKIOS_I2C_IOC_SCAN: {
		struct hakios_i2c_scan *scan = (struct hakios_i2c_scan *)data;
		i2c_scan_bus(scan);
		break;
	}

	case HAKIOS_I2C_IOC_RESET: {
		fts_hw_reset();
		printf("fts: hardware reset toggled via ioctl\n");
		break;
	}

	case HAKIOS_I2C_IOC_STATUS: {
		struct hakios_i2c_status *st = (struct hakios_i2c_status *)data;
		i2c_get_status(st);
		break;
	}

	case HAKIOS_I2C_IOC_SET_CLK: {
		uint32_t *clk_idx = (uint32_t *)data;
		if (*clk_idx > 7) {
			ret = EINVAL;
		} else {
			geni_i2c_apply_clock(*clk_idx, current_speed_khz);
		}
		break;
	}

	default:
		ret = ENOTTY;
		break;
	}

	lck_mtx_unlock(&i2c_mutex);
	return ret;
}

static const struct cdevsw i2c_cdevsw = {
	.d_open = i2c_dev_open,
	.d_close = i2c_dev_close,
	.d_read = eno_rdwrt,
	.d_write = eno_rdwrt,
	.d_ioctl = i2c_dev_ioctl,
	.d_stop = eno_stop,
	.d_reset = eno_reset,
	.d_select = eno_select,
	.d_mmap = eno_mmap,
	.d_strategy = eno_strat,
	.d_type = 0
};

static void
i2c_cdev_register(void)
{
	int major = cdevsw_add(-1, &i2c_cdevsw);
	if (major >= 0) {
		if (devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-i2c") != NULL) {
			printf("fts: created /dev/hakios-i2c (cdev major %d)\n", major);
			return;
		}
	}
	printf("fts: warning: could not register /dev/hakios-i2c\n");
}

/* ---- driver thread ----------------------------------------------------------------------------- */

static void
fts_thread(__unused void *param, __unused wait_result_t res)
{
	uint32_t poll_us = 8000;
	uint32_t do_scan = 0;
	uint32_t do_test = 0;
	uint32_t arg_clk = 2;
	uint32_t arg_speed = 100;
	uint32_t arg_pull = 0;
	uint32_t arg_addr = 0;
	uint32_t arg_touch_test = 0;

	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	PE_parse_boot_argn("touch_flip", &flip, sizeof(flip));
	PE_parse_boot_argn("touch_max_x", &max_x, sizeof(max_x));
	PE_parse_boot_argn("touch_max_y", &max_y, sizeof(max_y));
	PE_parse_boot_argn("touch_test", &arg_touch_test, sizeof(arg_touch_test));

	if (arg_touch_test) {
		fts_test_mode = 1;
	}

	PE_parse_boot_argn("i2c_scan", &do_scan, sizeof(do_scan));
	PE_parse_boot_argn("i2c_test", &do_test, sizeof(do_test));
	PE_parse_boot_argn("i2c_clk", &arg_clk, sizeof(arg_clk));
	PE_parse_boot_argn("i2c_speed", &arg_speed, sizeof(arg_speed));
	PE_parse_boot_argn("i2c_pull", &arg_pull, sizeof(arg_pull));
	PE_parse_boot_argn("i2c_addr", &arg_addr, sizeof(arg_addr));

	if (arg_clk <= 7) {
		current_clk_idx = arg_clk;
	}
	if (arg_speed == 400) {
		current_speed_khz = 400;
	}
	if (arg_pull) {
		pin_pull_mode = PULL_UP;
	}
	if (arg_addr) {
		fts_i2c_addr = arg_addr;
	}

	if (poll_us < 500 || poll_us > 50000) {
		poll_us = 8000;
	}

	/* Map small specific regions to never exhaust io_submap */
	tlmm_north = ml_io_map(TLMM_NORTH_PHYS, TLMM_NORTH_SIZE);
	tlmm_south = ml_io_map(TLMM_SOUTH_PHYS, TLMM_SOUTH_SIZE);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	se = ml_io_map(SE_PHYS, SE_SIZE);

	if (!tlmm_north || !tlmm_south || !gcc || !se) {
		printf("fts: failed to map MMIO regions (north=%p south=%p gcc=%p se=%p)\n",
		    (void *)tlmm_north, (void *)tlmm_south, (void *)gcc, (void *)se);
		return;
	}

	i2c_lck_grp = lck_grp_alloc_init("hakios-i2c", LCK_GRP_ATTR_NULL);
	lck_mtx_init(&i2c_mutex, i2c_lck_grp, LCK_ATTR_NULL);

	/* Initialize /dev/hakios-i2c first so userland/tests can always talk to hardware */
	i2c_cdev_register();

	bool online = fts_bringup();

	if (do_scan) {
		i2c_scan_bus(NULL);
	}

	if (!online && fts_probe_chip(0x49)) {
		printf("fts: slave 0x49 verified after scan, bringing touch online!\n");
		online = true;
	}

	stats.online = online;

	if (online) {
		/* FTS521 active scan mode: CMD 0xA0, mode 0x00 (SCAN_MODE_ACTIVE), settings 0x01 */
		const uint8_t cmd_scan_off[3] = { 0xA0, 0x00, 0x00 };
		i2c_write(fts_i2c_addr, cmd_scan_off, sizeof(cmd_scan_off), false);
		sleep_ms(20);

		const uint8_t cmd_scan_on[3] = { 0xA0, 0x00, 0x01 };
		i2c_write(fts_i2c_addr, cmd_scan_on, sizeof(cmd_scan_on), false);
		sleep_ms(20);

		printf("fts: touch active, FTS521 scan mode enabled, listening for events (INT=%s)\n",
		    gpio_get(PIN_INT) ? "high" : "low");
	}

	if (fts_test_mode) {
		fts_test_grid_init();
	}

	if (do_test) {
		struct hakios_i2c_status st;
		i2c_get_status(&st);
		printf("fts: === DIAGNOSTICS DUMP ===\n");
		printf("fts:   GENI_STATUS:     0x%08x\n", st.geni_status);
		printf("fts:   GENI_IOS:        0x%08x\n", st.geni_ios);
		printf("fts:   M_IRQ_STATUS:    0x%08x\n", st.m_irq_status);
		printf("fts:   SCL_COUNTERS:    0x%08x\n", st.scl_counters);
		printf("fts:   GPIOs: INT=%s RESET=%s VDDIO=%s SDA=%s SCL=%s\n",
		    st.pin_int ? "H" : "L", st.pin_reset ? "H" : "L",
		    st.pin_vddio ? "H" : "L", st.pin_sda ? "H" : "L", st.pin_scl ? "H" : "L");
		printf("fts:   CBCRs: M_AHB=0x%08x S_AHB=0x%08x SE6=0x%08x\n",
		    st.gcc_m_ahb, st.gcc_s_ahb, st.gcc_se6);
	}

	for (unsigned tick = 1;; tick++) {
		stats.polls++;
		if (!stats.online) {
			if (tick % 20 == 1) {
				stats.probe_count++;
				if (fts_probe_chip(0x49) || fts_probe_chip(0x48) || fts_probe_chip(0x4a)) {
					stats.online = true;
					const uint8_t cmd_scan_on[3] = { 0xA0, 0x00, 0x01 };
					lck_mtx_lock(&i2c_mutex);
					i2c_write(fts_i2c_addr, cmd_scan_on, sizeof(cmd_scan_on), false);
					lck_mtx_unlock(&i2c_mutex);
				}
			}
		} else if (!gpio_get(PIN_INT) || (tick % 125) == 0) {
			/* like the android driver: only drain the event FIFO when
			 * the chip pulls INT low (it holds it low while events are
			 * pending). The periodic poll is a fallback for a missed
			 * edge. Reading an empty FIFO on FTS521 can wedge it. */
			fts_poll_events();
		}

		if (fts_test_mode && (tick % 10 == 0)) {
			fts_test_draw_diag();
		}

		if (tick % (5 * (1000000 / poll_us)) == 0) {
			printf("fts: stats: %u events in %u polls, %u touches, INT=%s, online=%d\n",
			    stats.events, stats.polls, stats.touches,
			    gpio_get(PIN_INT) ? "high" : "low", stats.online ? 1 : 0);
		}

		sleep_us(poll_us);
	}
}

void
hakios_fts_init(void)
{
	uint32_t boot_arg = 0, dipper = 0;
	thread_t thread;

	PE_parse_boot_argn("touchscreen", &boot_arg, sizeof(boot_arg));
	PE_parse_boot_argn("dipper", &dipper, sizeof(dipper));

	/* dipper=1 or touchscreen=2 activates this driver */
	if (boot_arg != 2 && !dipper) {
		return;
	}

	kernel_thread_start(fts_thread, NULL, &thread);
	thread_deallocate(thread);
}
