/*
 * The battery's driver itself (kernel/overlay/bsd/dev/redwood_battery.c), run as a host program. The
 * loader keeps the firmware's charger drivers and publishes what they say in a page of memory, written
 * while the kernel runs; the kernel only reads it. So the model here is that page, as a loader would
 * write it: whole, in the middle of an update, with values that cannot be, or not there at all.
 *
 *   battery_driver_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <time.h>
#include <unistd.h>

#include "fakekernel.h"

extern char log_text[];

/* ---- the loader's page -------------------------------------------------------------------------- */

#define PAGE            0x4000ul
#define CACHE_PA        0x9f6c0140ull           /* (not at the start of its page, as the loader's is not) */
#define MAGIC           0x2154544142574452ull
#define VALID           (1u << 0)
#define PRESENT         (1u << 1)
#define CHARGER         (1u << 2)
#define VOLTAGE_VALID   (1u << 3)
#define CHARGING        (1u << 4)
#define ESTIMATED       (1u << 5)

struct cache {
	uint64_t magic;
	uint32_t version, size;
	volatile uint32_t sequence;
	uint32_t flags;
	int32_t soc_percent, voltage_mv, current_ma, temperature_c;
	uint32_t charger_action, polls, failures, reserved;
};

static _Alignas(16) uint8_t page[PAGE];
static struct cache *const cache = (struct cache *)(void *)(page + (CACHE_PA & (PAGE - 1)));
static unsigned mapped, mapped_elsewhere;

vm_offset_t
ml_io_map(vm_offset_t phys, vm_size_t size)
{
	if (phys == (CACHE_PA & ~(PAGE - 1)) && size == PAGE) {
		mapped++;
		return (vm_offset_t)page;
	}
	mapped_elsewhere++;
	fprintf(stderr, "MODEL: %#lx mapped, which is not the page of the loader's cache\n", (unsigned long)phys);
	return (vm_offset_t)page;
}

/* a whole update, as the loader makes it: the sequence odd while it is written */
static void
publish(uint32_t flags, int soc, int mv, int ma, int c)
{
	cache->sequence++;
	cache->flags = flags;
	cache->soc_percent = soc;
	cache->voltage_mv = mv;
	cache->current_ma = ma;
	cache->temperature_c = c;
	cache->charger_action = 7;
	cache->polls++;
	cache->sequence++;
}

/* what the kernel's power source would be handed (iokit/Kernel/RedwoodBattery.cpp) */
static unsigned reported, reported_size;
void
RedwoodBatteryReport(const void *state __unused, unsigned size)
{
	reported++;
	reported_size = size;
}

/* the driver's includes are the kernel's: the fake kernel answers for them */
#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#include "hakios_board.c"
#include "redwood_battery.c"
#undef printf                           /* (the driver's is its own: what the test prints is the test's) */

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited = 0; !(cond) && waited < (ms); waited += 5) usleep(5000); } while (0)

static int s(const char *name) { return (int)get(name); }

