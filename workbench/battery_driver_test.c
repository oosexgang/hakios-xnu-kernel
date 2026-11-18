/*
 * The battery's driver itself (drivers/board/battery.c), run as a host program. The
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

/* what the kernel's power source would be handed (iokit/Kernel/BoardBattery.cpp) */
static unsigned reported, reported_size;
static unsigned char last_report[256];
void
HakiosBatteryReport(const void *state, unsigned size)
{
	reported++;
	reported_size = size;
	memcpy(last_report, state, size < sizeof(last_report) ? size : sizeof(last_report));
}

void hakios_tl(const char *format __unused, ...) {}

/*
 * The driver's copy of the cache, which a test can have the loader write into while it is made: half of it
 * copied, then a whole update made (the sequence even before and after it), then the rest. What is copied
 * is then half of one update and half of the next.
 */
static int race_soc = -1, race_mv;
static void *
copy_while_updated(void *to, const void *from, size_t size)
{
	memcpy(to, from, size / 2);
	if (race_soc >= 0) {
		publish(VALID | PRESENT | VOLTAGE_VALID, race_soc, race_mv, 0, 28);
		race_soc = -1;
	}
	return memcpy((uint8_t *)to + size / 2, (const uint8_t *)from + size / 2, size - size / 2), to;
}

/*
 * The driver's clock, which a test can put forward: a minute without news is not waited for (the fake kernel's own
 * clock is the host's, and its waits are only shortened).
 */
static uint64_t clock_ahead_ns;
static uint64_t
driver_clock(void)
{
	return mach_absolute_time() + __atomic_load_n(&clock_ahead_ns, __ATOMIC_RELAXED);
}

static void
clock_ahead(unsigned seconds)
{
	__atomic_store_n(&clock_ahead_ns, (uint64_t)seconds * NSEC_PER_SEC, __ATOMIC_RELAXED);
}

/*
 * What the driver says, which a test can have the audio DSP's battery manager report in the middle of: armed, that
 * report is made when the driver says the loader's cache is passed on again -- after its look at which source is in
 * use, before it goes on with the cache's values.
 */
struct hakios_battery_state;
static const struct hakios_battery_state *report_when_passed_on;
static int said_while_reported(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* the driver's includes are the kernel's: the fake kernel answers for them */
#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#include "board/board.c"
#include "board/log.h"                  /* (the driver's own, here first: its hakios_say() is the one below) */
#define hakios_say said_while_reported
#define memcpy copy_while_updated
#define mach_absolute_time driver_clock
#include "board/battery.c"
#undef mach_absolute_time
#undef memcpy
#undef hakios_say
#undef printf                           /* (the driver's is its own: what the test prints is the test's) */

static int
said_while_reported(const char *format, ...)
{
	const struct hakios_battery_state *report;
	va_list list;
	int length;

	va_start(list, format);
	length = hakios_vsay(true, format, list);
	va_end(list);
	if (strstr(format, "is passed on again") != NULL &&
	    (report = __atomic_exchange_n(&report_when_passed_on, NULL, __ATOMIC_RELAXED)) != NULL) {
		hakios_battery_report(report);
	}
	return length;
}

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited = 0; !(cond) && waited < (ms); waited += 5) usleep(5000); } while (0)

static int s(const char *name) { return (int)get(name); }

/* how many times the log has TEXT */
static unsigned
times_said(const char *text)
{
	unsigned n = 0;

	for (const char *at = log_text; (at = strstr(at, text)) != NULL; at += strlen(text)) {
		n++;
	}
	return n;
}

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
	CHECK(hakios_battery_temperature() == -273, "the temperature is not known before the first look");
	hakios_battery_init();
	/* (a look's values are taken one after another: waited for until the last of them is in) */
	WAIT_FOR(s("battery_soc") == 73 && s("battery_temperature_c") == 31 && s("battery_charger_action") == 7, 2000);
	CHECK(s("battery_soc") == 73 && s("battery_voltage_mv") == 4012 && s("battery_current_ma") == 850 &&
	    s("battery_temperature_c") == 31, "charge %d, %d mV, %d mA, %d C", s("battery_soc"), s("battery_voltage_mv"),
	    s("battery_current_ma"), s("battery_temperature_c"));
	CHECK(s("battery_present") == 1 && s("battery_charger_present") == 1 && s("battery_charging") == 1 &&
	    s("battery_soc_estimated") == 0 && s("battery_charger_action") == 7, "the flags, one a sysctl");
	CHECK(hakios_battery_temperature() == 31, "... and for the flashlight's guard: %d C", hakios_battery_temperature());
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
	hakios_battery_init();
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
	hakios_battery_init();
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

