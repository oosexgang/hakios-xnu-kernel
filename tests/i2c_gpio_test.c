/*
 * I2C by hand on two of the SoC's pins, as the part drivers share it
 * (kernel/overlay/bsd/dev/hakios_i2c_gpio.h), against the model of the pins (tlmm_model.h) with a
 * device on the two lines that speaks I2C: it watches the lines for a start, takes its address and
 * bytes, acknowledges, gives bytes back, and can hold the clock low. What is tested is that the two
 * understand each other, that a line is never driven high, and that the pins are given back.
 *
 *   i2c_gpio_test SCENARIO
 */
#include "fakekernel.h"
#include "tlmm_model.h"

#define SDA     4
#define SCL     5
#define ADDRESS 0x5a

/* ---- the device on the bus ----------------------------------------------------------------------- */

static struct {
	uint8_t reg[256], at;           /* its registers, and which is next */
	bool active, reading, first, expect_register, pending_read, master_acked;
	unsigned bit;                   /* clock pulses seen of the nine a byte takes */
	uint8_t shift, out;
	bool sda_low, scl_low;          /* what it drives */
	bool was_sda, was_scl;
	unsigned starts, stops, bytes_in, bytes_out, stretch;   /* stretch: hold the clock this many looks before each acknowledge */
	unsigned stretch_left;
	bool absent;                    /* nobody at the address */
	bool stuck;                     /* it holds the data line low for good (a transfer that was cut off) */
	unsigned driven_high;           /* a line driven high by the SoC: never, on an open-drain bus */
} device;

static bool
soc_low(unsigned pin)
{
	return (model.cfg[pin] & 1u << 9) && !(model.io[pin] & 2);
}

static void
lines(void)
{
	model.input[SDA] = !(soc_low(SDA) || device.sda_low || device.stuck);
	model.input[SCL] = !(soc_low(SCL) || device.scl_low);
}

static void
changed(void)
{
	bool sda, scl;

	for (unsigned pin = SDA; pin <= SCL; pin++) {
		device.driven_high += (model.cfg[pin] & 1u << 9) && (model.io[pin] & 2);
	}
	lines();
	sda = model.input[SDA];
	scl = model.input[SCL];
	if (scl && device.was_scl && device.was_sda && !sda) {
		/* a start (also a repeated one): an address follows */
		device.starts++;
		device.active = true;
		device.first = true;
		device.reading = device.pending_read = false;
		device.bit = 0;
		device.shift = 0;
		device.sda_low = false;
	} else if (scl && device.was_scl && !device.was_sda && sda) {
		device.stops++;
		device.active = false;
		device.sda_low = false;
	} else if (scl && !device.was_scl && device.active) {
		/* the clock went up: a bit is read */
		if (device.bit < 8) {
			if (!device.reading) {
				device.shift = (uint8_t)(device.shift << 1 | sda);
			}
		} else if (device.reading) {
			device.master_acked = !sda;
		}
		device.bit++;
	} else if (!scl && device.was_scl && device.active) {
		/* the clock went down: what is on the data line may change */
		if (device.bit == 8) {
			if (!device.reading) {
				bool ours = true;

				if (device.first) {
					ours = !device.absent && (device.shift >> 1) == ADDRESS;
					device.pending_read = device.shift & 1;
					device.expect_register = !device.pending_read;
					device.first = false;
				} else if (device.expect_register) {
					device.at = device.shift;
					device.expect_register = false;
					device.bytes_in++;
				} else {
					device.reg[device.at++] = device.shift;
					device.bytes_in++;
				}
				device.active = ours;
				device.sda_low = ours;          /* the acknowledge */
				if (ours && device.stretch) {
					device.scl_low = true;  /* ... when it is ready */
					device.stretch_left = device.stretch;
				}
			} else {
				device.sda_low = false;         /* the SoC's turn to acknowledge */
			}
		} else if (device.bit == 9) {
			device.bit = 0;
			device.shift = 0;
			device.sda_low = false;
			if (device.pending_read) {
				device.reading = true;
				device.pending_read = false;
				device.master_acked = true;
			}
			if (device.reading) {
				if (device.master_acked) {
					device.out = device.reg[device.at++];
					device.bytes_out++;
					device.sda_low = !(device.out & 0x80);
				} else {
					device.active = false;  /* the last byte was said to be the last */
				}
			}
		} else if (device.reading) {
			device.sda_low = !(device.out & (0x80 >> device.bit));
		}
		lines();
	}
	device.was_sda = model.input[SDA];
	device.was_scl = model.input[SCL];
}

