/*
 * An example driver: a light on one of the SoC's pins. It is not built into the kernel; it is here to
 * be read beside Documentation/writing-a-driver.md and to be copied. `make check` compiles it and runs it
 * against a model of the pins (example_led_test.c), so what the guide shows is code that works.
 *
 *   led=1      a look: the pin's configuration and level are read and said, nothing is written
 *   led=2      the light blinks three times, then stays off
 *
 * Everything a driver should do is here once: it starts only when asked (its boot argument), only
 * on a board that has what it needs (the board's data), reads before it writes (levels), says
 * what it does (the log), and leaves the hardware as it found it (quiesce).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

#include "board/board.h"       /* which board this is and what it has */
#include "board/log.h"         /* hakios_say(): a line in the loader's log, and the kernel's */
#include "board/mmio.h"        /* rd(), wr(): registers, with the seam for the host test */
#include "board/time.h"        /* sleep_ms(), spin_us(), now_us() */

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

/*
 * Where the SoC's pins are is the chip's and comes from the SoC file (pins { }). Which pin the
 * light is on is this part's wiring: a constant here while one phone has the part, a key of the
 * board file once a second phone has it on another pin (arch/arm64/boot/dts/README.md, "Adding a kind of data").
 */
#define LED_PIN                 42
#define BLINKS                  3
#define BLINK_MS                200

static vm_offset_t tlmm;
#define HAKIOS_TLMM             tlmm
#include "board/tlmm.h"        /* gpio_configure(), gpio_output(), gpio_set(), gpio_get() */

static uint32_t level;
static uint32_t found_cfg;      /* the pin's configuration as the boot loader left it */
static bool taken;              /* the pin is ours: an output, to be given back */

void example_led_init(void);
void example_led_quiesce(void);

/* level 1: read and say, write nothing */
static void
look(void)
{
	found_cfg = rd(GPIO_CFG(LED_PIN));
	hakios_say("led: pin %u: configuration %#x (%s, function %u), reads %s\n", LED_PIN, found_cfg,
	    (found_cfg & 1u << 9) ? "an output" : "an input", (found_cfg >> 2) & 0xf, gpio_get(LED_PIN) ? "high" : "low");
}

static void
led_thread(__unused void *parameter, __unused wait_result_t result)
{
	look();
	if (level >= 2) {
		if (((found_cfg >> 2) & 0xf) != 0) {
			/* the pin is some block's, not a plain GPIO: this board is not wired as the driver takes it to be */
			hakios_say("led: pin %u is in use as function %u: left alone\n", LED_PIN, (found_cfg >> 2) & 0xf);
		} else {
			gpio_output(LED_PIN, false, PULL_NONE, 2);
			taken = true;
			for (unsigned i = 0; i < BLINKS; i++) {
				gpio_set(LED_PIN, true);
				sleep_ms(BLINK_MS);
				gpio_set(LED_PIN, false);
				sleep_ms(BLINK_MS);
			}
			hakios_say("led: blinked %u times\n", BLINKS);
		}
	}
	hakios_say("led: done\n");
	thread_terminate(current_thread());
}

/* Called once at boot (a line in bsd/dev/board/input.c's start, or wherever the driver belongs). */
void
example_led_init(void)
{
	thread_t thread;

	/* 1. Only when asked. A driver that is not asked for does nothing and says nothing. */
	if (!PE_parse_boot_argn("led", &level, sizeof(level)) || level == 0) {
		return;
	}
	/*
	 * 2. Only on a board that has it. A driver of a part asks for its feature here --
	 *        if (!hakios_board_has(HAKIOS_HAS_...)) { say which line of board.dts is missing; return; }
	 *    -- and one that needs addresses asks whether the board has them. hakios_board() is NULL on a
	 *    machine nobody described.
	 */
	if (hakios_board() == NULL || hakios_board()->pins.tlmm == 0) {
		hakios_say("led: disabled: the SoC file does not say where the pins are (pins { })\n");
		level = 0;
		return;
	}
	tlmm = ml_io_map(hakios_board()->pins.tlmm, TLMM_SIZE);
	/* 3. In a thread of its own: the boot does not wait for a driver. */
	if (kernel_thread_start(led_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/*
 * On the way back to the loader (osfmk/arm64/loader_return.c calls each driver's): the other cores
 * are stopped and interrupts are off, so nothing here sleeps or takes a lock, and hakios_say()
 * writes to the loader's log only. The pin goes back to what the boot loader left.
 */
void
example_led_quiesce(void)
{
	if (taken) {
		taken = false;
		gpio_set(LED_PIN, false);
		wr(GPIO_CFG(LED_PIN), found_cfg);
		hakios_say("led: the pin is as it was found\n");
	}
}