/* an update made from start to end while the driver copies the cache: what it copied is not taken, the next copy is */
static void
updated_while_copied(void)
{
	phone("phone-cmd");
	publish(VALID | PRESENT | VOLTAGE_VALID, 30, 3500, 0, 28);
	hakios_battery_init();
	WAIT_FOR(s("battery_soc") == 30, 2000);
	race_mv = 4200;
	race_soc = 90;                          /* (armed: the next copy is torn -- its percent the old, its voltage the new) */
	WAIT_FOR(race_soc == -1, 2000);
	usleep(15 * 1000);                      /* (a third of a look: the copy made again at once, not at the next look) */
	CHECK(s("battery_soc") == 90 && s("battery_voltage_mv") == 4200, "the whole new update, not half of each: %d%%, %d mV",
	    s("battery_soc"), s("battery_voltage_mv"));
}

/* empty and full are values like any other */
static void
empty_and_full(void)
{
	phone("phone-cmd");
	publish(VALID | PRESENT | VOLTAGE_VALID, 0, 3300, 0, 20);
	hakios_battery_init();
	WAIT_FOR(s("battery_soc") == 0, 2000);
	CHECK(s("battery_soc") == 0, "0%% is taken: %d", s("battery_soc"));
	publish(VALID | PRESENT | VOLTAGE_VALID, 100, 4400, 0, 20);
	WAIT_FOR(s("battery_soc") == 100, 2000);
	CHECK(s("battery_soc") == 100, "100%% is taken: %d", s("battery_soc"));
}

/* values that cannot be are not passed on */
static void
impossible_values(void)
{
	phone("phone-cmd");
	publish(VALID | PRESENT | VOLTAGE_VALID, 61, 3900, 0, 27);
	hakios_battery_init();
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
	hakios_battery_init();
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
	hakios_battery_init();
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
	hakios_battery_init();
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
	hakios_battery_init();
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
	hakios_battery_init();
	usleep(150 * 1000);
	CHECK(mapped == 0 && mapped_elsewhere == 0 && s("battery_soc") == -1, "nothing mapped, nothing known");
	CHECK(logged("battery: xnuloader did not provide battery_shared_pa"), "and why is said: %s", log_text);
}

/* battery=0 is not being asked; an address of 0 is no address */
static void
asked_for_none(void)
{
	static char arguments[96];

	phone("phone-cmd");
	snprintf(arguments, sizeof(arguments), "battery=0 battery_shared_pa=%#llx", CACHE_PA);
	boot_args = arguments;
	publish(VALID, 50, 0, 0, 20);
	hakios_battery_init();
	usleep(150 * 1000);
	CHECK(mapped == 0 && s("battery_soc") == -1 && !logged("battery:"), "battery=0: nothing mapped, nothing said");
}

static void
address_zero(void)
{
	phone("phone-cmd");
	boot_args = "battery=1 battery_shared_pa=0";
	hakios_battery_init();
	usleep(150 * 1000);
	CHECK(mapped == 0 && mapped_elsewhere == 0, "nothing mapped at 0");
	CHECK(logged("battery: xnuloader did not provide battery_shared_pa"), "said: %s", log_text);
}

static void
off_by_default(void)
{
	static char arguments[64];

	phone("phone-cmd");
	snprintf(arguments, sizeof(arguments), "battery_shared_pa=%#llx", CACHE_PA);
	boot_args = arguments;
	publish(VALID, 50, 0, 0, 20);
	hakios_battery_init();
	usleep(150 * 1000);
	CHECK(mapped == 0 && s("battery_soc") == -1, "without battery=1 the page is not even mapped");
	CHECK(!logged("battery:"), "and nothing is said: %s", log_text);
}

