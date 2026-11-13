/*
 * The SoC's own pins (Qualcomm's "TLMM"): a configuration register and a level register for each.
 * What the part drivers that drive pins each had a copy of. Where the pins are comes from the SoC
 * file's pins { } (hakios_board()->pins.tlmm); which pin a part is on is the part driver's.
 *
 * The file that includes this one has rd() and wr() (board/mmio.h), and says where it has the pins
 * mapped before it includes this:
 *
 *   static vm_offset_t tlmm;                   (= ml_io_map(hakios_board()->pins.tlmm, TLMM_SIZE) at its start)
 *   #define HAKIOS_TLMM tlmm
 *   #include "tlmm.h"
 *
 * Register layout: Linux drivers/pinctrl/qcom/pinctrl-msm.c.
 */
#ifndef HAKIOS_TLMM_H
#define HAKIOS_TLMM_H

#ifndef HAKIOS_TLMM
#error "say where the pins are mapped: #define HAKIOS_TLMM before including board/tlmm.h"
#endif

#define TLMM_SIZE               0x100000UL      /* 256 pins of 0x1000 each */
#define GPIO_CFG(n)             ((HAKIOS_TLMM) + 0x1000UL * (n))        /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              ((HAKIOS_TLMM) + 0x1000UL * (n) + 4)    /* bit 0 in, bit 1 out */
#define PULL_NONE               0
#define PULL_DOWN               1
#define PULL_KEEPER             2
#define PULL_UP                 3
/* the fields of a pin's configuration, for a driver that puts one together itself */
#define CFG_PULL_UP             PULL_UP
#define CFG_FUNCTION(f)         ((uint32_t)(f) << 2)
#define CFG_DRIVE_MA(ma)        ((uint32_t)((ma) / 2 - 1) << 6)
#define CFG_OUTPUT              (1u << 9)

/* `function' 0 is a plain GPIO, the others are the SoC's own uses of the pin; `ma' the drive strength, 2 to 16 in twos */
static inline void __attribute__((unused))
gpio_configure(unsigned pin, unsigned function, unsigned pull, unsigned ma, bool output)
{
	wr(GPIO_CFG(pin), pull | CFG_FUNCTION(function) | CFG_DRIVE_MA(ma) | (output ? CFG_OUTPUT : 0));
}

static inline void __attribute__((unused))
gpio_set(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), high ? 2 : 0);
}

static inline bool __attribute__((unused))
gpio_get(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

/* a plain output at that level */
static inline void __attribute__((unused))
gpio_output(unsigned pin, bool high, unsigned pull, unsigned ma)
{
	gpio_set(pin, high);            /* the level first, so that the pin never drives the wrong one */
	gpio_configure(pin, 0, pull, ma, true);
}

#endif /* HAKIOS_TLMM_H */
