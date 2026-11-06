/*
 * The phone's SPI2 (0x11012000), where its touch screen is: a Synaptics/OmniVision TCM
 * controller (Android drives it as omnivision_tcm_spi, see context.md). Registers follow Linux's
 * drivers/spi/spi-mt65xx.c for "mediatek,mt6765-spi", which is what the phone's own device tree
 * calls this controller.
 *
 * Polled, FIFO only, no DMA and no interrupt -- the same shape as the eMMC reader beside it.
 * Device memory with the MMU off: 32-bit accesses, nothing clever.
 */
#ifndef MTKSPI_H
#define MTKSPI_H

#include <stdint.h>

#define SPI2_BASE            0x11012000ULL
#define INFRACFG_BASE        0x10001000ULL
#define TOPCKGEN_BASE        0x10000000ULL

/*
 * Where the bit clock comes from (Linux clk-mt6768.c): the "spi_sel" mux is two bits at 24 in
 * CLK_CFG_2, with a power-down bit at 31, and each of these registers has a companion that clears
 * whatever bits you write to it. Source 0 is the 26 MHz crystal, which needs no PLL to be running
 * and is the one thing certain to be there this early.
 */
#define CLK_CFG_2_CLR        0x68
#define CLK_CFG_2_SET        0x64
#define CLK_CFG_2            0x60
#define SPI_SEL_FIELD        (3u << 24)
#define SPI_SEL_PDN          (1u << 31)

#define SPI_CFG0             0x00
#define SPI_CFG1             0x04
#define SPI_TX_SRC           0x08
#define SPI_RX_DST           0x0c
#define SPI_TX_DATA          0x10
#define SPI_RX_DATA          0x14
#define SPI_CMD              0x18
#define SPI_STATUS0          0x1c
#define SPI_STATUS1          0x20
#define SPI_PAD_SEL          0x24
#define SPI_CFG2             0x28

/*
 * The timing registers, in this chip's layout. Linux calls it "enhance_timing" and mt6765 (and so
 * mt6768) has it: the clock's high and low halves go in CFG2, sixteen bits each, while CFG0 holds
 * only the chip-select setup and hold. The older parts put all four in CFG0, eight bits each, and
 * using that layout here leaves the clock with no length at all -- the transfer still reports
 * itself done, and reads come back as nothing but the pull-ups.
 */
#define SPI_CFG0_CS_HOLD_SHIFT    0
#define SPI_CFG0_CS_SETUP_SHIFT   16
#define SPI_CFG2_SCK_HIGH_SHIFT   0
#define SPI_CFG2_SCK_LOW_SHIFT    16

#define SPI_CFG1_CS_IDLE_SHIFT    0
#define SPI_CFG1_PACKET_LOOP_SHIFT 8
#define SPI_CFG1_PACKET_LEN_SHIFT 16

#define SPI_CMD_ACT          (1u << 0)
#define SPI_CMD_RESUME       (1u << 1)
#define SPI_CMD_RST          (1u << 2)
#define SPI_CMD_PAUSE_EN     (1u << 4)
#define SPI_CMD_DEASSERT     (1u << 5)
#define SPI_CMD_SAMPLE_SEL   (1u << 6)
#define SPI_CMD_CS_POL       (1u << 7)
#define SPI_CMD_CPHA         (1u << 8)
#define SPI_CMD_CPOL         (1u << 9)
#define SPI_CMD_TXMSBF       (1u << 12)
#define SPI_CMD_RXMSBF       (1u << 13)
#define SPI_CMD_RX_ENDIAN    (1u << 14)
#define SPI_CMD_TX_ENDIAN    (1u << 15)

#define SPI_STATUS0_DONE     (1u << 0)

#define SPI_FIFO_BYTES       32

/*
 * The pins. On this chip SPI2 is GPIO25 (CSB), 26 (MO), 27 (CLK) and 28 (MI), each at function 2
 * -- from the MT6768 pin table, not the MT6765 one it is so often confused with, where those
 * functions sit on GPIO17-20 instead. A pin's function is four bits in GPIO_MODE<n> at
 * 0x10005300 + 0x10 per eight pins, so all four live in GPIO_MODE3.
 *
 * The preloader already sets them, which is why the phone's device tree carries no pinctrl state
 * for them -- Linux never has to. We set them anyway: it costs nothing and says what we rely on.
 */
#define GPIO_BASE            0x10005000ULL
#define GPIO_MODE3           0x330
#define SPI2_PIN_FUNC        2
#define SPI2_PIN_CS          25
#define SPI2_PIN_MO          26
#define SPI2_PIN_CLK         27
#define SPI2_PIN_MI          28