static void
board_without_it(void)
{
	phone("qemu-virt");
	publish(VALID, 50, 0, 0, 20);
	hakios_battery_init();
	usleep(150 * 1000);
	CHECK(mapped == 0 && mapped_elsewhere == 0 && s("battery_soc") == -1, "a board that does not list the feature maps nothing");
	CHECK(!logged("battery:"), "and says nothing: %s", log_text);
}

/* what the audio DSP's battery manager reports instead: kept for the sysctls, handed on to Darwin's power source */
static void
from_the_dsp(void)
{
	struct hakios_battery_state state = {
		.size = sizeof(state), .percent_hundredths = 8349, .voltage_uv = 4201000, .current_ua = -512000,
		.temperature_hundredths = 3149, .present = 1, .external = 1, .status = 1, .full_uah = 4100000,
		.design_uah = 4250000, .cycles = 212,
	};

	phone("phone-cmd");
	hakios_battery_report(&state);
	CHECK(s("battery_soc") == 83 && s("battery_voltage_mv") == 4201 && s("battery_current_ma") == -512 &&
	    s("battery_temperature_c") == 31, "rounded to what the sysctls carry: %d%%, %d mV, %d mA, %d C", s("battery_soc"),
	    s("battery_voltage_mv"), s("battery_current_ma"), s("battery_temperature_c"));
	CHECK(s("battery_charging") == 1 && s("battery_charger_present") == 1 && s("battery_from_dsp") == 1 && s("battery_present") == 1,
	    "its state");
	CHECK(s("battery_full_uah") == 4100000 && s("battery_design_uah") == 4250000 && s("battery_cycles") == 212,
	    "what the gauge has learnt");
	CHECK(reported == 1 && reported_size == sizeof(state), "handed on whole to the power source Darwin reads");
	state.percent_hundredths = 10160;       /* (a gauge above its own full charge: 102%) */
	state.full_uah = 0;
	hakios_battery_report(&state);
	CHECK(s("battery_soc") == 100 && s("battery_full_uah") == 4100000, "never above 100%%; a value not known yet keeps the last");
	state.design_uah = 0;
	state.cycles = 0;
	state.present = 0;
	state.external = 0;
	state.status = 2;
	state.percent_hundredths = -100;        /* (a gauge that has lost its way: under nothing) */
	hakios_battery_report(&state);
	CHECK(s("battery_design_uah") == 4250000 && s("battery_cycles") == 212, "nor the design's charge, nor the cycles");
	CHECK(s("battery_present") == 0 && s("battery_charger_present") == 0 && s("battery_charging") == 0, "no battery, no charger: said");
	CHECK(s("battery_soc") == 0, "never under 0%%: %d", s("battery_soc"));
	state.percent_hundredths = 10000;
	hakios_battery_report(&state);
	CHECK(s("battery_soc") == 100, "100%% is 100%%: %d", s("battery_soc"));
}

/* ---- what Darwin is told, from the firmware's readings (hakios_battery_core.h) ----------------------- */

static const struct hakios_battery_state *told(void) { return (const struct hakios_battery_state *)(const void *)last_report; }

