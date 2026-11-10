/*
 * Camera probe, stage 0 (tools/cam-probe.sh): read only. Nothing of the camera is switched on.
 *
 * What the camera needs on this phone was read off Android's own bring-up (docs/camera-x6826b.md):
 * the PMIC rails, the GPIO pins, the clock gates. What is not known yet, and what this prints so it
 * can be known without guessing: which pins carry the I2C buses (the GPIO mode registers say),
 * and the state of the I2C controller (its registers, with its clocks open). The only writes are
 * the two that open the I2C controller's clocks -- a write to the "clear" register of a gate, which
 * only ever allows a clock -- and they are put back the way they were before the probe ends.
 */
#ifndef MTKCAM_H
#define MTKCAM_H

#include "mtkpwrap.h"

/* the S5K JN1's register table: made by tools/camtable.py from a capture, never in git (it is the vendor's) */
#if defined(__has_include)
#if __has_include("camjn1.h")
#include "camjn1.h"
#define CAM_HAVE_JN1 1
#endif
#endif

#ifndef CAM_RX
#define CAM_RX 0
#endif
/* the receiver's registers as Android's HAL set them: made by tools/camrx.py, never in git (vendor's tuning) */
#if CAM_RX && defined(__has_include)
#if __has_include("camrx.h")
#include "camrx.h"
#define CAM_HAVE_RX 1
#endif
#endif

#define CAM_I2C4_BASE           0x11011000ULL   /* main camera's bus (Android: i2c-4) */
#define CAM_I2C2_BASE           0x11009000ULL   /* second camera's bus (Android: i2c-2) */

/* what Android writes to open the I2C clocks (docs/camera-x6826b.md): a bit in a "clear" register */
#define CAM_CFG6                0xa0            /* CLK_CFG_6: bit 7 is i2c_sel's power-down bit */
#define CAM_CFG6_SET            0xa4
#define CAM_CFG6_CLR            0xa8
#define CAM_CFG6_I2C_PDN        0x80u
#define CAM_INFRA_PDN_SET0      0x080           /* infra: ifr_i2c_ap is bit 11 */
#define CAM_INFRA_PDN_CLR0      0x084
#define CAM_INFRA_PDN_STA0      0x090
#define CAM_INFRA_I2C_AP        0x800u

static void cam_pmic(const char *name, uint32_t reg)
{
	uint16_t v = 0;

	out("  ");
	out(name);
	out(" [");
	out_hex(reg);
	out("] = ");
	if (pwrap_read(reg, &v)) {
		out_hex(v);
	} else {
		out("no answer");
	}
	out("\n");
}

/* one pin: mode (four bits in GPIO_MODEn at 0x300 + 0x10 per eight pins), direction, output, input */
static void cam_pin(const char *name, uint32_t pin)
{
	uint32_t mode = (rd32(GPIO_BASE + 0x300 + 0x10 * (pin / 8)) >> (4 * (pin % 8))) & 0xfu;
	uint32_t dir = (rd32(GPIO_BASE + 0x000 + 0x10 * (pin / 32)) >> (pin % 32)) & 1u;
	uint32_t dout = (rd32(GPIO_BASE + 0x100 + 0x10 * (pin / 32)) >> (pin % 32)) & 1u;
	uint32_t din = (rd32(GPIO_BASE + 0x200 + 0x10 * (pin / 32)) >> (pin % 32)) & 1u;

	out("  GPIO");
	out_dec(pin);
	out(" ");
	out(name);
	out(": mode ");
	out_dec(mode);
	out(dir ? ", output " : ", input, out ");
	out_dec(dout);
	out(", reads ");
	out_dec(din);
	out("\n");
}

static void cam_i2c_dump(const char *name, uint64_t base)
{
	out(name);
	out(" registers (the data port at 0 is not read: it is the FIFO)\n");
	for (uint32_t off = 0x04; off <= 0x8c; off += 4) {
		out("  +");
		out_hex(off);
		out(" = ");
		out_hex(rd32(base + off));
		out("\n");
	}
}

#ifndef CAM_POWER
#define CAM_POWER 0
#endif

#if CAM_POWER
/*
 * Stage 1: switch the main camera on in the order Android does (docs/camera-x6826b.md), read the
 * sensor's ID over I2C, and switch it all off again. Every step is named before it is taken; whatever
 * a check does not like stops the sequence and the cleanup runs from there. VCAMD's voltage is the
 * one that matters: LK leaves its register at 0x0600, Android writes 0x0500 (1.1 V) before it enables
 * the rail, so that is what is written here, read back, and a rail that does not read back is not enabled.
 */
#define CAM_VCAMIO_EN           0x1cb0
#define CAM_VCAMD_EN            0x1c9c
#define CAM_VCAMD_VOSEL         0x1eae
#define CAM_VCAMD_1V1           0x0500

#define CAM_CFG1                0x50
#define CAM_CFG1_SET            0x54
#define CAM_CFG1_CLR            0x58
#define CAM_CFG1_CAMTG_PDN      0x00800000u     /* camtg_sel's power-down bit */
#define CAM_CFG1_CAMTG1_PDN     0x80000000u     /* camtg1_sel's */

#define CAM_PIN_AVDD            23              /* external analog supply, both cameras */
#define CAM_PIN_PDN             98
#define CAM_PIN_RST             102
#define CAM_PIN_MCLK            100             /* function 1 is the clock */

#define CAM_I2C_SLAVE_ADDR      0x04
#define CAM_I2C_INTR_STAT       0x0c
#define CAM_I2C_CONTROL         0x10
#define CAM_I2C_TRANSFER_LEN    0x14
#define CAM_I2C_TRANSAC_LEN     0x18
#define CAM_I2C_START           0x24
#define CAM_I2C_FIFO_ADDR_CLR   0x38
#define CAM_I2C_TRANSFER_AUX    0x6c
#define CAM_I2C_COMP            0x01u
#define CAM_I2C_ACKERR          0x02u
#define CAM_I2C_NACKERR         0x04u
#define CAM_I2C_CTL_RS          0x02u
#define CAM_I2C_CTL_CLK_EXT     0x08u
#define CAM_I2C_CTL_DIR_CHANGE  0x10u
#define CAM_I2C_CTL_ACKERR_DET  0x20u

struct cam_pin_state {
	uint32_t pin, mode, dir, dout;
};

static void cam_pin_save(struct cam_pin_state *s, uint32_t pin)
{
	s->pin = pin;
	s->mode = (rd32(GPIO_BASE + 0x300 + 0x10 * (pin / 8)) >> (4 * (pin % 8))) & 0xfu;
	s->dir = (rd32(GPIO_BASE + 0x000 + 0x10 * (pin / 32)) >> (pin % 32)) & 1u;
	s->dout = (rd32(GPIO_BASE + 0x100 + 0x10 * (pin / 32)) >> (pin % 32)) & 1u;
}

static void cam_pin_mode(uint32_t pin, uint32_t mode)
{
	uint64_t a = GPIO_BASE + 0x300 + 0x10 * (uint64_t)(pin / 8);
	uint32_t shift = 4 * (pin % 8);

	wr32(a, (rd32(a) & ~(0xfu << shift)) | (mode << shift));
}

/* output: the level first, then the direction, so the pin never drives the wrong way round */
static void cam_pin_out(uint32_t pin, int level)
{
	uint32_t bank = pin / 32, bit = 1u << (pin % 32);

	wr32(GPIO_BASE + (level ? 0x104 : 0x108) + 0x10 * bank, bit);
	wr32(GPIO_BASE + 0x004 + 0x10 * bank, bit);
}