static void spi_pins_to_spi2(void)
{
	uint32_t mode = rd32(GPIO_BASE + GPIO_MODE3);

	for (int pin = SPI2_PIN_CS; pin <= SPI2_PIN_MI; pin++) {
		uint32_t shift = (uint32_t)(pin % 8) * 4;

		mode = (mode & ~(0xfu << shift)) | ((uint32_t)SPI2_PIN_FUNC << shift);
	}
	wr32(GPIO_BASE + GPIO_MODE3, mode);
	__asm__ volatile("dsb sy");
}

/*
 * The bus itself. Each field counts ticks of the source clock and is stored one less than it
 * counts. Ten ticks a half-period divides the source by twenty, which keeps us well under the
 * 8 MHz the chip's own device tree asks for.
 */
#define SPI_TICKS 2                             /* 26 MHz over four: 6.5, under the 8 the chip allows */

static void spi_setup(void)
{
	wr32(TOPCKGEN_BASE + CLK_CFG_2_CLR, SPI_SEL_FIELD | SPI_SEL_PDN);
	__asm__ volatile("dsb sy");

	wr32(SPI2_BASE + SPI_CMD, SPI_CMD_RST);         /* whatever LK left behind */
	wr32(SPI2_BASE + SPI_CMD, 0);
	wr32(SPI2_BASE + SPI_CFG2, ((SPI_TICKS - 1) << SPI_CFG2_SCK_HIGH_SHIFT) |
	    ((SPI_TICKS - 1) << SPI_CFG2_SCK_LOW_SHIFT));
	wr32(SPI2_BASE + SPI_CFG0, ((SPI_TICKS - 1) << SPI_CFG0_CS_HOLD_SHIFT) |
	    ((SPI_TICKS - 1) << SPI_CFG0_CS_SETUP_SHIFT));
	wr32(SPI2_BASE + SPI_PAD_SEL, 0);               /* the device tree says pad group 0 */
}

/*
 * The same four pins driven by hand. The SPI controller has many ways to be set up wrong -- the
 * clock mux, the sampling edge, the FIFO's byte order -- and this has none of them: it is the
 * wire protocol and nothing else, slow but beyond doubt. If the chip answers here and not through
 * the controller, the controller is misconfigured; if it answers neither way, the chip is not
 * listening at all.
 */
#define GPIO_DOUT0_SET       0x104
#define GPIO_DOUT0_CLR       0x108
#define GPIO_DIR0_SET        0x004
#define GPIO_DIR0_CLR        0x008
#define GPIO_DIN0            0x200
static inline void pin_set(uint32_t pin, int high)
{
	wr32(GPIO_BASE + (high ? GPIO_DOUT0_SET : GPIO_DOUT0_CLR), 1u << pin);
}

static inline int pin_get(uint32_t pin)
{
	return (rd32(GPIO_BASE + GPIO_DIN0) >> pin) & 1u;
}

/* Long enough for the pad to settle; a register read alone is already most of a microsecond. */
static inline void pin_settle(void)
{
	(void)rd32(GPIO_BASE + GPIO_DIN0);
	(void)rd32(GPIO_BASE + GPIO_DIN0);
}

static void spi_pins_to_gpio(void)
{
	uint32_t mode = rd32(GPIO_BASE + GPIO_MODE3);

	for (int pin = SPI2_PIN_CS; pin <= SPI2_PIN_MI; pin++) {
		mode &= ~(0xfu << ((uint32_t)(pin % 8) * 4));
	}
	wr32(GPIO_BASE + GPIO_MODE3, mode);
	/* three of them we drive, the chip drives the fourth */
	wr32(GPIO_BASE + GPIO_DIR0_SET, (1u << SPI2_PIN_MO) | (1u << SPI2_PIN_CLK) | (1u << SPI2_PIN_CS));
	wr32(GPIO_BASE + GPIO_DIR0_CLR, 1u << SPI2_PIN_MI);
	pin_set(SPI2_PIN_CS, 1);                        /* chip select idles high */
	pin_set(SPI2_PIN_CLK, 1);                       /* and in mode 3 so does the clock */
	__asm__ volatile("dsb sy");
	pin_settle();
}