static void
phone(const char *name)
{
	static char arguments[96];

	board = name;
	fake_delay_divisor = 100;               /* the driver looks every 5 seconds: every 50 ms here */
	snprintf(arguments, sizeof(arguments), "battery=1 battery_shared_pa=%#llx", CACHE_PA);
	boot_args = arguments;
	cache->magic = MAGIC;
	cache->version = 1;
	cache->size = sizeof(*cache);
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

/* what the firmware says is what the sysctls say, and a change is followed */
static void
read_and_followed(void)
{
	phone("phone-cmd");
	publish(VALID | PRESENT | CHARGER | VOLTAGE_VALID | CHARGING, 73, 4012, 850, 31);
	CHECK(redwood_battery_temperature() == -273, "the temperature is not known before the first look");
	redwood_battery_init();
	/* (a look's values are taken one after another: waited for until the last of them is in) */
	WAIT_FOR(s("battery_soc") == 73 && s("battery_temperature_c") == 31 && s("battery_charger_action") == 7, 2000);
	CHECK(s("battery_soc") == 73 && s("battery_voltage_mv") == 4012 && s("battery_current_ma") == 850 &&
	    s("battery_temperature_c") == 31, "charge %d, %d mV, %d mA, %d C", s("battery_soc"), s("battery_voltage_mv"),
	    s("battery_current_ma"), s("battery_temperature_c"));
	CHECK(s("battery_present") == 1 && s("battery_charger_present") == 1 && s("battery_charging") == 1 &&
	    s("battery_soc_estimated") == 0 && s("battery_charger_action") == 7, "the flags, one a sysctl");
	CHECK(redwood_battery_temperature() == 31, "... and for the flashlight's guard: %d C", redwood_battery_temperature());
	CHECK(mapped == 1 && mapped_elsewhere == 0, "the cache's page is mapped once, and nothing else");
	CHECK(logged("battery: Qualcomm UEFI cache at pa") && logged("polling every 5 seconds"), "said where a run can be watched: %s", log_text);
	/* (the thread takes the values, then says them: the line is waited for on its own) */
	EVENTUALLY(logged("battery: SOC=73%, V=4012 mV, I=850 mA, T=31 C"), 2000, "the first reading too: %s", log_text);
	publish(VALID | PRESENT | VOLTAGE_VALID, 72, 3987, -410, 33);
	WAIT_FOR(s("battery_soc") == 72 && s("battery_current_ma") == -410 && s("battery_charging") == 0 && s("battery_charger_present") == 0, 2000);
	CHECK(s("battery_soc") == 72 && s("battery_current_ma") == -410 && s("battery_charging") == 0 &&
	    s("battery_charger_present") == 0, "the next look has the change: %d%%, %d mA", s("battery_soc"), s("battery_current_ma"));
	CHECK(s("battery_from_dsp") == 0, "from the firmware, not the audio DSP");
}

/* a charge the firmware could only estimate from the voltage, and one without a voltage */
static void
estimated_and_without_voltage(void)
{
	phone("phone-cmd");
	publish(VALID | PRESENT | ESTIMATED, 40, 3700, 0, 25);
	redwood_battery_init();
	WAIT_FOR(s("battery_soc") == 40, 2000);
	CHECK(s("battery_soc_estimated") == 1, "an estimate is said to be one");
	CHECK(s("battery_voltage_mv") == -1, "a voltage the firmware does not vouch for is not passed on: %d", s("battery_voltage_mv"));
}

/* the loader in the middle of an update: what is half written is not taken */
static void
torn_update(void)
{
	phone("phone-cmd");
	publish(VALID | PRESENT | VOLTAGE_VALID, 55, 3800, -200, 28);
	redwood_battery_init();
	WAIT_FOR(s("battery_soc") == 55, 2000);
	cache->sequence++;                      /* an update begins ... */
	cache->soc_percent = 99;
	cache->voltage_mv = 1;
	usleep(300 * 1000);                     /* ... and several looks fall into it */
	CHECK(s("battery_soc") == 55 && s("battery_voltage_mv") == 3800, "the last whole values stand: %d%%, %d mV",
	    s("battery_soc"), s("battery_voltage_mv"));
	cache->voltage_mv = 4100;
	cache->sequence++;                      /* ... and ends */
	WAIT_FOR(s("battery_soc") == 99 && s("battery_voltage_mv") == 4100, 2000);
	CHECK(s("battery_soc") == 99 && s("battery_voltage_mv") == 4100, "and the whole update is taken once it is whole");
}

/* values that cannot be are not passed on */
static void
impossible_values(void)
{
	phone("phone-cmd");
	publish(VALID | PRESENT | VOLTAGE_VALID, 61, 3900, 0, 27);
	redwood_battery_init();
	WAIT_FOR(s("battery_soc") == 61, 2000);
	publish(VALID | PRESENT | VOLTAGE_VALID, 150, 3900, 0, 27);
	usleep(250 * 1000);
	CHECK(s("battery_soc") == 61, "a charge of 150%% is not believed: %d", s("battery_soc"));
	publish(PRESENT | VOLTAGE_VALID, 20, 3500, 0, 27);
	usleep(250 * 1000);
	CHECK(s("battery_soc") == 61, "nor what the loader has not marked valid: %d", s("battery_soc"));
	publish(VALID | PRESENT | VOLTAGE_VALID, -1, 3500, 0, 27);
	usleep(250 * 1000);
	CHECK(s("battery_soc") == 61, "nor a charge below nothing: %d", s("battery_soc"));
}

/* a page that is not the loader's cache: nothing of it is used */
static void
not_a_cache(void)
{
	phone("phone-cmd");
	cache->magic = 0x1122334455667788ull;
	cache->soc_percent = 50;
	cache->flags = VALID;
	redwood_battery_init();
	usleep(250 * 1000);
	CHECK(s("battery_soc") == -1 && s("battery_temperature_c") == -273, "unknown stays unknown: %d%%, %d C", s("battery_soc"),
	    s("battery_temperature_c"));
	CHECK(logged("battery: bad xnuloader cache at") && !logged("polling every 5 seconds"), "said, and not watched: %s", log_text);
}

/* a page shorter than the layout this driver reads: the same */
static void
shorter_cache(void)
{
	phone("phone-cmd");
	cache->size = sizeof(*cache) - 4;
	publish(VALID, 50, 0, 0, 20);
	redwood_battery_init();
	usleep(250 * 1000);
	CHECK(s("battery_soc") == -1, "not read: %d", s("battery_soc"));
	CHECK(logged("battery: bad xnuloader cache at") && !logged("polling every 5 seconds"), "said, and not watched: %s", log_text);
}

/*
 * A page that was good when the driver started and is not any more (a loader that stopped, memory written
 * over): every look checks it again, and the last good values stand.
 */
static void
goes_bad_later(void)
{
	phone("phone-cmd");
	publish(VALID | PRESENT | VOLTAGE_VALID, 61, 3900, 0, 27);
	redwood_battery_init();
	WAIT_FOR(s("battery_soc") == 61, 2000);
	cache->magic ^= 1;
	publish(VALID | PRESENT | VOLTAGE_VALID, 11, 3400, 0, 27);
	usleep(250 * 1000);
	CHECK(s("battery_soc") == 61, "another magic: %d", s("battery_soc"));
	cache->magic ^= 1;
	cache->version = 9;
	publish(VALID | PRESENT | VOLTAGE_VALID, 12, 3400, 0, 27);
	usleep(250 * 1000);
	CHECK(s("battery_soc") == 61, "another version: %d", s("battery_soc"));
	cache->version = 1;
	cache->size = 8;
	publish(VALID | PRESENT | VOLTAGE_VALID, 13, 3400, 0, 27);
	usleep(250 * 1000);
	CHECK(s("battery_soc") == 61, "a size that leaves out what is read: %d", s("battery_soc"));
	cache->size = sizeof(*cache);
	publish(VALID | PRESENT | VOLTAGE_VALID, 14, 3400, 0, 27);
	WAIT_FOR(s("battery_soc") == 14 && s("battery_voltage_mv") == 3400, 2000);
	CHECK(s("battery_soc") == 14 && s("battery_voltage_mv") == 3400, "whole again, it is read again: %d", s("battery_soc"));
}

static void
another_version(void)
{
	phone("phone-cmd");
	cache->version = 2;
	publish(VALID, 50, 0, 0, 20);
	redwood_battery_init();
	usleep(250 * 1000);
	CHECK(s("battery_soc") == -1, "a cache of a layout this driver does not know is not read: %d", s("battery_soc"));
	CHECK(logged("battery: bad xnuloader cache at") && logged("version 2"), "and why is said: %s", log_text);
}

/* a loader that gave no address, a kernel that was not asked, a board without the feature: nothing mapped */
static void
no_address(void)
{
	phone("phone-cmd");
	boot_args = "battery=1";
	publish(VALID, 50, 0, 0, 20);
	redwood_battery_init();
	usleep(150 * 1000);
	CHECK(mapped == 0 && mapped_elsewhere == 0 && s("battery_soc") == -1, "nothing mapped, nothing known");
	CHECK(logged("battery: xnuloader did not provide battery_shared_pa"), "and why is said: %s", log_text);
}

static void
off_by_default(void)
{
	static char arguments[64];

	phone("phone-cmd");
	snprintf(arguments, sizeof(arguments), "battery_shared_pa=%#llx", CACHE_PA);
	boot_args = arguments;
	publish(VALID, 50, 0, 0, 20);
	redwood_battery_init();
	usleep(150 * 1000);
	CHECK(mapped == 0 && s("battery_soc") == -1, "without battery=1 the page is not even mapped");
	CHECK(!logged("battery:"), "and nothing is said: %s", log_text);
}

static void
board_without_it(void)
{
	phone("qemu-virt");
	publish(VALID, 50, 0, 0, 20);
	redwood_battery_init();
	usleep(150 * 1000);
	CHECK(mapped == 0 && mapped_elsewhere == 0 && s("battery_soc") == -1, "a board that does not list the feature maps nothing");
	CHECK(!logged("battery:"), "and says nothing: %s", log_text);
}

/* what the audio DSP's battery manager reports instead: kept for the sysctls, handed on to iOS's power source */
static void
from_the_dsp(void)
{
	struct redwood_battery_state state = {
		.size = sizeof(state), .percent_hundredths = 8349, .voltage_uv = 4201000, .current_ua = -512000,
		.temperature_hundredths = 3149, .present = 1, .external = 1, .status = 1, .full_uah = 4100000,
		.design_uah = 4250000, .cycles = 212,
	};

	phone("phone-cmd");
	redwood_battery_report(&state);
	CHECK(s("battery_soc") == 83 && s("battery_voltage_mv") == 4201 && s("battery_current_ma") == -512 &&
	    s("battery_temperature_c") == 31, "rounded to what the sysctls carry: %d%%, %d mV, %d mA, %d C", s("battery_soc"),
	    s("battery_voltage_mv"), s("battery_current_ma"), s("battery_temperature_c"));
	CHECK(s("battery_charging") == 1 && s("battery_charger_present") == 1 && s("battery_from_dsp") == 1, "its state");
	CHECK(s("battery_full_uah") == 4100000 && s("battery_design_uah") == 4250000 && s("battery_cycles") == 212,
	    "what the gauge has learnt");
	CHECK(reported == 1 && reported_size == sizeof(state), "handed on whole to the power source iOS reads");
	state.percent_hundredths = 10160;       /* (a gauge above its own full charge: 102%) */
	state.full_uah = 0;
	redwood_battery_report(&state);
	CHECK(s("battery_soc") == 100 && s("battery_full_uah") == 4100000, "never above 100%%; a value not known yet keeps the last");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "read_and_followed", read_and_followed }, { "estimated_and_without_voltage", estimated_and_without_voltage },
		{ "torn_update", torn_update }, { "impossible_values", impossible_values }, { "not_a_cache", not_a_cache }, { "shorter_cache", shorter_cache }, { "goes_bad_later", goes_bad_later },
		{ "another_version", another_version }, { "no_address", no_address }, { "off_by_default", off_by_default },
		{ "board_without_it", board_without_it }, { "from_the_dsp", from_the_dsp },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			/* the driver's own lines are not the test's result */
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
