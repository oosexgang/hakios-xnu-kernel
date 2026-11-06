/*
 * Reading the phone's eMMC from mtkloader, to fetch the big system image (userland/system.packfs)
 * that XNU needs as its root: it is far too large for a boot image, so it lives in a partition.
 *
 * LK has already powered the card, set the bus up and read partitions of its own by the time we
 * run, so this does not initialise anything: it drives MSDC0 (0x11230000) in PIO mode and issues
 * plain block reads (CMD17/CMD18) to the card LK left in the transfer state. Registers follow
 * Linux's drivers/mmc/host/mtk-sd.c.
 *
 * Device memory with the MMU off: 32-bit accesses only, no SIMD (see build.sh).
 */
#ifndef MSDC_H
#define MSDC_H

#include <stdint.h>

#define MSDC0_BASE       0x11230000ULL
#define MSDC1_BASE       0x11240000ULL   /* the SD card slot; LK leaves it alone */

/* Which controller the calls below talk to; MSDC0 (the eMMC) unless set otherwise. */
static uint64_t msdc_base = MSDC0_BASE;

#define MSDC_CFG         0x00
#define MSDC_CFG_PIO     (1u << 3)
#define MSDC_IOCON       0x04
#define MSDC_PS          0x08
#define MSDC_INT         0x0c
#define MSDC_INTEN       0x10
#define MSDC_FIFOCS      0x14
#define MSDC_FIFOCS_CLR  (1u << 31)
#define MSDC_TXDATA      0x18
#define MSDC_RXDATA      0x1c
#define SDC_CFG          0x30
#define SDC_CMD          0x34
#define SDC_ARG          0x38
#define SDC_STS          0x3c
#define SDC_STS_SDCBUSY  (1u << 0)
#define SDC_STS_CMDBUSY  (1u << 1)
#define SDC_RESP0        0x40
#define SDC_BLK_NUM      0x50

/* MSDC_INT */
#define INT_CMDRDY       (1u << 8)
#define INT_CMDTMO       (1u << 9)
#define INT_RSPCRCERR    (1u << 10)
#define INT_XFER_COMPL   (1u << 12)
#define INT_DATTMO       (1u << 14)
#define INT_DATCRCERR    (1u << 15)

/* SDC_CMD */
#define CMD_OPC(c)       ((c) & 0x3f)
#define CMD_RSP_R1       (1u << 7)
#define CMD_RSP_R1B      (6u << 7)
#define CMD_DTYPE_SINGLE (1u << 11)
#define CMD_DTYPE_MULTI  (2u << 11)
#define CMD_STOP         (1u << 14)
#define CMD_BLKLEN(n)    (((n) & 0xfff) << 16)

#define MMC_READ_SINGLE  17
#define MMC_READ_MULTI   18
#define MMC_STOP         12
#define MMC_SEND_STATUS  13

struct msdc_err {
	const char *what;
	uint32_t status;
};

static struct msdc_err msdc_last;
/* Status bits already read out of MSDC_INT: reading it clears them, so keep what we have seen. */
static uint32_t msdc_int_seen;

static int msdc_fail(const char *what, uint32_t status)
{
	msdc_last.what = what;
	msdc_last.status = status;
	return 0;
}

static int msdc_wait_idle(void)
{
	for (uint32_t i = 0; i < 2000000; i++) {
		if (!(rd32(msdc_base + SDC_STS) & (SDC_STS_SDCBUSY | SDC_STS_CMDBUSY))) {
			return 1;
		}
	}
	return msdc_fail("controller busy", rd32(msdc_base + SDC_STS));
}

static void msdc_fifo_clear(void)
{
	wr32(msdc_base + MSDC_FIFOCS, rd32(msdc_base + MSDC_FIFOCS) | MSDC_FIFOCS_CLR);
	for (uint32_t i = 0; i < 100000 && (rd32(msdc_base + MSDC_FIFOCS) & MSDC_FIFOCS_CLR); i++) {
	}
}

