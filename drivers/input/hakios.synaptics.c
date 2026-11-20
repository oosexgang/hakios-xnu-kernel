/*
 * The touch screen of the Redmi Note 13 4G NFC (sapphiren, Snapdragon 685 "khaje"): a Synaptics controller speaking the "TCM"
 * protocol over I2C (address 0x20), as a source of touches for /dev/hakios-touch (sys/hakios.input.h). Only with boot-arg
 * synaptics_tcm=1: every pin below belongs to this phone.
 *
 * The phone's own Android device tree (i2c@4a88000 -- serial engine 2 of the QUP -- with synaptics_tcm@20, and the pin groups
 * qupv3_se2_i2c_active, ts_int_active, ts_reset_active, ts_avdd) says:
 *   SDA, SCL    GPIO 6, 7 (function "qup2")                   -- here driven by hand, as open-drain pins, 100 kHz or so
 *   interrupt   GPIO 80, low while the chip has a message     -- here looked at 125 times a second, no interrupt is used
 *   reset       GPIO 86, active low, 20 ms, then 200 ms before the first message
 *   analog      GPIO 36, high = the chip's analog supply on (the node "ts_avdd")
 * There is no clock, pin or I2C framework in this kernel, and nothing here touches the serial engine: the pins are plain GPIOs, so
 * the engine's clocks and firmware do not matter. The register layout of a pin is the TLMM's (U-Boot drivers/pinctrl/qcom/
 * pinctrl-sm6115.c: tile WEST at 0x500000, EAST at 0xd00000, 0x1000 a pin; Linux drivers/pinctrl/qcom/pinctrl-sm6115.c).
 *
 * TCM (Synaptics' "touch communication module"; Linux drivers/input/touchscreen/synaptics_tcm/, from memory of its message
 * format -- this driver logs what it reads so that a difference shows):
 *   a message from the chip is  0xA5, code, length (2 bytes, little endian), `length` bytes, 0x5A
 *   a command to the chip is    command, length (2 bytes), `length` bytes
 *   codes: 0x01 OK (answer), 0x03 continued read, 0x10 identify, 0x11 touch report ...
 * After a reset the chip says IDENTIFY (mode 1 = application firmware). Then: application info (the panel's extent), the touch
 * report's own description (a list of fields and their widths: it is read from the chip and the report decoded by it), the touch
 * reports switched on, and a thread decodes every report: a finger by index, with position, into hakios_touch_post().
 * No firmware is loaded: a chip that is not in application mode is reported and left alone.
 *
 * The phone's device tree lists three controllers on this bus (Synaptics TCM at 0x20, Novatek at 0x62, FocalTech FTS at 0x38) and
 * the bootloader picks one for the panel that is fitted; the first run on the phone found 0x38 answering. So the driver tells
 * them apart by which address acknowledges, and for the FocalTech FTS (Linux drivers/input/touchscreen/focaltech_touch/, from
 * memory of its register map) reads the touch registers by hand: register 0x01 gives the gesture, 0x02 the number of fingers (low
 * four bits), then 6 bytes a finger: event (2 bits: 0 down, 1 up, 2 contact), x and y (12 integer bits plus two fractional
 * bits in byte 4), the finger's id (4 bits), and area. This FT3519T reports 0..8639 x 0..9599 for the 1080 x 2400 panel.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define TLMM_WEST_PHYS          0x00500000UL
#define TLMM_EAST_PHYS          0x00d00000UL
#define TLMM_TILE_SIZE          0x00100000UL

#define PIN_SDA                 6
#define PIN_SCL                 7
#define PIN_INT                 80      /* low = the chip has a message */
#define PIN_RESET               86      /* low = in reset */
#define PIN_AVDD                36      /* high = analog supply on (an EAST pin) */

#define I2C_ADDRESS             0x20

static vm_offset_t tlmm_west, tlmm_east;
static uint64_t ticks_per_us;
static unsigned sda = PIN_SDA, scl = PIN_SCL;

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

static vm_offset_t
pin_base(unsigned pin)
{
	return (pin == PIN_AVDD ? tlmm_east : tlmm_west) + 0x1000UL * pin;
}

#define PULL_NONE 0
#define PULL_UP 3

/* [1:0] pull, [5:2] function, [8:6] drive, [9] output enable */
static void
gpio_configure(unsigned pin, unsigned function, unsigned pull, unsigned ma, bool output)
{
	wr(pin_base(pin), pull | function << 2 | (ma / 2 - 1) << 6 | (output ? 1u << 9 : 0));
}

static void
gpio_set(unsigned pin, bool high)
{
	wr(pin_base(pin) + 4, high ? 2 : 0);
}

static bool
gpio_get(unsigned pin)
{
	return rd(pin_base(pin) + 4) & 1;
}

static void
gpio_output(unsigned pin, bool high, unsigned pull, unsigned ma)
{
	gpio_set(pin, high);            /* the level first, so that the pin never drives the wrong one */
	gpio_configure(pin, 0, pull, ma, true);
}