static void cam_pin_restore(const struct cam_pin_state *s)
{
	uint32_t bank = s->pin / 32, bit = 1u << (s->pin % 32);

	wr32(GPIO_BASE + (s->dout ? 0x104 : 0x108) + 0x10 * bank, bit);
	wr32(GPIO_BASE + (s->dir ? 0x004 : 0x008) + 0x10 * bank, bit);
	cam_pin_mode(s->pin, s->mode);
}

static int cam_pin_read(uint32_t pin)
{
	return (rd32(GPIO_BASE + 0x200 + 0x10 * (pin / 32)) >> (pin % 32)) & 1u;
}

/* bit 0 of a rail's enable register, then read back: 1 if it holds */
static int cam_rail(uint32_t reg, int on)
{
	uint16_t v = 0;

	if (!pwrap_read(reg, &v)) {
		return 0;
	}
	pwrap_write(reg, on ? (v | 1u) : (v & ~1u));
	dbg_delay_us(200);
	if (!pwrap_read(reg, &v)) {
		return 0;
	}
	return (v & 1u) == (on ? 1u : 0u);
}

/*
 * I2C by hand on the camera bus's own pins (the device tree says SCL is GPIO105 and SDA GPIO106 on i2c4, both
 * already muxed to the bus). A line is let go (an input, the board's pull-up takes it high) or pulled low
 * (an output at 0): the way the loader already does SPI for the touch screen. It needs none of the
 * controller's registers, clocks or DMA, and what it answers is exactly what matters: does the sensor
 * acknowledge, and what is its ID.
 */
#define CAM_BB_SCL              105
#define CAM_BB_SDA              106

static inline void cam_bb_delay(void) { dbg_delay_us(5); }

static void cam_bb_line(uint32_t pin, int high)
{
	uint32_t bank = pin / 32, bit = 1u << (pin % 32);

	if (high) {
		wr32(GPIO_BASE + 0x008 + 0x10 * bank, bit);     /* input */
	} else {
		wr32(GPIO_BASE + 0x108 + 0x10 * bank, bit);     /* output 0 ... */
		wr32(GPIO_BASE + 0x004 + 0x10 * bank, bit);     /* ... then drive it */
	}
}

static void cam_bb_scl(int high)
{
	cam_bb_line(CAM_BB_SCL, high);
	if (high) {
		for (int i = 0; i < 200 && !cam_pin_read(CAM_BB_SCL); i++) {    /* a slave may hold it low */
			dbg_delay_us(5);
		}
	}
	cam_bb_delay();
}

static void cam_bb_sda(int high)
{
	cam_bb_line(CAM_BB_SDA, high);
	cam_bb_delay();
}

static void cam_bb_start(void)
{
	cam_bb_sda(1);
	cam_bb_scl(1);
	cam_bb_sda(0);
	cam_bb_scl(0);
}

static void cam_bb_stop(void)
{
	cam_bb_sda(0);
	cam_bb_scl(1);
	cam_bb_sda(1);
}

/* one byte out; 1 if the slave pulled SDA low for the ninth clock (an acknowledge) */
static int cam_bb_write(uint8_t byte)
{
	int ack;

	for (int i = 7; i >= 0; i--) {
		cam_bb_sda((byte >> i) & 1);
		cam_bb_scl(1);
		cam_bb_scl(0);
	}
	cam_bb_sda(1);
	cam_bb_scl(1);
	ack = !cam_pin_read(CAM_BB_SDA);
	cam_bb_scl(0);
	return ack;
}

static uint8_t cam_bb_read(int ack)
{
	uint8_t byte = 0;

	cam_bb_sda(1);
	for (int i = 0; i < 8; i++) {
		cam_bb_scl(1);
		byte = (uint8_t)((byte << 1) | (cam_pin_read(CAM_BB_SDA) ? 1 : 0));
		cam_bb_scl(0);
	}
	cam_bb_sda(!ack);
	cam_bb_scl(1);
	cam_bb_scl(0);
	cam_bb_sda(1);
	return byte;
}

/* write the 16-bit register address, then read one byte; returns a bit per acknowledge missed (0 = all answered) */
static int cam_bb_read8(uint32_t addr7, uint16_t reg, uint8_t *byte)
{
	int missed = 0;

	cam_bb_start();
	missed |= !cam_bb_write((uint8_t)(addr7 << 1)) << 0;
	missed |= !cam_bb_write((uint8_t)(reg >> 8)) << 1;
	missed |= !cam_bb_write((uint8_t)(reg & 0xffu)) << 2;
	cam_bb_start();                                 /* a repeated start */
	missed |= !cam_bb_write((uint8_t)((addr7 << 1) | 1)) << 3;
	*byte = cam_bb_read(0);
	cam_bb_stop();
	return missed;
}

#ifdef CAM_HAVE_RX
/*
 * The receiver. cam_rx_program() writes the table; cam_rx_snap() reads SENINF's first five pages (the blocks of the four
 * receivers and the PHYs) into a buffer; cam_rx_report() reads them again and prints what changed since: with a sensor
 * streaming into a programmed receiver, registers that were still at the values written move, and that is the first sign
 * that anything arrives.
 */
#define CAM_RXS_BASE            0x1a040000ULL
#define CAM_RXS_WORDS           (0x5000 / 4)

static uint32_t cam_rx_before[CAM_RXS_WORDS];

static void cam_rx_snap(uint32_t *buf)
{
	for (uint32_t i = 0; i < CAM_RXS_WORDS; i++) {
		buf[i] = rd32(CAM_RXS_BASE + 4 * i);
	}
}

#define CAM_RXSV_BASE           0x1a051000ULL   /* CAMSV_1 */
#define CAM_RXSV_WORDS          (0x1000 / 4)
#ifdef CAM_RX_MAIN
/* the main image: 4080x3072 RAW10 packed = 5100 bytes a line, 15.7 MB a frame; two of them, then the two headers */
#define CAM_RX_BUF_SIZE         0x02200000ULL
#define CAM_RX_IMG_LEN          0x01000000ULL
#define CAM_RX_HDR_LEN          0x00100000ULL
#define CAM_RX_IMG_OFF0         0x00000000ULL
#define CAM_RX_IMG_OFF1         0x01000000ULL
#define CAM_RX_HDR_OFF0         0x02000000ULL
#define CAM_RX_HDR_OFF1         0x02100000ULL
#else
#define CAM_RX_BUF_SIZE         0x00800000ULL   /* cleared and checked: the three areas the table points the DMA at */
#define CAM_RX_IMG_LEN          0x00200000ULL
#define CAM_RX_HDR_LEN          0x00100000ULL
#define CAM_RX_IMG_OFF0         0x00000000ULL
#define CAM_RX_IMG_OFF1         0x00500000ULL
#define CAM_RX_HDR_OFF0         0x00200000ULL
#define CAM_RX_HDR_OFF1         0x00700000ULL
#endif

static uint32_t cam_rxsv_before[CAM_RXSV_WORDS];

static void cam_rx_buf_clear(void)
{
	for (uint64_t a = CAM_RX_BUF; a < CAM_RX_BUF + CAM_RX_BUF_SIZE; a += 8) {
		*(volatile uint64_t *)a = 0;
	}
	__asm__ volatile("dsb sy");
}

