/*
 * I2C by hand on two of the SoC's pins, as the part drivers share it
 * (drivers/board/i2c_gpio.h), against the model of the pins (tlmm_model.h) with a
 * device on the two lines that speaks I2C: it watches the lines for a start, takes its address and
 * bytes, acknowledges, gives bytes back, and can hold the clock low. What is tested is that the two
 * understand each other, that a line is never driven high, and that the pins are given back.
 *
 *   i2c_gpio_test SCENARIO
 */
#include "fakekernel.h"

#define SDA     4
#define SCL     5
#define ADDRESS 0x5a
#define I2C_DEVICE_SDA SDA
#define I2C_DEVICE_SCL SCL
#define I2C_DEVICE_ADDRESS ADDRESS

#include "i2c_device_model.h"

static uint32_t
rd(vm_offset_t address)
{
	i2c_device_looked_at(address);
	return tlmm_model_rd(address);
}

static void wr(vm_offset_t address, uint32_t value) { tlmm_model_wr(address, value); }

#define KERNEL 1
static vm_offset_t tlmm;
#define HAKIOS_TLMM tlmm
#include "board/tlmm.h"
#include "board/i2c_gpio.h"

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

/* a device cut off in the middle of a byte holds the data line: clocked out of it, the bus is ours again */
static void
stuck_midbyte(void)
{
	unsigned clocks = 0;
	bool acked;

	a_bus();
	device.midbyte = 5;
	lines();
	device.was_sda = false;
	CHECK(!i2c_gpio_take(&bus), "not at rest: not taken as it is");
	CHECK(model.cfg[SDA] == AS_FOUND && model.cfg[SCL] == AS_FOUND, "... and the pins are as they were");
	CHECK(i2c_gpio_unwedge(&bus, &clocks) && clocks == 5, "after five clocks the data line is let go: %u", clocks);
	i2c_gpio_start(&bus);
	acked = i2c_gpio_write(&bus, ADDRESS << 1) && i2c_gpio_write(&bus, 0x46) && i2c_gpio_write(&bus, 0x12);
	i2c_gpio_stop(&bus);
	i2c_gpio_give_back(&bus);
	CHECK(acked && device.reg[0x46] == 0x12, "and the device answers again: %#x", device.reg[0x46]);
	never();
}

/* cut off at the first bit of a byte: the nine clocks of a whole byte free it, the last of them too */
static void
stuck_nine_clocks(void)
{
	unsigned clocks = 0;

	a_bus();
	device.midbyte = 9;
	lines();
	device.was_sda = false;
	CHECK(i2c_gpio_unwedge(&bus, &clocks) && clocks == 9, "freed at the ninth clock: %u", clocks);
	i2c_gpio_give_back(&bus);
	never();
}

/* a line that nine clocks do not free, or the clock line itself held: left alone */
static void
stuck_for_good(void)
{
	unsigned clocks = 0;

	a_bus();
	device.stuck = true;
	lines();
	device.was_sda = false;
	CHECK(!i2c_gpio_unwedge(&bus, &clocks), "nine clocks and the data line is still held: given up");
	CHECK(model.cfg[SDA] == AS_FOUND && model.cfg[SCL] == AS_FOUND && !soc_low(SDA) && !soc_low(SCL), "the pins are as they were");
	device.stuck = false;
	device.scl_low = true;
	device.stretch_left = 1000000;
	lines();
	CHECK(!i2c_gpio_unwedge(&bus, &clocks) && device.driven_high == 0, "a clock line held low is nobody's to clock");
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

/* a device that holds the clock low for good is not waited for for ever: the transfer goes on without it, and fails */
static void
clock_held_for_good(void)
{
	uint64_t start = mach_absolute_time(), taken;
	bool acked;

	a_bus();
	device.stretch = 1000000;
	CHECK(i2c_gpio_take(&bus), "taken");
	i2c_gpio_start(&bus);
	acked = i2c_gpio_write(&bus, ADDRESS << 1) && i2c_gpio_write(&bus, 0x30) && i2c_gpio_write(&bus, 0x99);
	i2c_gpio_stop(&bus);
	i2c_gpio_give_back(&bus);
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &taken);
	CHECK(device.reg[0x30] != 0x99 || !acked, "not written as if nothing happened");
	CHECK(taken < 1000000000ull, "given up within a second: %llu ns", (unsigned long long)taken);
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
		{ "not_at_rest", not_at_rest }, { "stuck_midbyte", stuck_midbyte }, { "stuck_for_good", stuck_for_good },
		{ "clock_held", clock_held }, { "clock_held_for_good", clock_held_for_good }, { "stuck_nine_clocks", stuck_nine_clocks }, { "given_back", given_back },
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