/* the arithmetic: the table, the resistance, the firmware's figure believed only when the voltage agrees */
static void
the_arithmetic(void)
{
	CHECK(bat_tenths_at(3880) == 500 && bat_tenths_at(3300) == 0 && bat_tenths_at(4500) == 1000, "the table's ends and a row");
	CHECK(bat_tenths_at(3910) == 550, "between two rows: %d", bat_tenths_at(3910));
	CHECK(bat_ocv_mv(4347, 557) == 4391 && bat_ocv_mv(4410, -397) == 4379, "redwood's two readings come to one cell: %d, %d",
	    bat_ocv_mv(4347, 557), bat_ocv_mv(4410, -397));
	CHECK(bat_estimate_tenths(73, 0, 4012, 850) == 730, "a figure the voltage agrees with is the firmware's");
	CHECK(bat_estimate_tenths(15, 0, 4405, -373) >= 990, "15%% at 4.4 V is not: %d", bat_estimate_tenths(15, 0, 4405, -373));
	CHECK(bat_estimate_tenths(40, 1, 3880, 0) == 500, "an estimate of the firmware's own gives way to this one's");
	CHECK(bat_estimate_tenths(40, 0, -1, 0) == 400, "no voltage: the firmware's figure as it is");
	CHECK(bat_shown_tenths(-1, 640, 0) == 640, "the first look is shown as it is");
	CHECK(bat_shown_tenths(640, 600, 0) == 638 && bat_shown_tenths(640, 600, 1) == 640, "down a little while it gives charge, not while it takes it");
	CHECK(bat_shown_tenths(640, 680, 1) == 642 && bat_shown_tenths(640, 680, 0) == 640, "up a little while it takes charge, not while it gives it");
	CHECK(bat_shown_tenths(150, 990, 0) == 170, "far off, toward it whichever way: %d", bat_shown_tenths(150, 990, 0));
}

/* redwood's day: the firmware said 15%% at 4.4 V on a computer's port, taking 373 mA */
static void
a_stale_figure_is_not_shown(void)
{
	phone("phone-cmd");
	publish(VALID | PRESENT | CHARGER | VOLTAGE_VALID | CHARGING, 15, 4405, -373, 45);
	hakios_battery_init();
	WAIT_FOR(reported > 0, 2000);
	CHECK(reported > 0 && reported_size == sizeof(struct hakios_battery_state), "Darwin is told");
	CHECK(told()->percent_hundredths >= 9900, "nearly full, not 15%%: %d", told()->percent_hundredths);
	CHECK(told()->status == 1 && told()->external == 1 && told()->current_ua == 373000, "charging, 373 mA in, Darwin's way round: %d %d %d",
	    told()->status, told()->external, told()->current_ua);
	CHECK(told()->voltage_uv == 4405000 && told()->temperature_hundredths == 4500 && told()->present == 1, "the rest as read");
	CHECK(s("battery_soc") == 15 && s("battery_shown") >= 990, "the sysctls: the firmware's figure, and what is shown: %d, %d",
	    s("battery_soc"), s("battery_shown"));
}

/* on a computer's port, drawing more than it gives: on a charger, not charging */
static void
on_a_port_that_gives_too_little(void)
{
	phone("phone-cmd");
	publish(VALID | PRESENT | CHARGER | VOLTAGE_VALID | CHARGING, 60, 3900, 450, 40);
	hakios_battery_init();
	WAIT_FOR(reported > 0, 2000);
	CHECK(told()->external == 1 && told()->status == 3 && told()->current_ua == -450000, "not charging: %d, %d mA",
	    told()->status, told()->current_ua / 1000);
	CHECK(told()->percent_hundredths == 6000, "the firmware's 60%%, which its voltage under load agrees with: %d", told()->percent_hundredths);
	publish(VALID | PRESENT | VOLTAGE_VALID, 60, 3880, 300, 40);
	WAIT_FOR(told()->external == 0, 2000);
	CHECK(told()->external == 0 && told()->status == 2, "off the charger: discharging (%d)", told()->status);
}

/* what is shown follows the firmware a little at a time: a drop of 4 points in one reading is not shown at once */
static void
shown_a_little_at_a_time(void)
{
	phone("phone-cmd");
	publish(VALID | PRESENT | VOLTAGE_VALID, 64, 3960, 200, 30);
	hakios_battery_init();
	WAIT_FOR(s("battery_shown") == 640, 2000);
	CHECK(s("battery_shown") == 640, "the first: %d", s("battery_shown"));
	publish(VALID | PRESENT | VOLTAGE_VALID, 60, 3950, 200, 30);
	WAIT_FOR(s("battery_shown") < 640, 2000);
	CHECK(s("battery_shown") >= 636 && s("battery_shown") <= 638, "two tenths a look: %d", s("battery_shown"));
	WAIT_FOR(s("battery_shown") == 600, 3000);
	CHECK(s("battery_shown") == 600, "and there in the end: %d", s("battery_shown"));
}