static void
spin_us(uint32_t us)
{
	uint64_t t0 = mach_absolute_time(), span = (uint64_t)us * ticks_per_us;

	while (mach_absolute_time() - t0 < span) {
	}
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- I2C by hand: open-drain, the pins' own pull-ups and the board's ------------------------------ */

#define HALF 4          /* microseconds; the writes to the pins take about as long again */

static void
line_low(unsigned pin)
{
	gpio_set(pin, false);
	gpio_configure(pin, 0, PULL_UP, 2, true);
}

static void
line_release(unsigned pin)
{
	gpio_configure(pin, 0, PULL_UP, 2, false);
}

static bool
scl_release(void)
{
	line_release(scl);
	for (int i = 0; i < 2000; i++) {        /* clock stretching */
		if (gpio_get(scl)) {
			return true;
		}
		spin_us(1);
	}
	return false;
}

static void
i2c_start(void)
{
	line_release(sda);
	scl_release();
	spin_us(HALF);
	line_low(sda);
	spin_us(HALF);
	line_low(scl);
	spin_us(HALF);
}

static void
i2c_stop(void)
{
	line_low(sda);
	spin_us(HALF);
	scl_release();
	spin_us(HALF);
	line_release(sda);
	spin_us(HALF * 2);
}

static bool
i2c_write_byte(uint8_t byte)    /* true if the chip acknowledged */
{
	for (int bit = 7; bit >= 0; bit--) {
		if (byte >> bit & 1) {
			line_release(sda);
		} else {
			line_low(sda);
		}
		spin_us(HALF);
		scl_release();
		spin_us(HALF);
		line_low(scl);
	}
	line_release(sda);
	spin_us(HALF);
	scl_release();
	spin_us(HALF);
	bool ack = !gpio_get(sda);
	line_low(scl);
	spin_us(HALF);
	return ack;
}

static uint8_t
i2c_read_byte(bool ack)
{
	uint8_t byte = 0;

	line_release(sda);
	for (int bit = 0; bit < 8; bit++) {
		spin_us(HALF);
		scl_release();
		spin_us(HALF);
		byte = (uint8_t)(byte << 1 | gpio_get(sda));
		line_low(scl);
	}
	if (ack) {
		line_low(sda);
	} else {
		line_release(sda);
	}
	spin_us(HALF);
	scl_release();
	spin_us(HALF);
	line_low(scl);
	line_release(sda);
	spin_us(HALF);
	return byte;
}

static bool
i2c_present(uint8_t address)
{
	bool ack;

	i2c_start();
	ack = i2c_write_byte((uint8_t)(address << 1));
	i2c_stop();
	return ack;
}

/* ---- TCM -------------------------------------------------------------------------------------------- */

#define TCM_MARKER              0xa5
#define TCM_PADDING             0x5a
#define TCM_MAX_PAYLOAD         1024

#define CODE_IDLE               0x00
#define CODE_OK                 0x01
#define CODE_CONTINUED          0x03
#define CODE_IDENTIFY           0x10
#define CODE_TOUCH              0x11

#define CMD_IDENTIFY            0x02
#define CMD_RESET               0x04
#define CMD_ENABLE_REPORT       0x05
#define CMD_GET_APPLICATION_INFO 0x20
#define CMD_GET_TOUCH_REPORT_CONFIG 0x25

#define MODE_APPLICATION        0x01

struct tcm_message {
	uint8_t code;
	uint16_t length;
	uint8_t payload[TCM_MAX_PAYLOAD];
};

static bool
tcm_send(uint8_t command, const uint8_t *payload, uint16_t length)
{
	bool ok = true;

	i2c_start();
	ok = ok && i2c_write_byte(I2C_ADDRESS << 1);
	ok = ok && i2c_write_byte(command);
	ok = ok && i2c_write_byte((uint8_t)length);
	ok = ok && i2c_write_byte((uint8_t)(length >> 8));
	for (uint16_t i = 0; ok && i < length; i++) {
		ok = i2c_write_byte(payload[i]);
	}
	i2c_stop();
	return ok;
}

/* one message: 0 read, -1 the chip did not answer, -2 not a message (the first bytes are in *raw) */
static int
tcm_read(struct tcm_message *m, uint8_t raw[4])
{
	i2c_start();
	if (!i2c_write_byte((I2C_ADDRESS << 1) | 1)) {
		i2c_stop();
		return -1;
	}
	for (int i = 0; i < 4; i++) {
		raw[i] = i2c_read_byte(true);
	}
	if (raw[0] != TCM_MARKER) {
		i2c_read_byte(false);
		i2c_stop();
		return -2;
	}
	m->code = raw[1];
	m->length = (uint16_t)(raw[2] | raw[3] << 8);
	if (m->length > TCM_MAX_PAYLOAD) {
		i2c_read_byte(false);
		i2c_stop();
		return -2;
	}
	for (uint16_t i = 0; i < m->length; i++) {
		m->payload[i] = i2c_read_byte(true);
	}
	(void)i2c_read_byte(false);     /* the padding byte, 0x5a */
	i2c_stop();
	return 0;
}

static bool
wait_interrupt(uint32_t ms)     /* the line goes low when the chip has a message */
{
	for (uint32_t t = 0; t < ms; t++) {
		if (!gpio_get(PIN_INT)) {
			return true;
		}
		sleep_ms(1);
	}
	return false;
}

static void
hex(const char *what, const uint8_t *p, unsigned n)
{
	char line[3 * 24 + 1];
	unsigned i, o = 0;

	for (i = 0; i < n && i < 24; i++) {
		o += (unsigned)snprintf(line + o, sizeof(line) - o, "%02x ", p[i]);
	}
	line[o] = 0;
	printf("synaptics: %s (%u bytes): %s%s\n", what, n, line, n > 24 ? "..." : "");
}

/* a message of the code asked for, or any, within `ms`; the others (reports) are dropped */
static bool
tcm_wait(struct tcm_message *m, int want, uint32_t ms)
{
	uint8_t raw[4];

	for (uint32_t waited = 0; waited < ms;) {
		if (wait_interrupt(20)) {
			int r = tcm_read(m, raw);

			if (r == 0 && (want < 0 || m->code == want)) {
				return true;
			}
			if (r != 0) {
				printf("synaptics: read %d (%02x %02x %02x %02x)\n", r, raw[0], raw[1], raw[2], raw[3]);
			}
		}
		waited += 20;
	}
	return false;
}

static bool fts_chip;
static uint8_t report_config[256];
static unsigned report_config_size;
static unsigned max_x = 1080, max_y = 2400;

struct finger {
	bool down;
	uint32_t x, y;
};
#define MAX_FINGERS 10
static struct finger fingers[MAX_FINGERS];
static uint32_t reports, bad;

/* touch report fields (Synaptics TCM): the report is described by pairs "code, width in bits" */
enum {
	T_END = 0, T_FOREACH_ACTIVE, T_FOREACH_OBJECT, T_FOREACH_END, T_PAD, T_TIMESTAMP, T_INDEX, T_CLASS, T_X, T_Y, T_Z,
	T_XW, T_YW, T_TX, T_RX, T_BUTTONS, T_DOUBLE_TAP, T_FRAME_RATE, T_POWER_IM, T_CID_IM, T_RAIL_IM, T_CID_VAR,
	T_NSM_FREQ, T_NSM_STATE, T_NUM_ACTIVE, T_CPU_CYCLES,
};

static uint32_t
get_bits(const uint8_t *data, unsigned size, unsigned bit_offset, unsigned width)
{
	uint32_t value = 0;

	for (unsigned i = 0; i < width && i < 32; i++) {
		unsigned bit = bit_offset + i;

		if (bit / 8 < size && (data[bit / 8] >> (bit % 8) & 1)) {
			value |= 1u << i;
		}
	}
	return value;
}

static void
touch_decode(const uint8_t *data, unsigned size)
{
	unsigned bits = 0, idx = 0, loop_start = 0, objects = 0, object = 0;
	bool in_loop = false, active_loop = false;
	struct {
		bool present;
		uint32_t x, y, cls;
	} seen[MAX_FINGERS];

	memset(seen, 0, sizeof(seen));
	while (idx < report_config_size) {
		uint8_t code = report_config[idx++], width = 0;

		switch (code) {
		case T_END:
			goto finished;
		case T_FOREACH_ACTIVE:
		case T_FOREACH_OBJECT:
			in_loop = true;
			active_loop = code == T_FOREACH_ACTIVE;
			loop_start = idx;
			object = 0;
			continue;
		case T_FOREACH_END:
			if (in_loop && ++object < (active_loop ? objects : MAX_FINGERS) && object < MAX_FINGERS) {
				idx = loop_start;
			} else {
				in_loop = false;
			}
			continue;
		case T_PAD:
			bits = (bits + 7) & ~7u;
			continue;
		default:
			width = idx < report_config_size ? report_config[idx++] : 0;
			break;
		}
		uint32_t v = get_bits(data, size, bits, width);

		bits += width;
		switch (code) {
		case T_NUM_ACTIVE:
			objects = v;
			break;
		case T_INDEX:
			if (in_loop && v < MAX_FINGERS) {
				seen[object].present = true;     /* (the slot is the object's number in this report) */
			}
			break;
		case T_CLASS:
			if (in_loop && object < MAX_FINGERS) {
				seen[object].cls = v;
				seen[object].present = true;
			}
			break;
		case T_X:
			if (in_loop && object < MAX_FINGERS) {
				seen[object].x = v;
			}
			break;
		case T_Y:
			if (in_loop && object < MAX_FINGERS) {
				seen[object].y = v;
			}
			break;
		default:
			break;
		}
	}
finished:
	/* object k of this report is finger k: a finger that was down and is not any more has lifted */
	for (unsigned f = 0; f < MAX_FINGERS; f++) {
		bool now = seen[f].present && seen[f].cls != 0 && (!active_loop || f < objects || objects == 0);
		uint32_t x = seen[f].x > max_x ? max_x : seen[f].x, y = seen[f].y > max_y ? max_y : seen[f].y;
		uint32_t px = (uint32_t)((uint64_t)x * 65535 / (max_x ? max_x : 1)), py = (uint32_t)((uint64_t)y * 65535 / (max_y ? max_y : 1));

		if (now && !fingers[f].down) {
			hakios_touch_post(HAKIOS_TOUCH_DOWN, f, px, py);
			fingers[f].down = true;
		} else if (now && (fingers[f].x != px || fingers[f].y != py)) {
			hakios_touch_post(HAKIOS_TOUCH_MOVE, f, px, py);
		} else if (!now && fingers[f].down) {
			hakios_touch_post(HAKIOS_TOUCH_UP, f, fingers[f].x, fingers[f].y);
			fingers[f].down = false;
		}
		if (now) {
			fingers[f].x = px;
			fingers[f].y = py;
		}
	}
}

static char fts_note[320];
static unsigned fts_note_len;
#define FTS_NOTE(...) do { \
	if (fts_note_len + 1 < sizeof(fts_note)) { \
		int n_ = snprintf(fts_note + fts_note_len, sizeof(fts_note) - fts_note_len, __VA_ARGS__); \
		if (n_ > 0) { fts_note_len += (unsigned)n_; } \
		if (fts_note_len >= sizeof(fts_note)) { fts_note_len = (unsigned)sizeof(fts_note) - 1; } \
	} \
} while (0)

