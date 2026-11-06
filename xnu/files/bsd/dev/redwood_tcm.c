/*
 * The touch screen of the Infinix Hot 20 (X6826B): an OmniVision TD4160 on SPI2 of the MT6768,
 * as a source of touches for /dev/redwood-touch (sys/redwood_input.h). Only with boot-arg
 * touchscreen=2: every address below belongs to this phone.
 *
 * The controller keeps its firmware in RAM and loses it at every reset, so somebody has to send
 * it a hundred kilobytes before it can say anything -- and that somebody is the loader
 * (mtkloader/tcm.h), which does it on the way here, as Android's driver does on the way to
 * Android. By the time this runs the chip is already up and reporting, and all that is left is
 * to read what it says.
 *
 * There is no clock, pin or SPI framework in this kernel, so the driver reaches the registers
 * itself:
 *   pins     GPIO25 is the chip select, 26 the data out, 27 the clock and 28 the data in, all at
 *            function 2 -- the MT6768 table, not the MT6765 one it is so often confused with;
 *            GPIO1 is the interrupt line, low while the chip has something to say
 *   SPI      the controller in FIFO mode, polled, 32 bytes at a time; it sometimes will not
 *            raise its done flag twice in a row, and then the four pins are driven by hand
 *   chip     a message is a 0xa5 marker, a code, two bytes of length, the payload and a 0x5a;
 *            every chip-select cycle after the first repeats the marker and the code 0x03 for
 *            "more of the same message"
 * No interrupt is used: a thread looks at the interrupt line 125 times a second (boot-arg
 * touch_poll_us=N: every N microseconds instead of 8000), the same way the Goodix driver beside
 * this one does.
 *
 * Register layouts: Linux drivers/spi/spi-mt65xx.c, drivers/pinctrl/mediatek/pinctrl-mtk-mt6768.h,
 * and for the chip's own language the driver the phone ships renamed, which is in the Redmi 12
 * kernel tree under the name of the earlier part: drivers/input/touchscreen/TD4320/.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

#define SPI2_PHYS       0x11012000
#define SPI2_SIZE       0x1000
#define GPIO_PHYS       0x10005000
#define GPIO_SIZE       0x1000

static vm_offset_t spi, gpio;

static inline uint32_t
rd(vm_offset_t base, uint32_t at)
{
	return *(volatile uint32_t *)(base + at);
}

static inline void
wr(vm_offset_t base, uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(base + at) = value;
}

/* ---- the pins ---- */

#define GPIO_DIR0_SET   0x004
#define GPIO_DIR0_CLR   0x008
#define GPIO_DOUT0_SET  0x104
#define GPIO_DOUT0_CLR  0x108
#define GPIO_DIN0       0x200
#define GPIO_MODE3      0x330           /* the function of pins 24 to 31, four bits each */

#define PIN_IRQ         1               /* low while a report waits */
#define PIN_CS          25
#define PIN_MO          26
#define PIN_CLK         27
#define PIN_MI          28
#define PIN_FUNC_SPI    2

static void
pins_to_spi(void)
{
	uint32_t mode = rd(gpio, GPIO_MODE3);

	for (unsigned pin = PIN_CS; pin <= PIN_MI; pin++) {
		unsigned shift = (pin % 8) * 4;

		mode = (mode & ~(0xfu << shift)) | ((uint32_t)PIN_FUNC_SPI << shift);
	}
	wr(gpio, GPIO_MODE3, mode);
	__asm__ volatile ("dsb sy");
}

static void
pins_to_gpio(void)
{
	uint32_t mode = rd(gpio, GPIO_MODE3);

	for (unsigned pin = PIN_CS; pin <= PIN_MI; pin++) {
		mode &= ~(0xfu << ((pin % 8) * 4));
	}
	wr(gpio, GPIO_MODE3, mode);
	wr(gpio, GPIO_DIR0_SET, (1u << PIN_MO) | (1u << PIN_CLK) | (1u << PIN_CS));
	wr(gpio, GPIO_DIR0_CLR, 1u << PIN_MI);
	wr(gpio, GPIO_DOUT0_SET, (1u << PIN_CS) | (1u << PIN_CLK));  /* both idle high in mode 3 */
	__asm__ volatile ("dsb sy");
}

static inline void
pin_set(unsigned pin, bool high)
{
	wr(gpio, high ? GPIO_DOUT0_SET : GPIO_DOUT0_CLR, 1u << pin);
}

static inline bool
pin_get(unsigned pin)
{
	return (rd(gpio, GPIO_DIN0) >> pin) & 1u;
}