/* once the audio DSP's battery manager speaks, the firmware's readings are not passed on */
static void
the_dsp_knows_better(void)
{
	struct hakios_battery_state state = {
		.size = sizeof(state), .percent_hundredths = 5100, .voltage_uv = 3900000, .current_ua = -100000,
		.temperature_hundredths = 3000, .present = 1, .external = 0, .status = 2,
	};

	phone("phone-cmd");
	publish(VALID | PRESENT | VOLTAGE_VALID, 64, 3960, 200, 30);
	hakios_battery_init();
	WAIT_FOR(reported > 0, 2000);
	hakios_battery_report(&state);
	reported = 0;
	publish(VALID | PRESENT | VOLTAGE_VALID, 20, 3700, 200, 30);
	usleep(250 * 1000);
	CHECK(reported == 0, "nothing more from the firmware: %u", reported);
}

/*
 * A loader that has stopped polling (its cache whole, its polls no longer moving on): what it said last is believed
 * for a minute, and then the temperature is no longer handed out -- said once -- until the cache moves on again.
 */
static void
loader_goes_quiet(void)
{
	phone("phone-cmd");
	publish(VALID | PRESENT | VOLTAGE_VALID, 70, 3990, 300, 31);
	hakios_battery_init();
	WAIT_FOR(hakios_battery_temperature() == 31, 2000);
	CHECK(hakios_battery_temperature() == 31, "known: %d C", hakios_battery_temperature());
	clock_ahead(50);
	usleep(250 * 1000);                     /* (looks at the same cache: nothing new, still within the minute) */
	CHECK(hakios_battery_temperature() == 31 && !logged("nothing new"), "believed for a minute: %d C", hakios_battery_temperature());
	clock_ahead(61);
	/* (said by the driver's own thread: nothing else need ask) */
	EVENTUALLY(logged("battery: nothing new from xnuloader's cache for 60 s"), 2000, "a quiet loader is said: %s", log_text);
	CHECK(hakios_battery_temperature() == -273, "and its temperature is not known any more: %d C", hakios_battery_temperature());
	usleep(250 * 1000);
	(void)hakios_battery_temperature();
	CHECK(times_said("nothing new from") == 1, "said once, not at every look: %u", times_said("nothing new from"));
	publish(VALID | PRESENT | VOLTAGE_VALID, 69, 3985, 300, 33);
	WAIT_FOR(hakios_battery_temperature() == 33, 2000);
	CHECK(hakios_battery_temperature() == 33, "polling again, it is known again: %d C", hakios_battery_temperature());
	EVENTUALLY(logged("battery: xnuloader's cache has news again"), 2000, "and that is said: %s", log_text);
	usleep(250 * 1000);
	CHECK(times_said("has news again") == 1 && times_said("nothing new from") == 1, "once each: %s", log_text);
}

/* the audio DSP's battery manager reports, then says nothing more (the DSP gone, its channel broken): the same */
static void
dsp_goes_quiet(void)
{
	struct hakios_battery_state state = {
		.size = sizeof(state), .percent_hundredths = 6000, .voltage_uv = 3900000, .current_ua = -100000,
		.temperature_hundredths = 3149, .present = 1, .external = 0, .status = 2,
	};

	phone("phone-cmd");
	clock_ahead(120);
	CHECK(hakios_battery_temperature() == -273 && !logged("nothing new"), "nothing ever heard: not known, and nothing to say");
	clock_ahead(0);
	hakios_battery_report(&state);
	CHECK(hakios_battery_temperature() == 31, "known: %d C", hakios_battery_temperature());
	clock_ahead(59);
	CHECK(hakios_battery_temperature() == 31, "believed for a minute: %d C", hakios_battery_temperature());
	clock_ahead(61);
	CHECK(hakios_battery_temperature() == -273, "a minute without a report: not known any more (%d C)", hakios_battery_temperature());
	CHECK(hakios_battery_temperature() == -273 && hakios_battery_temperature() == -273, "nor at the next questions");
	CHECK(times_said("battery: nothing new from the audio DSP's battery manager for 60 s") == 1, "said once: %s", log_text);
	hakios_battery_report(&state);
	CHECK(hakios_battery_temperature() == 31, "a report again: known again (%d C)", hakios_battery_temperature());
	CHECK(times_said("battery: the audio DSP's battery manager has news again") == 1, "and said once: %s", log_text);
	(void)hakios_battery_temperature();
	CHECK(times_said("nothing new from") == 1 && times_said("has news again") == 1, "once each: %s", log_text);
}