/*
 * `len` bytes each way, MSB first, in mode 3: the clock idles high, the data changes on the
 * falling edge and is read on the rising one.
 *
 * `byte_gap_us` is a pause left after each byte. The chip's ROM bootloader asks for twenty
 * microseconds there (omnivision,ubl-byte-delay-us in the phone's device tree) and will not
 * follow a stream sent any faster -- which is why the firmware goes out this way and not through
 * the SPI controller, whose bytes come back to back with nothing between them.
 */
static void spi_bitbang_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len, uint32_t byte_gap_us)
{
	int idle = 1;

	pin_set(SPI2_PIN_CLK, idle);
	pin_set(SPI2_PIN_CS, 0);
	pin_settle();

	for (uint32_t i = 0; i < len; i++) {
		uint8_t out_byte = tx ? tx[i] : 0xff, in_byte = 0;

		for (int bit = 7; bit >= 0; bit--) {
			pin_set(SPI2_PIN_MO, (out_byte >> bit) & 1);
			pin_set(SPI2_PIN_CLK, !idle);           /* the edge that moves the data */
			pin_settle();
			pin_set(SPI2_PIN_CLK, idle);            /* the edge that reads it */
			pin_settle();
			in_byte = (uint8_t)((in_byte << 1) | pin_get(SPI2_PIN_MI));
		}
		if (rx) {
			rx[i] = in_byte;
		}
		if (byte_gap_us) {
			dbg_delay_us(byte_gap_us);
		}
	}
	pin_set(SPI2_PIN_CS, 1);
	pin_settle();
}

/*
 * The chip's own reset, GPIO0, active low (context.md). Only for a probe that has given up on
 * reading: a reset throws away the firmware Android downloaded, and the chip comes back up in its
 * bootloader, which still answers -- it just cannot report touches until something reloads it.
 */
__attribute__((unused)) static void touch_reset(void)
{
	wr32(GPIO_BASE + GPIO_DOUT0_CLR, 1u);           /* hold it low */
	__asm__ volatile("dsb sy");
	dbg_delay_ms(20);
	wr32(GPIO_BASE + GPIO_DOUT0_SET, 1u);           /* let it go */
	__asm__ volatile("dsb sy");
	dbg_delay_ms(200);
}

/*
 * One transfer: `len` bytes out of tx (zeros where tx is NULL) and the same number in, into rx.
 * The chip select is asserted for the whole of it, which is what a TCM exchange wants. At most
 * SPI_FIFO_BYTES at a time -- the FIFO is all this uses.
 */
static int spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	uint32_t words = (len + 3) / 4;

	if (len == 0 || len > SPI_FIFO_BYTES) {
		return 0;
	}
	spi_setup();                                    /* the last transfer left its FIFO behind */
	/*
	 * Mode 3 (CPOL = CPHA = 1), MSB first, chip select active low. The endian bits stay clear:
	 * Linux sets them only on a big-endian host, and with them on the bytes of every word come
	 * back in the wrong order.
	 */
	wr32(SPI2_BASE + SPI_CMD, SPI_CMD_CPHA | SPI_CMD_CPOL | SPI_CMD_TXMSBF | SPI_CMD_RXMSBF);
	wr32(SPI2_BASE + SPI_CFG1, ((len - 1) << SPI_CFG1_PACKET_LEN_SHIFT) |
	    (0 << SPI_CFG1_PACKET_LOOP_SHIFT) | ((SPI_TICKS - 1) << SPI_CFG1_CS_IDLE_SHIFT));

	for (uint32_t w = 0; w < words; w++) {
		uint32_t v = 0;

		for (uint32_t b = 0; b < 4; b++) {
			uint32_t i = w * 4 + b;
			v |= (uint32_t)(tx && i < len ? tx[i] : 0xff) << (8 * b);
		}
		wr32(SPI2_BASE + SPI_TX_DATA, v);
	}
	wr32(SPI2_BASE + SPI_CMD, rd32(SPI2_BASE + SPI_CMD) | SPI_CMD_ACT);

	for (uint32_t spin = 0; ; spin++) {
		if (rd32(SPI2_BASE + SPI_STATUS0) & SPI_STATUS0_DONE) {
			break;
		}
		if (spin > 2000000) {
			return 0;
		}
	}
	for (uint32_t w = 0; w < words; w++) {
		uint32_t v = rd32(SPI2_BASE + SPI_RX_DATA);

		for (uint32_t b = 0; b < 4; b++) {
			uint32_t i = w * 4 + b;

			if (rx && i < len) {
				rx[i] = (uint8_t)(v >> (8 * b));
			}
		}
	}
	return 1;
}

#endif /* MTKSPI_H */