/* ---- FocalTech FTS (address 0x38) ---------------------------------------------------------------- */

#define FTS_ADDRESS             0x38
#define FTS_REG_CHIP_ID         0xa3
#define FTS_REG_FW_VER          0xa6
#define FTS_REG_VENDOR_ID       0xa8
#define FTS_REG_CHIP_ID2        0x9f
#define FTS_RAW_MAX_X           8639    /* 2160 * 4 - 1: two fractional bits */
#define FTS_RAW_MAX_Y           9599    /* 2400 * 4 - 1 */

static bool
fts_read(uint8_t reg, uint8_t *buf, unsigned n)
{
	bool ok;

	i2c_start();
	ok = i2c_write_byte(FTS_ADDRESS << 1) && i2c_write_byte(reg);
	if (ok) {
		i2c_start();                    /* a repeated start */
		ok = i2c_write_byte((FTS_ADDRESS << 1) | 1);
	}
	for (unsigned i = 0; ok && i < n; i++) {
		buf[i] = i2c_read_byte(i + 1 < n);
	}
	i2c_stop();
	return ok;
}

/*
 * The touch registers: 0x01.. (gesture, count, then 6 bytes a finger). Android's driver always reads the whole block in one go --
 * FTS_TOUCH_DATA_LEN (10 fingers x 6 + 3) plus the gesture area, 91 bytes -- and a shorter read made this chip
 * present a new frame only every 366 ms (the traces of 8 November: frames 366 ms apart, a swipe was five points). So the block is read
 * whole and only the fingers it counts are handed on.
 */
#define FTS_BLOCK       91
static bool
fts_read_touches(uint8_t *buf, unsigned *count)
{
	uint8_t block[FTS_BLOCK];
	bool ok;

	i2c_start();
	ok = i2c_write_byte(FTS_ADDRESS << 1) && i2c_write_byte(0x01);
	if (ok) {
		i2c_start();
		ok = i2c_write_byte((FTS_ADDRESS << 1) | 1);
	}
	if (!ok) {
		i2c_stop();
		return false;
	}
	for (unsigned i = 0; i < FTS_BLOCK; i++) {
		block[i] = i2c_read_byte(i + 1 < FTS_BLOCK);
	}
	i2c_stop();
	buf[0] = block[0];
	buf[1] = block[1];
	*count = block[1] & 0x0f;
	if (*count > MAX_FINGERS) {
		*count = 0;
		return false;
	}
	memcpy(buf + 2, block + 2, 6 * *count);
	return true;
}