static uint32_t
rd(vm_offset_t address)
{
	if (device.scl_low && address == model.base + 0x1000 * SCL + 4 && device.stretch_left-- == 0) {
		device.scl_low = false;                 /* the device is ready: the clock is let go */
		changed();
	}
	return tlmm_model_rd(address);
}

static void wr(vm_offset_t address, uint32_t value) { tlmm_model_wr(address, value); }

#define KERNEL 1
static vm_offset_t tlmm;
#define HAKIOS_TLMM tlmm
#include "hakios_tlmm.h"
#include "hakios_i2c_gpio.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static struct i2c_gpio bus = { .sda = SDA, .scl = SCL };
#define AS_FOUND        (1u << 2 | 3u)          /* the serial engine's function, pulled up: as a boot loader leaves the two */

static void
a_bus(void)
{
	tlmm_model_at(0x0f100000);
	tlmm = 0x0f100000;
	model.after_write = changed;
	model.cfg[SDA] = model.cfg[SCL] = AS_FOUND;
	fake_delay_divisor = 1;
	for (unsigned i = 0; i < 256; i++) {
		device.reg[i] = (uint8_t)(i * 3 + 1);
	}
	lines();
	device.was_sda = device.was_scl = true;
}

static void
never(void)
{
	CHECK(device.driven_high == 0, "a line was driven high %u times: an open-drain bus is only ever pulled low", device.driven_high);
	CHECK(model.elsewhere == 0, "%u accesses outside the pins' registers", model.elsewhere);
	CHECK(model.cfg[SDA] == AS_FOUND && model.cfg[SCL] == AS_FOUND, "the pins are not as they were found: %#x, %#x", model.cfg[SDA],
	    model.cfg[SCL]);
	for (unsigned i = 0; i < PINS; i++) {
		if (i != SDA && i != SCL && (model.cfg[i] != 0 || model.io[i] != 0)) {
			CHECK(false, "pin %u was touched", i);
			break;
		}
	}
}

/* a register written: start, the address, the register, the value, stop -- each acknowledged */
static void
a_write(void)
{
	bool acked;

	a_bus();
	CHECK(i2c_gpio_take(&bus), "the bus is at rest and is taken");
	i2c_gpio_start(&bus);
	acked = i2c_gpio_write(&bus, ADDRESS << 1) && i2c_gpio_write(&bus, 0x46) && i2c_gpio_write(&bus, 0x80);
	i2c_gpio_stop(&bus);
	i2c_gpio_give_back(&bus);
	CHECK(acked, "the address, the register and the value are acknowledged");
	CHECK(device.reg[0x46] == 0x80 && device.bytes_in == 2, "the device has the value in that register: %#x", device.reg[0x46]);
	CHECK(device.starts == 1 && device.stops == 1, "one start and one stop: %u, %u", device.starts, device.stops);
	never();
}

/* several registers read: the register's number, a second start, then bytes, all but the last acknowledged */
static void
a_read(void)
{
	uint8_t values[4] = { 0 };
	bool acked;

	a_bus();
	CHECK(i2c_gpio_take(&bus), "taken");
	i2c_gpio_start(&bus);
	acked = i2c_gpio_write(&bus, ADDRESS << 1) && i2c_gpio_write(&bus, 0x10);
	i2c_gpio_start(&bus);
	acked = acked && i2c_gpio_write(&bus, ADDRESS << 1 | 1);
	for (unsigned i = 0; i < 4; i++) {
		values[i] = i2c_gpio_read(&bus, i + 1 < 4);
	}
	i2c_gpio_stop(&bus);
	i2c_gpio_give_back(&bus);
	CHECK(acked, "acknowledged");
	CHECK(values[0] == 0x31 && values[1] == 0x34 && values[2] == 0x37 && values[3] == 0x3a, "registers 0x10 to 0x13: %#x %#x %#x %#x",
	    values[0], values[1], values[2], values[3]);
	CHECK(device.bytes_out == 4, "the device gave four bytes and no fifth: it was told the fourth is the last (%u)", device.bytes_out);
	CHECK(device.starts == 2 && device.stops == 1, "two starts, one stop");
	never();
}