/*
 * Wait for one of the bits in `want`, failing on any of `bad`. Reading MSDC_INT clears it, so
 * every bit seen is kept in msdc_int_seen: a transfer's end can arrive while a command is
 * still being waited for, and must not be thrown away.
 */
static int msdc_wait_int(uint32_t want, uint32_t bad, const char *what)
{
	for (uint32_t i = 0; i < 20000000; i++) {
		uint32_t st = rd32(msdc_base + MSDC_INT);

		if (st) {
			wr32(msdc_base + MSDC_INT, st);
			msdc_int_seen |= st;
		}
		if (msdc_int_seen & bad) {
			uint32_t bits = msdc_int_seen;
			msdc_int_seen = 0;
			return msdc_fail(what, bits);
		}
		if (msdc_int_seen & want) {
			msdc_int_seen &= ~want;
			return 1;
		}
	}
	return msdc_fail(what, msdc_int_seen);
}

static void msdc_pio_mode(void)
{
	wr32(msdc_base + MSDC_CFG, rd32(msdc_base + MSDC_CFG) | MSDC_CFG_PIO);
	wr32(msdc_base + MSDC_INT, rd32(msdc_base + MSDC_INT)); /* nothing stale */
	msdc_int_seen = 0;
	msdc_fifo_clear();
}

/*
 * msdc_read(lba, blocks, dst): blocks of 512 bytes, straight into memory. dst must be 4-aligned.
 * Returns 0 on failure, with msdc_last saying where.
 */
static int msdc_read(uint64_t lba, uint32_t blocks, void *dst)
{
	volatile uint32_t *out = (volatile uint32_t *)dst;

	if (blocks == 0) {
		return 1;
	}
	if (!msdc_wait_idle()) {
		return 0;
	}
	msdc_pio_mode();
	wr32(msdc_base + SDC_BLK_NUM, blocks);
	wr32(msdc_base + SDC_ARG, (uint32_t)lba);     /* high capacity: the unit is a block */
	wr32(msdc_base + SDC_CMD, CMD_OPC(blocks == 1 ? MMC_READ_SINGLE : MMC_READ_MULTI) |
	    CMD_RSP_R1 | (blocks == 1 ? CMD_DTYPE_SINGLE : CMD_DTYPE_MULTI) | CMD_BLKLEN(512));
	if (!msdc_wait_int(INT_CMDRDY, INT_CMDTMO | INT_RSPCRCERR, "command")) {
		return 0;
	}

	for (uint32_t left = blocks * 512 / 4; left; ) {
		uint32_t fifo = rd32(msdc_base + MSDC_FIFOCS) & 0xff, spin = 0;

		while (fifo < 4) {
			uint32_t st = rd32(msdc_base + MSDC_INT);
			if (st & (INT_DATTMO | INT_DATCRCERR)) {
				wr32(msdc_base + MSDC_INT, st);
				return msdc_fail("data", st);
			}
			if (++spin > 20000000) {
				return msdc_fail("data stalled", rd32(msdc_base + MSDC_FIFOCS));
			}
			fifo = rd32(msdc_base + MSDC_FIFOCS) & 0xff;
		}
		uint32_t words = fifo / 4;
		if (words > left) {
			words = left;
		}
		for (uint32_t i = 0; i < words; i++) {
			*out++ = rd32(msdc_base + MSDC_RXDATA);
		}
		left -= words;
	}

	if (!msdc_wait_int(INT_XFER_COMPL, INT_DATTMO | INT_DATCRCERR, "transfer end")) {
		return 0;
	}
	if (blocks > 1) {
		/* CMD18 reads until it is told to stop */
		wr32(msdc_base + SDC_ARG, 0);
		wr32(msdc_base + SDC_CMD, CMD_OPC(MMC_STOP) | CMD_RSP_R1B | CMD_STOP);
		if (!msdc_wait_int(INT_CMDRDY, INT_CMDTMO | INT_RSPCRCERR, "stop")) {
			return 0;
		}
		if (!msdc_wait_idle()) {
			return 0;
		}
	}
	return 1;
}

#endif /* MSDC_H */