/* Long enough for a pad to settle; one register read is already most of a microsecond. */
static inline void
pin_settle(void)
{
	(void)rd(gpio, GPIO_DIN0);
	(void)rd(gpio, GPIO_DIN0);
}

/* ---- the SPI controller ---- */

#define SPI_CFG0        0x00
#define SPI_CFG1        0x04
#define SPI_TX_DATA     0x10
#define SPI_RX_DATA     0x14
#define SPI_CMD         0x18
#define SPI_STATUS0     0x1c
#define SPI_PAD_SEL     0x24
#define SPI_CFG2        0x28

#define SPI_CMD_ACT     (1u << 0)
#define SPI_CMD_RST     (1u << 2)
#define SPI_CMD_CPHA    (1u << 8)
#define SPI_CMD_CPOL    (1u << 9)
#define SPI_CMD_TXMSBF  (1u << 12)
#define SPI_CMD_RXMSBF  (1u << 13)
#define SPI_STATUS0_DONE (1u << 0)
#define SPI_FIFO_BYTES  32

/*
 * The clock's half-periods live in CFG2 on this generation of the controller -- Linux calls it
 * "enhance timing" -- while CFG0 keeps only the chip select's setup and hold. Each field counts
 * ticks of the source clock and is stored one less than it counts, and the loader leaves that
 * source as the 26 MHz crystal, so two ticks a half-period is 6.5 MHz, under the 8 the chip's
 * own device tree allows.
 */
#define SPI_TICKS       2

static void
spi_setup(void)
{
	wr(spi, SPI_CMD, SPI_CMD_RST);
	wr(spi, SPI_CMD, 0);
	wr(spi, SPI_CFG2, ((SPI_TICKS - 1) << 0) | ((SPI_TICKS - 1) << 16));
	wr(spi, SPI_CFG0, ((SPI_TICKS - 1) << 0) | ((SPI_TICKS - 1) << 16));
	wr(spi, SPI_PAD_SEL, 0);
}

static unsigned stalls;

/* One transfer through the FIFO: `length` bytes out of tx (0xff where tx is NULL) and as many in. */
static bool
spi_fifo(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	uint32_t words = (length + 3) / 4;

	if (length == 0 || length > SPI_FIFO_BYTES) {
		return false;
	}
	spi_setup();
	wr(spi, SPI_CMD, SPI_CMD_CPHA | SPI_CMD_CPOL | SPI_CMD_TXMSBF | SPI_CMD_RXMSBF);
	wr(spi, SPI_CFG1, ((length - 1) << 16) | ((SPI_TICKS - 1) << 0));

	for (uint32_t word = 0; word < words; word++) {
		uint32_t value = 0;

		for (uint32_t byte = 0; byte < 4; byte++) {
			uint32_t at = word * 4 + byte;

			value |= (uint32_t)(tx && at < length ? tx[at] : 0xff) << (8 * byte);
		}
		wr(spi, SPI_TX_DATA, value);
	}
	wr(spi, SPI_CMD, rd(spi, SPI_CMD) | SPI_CMD_ACT);

	for (uint32_t spin = 0; !(rd(spi, SPI_STATUS0) & SPI_STATUS0_DONE); spin++) {
		if (spin > 200000) {
			return false;
		}
	}
	for (uint32_t word = 0; word < words; word++) {
		uint32_t value = rd(spi, SPI_RX_DATA);

		for (uint32_t byte = 0; byte < 4; byte++) {
			uint32_t at = word * 4 + byte;

			if (rx && at < length) {
				rx[at] = (uint8_t)(value >> (8 * byte));
			}
		}
	}
	return true;
}

/*
 * The same transfer with the four pins driven by hand, mode 3: the clock idles high, the data
 * changes on the falling edge and is read on the rising one. Slower, and needs nothing of the
 * controller to be right, which is what makes it the answer when the controller sulks.
 */
static void
spi_by_hand(const uint8_t *tx, uint8_t *rx, uint32_t length, uint32_t gap_us)
{
	pins_to_gpio();
	pin_set(PIN_CS, false);
	pin_settle();

	for (uint32_t at = 0; at < length; at++) {
		uint8_t out = tx ? tx[at] : 0xff, in = 0;

		for (int bit = 7; bit >= 0; bit--) {
			pin_set(PIN_MO, (out >> bit) & 1);
			pin_set(PIN_CLK, false);
			pin_settle();
			pin_set(PIN_CLK, true);
			pin_settle();
			in = (uint8_t)((in << 1) | (pin_get(PIN_MI) ? 1 : 0));
		}
		if (rx) {
			rx[at] = in;
		}
		if (gap_us) {
			delay(gap_us);
		}
	}
	pin_set(PIN_CS, true);
	pin_settle();
	pins_to_spi();
}

