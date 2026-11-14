/*
 * The example driver's test (example_led.c): the driver's own file, compiled as a host program on the
 * fake kernel (workbench/fakekernel/) against the model of the SoC's pins (workbench/tlmm_model.h).
 * workbench/run.sh builds and runs it for the host and, where the tools are there, for aarch64 under
 * qemu-aarch64. Copy it with the driver.
 *
 *   example_led_test SCENARIO      one scenario a process: a driver keeps its state in statics
 */
#include <unistd.h>

#include "fakekernel.h"
#include "tlmm_model.h"

extern char log_text[];

/* 1. The seam: with HAKIOS_MMIO_TEST the driver's rd() and wr() are hakios_test_rd() and hakios_test_wr() (after
 * the driver, below: it counts what it sees), and they are the model. */
uint32_t hakios_test_rd(vm_offset_t address) { return tlmm_model_rd(address); }
vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }

/* 2. The driver itself, unchanged, and the kernel's board code it asks. */
#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#include "board/board.c"
#include "example_led.c"

/* the light is on: the pin an output, driven high */
static bool
lit(void)
{
	return (model.cfg[LED_PIN] & 1u << 9) && (model.io[LED_PIN] & 2);
}

/* (the times it came on, counted as the driver writes: a test that looked now and then could miss one, its
 * thread not run for a while on a busy computer) */
static unsigned ons;
void
hakios_test_wr(vm_offset_t address, uint32_t value)
{
	bool was = lit();

	tlmm_model_wr(address, value);
	ons += !was && lit();
}

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited = 0; !(cond) && waited < (ms); waited += 5) usleep(5000); } while (0)

/* 3. A phone: the model where the board's SoC file has the pins, the pin as a boot loader leaves it. */
static void
phone(const char *name)
{
	board = name;
	fake_delay_divisor = 20;
	tlmm_model_at(hakios_board() != NULL ? hakios_board()->pins.tlmm : 0);
	model.cfg[LED_PIN] = 1;                 /* a plain input, pulled down */
}

static unsigned
others_touched(void)
{
	unsigned touched = 0;

	for (unsigned i = 0; i < PINS; i++) {
		touched += i != LED_PIN && (model.cfg[i] != 0 || model.io[i] != 0);
	}
	return touched;
}

/* 4. Scenarios. What a driver must never do comes first. */

/* without its boot argument: nothing read, nothing written, nothing said */
static void
off_by_default(void)
{
	phone("phone-cmd");
	boot_args = "";
	example_led_init();
	usleep(50000);
	CHECK(model.writes == 0 && model.elsewhere == 0 && !logged("led:"), "nothing: %s", log_text);
}

/* led=0 is not being asked: the same nothing */
static void
asked_for_none(void)
{
	phone("phone-cmd");
	boot_args = "led=0";
	example_led_init();
	usleep(50000);
	CHECK(model.writes == 0 && model.elsewhere == 0 && !logged("led:"), "nothing: %s", log_text);
}

/* the thread it starts runs on its own: the driver lets go of the reference it was given */
static void
lets_go_of_its_thread(void)
{
	phone("phone-cmd");
	boot_args = "led=1";
	example_led_init();
	CHECK(fake_threads_started == 1 && fake_threads_released == 1, "%u started, %u let go of", fake_threads_started,
	    fake_threads_released);
	/* (and the thread is let finish: a process that ends under a thread still writing its log can abort) */
	WAIT_FOR(logged("led: done"), 3000);
}

/* a look writes nothing */
static void
look_only(void)
{
	phone("phone-cmd");
	boot_args = "led=1";
	model.input[LED_PIN] = true;
	example_led_init();
	WAIT_FOR(logged("led: done"), 3000);
	CHECK(logged("led: pin 42: configuration 0x1 (an input, function 0), reads high"), "what is there is said: %s", log_text);
	CHECK(model.writes == 0 && model.elsewhere == 0, "and nothing is written (%u)", model.writes);
}

/* the light blinks three times and stays off; the way out gives the pin back */
static void
blinks(void)
{
	phone("phone-cmd");
	boot_args = "led=2";
	example_led_init();
	WAIT_FOR(logged("led: done"), 5000);
	CHECK(logged("led: blinked 3 times") && ons == 3, "three times: seen %u: %s", ons, log_text);
	CHECK((model.io[LED_PIN] & 2) == 0, "off at the end");
	CHECK(model.drove_wrong == 0, "the pin had its level before it became an output");
	CHECK(others_touched() == 0 && model.elsewhere == 0, "no other pin was touched");
	example_led_quiesce();
	CHECK(model.cfg[LED_PIN] == 1 && logged("led: the pin is as it was found"), "given back: %#x", model.cfg[LED_PIN]);
}

/* a pin that some block of the SoC uses is not taken over */
static void
pin_in_use(void)
{
	phone("phone-cmd");
	model.cfg[LED_PIN] = 3u << 2;
	boot_args = "led=2";
	example_led_init();
	WAIT_FOR(logged("led: done"), 3000);
	CHECK(logged("led: pin 42 is in use as function 3: left alone") && model.writes == 0, "left alone: %s", log_text);
	example_led_quiesce();
	CHECK(model.writes == 0, "and nothing is given back that was not taken");
}

/* a board whose SoC file does not say where the pins are, and a machine nobody described */
static void
no_pins(void)
{
	phone("qemu-virt");
	boot_args = "led=2";
	example_led_init();
	CHECK(logged("led: disabled: the SoC file does not say where the pins are (pins { })"), "said: %s", log_text);
	usleep(50000);
	CHECK(model.writes == 0 && model.elsewhere == 0, "nothing touched");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "off_by_default", off_by_default }, { "look_only", look_only }, { "blinks", blinks }, { "pin_in_use", pin_in_use },
		{ "no_pins", no_pins }, { "asked_for_none", asked_for_none }, { "lets_go_of_its_thread", lets_go_of_its_thread },
	};

	/* (run without an argument a test lists its scenarios, which workbench/run.sh then runs one a process) */
	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			/* the driver's own printing of its log is not the test's result (the log is kept: logged()) */
			if (freopen("/dev/null", "w", stdout) == NULL) {
				return 2;
			}
			scenarios[i].run();
			fprintf(stderr, "%-20s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
