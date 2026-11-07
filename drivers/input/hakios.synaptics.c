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
 * four bits), then 6 bytes a finger: event (2 bits: 0 down, 1 up, 2 contact) and x (12 bits), the finger's id (4 bits) and y
 * (12 bits), pressure, area. The device tree gives the extent 17279 x 38399: the panel's 1080 x 2400, each times 16.
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

/* ---- FocalTech FTS (address 0x38) ---------------------------------------------------------------- */

#define FTS_ADDRESS             0x38
#define FTS_REG_CHIP_ID         0xa3
#define FTS_REG_FW_VER          0xa6
#define FTS_REG_VENDOR_ID       0xa8
#define FTS_REG_CHIP_ID2        0x9f
#define FTS_MAX_X               17279   /* 1080 * 16 - 1, from the device tree's focaltech,display-coords */
#define FTS_MAX_Y               38399   /* 2400 * 16 - 1 */

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

/* the touch registers: 0x01.. (gesture, count, then 6 bytes a finger); only as many bytes as the count says */
static bool
fts_read_touches(uint8_t *buf, unsigned *count)
{
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
	buf[0] = i2c_read_byte(true);
	buf[1] = i2c_read_byte(true);
	*count = buf[1] & 0x0f;
	if (*count > MAX_FINGERS) {
		i2c_read_byte(false);
		i2c_stop();
		*count = 0;
		return false;
	}
	for (unsigned i = 0; i < *count * 6; i++) {
		buf[2 + i] = i2c_read_byte(i + 1 < *count * 6);
	}
	if (*count == 0) {
		(void)i2c_read_byte(false);
	}
	i2c_stop();
	return true;
}

static void
fts_thread(void)
{
	uint8_t buf[2 + 6 * MAX_FINGERS];
	uint32_t looks = 0, touches = 0, bad_reads = 0, irq_low = 0, valid = 0, invalid = 0;
	uint64_t last_seen[MAX_FINGERS] = { 0 }, release_after;
	bool seen[MAX_FINGERS];

	nanoseconds_to_absolutetime(60 * NSEC_PER_MSEC, &release_after);
	/*
	 * The chip's touch registers hold 0xff while it has no frame (the first run on the phone read 00 ff ff ... at rest), and
	 * a frame is announced by the interrupt line going low, for how long is not known. So the line is looked at every
	 * millisecond and a frame read whenever it is low, and in any case every sixteenth look; a finger is taken to have lifted
	 * when a frame says so or when 60 ms pass without a valid one.
	 */
	for (;; sleep_ms(1)) {
		unsigned count = 0;
		bool low = !gpio_get(PIN_INT);

		looks++;
		irq_low += low;
		if (looks % 60000 == 0) {
			printf("synaptics: FocalTech: %u looks, interrupt low in %u, %u valid frames, %u without data, %u finger positions, %u bad reads\n",
			    looks, irq_low, valid, invalid, touches, bad_reads);
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
			memset(seen, 0, sizeof(seen));
			for (unsigned i = 0; i < count; i++) {
				const uint8_t *p = buf + 2 + 6 * i;
				unsigned event = p[0] >> 6, id = p[2] >> 4;
				uint32_t x = (uint32_t)(p[0] & 0x0f) << 8 | p[1], y = (uint32_t)(p[2] & 0x0f) << 8 | p[3];

				if (id >= MAX_FINGERS || event == 3) {
					continue;
				}
				if (touches < 16) {
					printf("synaptics: FocalTech: finger %u event %u at %u, %u (pressure %u)\n", id, event, x, y, p[4]);
				}
				touches++;
				if (x > FTS_MAX_X) x = FTS_MAX_X;
				if (y > FTS_MAX_Y) y = FTS_MAX_Y;
				uint32_t px = (uint32_t)((uint64_t)x * 65535 / FTS_MAX_X), py = (uint32_t)((uint64_t)y * 65535 / FTS_MAX_Y);

				if (event == 1) {                            /* the chip says it lifted */
					continue;
				}
				seen[id] = true;
				last_seen[id] = mach_absolute_time();
				if (!fingers[id].down) {
					hakios_touch_post(HAKIOS_TOUCH_DOWN, id, px, py);
					fingers[id].down = true;
				} else if (fingers[id].x != px || fingers[id].y != py) {
					hakios_touch_post(HAKIOS_TOUCH_MOVE, id, px, py);
				}
				fingers[id].x = px;
				fingers[id].y = py;
			}
			/* a valid frame with fewer fingers than before: those it does not list have lifted */
			for (unsigned f = 0; f < MAX_FINGERS; f++) {
				if (fingers[f].down && !seen[f]) {
					hakios_touch_post(HAKIOS_TOUCH_UP, f, fingers[f].x, fingers[f].y);
					fingers[f].down = false;
				}
			}
		}
lifted:
		/* ... and a finger not heard of for 60 ms has lifted as well */
		for (unsigned f = 0; f < MAX_FINGERS; f++) {
			if (fingers[f].down && mach_absolute_time() - last_seen[f] > release_after) {
				hakios_touch_post(HAKIOS_TOUCH_UP, f, fingers[f].x, fingers[f].y);
				fingers[f].down = false;
			}
		}
	}
}

static bool
fts_bring_up(void)
{
	uint8_t id = 0, id2 = 0, fw = 0, vendor = 0;
	bool ok = fts_read(FTS_REG_CHIP_ID, &id, 1);

	ok = ok && fts_read(FTS_REG_CHIP_ID2, &id2, 1);
	(void)fts_read(FTS_REG_FW_VER, &fw, 1);
	(void)fts_read(FTS_REG_VENDOR_ID, &vendor, 1);
	printf("synaptics: FocalTech FTS at %#x: chip id %#x %#x, firmware %#x, vendor %#x (%s)\n", FTS_ADDRESS, id, id2, fw, vendor,
	    ok ? "read" : "the registers did not answer");
	return ok;
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
