/*
 * A device on two of the model's pins (tlmm_model.h) that speaks I2C: it watches the two lines for a
 * start, takes its address, a register's number and bytes, acknowledges, gives bytes back, and can
 * hold the clock low. For the tests of what talks I2C by hand. The test says which pins and which
 * address before it includes this:
 *
 *   #define I2C_DEVICE_SDA 36
 *   #define I2C_DEVICE_SCL 37
 *   #define I2C_DEVICE_ADDRESS 0x5a
 *
 * and makes `changed' the model's after_write. The lines are open drain: one is low if the SoC or the
 * device pulls it down. A line the SoC drives high is counted: on such a bus that is a fault.
 *
 * A second device answers at `address_b' if that is not 0, with registers of its own (`reg_b'); with
 * `wide' a register is two bytes, the most significant first, as some chips have them.
 */
#ifndef I2C_DEVICE_MODEL_H
#define I2C_DEVICE_MODEL_H

#include "tlmm_model.h"

static struct {
	uint8_t reg[256], at;           /* its registers, and which is next */
	uint8_t address_b, reg_b[256];  /* a second device on the same lines, if the test gives it an address */
	uint8_t *bank;                  /* the registers of the device that was addressed */
	bool wide;                      /* registers of two bytes, the most significant first: register r is bytes 2r and 2r + 1 */
	bool active, reading, first, expect_register, pending_read, master_acked;
	unsigned bit;                   /* clock pulses seen of the nine a byte takes */
	uint8_t shift, out;
	bool sda_low, scl_low;          /* what it drives */
	bool was_sda, was_scl;
	unsigned starts, stops, bytes_in, bytes_out, stretch;   /* stretch: hold the clock this many looks before each acknowledge */
	unsigned stretch_left;
	bool absent;                    /* nobody at the address */
	bool stuck;                     /* it holds the data line low for good (a transfer that was cut off) */
	unsigned midbyte;               /* it holds the data line low until it has seen this many more clocks */
	unsigned driven_high;           /* a line driven high by the SoC: never, on an open-drain bus */
	unsigned writes;                /* bytes written into its registers */
	void (*written)(uint8_t reg, uint8_t value);    /* a write the test wants to see or to change; NULL: the register takes it */
	uint8_t (*read)(uint8_t reg);   /* a read the test answers; NULL: the register's value */
	bool streams;                   /* stream_register is a port: bytes after the first go to it too, not to the next */
	uint8_t stream_register;
} device;

/* the register a transfer's next byte is of: the next one, unless this one is a port */
static inline void
i2c_device_next(void)
{
	if (!(device.streams && device.at == device.stream_register)) {
		device.at++;
	}
}

static inline bool
soc_low(unsigned pin)
{
	return (model.cfg[pin] & 1u << 9) && !(model.io[pin] & 2);
}

static inline void
lines(void)
{
	model.input[I2C_DEVICE_SDA] = !(soc_low(I2C_DEVICE_SDA) || device.sda_low || device.stuck || device.midbyte != 0);
	model.input[I2C_DEVICE_SCL] = !(soc_low(I2C_DEVICE_SCL) || device.scl_low);
}

static inline void
changed(void)
{
	bool sda, scl;

	for (unsigned pin = I2C_DEVICE_SDA; pin <= I2C_DEVICE_SCL; pin++) {
		device.driven_high += (model.cfg[pin] & 1u << 9) && (model.io[pin] & 2);
	}
	lines();
	if (device.midbyte != 0 && !model.input[I2C_DEVICE_SCL] && device.was_scl) {
		device.midbyte--;               /* a clock: one bit less of the byte it was cut off in */
		lines();
	}
	sda = model.input[I2C_DEVICE_SDA];
	scl = model.input[I2C_DEVICE_SCL];
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
					ours = !device.absent && ((device.shift >> 1) == I2C_DEVICE_ADDRESS ||
					    (device.address_b != 0 && (device.shift >> 1) == device.address_b));
					device.bank = (device.shift >> 1) == I2C_DEVICE_ADDRESS ? device.reg : device.reg_b;
					device.pending_read = device.shift & 1;
					device.expect_register = !device.pending_read;
					device.first = false;
				} else if (device.expect_register) {
					device.at = device.wide ? (uint8_t)(device.shift * 2) : device.shift;
					device.expect_register = false;
					device.bytes_in++;
				} else {
					if (device.written != NULL) {
						device.written(device.at, device.shift);
					} else {
						device.bank[device.at] = device.shift;
					}
					i2c_device_next();
					device.bytes_in++;
					device.writes++;
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
					device.out = device.read != NULL && device.bank == device.reg ? device.read(device.at) : device.bank[device.at];
					i2c_device_next();
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
	device.was_sda = model.input[I2C_DEVICE_SDA];
	device.was_scl = model.input[I2C_DEVICE_SCL];
}

/* a look at the clock line by the SoC: a device that held the clock lets it go when it is ready */
static inline void
i2c_device_looked_at(vm_offset_t address)
{
	if (device.scl_low && address == model.base + 0x1000 * I2C_DEVICE_SCL + 4 && device.stretch_left-- == 0) {
		device.scl_low = false;
		changed();
	}
}

#endif /* I2C_DEVICE_MODEL_H */