static void
spi_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	if (spi_fifo(tx, rx, length)) {
		return;
	}
	stalls++;
	spi_by_hand(tx, rx, length, 0);
}

/*
 * Reading is done by hand, with twenty microseconds after every byte: that is the way the loader
 * reads thousands of reports off this chip, while through the controller -- bytes back to back --
 * a run read none, and the controller would not even finish half its transfers.
 */
#define READ_GAP_US     20

static void
spi_read(uint8_t *rx, uint32_t length)
{
	spi_by_hand(NULL, rx, length, READ_GAP_US);
}

/* ---- the chip's language ---- */

#define TCM_MARKER              0xa5
#define TCM_STATUS_CONTINUED    0x03
#define TCM_REPORT_TOUCH        0x11

/*
 * One message in: its code, and as much of its payload as `max` has room for. A chip-select
 * cycle carries at most the FIFO's 32 bytes, so a message comes in pieces and every piece after
 * the first opens with the marker and the code 0x03.
 */
static bool
tcm_read(uint8_t *code, uint8_t *buffer, uint32_t max, uint32_t *length)
{
	uint8_t head[4], piece[SPI_FIFO_BYTES];
	uint32_t room = SPI_FIFO_BYTES - 2, left, at = 0;

	spi_read(head, sizeof(head));
	if (head[0] != TCM_MARKER) {
		return false;
	}
	*code = head[1];
	*length = (uint32_t)head[2] | ((uint32_t)head[3] << 8);
	left = *length + 1;                     /* the payload, and the byte that closes it */

	while (left > 0) {
		uint32_t want = left > room ? room : left;

		spi_read(piece, want + 2);
		if (piece[0] != TCM_MARKER || piece[1] != TCM_STATUS_CONTINUED) {
			return false;
		}
		for (uint32_t i = 0; i < want; i++) {
			if (at + i < max) {
				buffer[at + i] = piece[2 + i];
			}
		}
		at += want;
		left -= want;
	}
	return true;
}

/* ---- what a touch report holds ---- */

/*
 * The layout the chip describes when asked, and the one it then uses: a byte for the capacitive
 * buttons, and after it, for every finger it can see, eleven bytes --
 *
 *   which finger       4 bits
 *   what it is         4 bits  (0 lifted, 1 a finger, 2 a gloved one)
 *   x                 12 bits
 *   y                 12 bits
 *   pressure           8 bits
 *   width across       8 bits
 *   width down         8 bits
 *   column            16 bits
 *   row               16 bits
 *
 * -- and the report runs as long as it has fingers to talk about. Bits run from the least
 * significant and on across the byte boundaries.
 */
#define FINGER_BITS     88
#define PANEL_W         720
#define PANEL_H         1640
#define MAX_FINGERS     10
/*
 * The display driver shows the iPhone's picture at its own proportions, 720 x 1559 in the middle of
 * the panel (MiniIOMobileFramebuffer.cpp), so a finger is placed within that band: above or below
 * it counts as its edge.
 */
#define PICTURE_H       (PANEL_W * 2436 / 1125)
#define PICTURE_TOP     ((PANEL_H - PICTURE_H) / 2)

static uint32_t fit;                    /* boot-arg fbfit: how the picture sits on the panel */

/* fbfit=2: the picture fills the height, PICTURE_W wide, its sides cut off by PICTURE_CROP each. */
#define PICTURE_W       (PANEL_H * 1125 / 2436)
#define PICTURE_CROP    ((PICTURE_W - PANEL_W) / 2)

/* fbfit=3 (with fbscale=2): the picture one to one, 750 x 1624, 15 columns cut at each side, 8 rows above */
#define ONE_W           750
#define ONE_H           1624
#define ONE_CROP        ((ONE_W - PANEL_W) / 2)
#define ONE_TOP         ((PANEL_H - ONE_H) / 2)

static uint32_t
picture_x(uint32_t x)
{
	if (fit == 3) {
		return (x + ONE_CROP) * 65535 / (ONE_W - 1);
	}
	if (fit != 2) {
		return x * 65535 / (PANEL_W - 1);
	}
	return (x + PICTURE_CROP) * 65535 / (PICTURE_W - 1);
}