/*
 * Both: while the audio DSP's battery manager reports, the loader's cache is not passed on, nor kept for the
 * sysctls; once the DSP has been quiet for a minute and the cache is new, the cache is passed on again, until the
 * DSP speaks again.
 */
static void
dsp_quiet_loader_takes_over(void)
{
	struct hakios_battery_state state = {
		.size = sizeof(state), .percent_hundredths = 5100, .voltage_uv = 3900000, .current_ua = -100000,
		.temperature_hundredths = 2500, .present = 1, .external = 0, .status = 2,
	};

	phone("phone-cmd");
	publish(VALID | PRESENT | VOLTAGE_VALID, 64, 3960, 200, 30);
	hakios_battery_init();
	WAIT_FOR(reported > 0, 2000);
	hakios_battery_report(&state);
	reported = 0;
	publish(VALID | PRESENT | VOLTAGE_VALID, 20, 3700, 200, 35);
	usleep(250 * 1000);
	CHECK(reported == 0 && s("battery_from_dsp") == 1, "the DSP's: %u reports from the cache", reported);
	CHECK(s("battery_soc") == 51 && hakios_battery_temperature() == 25, "the sysctls and the guard keep the DSP's: %d%%, %d C",
	    s("battery_soc"), hakios_battery_temperature());
	clock_ahead(61);
	publish(VALID | PRESENT | VOLTAGE_VALID, 50, 3880, 200, 36);
	WAIT_FOR(s("battery_from_dsp") == 0 && reported > 0, 2000);
	CHECK(s("battery_from_dsp") == 0 && reported > 0, "a quiet DSP gives way to the cache, which is new");
	CHECK(hakios_battery_temperature() == 36 && s("battery_soc") == 50, "the cache's: %d C, %d%%", hakios_battery_temperature(),
	    s("battery_soc"));
	CHECK(logged("nothing new from the audio DSP's battery manager for 60 s: xnuloader's cache, which is new, is passed on again"),
	    "and that is said: %s", log_text);
	CHECK(told()->percent_hundredths >= 4900 && told()->percent_hundredths <= 5100, "what is shown goes on from the DSP's 51%%: %d",
	    told()->percent_hundredths);
	usleep(250 * 1000);
	CHECK(hakios_battery_temperature() == 36, "not quiet: the cache is the source now, and was new a moment ago (%d C)",
	    hakios_battery_temperature());
	hakios_battery_report(&state);
	CHECK(s("battery_from_dsp") == 1 && logged("battery: the audio DSP's battery manager speaks again"), "the DSP again: %s", log_text);
	reported = 0;
	publish(VALID | PRESENT | VOLTAGE_VALID, 49, 3870, 200, 36);
	usleep(250 * 1000);
	CHECK(reported == 0 && hakios_battery_temperature() == 25, "and the cache is not passed on: %u, %d C", reported,
	    hakios_battery_temperature());
}

/*
 * The audio DSP's battery manager speaks again just as the loader's cache is passed on in its place (it had been
 * quiet for a minute): the DSP is the source from then on, and what is kept -- for the sysctls, for the flashlight's
 * guard -- is what it said, not the cache's values that the loader's readings took after their look at the source.
 */
