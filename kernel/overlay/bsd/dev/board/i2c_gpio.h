/*
 * I2C by hand on two of the SoC's pins: for a part on a bus whose serial engine nobody here drives,
 * or that is to be talked to before anything else is up. What the drivers of such parts each had a
 * copy of.
 *
 * The two lines are open drain: a pin is let go (an input with its pull-up) for a one and driven
 * low for a zero, never driven high. A transaction takes the two pins from whoever has them (the
 * serial engine's function, as the boot loader left them) and gives them back as found; if the bus
 * is not at rest when it is taken, nothing is driven. 100 kHz at most; a device may hold the clock
 * low until it is ready.
 *
 * The file that includes this one has rd() and wr() (board/mmio.h) and the pins (board/tlmm.h).
 */
#ifndef HAKIOS_I2C_GPIO_H
#define HAKIOS_I2C_GPIO_H

#include "time.h"

struct i2c_gpio {
	unsigned sda, scl;              /* the two pins, by number */
	uint32_t found_cfg[2], found_io[2];     /* as they were when the bus was taken */
};

static inline void
i2c_gpio_half_bit(void)
{
	spin_us(5);                     /* 100 kHz at most */
}

/* a line let go (high, unless the other side holds it) or driven low */
static inline void
i2c_gpio_line(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), 0);
	wr(GPIO_CFG(pin), CFG_PULL_UP | (high ? 0 : CFG_OUTPUT));
}

static inline bool
i2c_gpio_level(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static inline void
i2c_gpio_start(const struct i2c_gpio *bus)
{
	i2c_gpio_line(bus->sda, true);
	i2c_gpio_line(bus->scl, true);
	i2c_gpio_half_bit();
	i2c_gpio_line(bus->sda, false);
	i2c_gpio_half_bit();
	i2c_gpio_line(bus->scl, false);
	i2c_gpio_half_bit();
}

static inline void
i2c_gpio_stop(const struct i2c_gpio *bus)
{
	i2c_gpio_line(bus->sda, false);
	i2c_gpio_half_bit();
	i2c_gpio_line(bus->scl, true);
	i2c_gpio_half_bit();
	i2c_gpio_line(bus->sda, true);
	i2c_gpio_half_bit();
}

/* One bit out (the other side's bit in, if we let the data line go): the clock goes up and down once. */
static inline bool
i2c_gpio_bit(const struct i2c_gpio *bus, bool out)
{
	bool in;

	i2c_gpio_line(bus->sda, out);
	i2c_gpio_half_bit();
	i2c_gpio_line(bus->scl, true);
	for (unsigned stretched = 0; !i2c_gpio_level(bus->scl) && stretched < 100; stretched++) {
		i2c_gpio_half_bit();    /* (a device may hold the clock low until it is ready) */
	}
	i2c_gpio_half_bit();
	in = i2c_gpio_level(bus->sda);
	i2c_gpio_line(bus->scl, false);
	return in;
}

/* A byte out. True if it was acknowledged. */
static inline bool
i2c_gpio_write(const struct i2c_gpio *bus, uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_gpio_bit(bus, (byte >> bit) & 1);
	}
	return !i2c_gpio_bit(bus, true);
}

/* A byte in; `more': another is wanted after it (it is acknowledged). */
static inline uint8_t
i2c_gpio_read(const struct i2c_gpio *bus, bool more)
{
	uint8_t byte = 0;

	for (int bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)(byte << 1 | i2c_gpio_bit(bus, true));
	}
	i2c_gpio_bit(bus, !more);       /* acknowledge all but the last byte */
	return byte;
}

/* The two pins back as they were found. */
static inline void
i2c_gpio_give_back(const struct i2c_gpio *bus)
{
	wr(GPIO_IO(bus->sda), bus->found_io[0] & 2);
	wr(GPIO_IO(bus->scl), bus->found_io[1] & 2);
	wr(GPIO_CFG(bus->sda), bus->found_cfg[0]);
	wr(GPIO_CFG(bus->scl), bus->found_cfg[1]);
}

/*
 * The two pins taken for one transaction. False if the bus is not at rest (a line is held low):
 * then the pins are as they were and nothing was driven.
 */
static inline bool
i2c_gpio_take(struct i2c_gpio *bus)
{
	bus->found_cfg[0] = rd(GPIO_CFG(bus->sda));
	bus->found_cfg[1] = rd(GPIO_CFG(bus->scl));
	bus->found_io[0] = rd(GPIO_IO(bus->sda));
	bus->found_io[1] = rd(GPIO_IO(bus->scl));
	i2c_gpio_line(bus->sda, true);
	i2c_gpio_line(bus->scl, true);
	i2c_gpio_half_bit();
	if (!i2c_gpio_level(bus->sda) || !i2c_gpio_level(bus->scl)) {
		i2c_gpio_give_back(bus);
		return false;
	}
	return true;
}

/*
 * A device stopped in the middle of a byte -- its master went away while it was answering -- goes on
 * holding the data line down, and the bus never comes to rest again however long one waits. The way
 * out is the one Linux's bit-banged master takes (i2c_generic_scl_recovery): clock the line that is
 * free until the device has finished that byte and lets the data line go, then a stop. Nine clocks
 * are a byte and the acknowledgement after it.
 *
 * For a bus i2c_gpio_take() refused: true if the data line alone was held and came back, and then
 * the bus is taken, `clocks' saying after how many; else the pins are as they were.
 */
static inline bool
i2c_gpio_unwedge(struct i2c_gpio *bus, unsigned *clocks)
{
	bus->found_cfg[0] = rd(GPIO_CFG(bus->sda));
	bus->found_cfg[1] = rd(GPIO_CFG(bus->scl));
	bus->found_io[0] = rd(GPIO_IO(bus->sda));
	bus->found_io[1] = rd(GPIO_IO(bus->scl));
	i2c_gpio_line(bus->sda, true);
	i2c_gpio_line(bus->scl, true);
	i2c_gpio_half_bit();
	for (*clocks = 1; i2c_gpio_level(bus->scl) && !i2c_gpio_level(bus->sda) && *clocks <= 9; (*clocks)++) {
		i2c_gpio_line(bus->scl, false);
		i2c_gpio_half_bit();
		i2c_gpio_line(bus->scl, true);
		i2c_gpio_half_bit();
		if (i2c_gpio_level(bus->sda)) {
			i2c_gpio_stop(bus);
			if (i2c_gpio_level(bus->scl) && i2c_gpio_level(bus->sda)) {
				return true;
			}
			break;
		}
	}
	i2c_gpio_give_back(bus);
	return false;
}

#endif /* HAKIOS_I2C_GPIO_H */