static void cam_rx_program(void)
{
	out("clearing the frame buffer at ");
	out_hex(CAM_RX_BUF);
	out(", ");
	out_hex(CAM_RX_BUF_SIZE);
	out(" bytes\n");
	cam_rx_buf_clear();
	/*
	 * The camera's SMI ports come out of reset with MMU_EN set (smi_larb3 +0x380.. = 1): the DMA's addresses are
	 * IOVAs, translated by the IOMMU, which has no tables here. Clear it and the DMA writes to physical addresses.
	 */
	out("smi_larb3: MMU_EN off for the camera's ports (they were ");
	out_hex(rd32(0x1a002000ULL + 0x380));
	out(")\n");
	for (int port = 0; port < 21; port++) {
		wr32(0x1a002000ULL + 0x380 + 4 * (uint64_t)port, 0);
	}
	__asm__ volatile("dsb sy");
	out("programming the receiver: ");
	out_dec(CAM_RX_N);
	out(" registers, then CAMSV_1: ");
	out_dec(CAM_RX_SV_N);
	out("\n");
	for (int i = 0; i < CAM_RX_N; i++) {
		wr32(cam_rx_table[i].addr, cam_rx_table[i].val);
		__asm__ volatile("dsb sy");
	}
	/*
	 * The CAMSV_1 table holds everything but the queue's own registers. The queue is filled as the HAL does it
	 * (CAMSV_BUF_CTRL::enqueueHwBuf): the data address into +0x118, the header's into +0xe04, THEN bit 0 of +0x2c, the
	 * "buffer ready" pulse. The first version wrote the pulse before the addresses (the table is in address order) and the
	 * queue got a buffer at address 0. The table's last two registers (+0x500, +0x504: the sensor interface and the
	 * view-finder) come after, so what is queued is there when frames start.
	 */
	for (int i = 0; i < CAM_RX_SV_N; i++) {
		if (cam_rx_sv_table[i].addr == CAM_RXSV_BASE + 0x500 || cam_rx_sv_table[i].addr == CAM_RXSV_BASE + 0x504) {
			continue;
		}
		wr32(cam_rx_sv_table[i].addr, cam_rx_sv_table[i].val);
		__asm__ volatile("dsb sy");
	}
#ifdef CAM_RX_MAIN
	/*
	 * The main image (VC0, DT 0x2b, 4080x3072) instead of the phase-detect channel. The live SENINF shows two muxes in use:
	 * mux0 measures 4080x3072 (the image, to the ISP's CAM_A) and mux3 635x3056 (the PD data, to CAMSV_1). A mux's source is
	 * bits 15:12 of its first register (SeninfDrvImp::setSeninfMuxCtrl): mux0 has 8 (virtual channel 0), mux3 has 0xa
	 * (channel 2). Point mux3 at channel 0 and give CAMSV_1 the image's geometry; the DMA is told 5100 bytes a line
	 * (4080 pixels * 10 bits) and 3072 lines. The stride register keeps its other fields (0x0107....).
	 */
	out("MAIN: mux3 source 0xa -> 0x8, CAMSV_1 grab 4080x3072, DMA 5100 bytes x 3072\n");
	wr32(0x1a040000ULL + 0x3d00, (rd32(0x1a040000ULL + 0x3d00) & ~0xf000u) | 0x8000u);
	wr32(CAM_RXSV_BASE + 0x508, 0x0ff00000);
	wr32(CAM_RXSV_BASE + 0x50c, 0x0c000000);
	wr32(CAM_RXSV_BASE + 0x230, 5100 - 1);
	wr32(CAM_RXSV_BASE + 0x234, 3072 - 1);
	wr32(CAM_RXSV_BASE + 0x238, (rd32(CAM_RXSV_BASE + 0x238) & 0xffff0000u) | 5100u);
	__asm__ volatile("dsb sy");
#endif
	for (int k = 0; k < 2; k++) {
		static const uint64_t img[2] = { CAM_RX_IMG_OFF0, CAM_RX_IMG_OFF1 }, hdr[2] = { CAM_RX_HDR_OFF0, CAM_RX_HDR_OFF1 };

		wr32(CAM_RXSV_BASE + 0x118, (uint32_t)(CAM_RX_BUF + img[k]));
		wr32(CAM_RXSV_BASE + 0xe04, (uint32_t)(CAM_RX_BUF + hdr[k]));
		__asm__ volatile("dsb sy");
		wr32(CAM_RXSV_BASE + 0x2c, rd32(CAM_RXSV_BASE + 0x2c) | 1u);
		__asm__ volatile("dsb sy");
		dbg_delay_ms(1);
	}
	for (int i = 0; i < CAM_RX_SV_N; i++) {
		if (cam_rx_sv_table[i].addr == CAM_RXSV_BASE + 0x500 || cam_rx_sv_table[i].addr == CAM_RXSV_BASE + 0x504) {
			wr32(cam_rx_sv_table[i].addr, cam_rx_sv_table[i].val);
			__asm__ volatile("dsb sy");
		}
	}
	dbg_delay_ms(2);
	cam_rx_snap(cam_rx_before);
	for (uint32_t i = 0; i < CAM_RXSV_WORDS; i++) {
		cam_rxsv_before[i] = rd32(CAM_RXSV_BASE + 4 * i);
	}
}

#ifdef CAM_RX_MAIN
/*
 * What a buffer holds, line by line (pitch bytes a line, lines lines): how many lines have any non-zero byte, where the
 * last written byte is, the byte min/max/mean over the first 64 lines, and a few lines' first bytes. Packed RAW10 is
 * 4 pixels in 5 bytes: bytes 0..3 the high 8 bits of each pixel, byte 4 the low 2 bits of all four.
 */
static void cam_rx_stats(uint64_t base, uint32_t pitch, uint32_t lines)
{
	uint32_t written = 0, first = 0xffffffffu, last = 0;
	uint32_t mn = 255, mx = 0;
	uint64_t sum = 0, cnt = 0;

	for (uint32_t y = 0; y < lines; y++) {
		const volatile uint8_t *p = (const volatile uint8_t *)(base + (uint64_t)y * pitch);
		uint32_t any = 0;

		for (uint32_t x = 0; x < pitch; x += 16) {
			if (p[x] | p[x + 1] | p[x + 2] | p[x + 3]) {
				any = 1;
				break;
			}
		}
		if (any) {
			written++;
			if (first == 0xffffffffu) {
				first = y;
			}
			last = y;
		}
	}
	for (uint32_t y = 0; y < 64 && y < lines; y++) {
		const volatile uint8_t *p = (const volatile uint8_t *)(base + (uint64_t)y * pitch);

		for (uint32_t x = 0; x < pitch; x++) {
			if (x % 5 == 4) {
				continue;   /* the low-bits byte */
			}
			uint32_t v = p[x];

			if (v < mn) {
				mn = v;
			}
			if (v > mx) {
				mx = v;
			}
			sum += v;
			cnt++;
		}
	}
	out("image ");
	out_hex(base);
	out(": ");
	out_dec(written);
	out(" of ");
	out_dec(lines);
	out(" lines written (first ");
	out_dec(first == 0xffffffffu ? 0 : first);
	out(", last ");
	out_dec(last);
	out("); high bytes of lines 0..63: min ");
	out_dec(mn);
	out(" max ");
	out_dec(mx);
	out(" mean ");
	out_dec(cnt ? (uint32_t)(sum / cnt) : 0);
	out("\n");
	static const uint32_t rows[4] = { 0, 1, 1536, 3071 };

	for (int r = 0; r < 4; r++) {
		const volatile uint8_t *p = (const volatile uint8_t *)(base + (uint64_t)rows[r] * pitch);

		out("  line ");
		out_dec(rows[r]);
		out(":");
		for (int i = 0; i < 20; i++) {
			out(" ");
			out_hex(p[2000 + i]);
		}
		out("\n");
	}
}
#endif