/* One line for each touch, to see what the chip really sends during a swipe: how long, how many frames, how many different positions, the longest silence. */
struct fts_stroke {
	uint64_t t0, last, longest_gap;
	uint32_t frames, positions, x0, y0, x1, y1;
};
static struct fts_stroke fts_stroke[MAX_FINGERS];
static uint32_t fts_strokes_shown;

static void
fts_stroke_frame(unsigned f, uint32_t x, uint32_t y, bool first)
{
	struct fts_stroke *k = &fts_stroke[f];
	uint64_t now = mach_absolute_time();

	if (first) {
		memset(k, 0, sizeof(*k));
		k->t0 = now;
		k->x0 = x;
		k->y0 = y;
		k->positions = 1;
	} else {
		if (now - k->last > k->longest_gap) {
			k->longest_gap = now - k->last;
		}
		k->positions += (x != k->x1 || y != k->y1);
	}
	k->last = now;
	k->frames++;
	k->x1 = x;
	k->y1 = y;
}

static void
fts_stroke_end(unsigned f, const char *how)
{
	struct fts_stroke *k = &fts_stroke[f];
	uint64_t ns, gap_ns;

	if (fts_strokes_shown++ >= 80 || k->frames == 0) {
		return;
	}
	absolutetime_to_nanoseconds(k->last - k->t0, &ns);
	absolutetime_to_nanoseconds(k->longest_gap, &gap_ns);
	printf("synaptics: FocalTech: touch %u over (%s): %u frames in %u ms, %u positions, longest silence %u ms, from %u,%u to %u,%u\n",
	    f, how, k->frames, (unsigned)(ns / 1000000), k->positions, (unsigned)(gap_ns / 1000000), k->x0, k->y0, k->x1, k->y1);
	k->frames = 0;
}