static void
dsp_speaks_as_the_cache_is_taken(void)
{
	struct hakios_battery_state state = {
		.size = sizeof(state), .percent_hundredths = 5100, .voltage_uv = 3900000, .current_ua = -100000,
		.temperature_hundredths = 2500, .present = 1, .external = 0, .status = 2,
	};

	phone("phone-cmd");
	publish(VALID | PRESENT | VOLTAGE_VALID, 64, 3960, 200, 30);
	hakios_battery_init();
	WAIT_FOR(reported > 0, 2000);
	hakios_battery_report(&state);
	clock_ahead(61);
	__atomic_store_n(&report_when_passed_on, &state, __ATOMIC_RELAXED);
	publish(VALID | PRESENT | VOLTAGE_VALID, 50, 3880, 200, 36);
	WAIT_FOR(__atomic_load_n(&report_when_passed_on, __ATOMIC_RELAXED) == NULL, 2000);
	usleep(250 * 1000);
	CHECK(logged("battery: the audio DSP's battery manager speaks again"), "the DSP spoke while the cache was taken: %s", log_text);
	CHECK(s("battery_from_dsp") == 1, "the DSP is the source");
	CHECK(s("battery_soc") == 51 && s("battery_voltage_mv") == 3900 && hakios_battery_temperature() == 25,
	    "and what is kept is the DSP's, not the cache's: %d%%, %d mV, %d C", s("battery_soc"), s("battery_voltage_mv"),
	    hakios_battery_temperature());
}

/* full on the charger, taking nothing: full, not "not charging" */
static void
full_on_the_charger(void)
{
	phone("phone-cmd");
	publish(VALID | PRESENT | CHARGER | VOLTAGE_VALID, 100, 4450, 0, 30);
	hakios_battery_init();
	WAIT_FOR(reported > 0, 2000);
	CHECK(told()->percent_hundredths == 10000 && told()->external == 1 && told()->status == 4, "full: %d%%, status %d",
	    told()->percent_hundredths / 100, told()->status);
}

/*
 * A current no cell carries (the firmware's word of its status, which nothing on the way checks): taken for none.
 * Believed, 60 A into the cell made the voltage's estimate 4.8 V lower -- a battery at 70% shown as empty at the
 * first look -- and a current near the int's end was multiplied past it.
 */
static void
a_current_that_cannot_be(void)
{
	phone("phone-cmd");
	publish(VALID | PRESENT | VOLTAGE_VALID, 70, 3990, -60000, 31);
	hakios_battery_init();
	WAIT_FOR(reported > 0, 2000);
	CHECK(reported > 0 && told()->percent_hundredths == 7000, "the firmware's 70%%, which the voltage at rest agrees with, not empty: %d",
	    told()->percent_hundredths);
	CHECK(told()->current_ua == 0 && told()->status == 2, "no current, discharging: %d uA, status %d", told()->current_ua,
	    told()->status);
	publish(VALID | PRESENT | CHARGER | VOLTAGE_VALID, 70, 3990, INT32_MIN, 31);
	WAIT_FOR(told()->external == 1, 2000);
	usleep(250 * 1000);
	CHECK(told()->external == 1 && told()->current_ua == 0 && told()->status == 3, "nor the int's end, taken for charging: %d uA, status %d",
	    told()->current_ua, told()->status);
	CHECK(told()->percent_hundredths == 7000 && s("battery_shown") == 700, "still 70%%: %d", s("battery_shown"));
}

/*
 * A temperature no cell has (an open thermistor, a word of the firmware's status that is something else, a DSP's
 * number near the int's end): not known, and -- known earlier -- no longer reported, for the flashlight's guard.
 * Taken as it was, -400 C was cool enough for the guard, and the int's end was multiplied past it for Darwin.
 */