static void cam_rx_report(void)
{
	uint32_t shown = 0, changed = 0;
	uint64_t nz[4] = { 0, 0, 0, 0 };

	out("the receiver while the sensor streams, registers that changed since they were written:\n");
	for (uint32_t i = 0; i < CAM_RXS_WORDS; i++) {
		uint32_t now = rd32(CAM_RXS_BASE + 4 * i);

		if (now != cam_rx_before[i]) {
			changed++;
			if (shown < 40) {
				out("  +");
				out_hex(4 * i);
				out(": ");
				out_hex(cam_rx_before[i]);
				out(" -> ");
				out_hex(now);
				out("\n");
				shown++;
			}
		}
	}
	out("  ");
	out_dec(changed);
	out(" SENINF registers changed\nCAMSV_1, what changed:\n");
	shown = 0;
	changed = 0;
	for (uint32_t i = 0; i < CAM_RXSV_WORDS; i++) {
		uint32_t now = rd32(CAM_RXSV_BASE + 4 * i);

		if (now != cam_rxsv_before[i]) {
			changed++;
			if (shown < 40) {
				out("  +");
				out_hex(4 * i);
				out(": ");
				out_hex(cam_rxsv_before[i]);
				out(" -> ");
				out_hex(now);
				out("\n");
				shown++;
			}
		}
	}
	out("  ");
	out_dec(changed);
	out(" CAMSV_1 registers changed\n");
	/* did anything arrive in memory? the four areas (image and header of each of the two queued buffers) */
	for (int k = 0; k < 4; k++) {
		static const uint64_t off[4] = { CAM_RX_IMG_OFF0, CAM_RX_HDR_OFF0, CAM_RX_IMG_OFF1, CAM_RX_HDR_OFF1 };
		static const uint64_t len[4] = { CAM_RX_IMG_LEN, CAM_RX_HDR_LEN, CAM_RX_IMG_LEN, CAM_RX_HDR_LEN };
		uint64_t base = CAM_RX_BUF + off[k];

		for (uint64_t a = base; a < base + len[k]; a += 8) {
			if (*(volatile uint64_t *)a) {
				nz[k]++;
			}
		}
		out("frame buffer area ");
		out_hex(base);
		out(": ");
		out_dec(nz[k]);
		out(" non-zero 64-bit words; first words ");
		for (int w = 0; w < 6; w++) {
			out_hex(*(volatile uint32_t *)(base + 4 * (uint64_t)w));
			out(" ");
		}
		out("\n");
	}
	out((nz[0] || nz[1] || nz[2] || nz[3]) ? "  <- SOMETHING WAS WRITTEN TO MEMORY\n" : "  (nothing was written to memory)\n");
#ifdef CAM_RX_MAIN
	cam_rx_stats(CAM_RX_BUF + CAM_RX_IMG_OFF0, 5100, 3072);
	cam_rx_stats(CAM_RX_BUF + CAM_RX_IMG_OFF1, 5100, 3072);
#endif
	/* everything CAMSV_1 holds now, to compare line by line with what Android's HAL leaves in it; and the camera's SMI port */
	out("CAMSV_1 in full, now:\n");
	for (uint32_t i = 0; i < CAM_RXSV_WORDS; i++) {
		uint32_t v = rd32(CAM_RXSV_BASE + 4 * i);

		if (v) {
			out("  +");
			out_hex(4 * i);
			out(" = ");
			out_hex(v);
			out("\n");
		}
	}
	out("smi_larb3 (0x1a002000), non-zero:\n");
	for (uint32_t i = 0; i < 0x1000 / 4; i++) {
		uint32_t v = rd32(0x1a002000ULL + 4 * i);

		if (v) {
			out("  +");
			out_hex(4 * i);
			out(" = ");
			out_hex(v);
			out("\n");
		}
	}
}
#endif /* CAM_HAVE_RX */

#ifdef CAM_HAVE_JN1
/* one register write: 16-bit address, then one or two data bytes; 1 if every byte was acknowledged */
static int cam_bb_write_reg(uint32_t addr7, uint16_t reg, uint16_t val, int width)
{
	int ok;

	cam_bb_start();
	ok = cam_bb_write((uint8_t)(addr7 << 1));
	ok &= cam_bb_write((uint8_t)(reg >> 8));
	ok &= cam_bb_write((uint8_t)(reg & 0xffu));
	if (width == 2) {
		ok &= cam_bb_write((uint8_t)(val >> 8));
	}
	ok &= cam_bb_write((uint8_t)(val & 0xffu));
	cam_bb_stop();
	return ok;
}

/*
 * The JN1's table as Android wrote it, then the stream is started, looked at, and stopped. "Looked at": the mode
 * register reads back what was written, and the frame counter (register 0x0005) is read three times, 30 ms apart.
 * Nothing receives the frames (there is no CSI-2 receiver yet); that the counter moves is the sensor saying it runs.
 */
static void cam_jn1_run(void)
{
	uint8_t mode = 0xff, c1 = 0xff, c2 = 0xff, c3 = 0xff;
	int bad = -1;

	out("JN1: writing its table, ");
	out_dec(CAM_JN1_OPS);
	out(" writes\n");
	for (int i = 0; i < CAM_JN1_OPS; i++) {
		if (!cam_bb_write_reg(0x10, cam_jn1_table[i].reg, cam_jn1_table[i].val, cam_jn1_table[i].width)) {
			bad = i;
			break;
		}
		if (cam_jn1_table[i].pause_us) {
			dbg_delay_us(cam_jn1_table[i].pause_us);
		}
	}
	if (bad >= 0) {
		out("  a write was not acknowledged: number ");
		out_dec(bad);
		out(", register ");
		out_hex(cam_jn1_table[bad].reg);
		out("\n");
		return;
	}
	out("  all written and acknowledged\n");
#ifdef CAM_HAVE_RX
	cam_rx_program();
#endif
	out("stream on (register 0x0100 <- 1)\n");
	cam_bb_write_reg(0x10, 0x0100, 1, 1);
	dbg_delay_ms(40);
	cam_bb_read8(0x10, 0x0100, &mode);
	cam_bb_read8(0x10, 0x0005, &c1);
	dbg_delay_ms(30);
	cam_bb_read8(0x10, 0x0005, &c2);
	dbg_delay_ms(30);
	cam_bb_read8(0x10, 0x0005, &c3);
	out("  mode register 0x0100 reads ");
	out_hex(mode);
	out(", frame counter 0x0005 reads ");
	out_hex(c1);
	out(", ");
	out_hex(c2);
	out(", ");
	out_hex(c3);
	out(c1 != c2 || c2 != c3 ? "  <- THE COUNTER MOVES: THE SENSOR IS STREAMING\n" : "  (the counter does not move)\n");
#ifdef CAM_HAVE_RX
	dbg_delay_ms(150);              /* a few whole frames, not the start of the first */
	cam_rx_report();
#endif
	out("stream off (register 0x0100 <- 0)\n");
	cam_bb_write_reg(0x10, 0x0100, 0, 1);
	dbg_delay_ms(50);
	cam_bb_read8(0x10, 0x0100, &mode);
	out("  mode register now ");
	out_hex(mode);
	out("\n");
}
#endif /* CAM_HAVE_JN1 */

