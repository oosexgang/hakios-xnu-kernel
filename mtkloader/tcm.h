/*
 * The touch controller's own language, TCM, over the SPI2 set up in mtkspi.h.
 *
 * A message is a 0xa5 marker, a code, two bytes of payload length, the payload, and a 0x5a to
 * close it. Reading one takes several chip-select cycles because the controller's FIFO holds only
 * 32 bytes, and every cycle after the first begins with the marker and the code 0x03 for "more of
 * the same message"; writing a command is the command byte, the length, and the payload, split
 * the same way with 0x01 for "more of the same command".
 *
 * The chip keeps its firmware in RAM, so a reset leaves it in its ROM bootloader with nothing to
 * run. Android's driver boots it in two moves, tcm_load_firmware() and tcm_run_firmware(): the
 * first sends the whole image as one message -- a 14-byte header that is all zeros here, then
 * the bytes -- and reads nothing back; a pause later the second says 0x42, and the chip
 * interrupts with an identify that names the mode it moved to.
 */
#ifndef TCM_H
#define TCM_H

#include <stdint.h>
#include "mtkspi.h"
#include "touchfw.h"

#define TCM_MARKER           0xa5
#define TCM_PADDING          0x5a
#define TCM_STATUS_OK        0x01
#define TCM_STATUS_CONTINUED 0x03

#define TCM_CMD_CONTINUE_WRITE   0x01
#define TCM_CMD_IDENTIFY         0x02
#define TCM_CMD_ENABLE_REPORT    0x05
#define TCM_CMD_GET_APP_INFO     0x20
#define TCM_CMD_GET_REPORT_CONF  0x25
#define TCM_CMD_ROMBOOT_DOWNLOAD 0x45
#define TCM_ROMBOOT_UNIT         16      /* ROMBOOT_DOWNLOAD_UNIT: where its chunks may end */
#define TCM_CMD_ROMBOOT_RUN      0x42

#define TCM_REPORT_IDENTIFY  0x10
#define TCM_REPORT_TOUCH     0x11

#define TCM_MODE_APPLICATION 0x01
#define TCM_MODE_HOSTDOWNLOAD 0x02
#define TCM_MODE_ROMBOOT     0x04

#define TCM_IRQ_PIN          1       /* GPIO1, low while the chip has something to say */
#define TCM_RESET_PIN        0       /* GPIO0, low holds it in reset */

/*
 * Two ways to reach the chip, and it wants one for each direction.
 *
 * Reading, it needs time: the ROM bootloader asks for twenty microseconds between bytes and
 * answers a faster stream with nothing but the pull-ups. The SPI controller cannot leave a gap
 * between bytes, so reads drive the four pins by hand.
 *
 * Writing, it needs speed: Android pushes the whole hundred kilobytes of firmware in about a
 * tenth of a second, and a download stretched over seconds is one the chip no longer answers.
 * So writes go through the controller, 32 bytes at a time, which is all its FIFO holds.
 */
#define TCM_SLOW_GAP_US      20
#define TCM_READ_CHUNK       1024
#define TCM_WRITE_CHUNK      SPI_FIFO_BYTES

static int tcm_spi_stalls;                              /* chunks the controller would not finish */

static int tcm_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	if (rx) {
		spi_pins_to_gpio();
		spi_bitbang_xfer(tx, rx, len, TCM_SLOW_GAP_US);
		return 1;
	}
	spi_pins_to_spi2();
	spi_setup();
	if (spi_xfer(tx, rx, len)) {
		return 1;
	}
	/*
	 * The controller sometimes will not raise its done flag for a second transfer in a row.
	 * Writing by hand needs no flag and no FIFO, and a write wants no gap between its bytes --
	 * the chip's own driver sends the download at 8 MHz with nothing in between -- so this is
	 * slower than the controller but not slow enough for the ROM bootloader to lose patience.
	 */
	tcm_spi_stalls++;
	spi_pins_to_gpio();
	spi_bitbang_xfer(tx, rx, len, 0);
	return 1;
}

static int tcm_irq(void)
{
	return pin_get(TCM_IRQ_PIN) == 0;
}