static void
fts_thread(void)
{
	uint8_t buf[2 + 6 * MAX_FINGERS];
	uint32_t looks = 0, touches = 0, bad_reads = 0, irq_low = 0, valid = 0, invalid = 0, n_down = 0, n_move = 0, n_up = 0, n_empty = 0, n_stale = 0;
	uint64_t last_seen[MAX_FINGERS] = { 0 }, pending_since[MAX_FINGERS] = { 0 }, lift_since[MAX_FINGERS] = { 0 }, confirm_after, release_after, lift_hold, starved_after, previous_look = 0, longest_gap = 0;
	uint32_t n_multi = 0;
	uint32_t n_starved = 0;
	bool seen[MAX_FINGERS];

	nanoseconds_to_absolutetime(400 * NSEC_PER_MSEC, &release_after);
	nanoseconds_to_absolutetime(40 * NSEC_PER_MSEC, &confirm_after);
	/* a lift is believed only when the finger stays away for this long: the chip loses a finger for a frame or two
	 * and gets it back (with the same id), which read as a lift and a new touch -- two taps for one press, on the keyboard */
	nanoseconds_to_absolutetime(35 * NSEC_PER_MSEC, &lift_hold);
	nanoseconds_to_absolutetime(20 * NSEC_PER_MSEC, &starved_after);
	/*
	 * The chip's touch registers hold 0xff while it has no frame (the first run on the phone read 00 ff ff ... at rest), and
	 * a frame is announced by the interrupt line going low, for how long is not known. So the line is looked at every
	 * millisecond and a frame read whenever it is low, and in any case every sixteenth look; a finger is taken to have lifted
	 * when a frame says so or when 400 ms pass without a valid one. With the whole block read (fts_read_touches) the chip does say when a finger lifts (a frame without it), and
	 * it goes quiet for ~250 ms at the start of a stroke: 80 ms cut every swipe in two (a dot, a gap, then the line: Freeform, 8 November).
	 */
	for (;; sleep_ms(1)) {
		unsigned count = 0;
		bool low = !gpio_get(PIN_INT);
		uint64_t now_look = mach_absolute_time();

		/* When this thread was not run for a while (the system is busy), the time without frames was not the chip's silence: do not count it
		 * against the fingers that are down. */
		if (previous_look != 0 && now_look - previous_look > starved_after) {
			uint64_t gap = now_look - previous_look;

			n_starved++;
			longest_gap = gap > longest_gap ? gap : longest_gap;
			for (unsigned f = 0; f < MAX_FINGERS; f++) {
				if (fingers[f].down) {
					last_seen[f] += gap;
				}
			}
		}
		previous_look = now_look;
		looks++;
		irq_low += low;
		if (looks % 10000 == 0) {
			printf("synaptics: FocalTech %s: %u looks, interrupt low in %u, %u valid frames (%u empty), %u without data, %u finger positions, %u bad reads; posted %u down, %u move, %u up; thread starved %u times, %u ms at most; %u stale reads ignored\n", fts_note,
			    looks, irq_low, valid, n_empty, invalid, touches, bad_reads, n_down, n_move, n_up, n_starved, (unsigned)({ uint64_t ns_; absolutetime_to_nanoseconds(longest_gap, &ns_); ns_ / 1000000; }), n_stale);
		}
		if (low || looks % 16 == 0) {
			if (!fts_read_touches(buf, &count)) {
				/* (the count nibble was above the maximum: no frame) */
				invalid++;
				if (!low) {
					goto lifted;
				}
				if (bad_reads++ < 4) {
					printf("synaptics: FocalTech: the interrupt line is low and the registers hold no frame (%02x %02x)\n", buf[0], buf[1]);
				}
				goto lifted;
			}
			valid++;
			n_empty += count == 0;
			{
				/* a trace of the frames that differ from the one before, to see what a swipe really is on the wire */
				static uint8_t last_raw[2 + 6 * MAX_FINGERS];
				static unsigned last_len;
				static uint64_t last_time;
				static uint32_t traced;
				unsigned len = 2 + 6 * count;

				if (traced < 700 && (len != last_len || memcmp(last_raw, buf, len) != 0)) {
					uint64_t now_t = mach_absolute_time(), dt_ns;

					absolutetime_to_nanoseconds(now_t - last_time, &dt_ns);
					printf("tr %u: +%u us int=%d n=%u", traced, (unsigned)(dt_ns / 1000), low, count);
					for (unsigned i = 0; i < count && i < 3; i++) {
						const uint8_t *q = buf + 2 + 6 * i;

						printf(" [id%u ev%u %u,%u]", q[2] >> 4, q[0] >> 6, ((q[0] & 0xf) << 8 | q[1]) << 2 | q[4] >> 6,
						    ((q[2] & 0xf) << 8 | q[3]) << 2 | (q[4] >> 4 & 3));
					}
					printf("\n");
					traced++;
					last_time = now_t;
					last_len = len;
					memcpy(last_raw, buf, len);
				}
			}
			if (!low && count != 0) {
				/* read without the chip's call: its registers keep the last report after a lift, so this is a stale one -- not proof that a
				 * finger is down (taking it for one held a lifted finger "down" for seconds, and a tap became a long press) */
				n_stale++;
				goto lifted;
			}
			memset(seen, 0, sizeof(seen));
			for (unsigned i = 0; i < count; i++) {
				const uint8_t *p = buf + 2 + 6 * i;
				unsigned event = p[0] >> 6, id = p[2] >> 4;
				uint32_t whole_x = (uint32_t)(p[0] & 0x0f) << 8 | p[1];
				uint32_t whole_y = (uint32_t)(p[2] & 0x0f) << 8 | p[3];
				uint32_t x = whole_x << 2 | p[4] >> 6;
				uint32_t y = whole_y << 2 | (p[4] >> 4 & 3);

				if (id >= MAX_FINGERS || event == 3) {
					continue;
				}
				if (touches < 16) {
					printf("synaptics: FocalTech: finger %u event %u at %u, %u (raw %u.%u, %u.%u)\n", id, event,
					    x, y, whole_x, p[4] >> 6, whole_y, p[4] >> 4 & 3);
				}
				touches++;
				if (x > FTS_RAW_MAX_X) x = FTS_RAW_MAX_X;
				if (y > FTS_RAW_MAX_Y) y = FTS_RAW_MAX_Y;
				uint32_t px = (uint32_t)((uint64_t)x * 65535 / FTS_RAW_MAX_X);
				uint32_t py = (uint32_t)((uint64_t)y * 65535 / FTS_RAW_MAX_Y);

				if (event == 1) {                            /* the chip says it lifted */
					continue;
				}
				seen[id] = true;
				last_seen[id] = mach_absolute_time();
				lift_since[id] = 0;                  /* (it is back: the lift was a dropout) */
				if (!fingers[id].down) {
					/* a finger that arrives while another is down must last 40 ms before it counts: the ghosts of this panel (a second
					 * touch with nobody's finger on it) come and go in a frame or two */
					bool others = false;

					for (unsigned o = 0; o < MAX_FINGERS; o++) {
						others = others || (o != id && fingers[o].down);
					}
					if (pending_since[id] == 0) {
						pending_since[id] = mach_absolute_time();
						if (n_multi++ < 40 && others) {
							printf("synaptics: FocalTech: a further finger %u appears at %u, %u (frame count %u)\n", id, x, y, count);
						}
					}
					if (others && mach_absolute_time() - pending_since[id] < confirm_after) {
						continue;
					}
					pending_since[id] = 0;
				}
				fts_stroke_frame(id, x, y, !fingers[id].down);
				if (!fingers[id].down) {
					hakios_touch_post(HAKIOS_TOUCH_DOWN, id, px, py);
					if (n_down++ < 60) {
						printf("synaptics: FocalTech: DOWN %u at %u, %u (frame count %u, event %u)\n", id, x, y, count, event);
					}
					fingers[id].down = true;
				} else if (fingers[id].x != px || fingers[id].y != py) {
					hakios_touch_post(HAKIOS_TOUCH_MOVE, id, px, py);
					n_move++;
				}
				fingers[id].x = px;
				fingers[id].y = py;
			}
			for (unsigned f = 0; f < MAX_FINGERS; f++) {
				if (!seen[f] && !fingers[f].down) {
					pending_since[f] = 0;           /* (not in this frame: a ghost that has gone) */
				}
			}
			/* a valid frame with fewer fingers than before: those it does not list have lifted */
			for (unsigned f = 0; f < MAX_FINGERS; f++) {
				if (fingers[f].down && !seen[f]) {
					if (lift_since[f] == 0) {
						lift_since[f] = mach_absolute_time();
					}
					if (mach_absolute_time() - lift_since[f] < lift_hold) {
						continue;                   /* wait: it may be a dropout; the loop below ends it if no frame comes */
					}
					lift_since[f] = 0;
					if (n_up++ < 60) {
						printf("synaptics: FocalTech: UP %u at %u, %u (frame count %u)\n", f, fingers[f].x, fingers[f].y, count);
					}
					fts_stroke_end(f, "frame");
					hakios_touch_post(HAKIOS_TOUCH_UP, f, fingers[f].x, fingers[f].y);
					fingers[f].down = false;
				}
			}
		}
lifted:
		/* ... and a finger not heard of for 400 ms has lifted as well */
		for (unsigned f = 0; f < MAX_FINGERS; f++) {
			if (fingers[f].down && lift_since[f] != 0 && mach_absolute_time() - lift_since[f] >= lift_hold) {
				lift_since[f] = 0;
				fts_stroke_end(f, "lift");
				hakios_touch_post(HAKIOS_TOUCH_UP, f, fingers[f].x, fingers[f].y);
				fingers[f].down = false;
				continue;
			}
			if (fingers[f].down && mach_absolute_time() - last_seen[f] > release_after) {
				static unsigned timeouts;

				if (timeouts++ < 8) {
					printf("synaptics: FocalTech: finger %u lifted by the timeout (no frame for 400 ms)\n", f);
				}
				fts_stroke_end(f, "timeout");
				hakios_touch_post(HAKIOS_TOUCH_UP, f, fingers[f].x, fingers[f].y);
				fingers[f].down = false;
			}
		}
	}
}


/* ---- FocalTech FT3519T: loading the controller's firmware ------------------------------------------
 * After a cold start the chip answers 0xef to everything: its application is gone.  Android's driver (focaltech_ts_i2c) fixes that
 * by putting the chip in its boot ROM, loading a small RAM program (the "pramboot"), and with it erasing and writing the
 * application into the chip's flash.  The sequence below is that driver's, read from the module.  The application is only written
 * after the pramboot has proven itself (it runs, and its checksum matches), and only when the chip has no valid application.
 */