static void cam_power_on_read_id(void)
{
	struct cam_pin_state avdd, pdn, rst, mclk;
	uint16_t hwcid = 0, e1 = 0, e2 = 0, vosel_before = 0, vosel = 0;
	uint32_t cfg1_before;
	int have_io = 0, have_d = 0;

	out("== stage 1: the main camera on ==\n");
	if (!pwrap_read(MT6358_HWCID, &hwcid) || hwcid < 0x5830 || hwcid > 0x5832) {
		out("the PMIC is not the one this was written for: stopping\n");
		return;
	}
	if (!pwrap_read(CAM_VCAMIO_EN, &e1) || !pwrap_read(CAM_VCAMD_EN, &e2) || (e1 & 1u) || (e2 & 1u) ||
	    !pwrap_read(CAM_VCAMD_VOSEL, &vosel_before)) {
		out("a camera rail is already on or unreadable: stopping\n");
		return;
	}
	cam_pin_save(&avdd, CAM_PIN_AVDD);
	cam_pin_save(&pdn, CAM_PIN_PDN);
	cam_pin_save(&rst, CAM_PIN_RST);
	cam_pin_save(&mclk, CAM_PIN_MCLK);
	cfg1_before = rd32(TOPCKGEN_BASE + CAM_CFG1);
	out("CLK_CFG_1 before ");
	out_hex(cfg1_before);
	out("\n");

	out("pins to their quiet state: reset, power-down and analog supply low, MCLK as a plain pin\n");
	cam_pin_out(CAM_PIN_RST, 0);
	cam_pin_out(CAM_PIN_PDN, 0);
	cam_pin_out(CAM_PIN_AVDD, 0);
	cam_pin_mode(CAM_PIN_MCLK, 0);
	cam_pin_out(CAM_PIN_MCLK, 0);
	dbg_delay_ms(1);
	out("opening camtg_sel and camtg1_sel (MCLK's muxes): CLK_CFG_1_CLR\n");
	wr32(TOPCKGEN_BASE + CAM_CFG1_CLR, CAM_CFG1_CAMTG_PDN | CAM_CFG1_CAMTG1_PDN);
	__asm__ volatile("dsb sy");

	out("VCAMIO (1.8 V) on\n");
	have_io = cam_rail(CAM_VCAMIO_EN, 1);
	out(have_io ? "  reads back on\n" : "  DOES NOT READ BACK: stopping\n");
	if (!have_io) {
		goto down;
	}
	dbg_delay_us(500);
	out("VCAMD voltage: ");
	out_hex(vosel_before);
	out(" -> 0x0500 (1.1 V, as Android)\n");
	pwrap_write(CAM_VCAMD_VOSEL, CAM_VCAMD_1V1);
	dbg_delay_us(200);
	if (!pwrap_read(CAM_VCAMD_VOSEL, &vosel) || vosel != CAM_VCAMD_1V1) {
		out("  READS BACK ");
		out_hex(vosel);
		out(": VCAMD stays off, stopping\n");
		goto down;
	}
	out("  reads back 0x0500\nVCAMD on\n");
	have_d = cam_rail(CAM_VCAMD_EN, 1);
	out(have_d ? "  reads back on\n" : "  DOES NOT READ BACK: stopping\n");
	if (!have_d) {
		goto down;
	}
	dbg_delay_us(1500);
	out("analog supply: GPIO23 high\n");
	cam_pin_out(CAM_PIN_AVDD, 1);
	dbg_delay_ms(1);
	out("  pin reads ");
	out_dec(cam_pin_read(CAM_PIN_AVDD));
	out("\nreset: GPIO102 high\n");
	cam_pin_out(CAM_PIN_RST, 1);
	dbg_delay_ms(1);
	out("  pin reads ");
	out_dec(cam_pin_read(CAM_PIN_RST));
	out("\nMCLK: GPIO100 to function 1\n");
	cam_pin_mode(CAM_PIN_MCLK, 1);
	dbg_delay_ms(12);

	{
		struct cam_pin_state scl, sda;
		uint8_t hi = 0, lo = 0;
		int m1, m2;

		cam_pin_save(&scl, CAM_BB_SCL);
		cam_pin_save(&sda, CAM_BB_SDA);
		out("I2C4's pins ");
		out_dec(CAM_BB_SCL);
		out(" (SCL) and ");
		out_dec(CAM_BB_SDA);
		out(" (SDA) were in function ");
		out_dec(scl.mode);
		out(" and ");
		out_dec(sda.mode);
		out("; taking them over as plain pins\n");
		cam_pin_mode(CAM_BB_SCL, 0);
		cam_pin_mode(CAM_BB_SDA, 0);
		cam_bb_line(CAM_BB_SCL, 1);
		cam_bb_line(CAM_BB_SDA, 1);
		dbg_delay_us(50);
		out("bus idle, both lines let go: SCL reads ");
		out_dec(cam_pin_read(CAM_BB_SCL));
		out(", SDA reads ");
		out_dec(cam_pin_read(CAM_BB_SDA));
		out(" (1 and 1 expected: the pull-ups)\n");
		out("reading the sensor's ID by hand (address 0x10, registers 0x0000 and 0x0001)\n");
		m1 = cam_bb_read8(0x10, 0x0000, &hi);
		m2 = cam_bb_read8(0x10, 0x0001, &lo);
		out("  register 0x0000: ");
		out_hex(hi);
		out(m1 ? "  acknowledges missed (bit mask) " : "  all acknowledged");
		if (m1) { out_hex(m1); }
		out("\n  register 0x0001: ");
		out_hex(lo);
		out(m2 ? "  acknowledges missed (bit mask) " : "  all acknowledged");
		if (m2) { out_hex(m2); }
		out("\n");
		if (!m1 && !m2) {
			out("  sensor ID ");
			out_hex(((uint32_t)hi << 8) | lo);
			out(((hi << 8) | lo) == 0x38e1 ? "  <- THE SENSOR ANSWERED: S5K JN1\n" : "  (answered, but not 0x38e1)\n");
#ifdef CAM_HAVE_JN1
			if (((hi << 8) | lo) == 0x38e1) {
				cam_jn1_run();
			}
#endif
		}
		cam_pin_restore(&sda);
		cam_pin_restore(&scl);
	}

down:
	out("switching it all off again\n");
	cam_pin_mode(CAM_PIN_MCLK, 0);
	cam_pin_out(CAM_PIN_RST, 0);
	dbg_delay_us(500);
	cam_pin_out(CAM_PIN_AVDD, 0);
	dbg_delay_us(500);
	out("  VCAMD off: ");
	out(cam_rail(CAM_VCAMD_EN, 0) ? "ok\n" : "NO\n");
	pwrap_write(CAM_VCAMD_VOSEL, vosel_before);
	out("  VCAMIO off: ");
	out(cam_rail(CAM_VCAMIO_EN, 0) ? "ok\n" : "NO\n");
	if (cfg1_before & CAM_CFG1_CAMTG_PDN) {
		wr32(TOPCKGEN_BASE + CAM_CFG1_SET, CAM_CFG1_CAMTG_PDN);
	}
	if (cfg1_before & CAM_CFG1_CAMTG1_PDN) {
		wr32(TOPCKGEN_BASE + CAM_CFG1_SET, CAM_CFG1_CAMTG1_PDN);
	}
	cam_pin_restore(&mclk);
	cam_pin_restore(&rst);
	cam_pin_restore(&pdn);
	cam_pin_restore(&avdd);
	__asm__ volatile("dsb sy");
	out("pins and clocks put back; CLK_CFG_1 ");
	out_hex(rd32(TOPCKGEN_BASE + CAM_CFG1));
	out("\n");
}
#endif /* CAM_POWER */