static int tcm_wait_irq(uint32_t ms)
{
	for (uint32_t i = 0; i < ms; i++) {
		if (tcm_irq()) {
			return 1;
		}
		dbg_delay_ms(1);
	}
	return 0;
}

/* One command out. The two length bytes count as payload when the chunks are measured.
 * A stalled chunk is taken straight back in -- waiting, marking each try with '|' -- because
 * the very last chunk of the long download stalled like this, with the chip already having
 * every byte but those. If the controller never finishes it, the same bytes go out by hand
 * (mark '/'): the chip cannot tell the two drivers apart, but the missing 23 bytes matter to
 * it. `steps` collects how many payload bytes left before giving up: where in the message the
 * stall sits. */
/* Where a write gave up, for a caller that wants to say so. */
static uint32_t tcm_stall_chunk, tcm_stall_size;

static int tcm_write(uint8_t cmd, const uint8_t *payload, uint32_t len, uint32_t unit,
    uint32_t *steps)
{
	static uint8_t chunk[TCM_WRITE_CHUNK];
	uint32_t space = TCM_WRITE_CHUNK - 1, remaining = len + 2, sent = 0;
	int first = 1;

	/*
	 * `unit` constrains where a chunk may end. The download wants 16, and the stream it counts
	 * is the two length bytes together with the payload -- which is why its payload carries
	 * fourteen bytes of header: two plus fourteen plus a hundred kilobytes divides by sixteen
	 * exactly. Chunks that end anywhere else leave the chip reporting nonsense for how much it
	 * wrote, and then refusing to run it.
	 */
	if (unit > 1) {
		space -= space % unit;
	}
	if (space == 0) {
		return 0;
	}

	while (remaining > 0) {
		uint32_t xfer = remaining > space ? space : remaining, n = 0;

		if (first) {
			chunk[n++] = cmd;
			chunk[n++] = (uint8_t)len;
			chunk[n++] = (uint8_t)(len >> 8);
			for (uint32_t i = 2; i < xfer; i++) {
				chunk[n++] = payload[sent++];
			}
			first = 0;
		} else {
			chunk[n++] = TCM_CMD_CONTINUE_WRITE;
			for (uint32_t i = 0; i < xfer; i++) {
				chunk[n++] = payload[sent++];
			}
		}
		/*
		 * No pause between chunks and no second attempt at one. The reference driver hands
		 * the whole download to its transport as a single chunk and the phone gets it out in
		 * about a tenth of a second; a pause per chunk would stretch that to seconds, and a
		 * chunk sent twice would put its bytes into the stream twice.
		 */
		if (!tcm_xfer(chunk, NULL, n)) {
			if (steps) {
				*steps = sent;
			}
			return 0;
		}
		remaining -= xfer;
	}
	if (steps) {
		*steps = len;
	}
	return 1;
}

/*
 * One message in. Returns its code, and its payload up to `max` bytes; `*got` says how much of
 * the payload there was, which may be more than was kept.
 */
static int tcm_read(uint8_t *code, uint8_t *buf, uint32_t max, uint32_t *got)
{
	static uint8_t tmp[TCM_READ_CHUNK];
	uint8_t head[4];
	uint32_t space = TCM_READ_CHUNK - 2, len, left, at = 0;

	if (!tcm_xfer(NULL, head, 4)) {
		return 0;
	}
	if (head[0] != TCM_MARKER) {
		*code = head[0];
		*got = 0;
		return -1;
	}
	*code = head[1];
	len = (uint32_t)head[2] | ((uint32_t)head[3] << 8);
	*got = len;
	left = len + 1;                                 /* the payload and the byte that closes it */
	if (len == 0) {
		left = 1;                               /* even an empty answer has that byte, and
		                                           leaving it unread leaves the chip talking */
	}

	while (left > 0) {
		uint32_t xfer = left > space ? space : left;

		if (!tcm_xfer(NULL, tmp, xfer + 2)) {
			return 0;
		}
		if (tmp[0] != TCM_MARKER || tmp[1] != TCM_STATUS_CONTINUED) {
			return -2;
		}
		for (uint32_t i = 0; i < xfer; i++) {
			if (at + i < max) {
				buf[at + i] = tmp[2 + i];
			}
		}
		at += xfer;
		left -= xfer;
	}
	return 1;
}