/*
 * The controller's firmware is FocalTech's, taken from the phone, and is not in the repository (tools/provenance.allow): put
 * drivers/input/hakios.focaltech_ft5452j.h (the arrays ft_pramboot and ft_app) in the kernel tree's bsd/dev/ to build the loader.
 * Without it the chip is only talked to, never reloaded.
 */
#if __has_include("hakios.focaltech_ft5452j.h")
#include "hakios.focaltech_ft5452j.h"
#define FT_FIRMWARE 1
#else
static const unsigned char ft_pramboot[1], ft_app[1];
#define FT_FIRMWARE 0
#endif

#define FTS_STATE_ROM   2
#define FTS_STATE_PRAM  3

static bool
fts_wr(const uint8_t *b, unsigned n)
{
	bool ok;

	i2c_start();
	ok = i2c_write_byte(FTS_ADDRESS << 1);
	for (unsigned i = 0; ok && i < n; i++) {
		ok = i2c_write_byte(b[i]);
	}
	i2c_stop();
	return ok;
}

static bool
fts_rd(const uint8_t *cmd, unsigned cn, uint8_t *out, unsigned n)
{
	bool ok;

	i2c_start();
	ok = i2c_write_byte(FTS_ADDRESS << 1);
	for (unsigned i = 0; ok && i < cn; i++) {
		ok = i2c_write_byte(cmd[i]);
	}
	if (ok) {
		i2c_start();
		ok = i2c_write_byte((FTS_ADDRESS << 1) | 1);
	}
	for (unsigned i = 0; ok && i < n; i++) {
		out[i] = i2c_read_byte(i + 1 < n);
	}
	i2c_stop();
	return ok;
}

static bool
fts_wr1(uint8_t cmd)
{
	return fts_wr(&cmd, 1);
}

/* 55 AA, 12 ms, then read command 90: the boot ID.  54 52 = boot ROM, 54 5b = pramboot, 54 5e = bootloader */
static unsigned
fts_boot_state(void)
{
	static const uint8_t ask[2] = { 0x55, 0xaa }, rd[4] = { 0x90, 0, 0, 0 };
	uint8_t id[3] = { 0 };

	if (!fts_wr(ask, 2)) {
		return 0;
	}
	sleep_ms(12);
	if (!fts_rd(rd, 4, id, 3)) {
		return 0;
	}
	printf("synaptics: FocalTech boot id %02x %02x %02x\n", id[0], id[1], id[2]);
	if (id[0] == 0x54 && id[1] == 0x52) {
		return FTS_STATE_ROM;
	}
	if (id[0] == 0x54 && id[1] == 0x5b) {
		return FTS_STATE_PRAM;
	}
	return id[0] == 0x54 && id[1] == 0x5e ? 4 : 0;
}

static uint16_t
fts_crc16(const uint8_t *b, unsigned n)
{
	uint16_t ecc = 0;

	for (unsigned i = 0; i + 1 < n; i += 2) {
		ecc ^= (uint16_t)(b[i] << 8 | b[i + 1]);
		for (unsigned j = 0; j < 16; j++) {
			ecc = (ecc & 1) ? (uint16_t)((ecc >> 1) ^ 0x8408) : (uint16_t)(ecc >> 1);
		}
	}
	return ecc;
}

/* command 6A answers a 16-bit status, big endian */
static bool
fts_status_is(uint16_t want)
{
	uint8_t cmd = 0x6a, v[2] = { 0 };

	return fts_rd(&cmd, 1, v, 2) && (uint16_t)(v[0] << 8 | v[1]) == want;
}