#ifndef CAM_SYS
#define CAM_SYS 0
#endif

#if CAM_SYS || CAM_RX
/*
 * Stage 2: the camera subsystem (CAM) on, its receiver registers read as the hardware leaves them, and off again.
 * The sequences are the kernel's own: spm_mtcmos_ctrl_cam_pwr and spm_mtcmos_ctrl_cam_bus_prot, read out of the
 * running Android kernel and disassembled (docs/camera-x6826b.md). The same code for the GPU gave the values this
 * loader already uses for it. Every step is named before it is taken, every wait has a time limit.
 */
#define CAM_SPM_BASE            0x10006000ULL
#define CAM_SPM_CON             0x344           /* PWR_CON of the CAM domain */
#define CAM_SPM_STA             0x180
#define CAM_SPM_STA2            0x184
#define CAM_STA_BIT             (1u << 7)
#define CAM_PWR_RST_B           (1u << 0)
#define CAM_PWR_ISO             (1u << 1)
#define CAM_PWR_ON              (1u << 2)
#define CAM_PWR_ON_2ND          (1u << 3)
#define CAM_PWR_CLK_DIS         (1u << 4)
#define CAM_SRAM_PDN            (3u << 8)       /* bits 8 and 9, acknowledged in bits 12 and 13 */
#define CAM_SRAM_ACK            (3u << 12)

#define CAM_INFRA_PROT_STA1     0x228           /* bit 20 */
#define CAM_INFRA_PROT_STA2     0x258           /* bits 19 and 21 */
#define CAM_INFRA_PROT_SET      0x2a0
#define CAM_INFRA_PROT_CLR      0x2a4
#define CAM_INFRA_PROT1_SET     0x2a8
#define CAM_INFRA_PROT1_CLR     0x2ac
#define CAM_PROT_A              0x00100000u
#define CAM_PROT_B              0x00280000u
#define CAM_SMI_BASE            0x14002000ULL   /* smi_common */
#define CAM_SMI_STA             0x3c0
#define CAM_SMI_SET             0x3c4
#define CAM_SMI_CLR             0x3c8

#define CAM_MMSYS_BASE          0x14000000ULL
#define CAM_MMSYS_CG_CON0       0x100
#define CAM_MMSYS_CG_SET0       0x104
#define CAM_MMSYS_CG_CLR0       0x108
#define CAM_MMSYS_SMI_CAM       0x02000000u
#define CAM_CFG7                0xb0            /* topckgen: cam_sel (byte 3) and camtm_sel (byte 1) */
#define CAM_CFG7_SET            0xb4
#define CAM_CFG7_CLR            0xb8
#define CAM_CFG7_PDN            (0x80000000u | 0x00008000u)
#define CAM_CFG6_SENINF_PDN     0x00800000u     /* seninf_sel, in CLK_CFG_6 (0xa0, set 0xa4, clear 0xa8) */

#define CAM_CAMSYS_BASE         0x1a000000ULL
#define CAM_CAMSYS_CG_CON       0x0
#define CAM_CAMSYS_CG_SET       0x4
#define CAM_CAMSYS_CG_CLR       0x8
#define CAM_CAMSYS_GATES        0x00001fc3u     /* larb3, dfp_vad, cam, ccu, camtg, seninf, camsv0..2 */
#define CAM_SENINF_BASE         0x1a040000ULL

static int cam_poll(uint64_t addr, uint32_t mask, uint32_t want, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10; i++) {
		if ((rd32(addr) & mask) == want) {
			return 1;
		}
		dbg_delay_us(100);
	}
	return 0;
}

static void cam_con_set(uint32_t bits) { wr32(CAM_SPM_BASE + CAM_SPM_CON, rd32(CAM_SPM_BASE + CAM_SPM_CON) | bits); __asm__ volatile("dsb sy"); }
static void cam_con_clr(uint32_t bits) { wr32(CAM_SPM_BASE + CAM_SPM_CON, rd32(CAM_SPM_BASE + CAM_SPM_CON) & ~bits); __asm__ volatile("dsb sy"); }

static void cam_state(const char *when)
{
	out(when);
	out(": PWR_CON ");
	out_hex(rd32(CAM_SPM_BASE + CAM_SPM_CON));
	out(", PWR_STATUS ");
	out_hex(rd32(CAM_SPM_BASE + CAM_SPM_STA));
	out(" / ");
	out_hex(rd32(CAM_SPM_BASE + CAM_SPM_STA2));
	out(", bus protection: infra ");
	out_hex(rd32(INFRACFG_BASE + CAM_INFRA_PROT_STA1));
	out(" / ");
	out_hex(rd32(INFRACFG_BASE + CAM_INFRA_PROT_STA2));
	out(", smi_common ");
	out_hex(rd32(CAM_SMI_BASE + CAM_SMI_STA));
	out("\n");
}

/* 1: switched on by this; 0: it did not come up (what is half done is the caller's to put back); -1: it was on already */
static int cam_domain_on(void)
{
	if ((rd32(CAM_SPM_BASE + CAM_SPM_STA) & CAM_STA_BIT) || (rd32(CAM_SPM_BASE + CAM_SPM_STA2) & CAM_STA_BIT)) {
		out("the CAM domain is on already: left alone\n");
		return -1;
	}
	out("CAM domain: PWR_ON, PWR_ON_2ND\n");
	cam_con_set(CAM_PWR_ON);
	cam_con_set(CAM_PWR_ON_2ND);
	if (!cam_poll(CAM_SPM_BASE + CAM_SPM_STA, CAM_STA_BIT, CAM_STA_BIT, 20) ||
	    !cam_poll(CAM_SPM_BASE + CAM_SPM_STA2, CAM_STA_BIT, CAM_STA_BIT, 20)) {
		out("  the status bit did not come up\n");
		return 0;
	}
	out("CAM domain: clock on, isolation off, reset released\n");
	cam_con_clr(CAM_PWR_CLK_DIS);
	cam_con_clr(CAM_PWR_ISO);
	cam_con_set(CAM_PWR_RST_B);
	out("CAM domain: SRAM awake\n");
	cam_con_clr(1u << 8);
	if (!cam_poll(CAM_SPM_BASE + CAM_SPM_CON, 1u << 12, 0, 20)) {
		out("  SRAM 0 did not acknowledge\n");
		return 0;
	}
	cam_con_clr(1u << 9);
	if (!cam_poll(CAM_SPM_BASE + CAM_SPM_CON, 1u << 13, 0, 20)) {
		out("  SRAM 1 did not acknowledge\n");
		return 0;
	}
	out("CAM domain: bus protection released\n");
	wr32(INFRACFG_BASE + CAM_INFRA_PROT_CLR, CAM_PROT_A);
	__asm__ volatile("dsb sy");
	wr32(CAM_SMI_BASE + CAM_SMI_CLR, 8);
	__asm__ volatile("dsb sy");
	wr32(INFRACFG_BASE + CAM_INFRA_PROT1_CLR, CAM_PROT_B);
	__asm__ volatile("dsb sy");
	return 1;
}