/*
 * The same read, but patient. The chip's first answer after something has happened to it is often
 * nothing at all -- Android's own driver logs exactly this at every boot -- so wait for it to
 * raise its interrupt line and try again rather than believing the first silence.
 */
/*
 * The same read, waiting as long as the reference driver does. It gives a command three seconds
 * (RESPONSE_TIMEOUT_MS) and expects the download in particular to take most of them: its own
 * write call times out and it then reads the answer separately.
 */
__attribute__((unused)) static int tcm_read_within(uint8_t *code, uint8_t *buf, uint32_t max, uint32_t *got, uint32_t ms)
{
	for (uint32_t waited = 0; waited <= ms; waited += 120) {
		int r;

		tcm_wait_irq(100);
		r = tcm_read(code, buf, max, got);
		if (r == 1) {
			return 1;
		}
		dbg_delay_ms(20);
	}
	return 0;
}

static int tcm_read_patiently(uint8_t *code, uint8_t *buf, uint32_t max, uint32_t *got)
{
	for (int tries = 0; tries < 6; tries++) {
		int r;

		tcm_wait_irq(100);
		r = tcm_read(code, buf, max, got);
		if (r == 1) {
			return 1;
		}
		dbg_delay_ms(20);
	}
	return 0;
}

/*
 * The firmware, in the one transfer the ROM bootloader wants.
 *
 * The caller lays the whole message out in DRAM -- the command, the two length bytes, fourteen
 * bytes of header whose first holds the top byte of the image's size, then the image -- and this
 * clocks all of it out without letting the chip select go. That is what the reference driver does:
 * its transport takes the download as a single chunk of a hundred kilobytes. Splitting it into
 * continuations, which every other command tolerates, does not work here: the bootloader reads
 * the 0x01 that marks a continuation as a command of its own, and answers either nonsense for how
 * much it wrote or by resetting itself.
 *
 * Hence by hand rather than through the controller, whose FIFO holds 32 bytes and cannot hold a
 * chip select across a refill. No gap between bytes: the phone sends the download at 8 MHz with
 * nothing in between, and the bootloader stops waiting long before a gap of microseconds a byte
 * would get through it.
 */
#define TCM_MESSAGE_HEAD     3           /* the command and its two length bytes */
#define TCM_ROMBOOT_HEADER   14

static void tcm_load_firmware(uint8_t *message)
{
	uint32_t len = TCM_ROMBOOT_HEADER + TOUCH_FW_SIZE;

	message[0] = TCM_CMD_ROMBOOT_DOWNLOAD;
	message[1] = (uint8_t)len;
	message[2] = (uint8_t)(len >> 8);
	for (uint32_t i = 0; i < TCM_ROMBOOT_HEADER; i++) {
		message[TCM_MESSAGE_HEAD + i] = 0;
	}
	message[TCM_MESSAGE_HEAD] = (uint8_t)(TOUCH_FW_SIZE >> 16);

	spi_pins_to_gpio();
	spi_bitbang_xfer(message, NULL, TCM_MESSAGE_HEAD + len, 0);
}

/* The download wants the pause the Android driver puts after it (MODE_SWITCH_DELAY_MS), then go. */
static void tcm_run_firmware(void)
{
	dbg_delay_ms(100);
	tcm_write(TCM_CMD_ROMBOOT_RUN, 0, 0, 0, 0);
}

/* Reset, which the chip answers with an identify report once it is up again. */
static void tcm_reset(void)
{
	pin_set(TCM_RESET_PIN, 0);
	__asm__ volatile("dsb sy");
	dbg_delay_ms(20);                               /* reset-active-ms from the device tree */
	pin_set(TCM_RESET_PIN, 1);
	__asm__ volatile("dsb sy");
	dbg_delay_ms(200);                              /* reset-delay-ms */
}

#endif /* TCM_H */