static bool
fts_load_pramboot(void)
{
	const unsigned len = sizeof(ft_pramboot);
	uint8_t p[6 + 32];

	if (!FT_FIRMWARE) {
		return false;
	}

	for (unsigned i = 0, off = 0; off < len; i++, off += 32) {
		unsigned n = len - off < 32 ? len - off : 32;

		p[0] = 0xad; p[1] = (uint8_t)(off >> 16); p[2] = (uint8_t)(off >> 8); p[3] = (uint8_t)off;
		if (!fts_wr(p, 4)) {
			printf("synaptics: FocalTech: pramboot address %u failed\n", i);
			return false;
		}
		p[0] = 0xae;
		memcpy(p + 1, ft_pramboot + off, n);
		if (!fts_wr(p, n + 1)) {
			printf("synaptics: FocalTech: pramboot data %u failed\n", i);
			return false;
		}
	}
	uint16_t crc = fts_crc16(ft_pramboot, len);
	uint8_t ecc[8] = { 0xcc, 0, 0, 0, (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len, 0xcc };
	uint8_t c = 0xce, v = 0, cd = 0xcd, r[2] = { 0 };
	unsigned t;

	if (!fts_wr(ecc, 8)) {
		return false;
	}
	for (t = 0; t < 100; t++) {
		sleep_ms(1);
		if (fts_rd(&c, 1, &v, 1) && v == 0x33) {
			break;
		}
	}
	if (t == 100 || !fts_rd(&cd, 1, r, 2)) {
		printf("synaptics: FocalTech: pramboot checksum did not finish (last %02x)\n", v);
		return false;
	}
	printf("synaptics: FocalTech: pramboot checksum in chip %04x, here %04x\n", r[0] << 8 | r[1], crc);
	if ((uint16_t)(r[0] << 8 | r[1]) != crc) {
		return false;
	}
	if (!fts_wr1(0x08)) {
		return false;
	}
	sleep_ms(100);
	for (t = 0; t < 30; t++) {
		if (fts_boot_state() == FTS_STATE_PRAM) {
			return true;
		}
		sleep_ms(20);
	}
	printf("synaptics: FocalTech: the pramboot does not run\n");
	return false;
}

static bool
fts_flash_app(void)
{
	const unsigned len = sizeof(ft_app);
	uint8_t p[6 + 32], x = 0;
	unsigned t;

	if (!FT_FIRMWARE) {
		return false;
	}

	{
		uint8_t a[2] = { 0x09, 0x0b }, b[4] = { 0xb0, (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len };

		if (!fts_wr(a, 2) || !fts_wr(b, 4)) {
			return false;
		}
	}
	printf("synaptics: FocalTech: erasing the chip's flash\n");
	if (!fts_wr1(0x61)) {
		return false;
	}
	sleep_ms(1350);
	for (t = 0; t < 50 && !fts_status_is(0xf0aa); t++) {
		sleep_ms(400);
	}
	if (t == 50) {
		printf("synaptics: FocalTech: the erase did not finish\n");
		return false;
	}
	printf("synaptics: FocalTech: writing %u bytes of application\n", len);
	for (unsigned off = 0; off < len; off += 32) {
		unsigned n = len - off < 32 ? len - off : 32;

		p[0] = 0xbf; p[1] = (uint8_t)(off >> 16); p[2] = (uint8_t)(off >> 8); p[3] = (uint8_t)off; p[4] = 0; p[5] = (uint8_t)n;
		memcpy(p + 6, ft_app + off, n);
		for (unsigned i = 0; i < n; i++) {
			x ^= ft_app[off + i];
		}
		if (!fts_wr(p, n + 6)) {
			printf("synaptics: FocalTech: flash write failed at %u\n", off);
			return false;
		}
		sleep_ms(1);
		for (t = 0; t < 100 && !fts_status_is((uint16_t)(0x1000 + off / n)); t++) {
			sleep_ms(1);
		}
	}
	{
		uint8_t e[7] = { 0x65, 0, 0, 0, (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len }, c = 0x66, v = 0;

		if (!fts_wr1(0x64) || !fts_wr(e, 7)) {
			return false;
		}
		sleep_ms(200);
		for (t = 0; t < 40 && !fts_status_is(0xf055); t++) {
			sleep_ms(50);
		}
		if (!fts_rd(&c, 1, &v, 1)) {
			return false;
		}
		printf("synaptics: FocalTech: application checksum in chip %02x, here %02x\n", v, x);
		if (v != x) {
			return false;
		}
	}
	return true;
}

static bool
fts_recover(void)
{
	unsigned state = fts_boot_state();
	bool ok = false;

	FTS_NOTE("[recover: boot state %u]", state);
	printf("synaptics: FocalTech: no valid application; loading the firmware (boot state %u)\n", state);
	for (unsigned attempt = 0; state != FTS_STATE_ROM && attempt < 3; attempt++) {
		uint8_t aa[2] = { 0xfc, 0xaa }, f5[2] = { 0xfc, 0x55 };

		/* an application that runs is told to jump to the ROM; anything else is reset */
		(void)fts_wr(aa, 2);
		sleep_ms(10);
		(void)fts_wr(f5, 2);
		sleep_ms(80);
		state = fts_boot_state();
		for (unsigned t = 0; state != FTS_STATE_ROM && t < 30; t++) {
			(void)fts_wr1(0x07);
			sleep_ms(10);
			state = fts_boot_state();
			sleep_ms(5);
		}
	}
	if (state != FTS_STATE_ROM) {
		FTS_NOTE("[no boot ROM, state %u]", state);
		printf("synaptics: FocalTech: the chip does not reach its boot ROM (state %u)\n", state);
		return false;
	}
	if (!fts_load_pramboot()) {
		FTS_NOTE("[pramboot FAILED]");
		return false;
	}
	FTS_NOTE("[pramboot ok]");
	ok = fts_flash_app();
	FTS_NOTE("[flash %s]", ok ? "ok" : "FAILED");
	(void)fts_wr1(0x07);                    /* reset: the new application starts */
	sleep_ms(300);
	printf("synaptics: FocalTech: firmware %s\n", ok ? "written" : "NOT written");
	return ok;
}

static bool
fts_bring_up(void)
{
	uint8_t id = 0, id2 = 0, fw = 0, vendor = 0;
	bool ok = fts_read(FTS_REG_CHIP_ID, &id, 1);

	ok = ok && fts_read(FTS_REG_CHIP_ID2, &id2, 1);
	(void)fts_read(FTS_REG_FW_VER, &fw, 1);
	(void)fts_read(FTS_REG_VENDOR_ID, &vendor, 1);
	FTS_NOTE("[id %02x %02x fw %02x]", id, id2, fw);
	{
		/* the mode and rate registers: 0x80 sensitivity, 0x85 gesture/idle, 0x86 idle->monitor time, 0x87 idle rate, 0x88 active rate (x10 Hz), 0x89 monitor rate, 0xa5 power mode */
		static const uint8_t regs[] = { 0x80, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8c, 0xa4, 0xa5, 0xa7, 0xad };
		uint8_t v;

		FTS_NOTE("[regs");
		for (unsigned i = 0; i < sizeof(regs); i++) {
			v = 0xee;
			(void)fts_read(regs[i], &v, 1);
			FTS_NOTE(" %02x=%02x", regs[i], v);
		}
		FTS_NOTE("]");
	}
	printf("synaptics: FocalTech FTS at %#x: chip id %#x %#x, firmware %#x, vendor %#x (%s)\n", FTS_ADDRESS, id, id2, fw, vendor,
	    ok && id == 0x54 && id2 == 0x52 ? "read" : ok ? "invalid; the controller is not ready" : "the registers did not answer");
	/* An acknowledged I2C transaction is not enough to call the controller ready.  During GPU bring-up this panel has
	 * answered every register with 0xef while it was still coming out of reset; accepting that left the reader polling
	 * a controller which would never assert its interrupt. */
	return ok && id == 0x54 && id2 == 0x52;
}

static void
bus_pins(void)
{
	line_release(sda);
	line_release(scl);
}

static bool
bring_up(void)
{
	struct tcm_message *m = kalloc_data(sizeof(*m), Z_WAITOK | Z_ZERO);
	bool ok = false;

	if (m == NULL) {
		return false;
	}
	/* the analog supply, then the reset: low 20 ms, high, and 200 ms for the chip to start */
	gpio_configure(PIN_INT, 0, PULL_UP, 8, false);
	gpio_output(PIN_AVDD, true, PULL_NONE, 8);
	sleep_ms(20);
	gpio_output(PIN_RESET, false, PULL_UP, 8);
	sleep_ms(20);
	gpio_set(PIN_RESET, true);
	bus_pins();
	sleep_ms(200);
	printf("synaptics: SDA %u SCL %u: SDA reads %u, SCL %u; interrupt line %s; AVDD cfg %#x, RESET cfg %#x\n", sda, scl, gpio_get(sda),
	    gpio_get(scl), gpio_get(PIN_INT) ? "high" : "low", rd(pin_base(PIN_AVDD)), rd(pin_base(PIN_RESET)));
	/* who answers on this bus (a table of the addresses that acknowledge) */
	{
		char found[128];
		unsigned o = 0;

		for (uint8_t a = 0x08; a < 0x78; a++) {
			if (i2c_present(a)) {
				o += (unsigned)snprintf(found + o, sizeof(found) - o, "%02x ", a);
			}
		}
		found[o] = 0;
		printf("synaptics: addresses that answer with SDA=%u, SCL=%u: %s\n", sda, scl, o ? found : "none");
		if (o == 0) {
			unsigned t = sda;

			sda = scl;
			scl = t;
			bus_pins();
			for (uint8_t a = 0x08; a < 0x78; a++) {
				if (i2c_present(a)) {
					o += (unsigned)snprintf(found + o, sizeof(found) - o, "%02x ", a);
				}
			}
			found[o] = 0;
			printf("synaptics: addresses that answer with SDA=%u, SCL=%u: %s\n", sda, scl, o ? found : "none");
		}
		if (o == 0) {
			goto out;
		}
	}
	if (i2c_present(FTS_ADDRESS) && !i2c_present(I2C_ADDRESS)) {
		fts_chip = fts_bring_up();
		for (unsigned attempt = 1; !fts_chip && attempt <= 2; attempt++) {
			printf("synaptics: FocalTech is not ready; power-cycle retry %u of 2\n", attempt);
			gpio_output(PIN_RESET, false, PULL_UP, 8);
			gpio_output(PIN_AVDD, false, PULL_NONE, 8);
			sleep_ms(50);
			gpio_set(PIN_AVDD, true);
			sleep_ms(20);
			gpio_set(PIN_RESET, true);
			bus_pins();
			sleep_ms(300);
			fts_chip = fts_bring_up();
		}
		if (!fts_chip && fts_recover()) {
			bus_pins();
			fts_chip = fts_bring_up();
		}
		goto out;
	}
	if (!i2c_present(I2C_ADDRESS)) {
		printf("synaptics: nothing answers at %#x\n", I2C_ADDRESS);
		goto out;
	}
	/* the first message after the reset */
	if (!tcm_wait(m, -1, 1500)) {
		printf("synaptics: no message after the reset; asking for the identity\n");
		tcm_send(CMD_IDENTIFY, NULL, 0);
		if (!tcm_wait(m, -1, 1000)) {
			printf("synaptics: the chip says nothing\n");
			goto out;
		}
	}
	hex("first message, code", &m->code, 1);
	hex("  payload", m->payload, m->length);
	if (m->code != CODE_IDENTIFY) {
		tcm_send(CMD_IDENTIFY, NULL, 0);
		if (!tcm_wait(m, CODE_IDENTIFY, 1000)) {
			printf("synaptics: no identify answer\n");
			goto out;
		}
		hex("identify", m->payload, m->length);
	}
	if (m->length < 2 || m->payload[1] != MODE_APPLICATION) {
		printf("synaptics: the chip is in mode %#x, not in application firmware: no firmware is loaded here; left alone\n",
		    m->length > 1 ? m->payload[1] : 0xff);
		goto out;
	}
	tcm_send(CMD_GET_APPLICATION_INFO, NULL, 0);
	if (tcm_wait(m, CODE_OK, 1000)) {
		hex("application info", m->payload, m->length);
		if (m->length >= 38) {
			max_x = m->payload[32] | m->payload[33] << 8;
			max_y = m->payload[34] | m->payload[35] << 8;
			printf("synaptics: the panel: %u x %u, up to %u objects\n", max_x, max_y, m->payload[36] | m->payload[37] << 8);
		}
	} else {
		printf("synaptics: no answer to the application info\n");
	}
	tcm_send(CMD_GET_TOUCH_REPORT_CONFIG, NULL, 0);
	if (tcm_wait(m, CODE_OK, 1000)) {
		hex("touch report config", m->payload, m->length);
		report_config_size = m->length < sizeof(report_config) ? m->length : (unsigned)sizeof(report_config);
		memcpy(report_config, m->payload, report_config_size);
	} else {
		printf("synaptics: no answer to the touch report config\n");
	}
	{
		uint8_t on = CODE_TOUCH;

		tcm_send(CMD_ENABLE_REPORT, &on, 1);
		(void)tcm_wait(m, CODE_OK, 500);
	}
	ok = report_config_size != 0;
out:
	kfree_data(m, sizeof(*m));
	return ok || fts_chip;
}

static void
synaptics_thread(__unused void *parameter, __unused wait_result_t result)
{
	struct tcm_message *m;
	uint8_t raw[4];
	uint32_t looks = 0;

	if (!bring_up()) {
		printf("synaptics: the touch screen is not available\n");
		return;
	}
	if (fts_chip) {
		printf("synaptics: reading the FocalTech touch registers\n");
		fts_thread();
		return;
	}
	m = kalloc_data(sizeof(*m), Z_WAITOK | Z_ZERO);
	if (m == NULL) {
		return;
	}
	printf("synaptics: reading touch reports\n");
	for (;; sleep_ms(8)) {
		if (++looks % 7500 == 0) {
			printf("synaptics: so far %u reports (%u bad); interrupt line %s\n", reports, bad, gpio_get(PIN_INT) ? "high" : "low");
		}
		while (!gpio_get(PIN_INT)) {
			int r = tcm_read(m, raw);

			if (r != 0) {
				bad++;
				break;
			}
			if (m->code == CODE_TOUCH) {
				if (reports++ < 8) {
					hex("touch report", m->payload, m->length);
				}
				touch_decode(m->payload, m->length);
			} else if (m->code == CODE_IDENTIFY) {
				printf("synaptics: the chip identified itself again (it was reset): the touch reports are switched on again\n");
				uint8_t on = CODE_TOUCH;

				tcm_send(CMD_ENABLE_REPORT, &on, 1);
			}
		}
	}
}

void
hakios_synaptics_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;
	uint64_t t;

	if (!PE_parse_boot_argn("synaptics_tcm", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	nanoseconds_to_absolutetime(1000, &t);
	ticks_per_us = t ? t : 1;
	tlmm_west = ml_io_map(TLMM_WEST_PHYS, TLMM_TILE_SIZE);
	tlmm_east = ml_io_map(TLMM_EAST_PHYS, TLMM_TILE_SIZE);
	if (tlmm_west == 0 || tlmm_east == 0) {
		printf("synaptics: cannot map the pins\n");
		return;
	}
	if (kernel_thread_start(synaptics_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