/* the same in the other order; 1 if it ended with the status bit down */
static int cam_domain_off(void)
{
	int ok = 1;

	out("CAM domain: bus protection on\n");
	wr32(INFRACFG_BASE + CAM_INFRA_PROT1_SET, CAM_PROT_B);
	__asm__ volatile("dsb sy");
	ok &= cam_poll(INFRACFG_BASE + CAM_INFRA_PROT_STA2, CAM_PROT_B, CAM_PROT_B, 20);
	wr32(INFRACFG_BASE + CAM_INFRA_PROT_SET, CAM_PROT_A);
	__asm__ volatile("dsb sy");
	ok &= cam_poll(INFRACFG_BASE + CAM_INFRA_PROT_STA1, CAM_PROT_A, CAM_PROT_A, 20);
	wr32(CAM_SMI_BASE + CAM_SMI_SET, 8);
	__asm__ volatile("dsb sy");
	ok &= cam_poll(CAM_SMI_BASE + CAM_SMI_STA, 8, 8, 20);
	if (!ok) {
		out("  bus protection did not acknowledge: leaving the domain as it is\n");
		return 0;
	}
	out("CAM domain: SRAM to sleep\n");
	cam_con_set(CAM_SRAM_PDN);
	cam_poll(CAM_SPM_BASE + CAM_SPM_CON, CAM_SRAM_ACK, CAM_SRAM_ACK, 20);
	out("CAM domain: isolation on, clock off, reset, power off\n");
	cam_con_set(CAM_PWR_ISO);
	cam_con_set(CAM_PWR_CLK_DIS);
	cam_con_clr(CAM_PWR_RST_B);
	cam_con_clr(CAM_PWR_ON);
	cam_con_clr(CAM_PWR_ON_2ND);
	return cam_poll(CAM_SPM_BASE + CAM_SPM_STA, CAM_STA_BIT, 0, 20) && cam_poll(CAM_SPM_BASE + CAM_SPM_STA2, CAM_STA_BIT, 0, 20);
}

/* the non-zero words of a block, as "+offset = value" */
static void cam_dump_block(const char *name, uint64_t base, uint32_t from, uint32_t to)
{
	uint32_t shown = 0;

	out(name);
	out(" ");
	out_hex(base + from);
	out("..");
	out_hex(base + to - 1);
	out(":\n");
	for (uint32_t off = from; off < to; off += 4) {
		uint32_t v = rd32(base + off);

		if (v) {
			out("  +");
			out_hex(off);
			out(" = ");
			out_hex(v);
			out("\n");
			shown++;
		}
	}
	out("  (");
	out_dec(shown);
	out(" non-zero)\n");
}

#if CAM_SYS
static void cam_sys_probe(void)
{
	uint32_t cfg7, cfg6, mm_cg, cam_cg;
	int dom;

	(void)cam_cg;
	out("== stage 2: the camera subsystem ==\n");
	cam_state("before");
	dom = cam_domain_on();
	if (dom <= 0) {
		cam_state("after the failed start");
		if (dom == 0) {
			cam_domain_off();
		}
		return;
	}
	cam_state("domain on");

	cfg7 = rd32(TOPCKGEN_BASE + CAM_CFG7);
	cfg6 = rd32(TOPCKGEN_BASE + 0xa0);
	mm_cg = rd32(CAM_MMSYS_BASE + CAM_MMSYS_CG_CON0);
	out("clocks before: CLK_CFG_7 ");
	out_hex(cfg7);
	out(", CLK_CFG_6 ");
	out_hex(cfg6);
	out(", mmsys CG_CON0 ");
	out_hex(mm_cg);
	out("\nopening cam_sel, camtm_sel, seninf_sel and mm_smi_cam_ck\n");
	wr32(TOPCKGEN_BASE + CAM_CFG7_CLR, CAM_CFG7_PDN);
	wr32(TOPCKGEN_BASE + 0xa8, CAM_CFG6_SENINF_PDN);
	wr32(CAM_MMSYS_BASE + CAM_MMSYS_CG_CLR0, CAM_MMSYS_SMI_CAM);
	__asm__ volatile("dsb sy");
	dbg_delay_us(100);

	out("about to read camsys's gates\n");
	cam_cg = rd32(CAM_CAMSYS_BASE + CAM_CAMSYS_CG_CON);
	out("camsys CG_CON ");
	out_hex(cam_cg);
	out("\nopening the camera gates (larb3, dfp_vad, cam, ccu, camtg, seninf, camsv0..2)\n");
	wr32(CAM_CAMSYS_BASE + CAM_CAMSYS_CG_CLR, CAM_CAMSYS_GATES);
	__asm__ volatile("dsb sy");
	dbg_delay_us(100);
	out("camsys CG_CON now ");
	out_hex(rd32(CAM_CAMSYS_BASE + CAM_CAMSYS_CG_CON));
	out("\n");

	/* the receiver, a page at a time: the line before a hang is the page that hung */
	for (uint32_t blk = 0; blk < 0x8000; blk += 0x1000) {
		out("reading SENINF page ");
		out_hex(blk);
		out("\n");
		cam_dump_block("SENINF", CAM_SENINF_BASE, blk, blk + 0x1000);
	}
	out("reading CAMSV0 and CAMSV1\n");
	cam_dump_block("CAMSV0", 0x1a050000ULL, 0, 0x1000);
	cam_dump_block("CAMSV1", 0x1a051000ULL, 0, 0x1000);

	/*
	 * Back the way it was. The domain goes down FIRST, with its clocks still running: the bus protection is
	 * acknowledged by the bus the clocks drive (the first version closed the gates before asking, and the
	 * acknowledge of bit 21 never came). Its gates need no closing: they are reset with the domain.
	 */
	cam_state("receiver read");
	out(cam_domain_off() ? "the CAM domain is off again\n" : "THE CAM DOMAIN DID NOT GO DOWN\n");
	out("closing mmsys's and topckgen's gates again\n");
	wr32(CAM_MMSYS_BASE + CAM_MMSYS_CG_SET0, (mm_cg & CAM_MMSYS_SMI_CAM));
	if (cfg7 & CAM_CFG7_PDN) {
		wr32(TOPCKGEN_BASE + CAM_CFG7_SET, cfg7 & CAM_CFG7_PDN);
	}
	if (cfg6 & CAM_CFG6_SENINF_PDN) {
		wr32(TOPCKGEN_BASE + 0xa4, CAM_CFG6_SENINF_PDN);
	}
	__asm__ volatile("dsb sy");
	cam_state("after");
}
#endif /* CAM_SYS (the probe) */

#if CAM_RX
/* the subsystem up with the MIPI receivers' clocks too, and down again; what it changed is remembered here */
struct cam_sysst { uint32_t cfg7, cfg6, mm_cg, apmix, csr[6]; };
#define CAM_APMIXED_MIPI        0x1000c014ULL   /* AP_PLL_CON: bits 6 and 11 are the 26 MHz clocks of MIPI C0 and C1 */
#define CAM_APMIXED_BITS        0x00000840u
#define CAM_MIPIRX_BASE         0x11c10000ULL   /* six analog blocks, 0x1000 apart; +0x80 bit 1 is each one's clock */