static void
a_temperature_that_cannot_be(void)
{
	struct hakios_battery_state state = {
		.size = sizeof(state), .percent_hundredths = 6000, .voltage_uv = 3900000, .current_ua = -100000,
		.temperature_hundredths = INT32_MAX, .present = 1, .external = 0, .status = 2,
	};

	phone("phone-cmd");
	publish(VALID | PRESENT | VOLTAGE_VALID, 70, 3990, 300, 31);
	CHECK(!hakios_battery_went_quiet(), "nothing known yet, nothing lost");
	hakios_battery_init();
	WAIT_FOR(hakios_battery_temperature() == 31, 2000);
	CHECK(hakios_battery_temperature() == 31 && !hakios_battery_went_quiet(), "known: %d C", hakios_battery_temperature());
	publish(VALID | PRESENT | VOLTAGE_VALID, 70, 3990, 300, -400);
	WAIT_FOR(s("battery_temperature_c") != 31, 2000);
	CHECK(hakios_battery_temperature() == -273 && s("battery_temperature_c") == -273, "-400 C is not a temperature: %d C",
	    hakios_battery_temperature());
	CHECK(hakios_battery_went_quiet(), "and the guard is told the one known is no longer reported");
	publish(VALID | PRESENT | VOLTAGE_VALID, 70, 3990, 300, INT32_MAX);
	WAIT_FOR(told()->temperature_hundredths == -27300, 2000);
	CHECK(told()->temperature_hundredths == -27300 && hakios_battery_temperature() == -273,
	    "nor the int's end, for Darwin as for the guard: %d", told()->temperature_hundredths);
	publish(VALID | PRESENT | VOLTAGE_VALID, 70, 3990, 300, 33);
	WAIT_FOR(hakios_battery_temperature() == 33, 2000);
	CHECK(hakios_battery_temperature() == 33 && !hakios_battery_went_quiet(), "a temperature again: %d C", hakios_battery_temperature());
	hakios_battery_report(&state);
	CHECK(s("battery_temperature_c") == -273 && hakios_battery_went_quiet(), "the DSP's int's end neither: %d C",
	    s("battery_temperature_c"));
	state.temperature_hundredths = 2950;
	hakios_battery_report(&state);
	CHECK(hakios_battery_temperature() == 30 && !hakios_battery_went_quiet(), "and its 29.5 C is: %d C", hakios_battery_temperature());
}

/* battery_shown=N: N%% shown whatever the firmware says, 100 included */
static void
shown_as_told(void)
{
	static char arguments[128];

	phone("phone-cmd");
	snprintf(arguments, sizeof(arguments), "%s battery_shown=100", boot_args);
	boot_args = arguments;
	publish(VALID | PRESENT | VOLTAGE_VALID, 40, 3750, 200, 30);
	hakios_battery_init();
	WAIT_FOR(reported > 0, 2000);
	CHECK(told()->percent_hundredths == 10000 && s("battery_soc") == 40, "100%% shown, the firmware's 40%% read: %d, %d",
	    told()->percent_hundredths, s("battery_soc"));
}


int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "read_and_followed", read_and_followed }, { "estimated_and_without_voltage", estimated_and_without_voltage },
		{ "torn_update", torn_update }, { "updated_while_copied", updated_while_copied }, { "empty_and_full", empty_and_full }, { "impossible_values", impossible_values }, { "not_a_cache", not_a_cache }, { "shorter_cache", shorter_cache }, { "goes_bad_later", goes_bad_later },
		{ "another_version", another_version }, { "no_address", no_address }, { "off_by_default", off_by_default },
		{ "board_without_it", board_without_it }, { "from_the_dsp", from_the_dsp }, { "asked_for_none", asked_for_none },
		{ "the_arithmetic", the_arithmetic }, { "a_stale_figure_is_not_shown", a_stale_figure_is_not_shown },
		{ "on_a_port_that_gives_too_little", on_a_port_that_gives_too_little },
		{ "shown_a_little_at_a_time", shown_a_little_at_a_time }, { "the_dsp_knows_better", the_dsp_knows_better },
		{ "address_zero", address_zero }, { "full_on_the_charger", full_on_the_charger }, { "shown_as_told", shown_as_told },
		{ "loader_goes_quiet", loader_goes_quiet }, { "dsp_goes_quiet", dsp_goes_quiet },
		{ "dsp_quiet_loader_takes_over", dsp_quiet_loader_takes_over },
		{ "a_current_that_cannot_be", a_current_that_cannot_be }, { "a_temperature_that_cannot_be", a_temperature_that_cannot_be },
		{ "dsp_speaks_as_the_cache_is_taken", dsp_speaks_as_the_cache_is_taken },
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