/* bytes of every shape survive the wire, both ways */
static void
every_bit(void)
{
	static const uint8_t patterns[] = { 0x00, 0xff, 0x55, 0xaa, 0x01, 0x80, 0x7e, 0x81 };
	uint8_t back[sizeof(patterns)];

	a_bus();
	CHECK(i2c_gpio_take(&bus), "taken");
	i2c_gpio_start(&bus);
	(void)i2c_gpio_write(&bus, ADDRESS << 1);
	(void)i2c_gpio_write(&bus, 0x20);
	for (unsigned i = 0; i < sizeof(patterns); i++) {
		CHECK(i2c_gpio_write(&bus, patterns[i]), "byte %u acknowledged", i);
	}
	i2c_gpio_stop(&bus);
	i2c_gpio_start(&bus);
	(void)i2c_gpio_write(&bus, ADDRESS << 1);
	(void)i2c_gpio_write(&bus, 0x20);
	i2c_gpio_start(&bus);
	(void)i2c_gpio_write(&bus, ADDRESS << 1 | 1);
	for (unsigned i = 0; i < sizeof(patterns); i++) {
		back[i] = i2c_gpio_read(&bus, i + 1 < sizeof(patterns));
	}
	i2c_gpio_stop(&bus);
	i2c_gpio_give_back(&bus);
	CHECK(memcmp(device.reg + 0x20, patterns, sizeof(patterns)) == 0, "the device has them as sent");
	CHECK(memcmp(back, patterns, sizeof(patterns)) == 0, "and they read back the same");
	never();
}

/* nobody at the address: not acknowledged, and said */
static void
nobody_there(void)
{
	a_bus();
	device.absent = true;
	CHECK(i2c_gpio_take(&bus), "taken");
	i2c_gpio_start(&bus);
	CHECK(!i2c_gpio_write(&bus, ADDRESS << 1), "the address is not acknowledged");
	i2c_gpio_stop(&bus);
	i2c_gpio_give_back(&bus);
	CHECK(device.bytes_in == 0, "nothing was taken for a register");
	never();
}

/* a line held low by somebody: the bus is not taken, and nothing is driven */
static void
not_at_rest(void)
{
	a_bus();
	device.stuck = true;
	lines();
	device.was_sda = false;         /* (it was low before we came) */
	CHECK(!i2c_gpio_take(&bus), "not taken");
	CHECK(model.cfg[SDA] == AS_FOUND && model.cfg[SCL] == AS_FOUND && !soc_low(SDA) && !soc_low(SCL),
	    "the pins are as they were, and neither line is driven");
	CHECK(device.starts == 0, "no start was made on a bus that is not ours to use");
	never();
}

/* a device that holds the clock low until it is ready is waited for */
static void
clock_held(void)
{
	bool acked;

	a_bus();
	device.stretch = 20;
	CHECK(i2c_gpio_take(&bus), "taken");
	i2c_gpio_start(&bus);
	acked = i2c_gpio_write(&bus, ADDRESS << 1) && i2c_gpio_write(&bus, 0x30) && i2c_gpio_write(&bus, 0x99);
	i2c_gpio_stop(&bus);
	i2c_gpio_give_back(&bus);
	CHECK(acked && device.reg[0x30] == 0x99, "written all the same: %#x", device.reg[0x30]);
	never();
}

/* the pins go back to whatever they were, not to one fixed setting */
static void
given_back(void)
{
	a_bus();
	model.cfg[SDA] = 2u << 2 | 1u << 6;             /* (another function, another drive strength, no pull) */
	model.cfg[SCL] = 2u << 2 | 1u << 6;
	CHECK(i2c_gpio_take(&bus), "taken");
	i2c_gpio_start(&bus);
	(void)i2c_gpio_write(&bus, ADDRESS << 1);
	i2c_gpio_stop(&bus);
	i2c_gpio_give_back(&bus);
	CHECK(model.cfg[SDA] == (2u << 2 | 1u << 6) && model.cfg[SCL] == (2u << 2 | 1u << 6), "as found: %#x, %#x", model.cfg[SDA],
	    model.cfg[SCL]);
	CHECK(device.driven_high == 0 && model.elsewhere == 0, "and nothing else was done to them");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "a_write", a_write }, { "a_read", a_read }, { "every_bit", every_bit }, { "nobody_there", nobody_there },
		{ "not_at_rest", not_at_rest }, { "clock_held", clock_held }, { "given_back", given_back },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			scenarios[i].run();
			fprintf(stderr, "%-20s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