static int cam_sys_up(struct cam_sysst *st)
{
	int dom = cam_domain_on();

	if (dom <= 0) {
		if (dom == 0) {
			cam_domain_off();
		}
		return 0;
	}
	st->cfg7 = rd32(TOPCKGEN_BASE + CAM_CFG7);
	st->cfg6 = rd32(TOPCKGEN_BASE + 0xa0);
	st->mm_cg = rd32(CAM_MMSYS_BASE + CAM_MMSYS_CG_CON0);
	wr32(TOPCKGEN_BASE + CAM_CFG7_CLR, CAM_CFG7_PDN);
	wr32(TOPCKGEN_BASE + 0xa8, CAM_CFG6_SENINF_PDN);
	wr32(CAM_MMSYS_BASE + CAM_MMSYS_CG_CLR0, CAM_MMSYS_SMI_CAM);
	__asm__ volatile("dsb sy");
	dbg_delay_us(100);
	wr32(CAM_CAMSYS_BASE + CAM_CAMSYS_CG_CLR, CAM_CAMSYS_GATES);
	__asm__ volatile("dsb sy");
	dbg_delay_us(100);
	st->apmix = rd32(CAM_APMIXED_MIPI);
	wr32(CAM_APMIXED_MIPI, st->apmix | CAM_APMIXED_BITS);
	out("MIPI 26 MHz clocks: ");
	out_hex(st->apmix);
	out(" -> ");
	out_hex(rd32(CAM_APMIXED_MIPI));
	out("\n");
	for (int n = 0; n < 6; n++) {
		uint64_t a = CAM_MIPIRX_BASE + 0x1000 * (uint64_t)n + 0x80;

		st->csr[n] = rd32(a);
		wr32(a, st->csr[n] | 2u);
	}
	__asm__ volatile("dsb sy");
	out("the camera subsystem is up\n");
	return 1;
}

static void cam_sys_down(const struct cam_sysst *st)
{
	for (int n = 0; n < 6; n++) {
		wr32(CAM_MIPIRX_BASE + 0x1000 * (uint64_t)n + 0x80, st->csr[n]);
	}
	wr32(CAM_APMIXED_MIPI, st->apmix);
	__asm__ volatile("dsb sy");
	out(cam_domain_off() ? "the CAM domain is off again\n" : "THE CAM DOMAIN DID NOT GO DOWN\n");
	wr32(CAM_MMSYS_BASE + CAM_MMSYS_CG_SET0, (st->mm_cg & CAM_MMSYS_SMI_CAM));
	if (st->cfg7 & CAM_CFG7_PDN) {
		wr32(TOPCKGEN_BASE + CAM_CFG7_SET, st->cfg7 & CAM_CFG7_PDN);
	}
	if (st->cfg6 & CAM_CFG6_SENINF_PDN) {
		wr32(TOPCKGEN_BASE + 0xa4, CAM_CFG6_SENINF_PDN);
	}
	__asm__ volatile("dsb sy");
}
#endif /* CAM_RX */

#endif /* CAM_SYS || CAM_RX */

static void cam_probe(void)
{
	uint16_t hwcid = 0;
	uint32_t cfg6, sta0;
	int cfg6_was_gated, i2c_was_gated;

	/* ---- the PMIC: read only ---- */
	out("PMIC: ");
	if (!pwrap_read(MT6358_HWCID, &hwcid)) {
		out("the wrapper does not answer: stopping\n");
		return;
	}
	out("chip ");
	out_hex(hwcid);
	out("\nthe camera rails (enable registers: bit 0; Android sets VCAMD's voltage with 0x1eae = 0x0500, 1.1 V):\n");
	cam_pmic("VCAMIO enable", 0x1cb0);
	cam_pmic("VCAMD  enable", 0x1c9c);
	cam_pmic("VCAMD  voltage", 0x1eae);
	cam_pmic("VLDO28 enable", 0x1d32);
	cam_pmic("VLDO28 voltage", 0x1e3c);
	cam_pmic("VCAMA2 voltage", 0x1e0c);

	/* ---- the pins: all of GPIO_MODE, then the camera's own ---- */
	out("GPIO_MODE registers (eight pins each, four bits a pin; a pin on an I2C bus shows its function number):\n");
	for (uint32_t n = 0; n < 26; n++) {
		out("  MODE");
		out_dec(n);
		out(" (pins ");
		out_dec(n * 8);
		out("-");
		out_dec(n * 8 + 7);
		out(") = ");
		out_hex(rd32(GPIO_BASE + 0x300 + 0x10 * n));
		out("\n");
	}
	out("the camera's pins as LK left them:\n");
	cam_pin("analog supply (both cameras)", 23);
	cam_pin("main reset", 102);
	cam_pin("main power-down", 98);
	cam_pin("main MCLK", 100);
	cam_pin("second reset", 101);
	cam_pin("second power-down", 97);
	cam_pin("second MCLK", 99);
	cam_pin("flash enable", 169);

	/* ---- the I2C controller: open its clocks the way Android does, look, close them ---- */
	cfg6 = rd32(TOPCKGEN_BASE + CAM_CFG6);
	sta0 = rd32(INFRACFG_BASE + CAM_INFRA_PDN_STA0);
	cfg6_was_gated = (cfg6 & CAM_CFG6_I2C_PDN) != 0;
	i2c_was_gated = (sta0 & CAM_INFRA_I2C_AP) != 0;
	out("clocks before: CLK_CFG_6 ");
	out_hex(cfg6);
	out(cfg6_was_gated ? " (i2c_sel gated)" : " (i2c_sel running)");
	out(", INFRA_PDN_STA0 ");
	out_hex(sta0);
	out(i2c_was_gated ? " (ifr_i2c_ap gated)\n" : " (ifr_i2c_ap running)\n");

	out("opening the I2C clocks (a write to a \"clear\" register each)\n");
	wr32(TOPCKGEN_BASE + CAM_CFG6_CLR, CAM_CFG6_I2C_PDN);
	wr32(INFRACFG_BASE + CAM_INFRA_PDN_CLR0, CAM_INFRA_I2C_AP);
	__asm__ volatile("dsb sy");
	dbg_delay_ms(1);
	out("clocks now: CLK_CFG_6 ");
	out_hex(rd32(TOPCKGEN_BASE + CAM_CFG6));
	out(", INFRA_PDN_STA0 ");
	out_hex(rd32(INFRACFG_BASE + CAM_INFRA_PDN_STA0));
	out("\nabout to read the I2C controllers\n");
	cam_i2c_dump("I2C4 (main camera)", CAM_I2C4_BASE);
	cam_i2c_dump("I2C2 (second camera)", CAM_I2C2_BASE);

	/* back the way it was */
	if (cfg6_was_gated) {
		wr32(TOPCKGEN_BASE + CAM_CFG6_SET, CAM_CFG6_I2C_PDN);
	}
	if (i2c_was_gated) {
		wr32(INFRACFG_BASE + CAM_INFRA_PDN_SET0, CAM_INFRA_I2C_AP);
	}
	__asm__ volatile("dsb sy");
	out("clocks put back: CLK_CFG_6 ");
	out_hex(rd32(TOPCKGEN_BASE + CAM_CFG6));
	out(", INFRA_PDN_STA0 ");
	out_hex(rd32(INFRACFG_BASE + CAM_INFRA_PDN_STA0));
	out("\n");
#if CAM_RX
	{
		struct cam_sysst st;

		out("== stage 3: the receiver ==\n");
		if (cam_sys_up(&st)) {
			cam_power_on_read_id();
			cam_sys_down(&st);
		}
	}
#elif CAM_POWER
	cam_power_on_read_id();
#endif
#if CAM_SYS
	cam_sys_probe();
#endif
	out("CAM probe done\n");
}

#endif /* MTKCAM_H */