static uint32_t
picture_y(uint32_t y)
{
	if (fit == 3) {
		y = y <= ONE_TOP ? 0 : y - ONE_TOP;
		return y >= ONE_H ? 65535 : y * 65535 / (ONE_H - 1);
	}
	if (fit != 1) {
		return y * 65535 / (PANEL_H - 1);
	}
	if (y <= PICTURE_TOP) {
		return 0;
	}
	y -= PICTURE_TOP;
	return y >= PICTURE_H ? 65535 : y * 65535 / (PICTURE_H - 1);
}

static uint32_t
bits(const uint8_t *buffer, uint32_t at, uint32_t count)
{
	uint32_t value = 0;

	for (uint32_t i = 0; i < count; i++) {
		uint32_t bit = at + i;

		value |= (uint32_t)((buffer[bit / 8] >> (bit % 8)) & 1u) << i;
	}
	return value;
}

static bool down[MAX_FINGERS];
static unsigned reports, touches;
static uint32_t last_x, last_y;

/*
 * A report turned into what /dev/redwood-touch wants: the position across the panel rather than
 * in the panel's own pixels, and a landing and a lifting of its own rather than a state to be
 * compared. A finger the report no longer mentions has lifted.
 */
static void
report(const uint8_t *buffer, uint32_t length)
{
	bool seen[MAX_FINGERS] = { false };
	uint32_t at = 8;                        /* the button byte comes first */

	while (at + FINGER_BITS <= length * 8) {
		uint32_t which = bits(buffer, at, 4);
		uint32_t kind = bits(buffer, at + 4, 4);
		uint32_t x = bits(buffer, at + 8, 12);
		uint32_t y = bits(buffer, at + 20, 12);

		at += FINGER_BITS;
		if (which >= MAX_FINGERS || kind == 0) {
			continue;               /* that one has lifted, or is not one we can carry */
		}
		if (x >= PANEL_W) {
			x = PANEL_W - 1;
		}
		if (y >= PANEL_H) {
			y = PANEL_H - 1;
		}
		last_x = x;
		last_y = y;
		seen[which] = true;
		touches++;
		redwood_touch_post(down[which] ? REDWOOD_TOUCH_MOVE : REDWOOD_TOUCH_DOWN, which,
		    picture_x(x), picture_y(y));
		down[which] = true;
	}
	for (unsigned which = 0; which < MAX_FINGERS; which++) {
		if (down[which] && !seen[which]) {
			redwood_touch_post(REDWOOD_TOUCH_UP, which,
			    picture_x(last_x), picture_y(last_y));
			down[which] = false;
		}
	}
	reports++;
}

/* ---- the thread that watches the line ---- */

static void
tcm_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint8_t buffer[256], code = 0;
	uint32_t length = 0, poll_us = 8000;

	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	PE_parse_boot_argn("fbfit", &fit, sizeof(fit));
	if (poll_us < 250 || poll_us > 100000) {
		poll_us = 8000;
	}
	spi = ml_io_map(SPI2_PHYS, SPI2_SIZE);
	gpio = ml_io_map(GPIO_PHYS, GPIO_SIZE);
	pins_to_spi();
	spi_setup();
	printf("tcm: the touch screen's interrupt line is looked at every %u us\n", poll_us);

	for (unsigned tick = 1;; tick++) {
		/*
		 * Nothing is read unless the chip asks: reading one that has nothing to say puts its
		 * side of the bus out of step, and then the next real report cannot be read either.
		 */
		while (!pin_get(PIN_IRQ)) {
			if (!tcm_read(&code, buffer, sizeof(buffer), &length)) {
				break;
			}
			if (code == TCM_REPORT_TOUCH) {
				report(buffer, length);
			}
		}
		if (tick % (60 * (1000000 / poll_us)) == 0) {
			printf("tcm: so far %u reports and %u finger positions, the last at %u, %u; "
			    "the interrupt line is %s; %u transfers the controller would not finish\n",
			    reports, touches, last_x, last_y, pin_get(PIN_IRQ) ? "idle" : "asserted", stalls);
		}
		delay(poll_us);
	}
}

void
redwood_tcm_init(void)
{
	uint32_t which = 0;
	thread_t thread;

	/* 2 is this phone; 1 is the other one's Goodix, which has a driver of its own beside this. */
	if (!PE_parse_boot_argn("touchscreen", &which, sizeof(which)) || which != 2) {
		return;
	}
	if (kernel_thread_start(tcm_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("tcm: cannot start the thread that reads the touch screen\n");
	}
}
