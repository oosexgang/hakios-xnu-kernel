/*
 * The list of drivers itself (kernel/overlay/bsd/dev/hakios_drivers.c), run as a host program with a
 * stand-in for every driver's start function that only writes its name down. What is tested: each
 * start function is called once, in the order the drivers depend on each other, on every board; and
 * a board gets the driver of the touch controller it lists, and no other.
 *
 *   drivers_list_test SCENARIO      one scenario a process
 */
#include <unistd.h>

#include "fakekernel.h"

extern char log_text[];

static char started[1024];
#define STARTS(name) void name(void); void name(void) { strcat(started, #name " "); }
STARTS(redwood_input_init) STARTS(redwood_cpufreq_init) STARTS(redwood_virtio_input_init) STARTS(renoir_fts_init)
STARTS(redwood_goodix_init) STARTS(hakios_prof_start) STARTS(redwood_buttons_init) STARTS(redwood_battery_init)
STARTS(redwood_ufs_init) STARTS(redwood_net_init) STARTS(redwood_adreno_init) STARTS(hakios_backlight_init)
STARTS(hakios_thermal_init) STARTS(redwood_wifi_init) STARTS(redwood_adsp_init) STARTS(redwood_audio_init)
STARTS(redwood_virtio_sound_init) STARTS(redwood_speaker_init) STARTS(redwood_haptic_init) STARTS(redwood_torch_init)
STARTS(redwood_camera_init) STARTS(redwood_virtio_blk_init) STARTS(redwood_sparse_init) STARTS(redwood_apfs_check_init)

#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define APFS 1
#define hakios_board_init real_board_init
#include "hakios_board.c"
#undef hakios_board_init
/* (the board is looked up as on a phone; that it was asked for is written down like the rest) */
void hakios_board_init(void);
void hakios_board_init(void) { strcat(started, "hakios_board_init "); real_board_init(); }
#include "hakios_drivers.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* each of them once, and `first' before `then' */
static void
before(const char *first, const char *then)
{
	const char *a = strstr(started, first), *b = strstr(started, then);

	CHECK(a != NULL && b != NULL && a < b, "%s is to start before %s: %s", first, then, started);
}

static unsigned
times(const char *name)
{
	unsigned count = 0;

	for (const char *at = started; (at = strstr(at, name)) != NULL; at += strlen(name)) {
		count++;
	}
	return count;
}

static void
all_once(const char *touch)
{
	static const char *const every[] = {
		"redwood_input_init ", "hakios_board_init ", "redwood_cpufreq_init ", "redwood_virtio_input_init ", "hakios_prof_start ",
		"redwood_buttons_init ", "redwood_battery_init ", "redwood_ufs_init ", "redwood_net_init ", "redwood_adreno_init ",
		"hakios_backlight_init ", "hakios_thermal_init ", "redwood_wifi_init ", "redwood_adsp_init ", "redwood_audio_init ",
		"redwood_virtio_sound_init ", "redwood_speaker_init ", "redwood_haptic_init ", "redwood_torch_init ", "redwood_camera_init ",
		"redwood_virtio_blk_init ", "redwood_sparse_init ", "redwood_apfs_check_init ",
	};

	for (unsigned i = 0; i < sizeof(every) / sizeof(every[0]); i++) {
		CHECK(times(every[i]) == 1, "%sis started %u times", every[i], times(every[i]));
	}
	CHECK(times("renoir_fts_init ") == (strcmp(touch, "renoir_fts_init ") == 0), "the FT3658U's driver: %s", started);
	CHECK(times("redwood_goodix_init ") == (strcmp(touch, "redwood_goodix_init ") == 0), "the Goodix driver: %s", started);
	/* what the order is for */
	before("redwood_input_init ", "redwood_virtio_input_init ");    /* the queue before whoever feeds it */
	before("redwood_input_init ", "redwood_buttons_init ");
	before("hakios_board_init ", "redwood_cpufreq_init ");          /* the board before whoever asks what it has */
	before("redwood_cpufreq_init ", "hakios_thermal_init ");        /* the clock tables before what holds them back */
	before("redwood_buttons_init ", "redwood_wifi_init ");          /* the PMIC arbiter's lock before the others that use the bus */
	before("redwood_adsp_init ", "redwood_audio_init ");
	before("redwood_audio_init ", "redwood_virtio_sound_init ");    /* the device before its sinks */
	before("redwood_virtio_blk_init ", "redwood_apfs_check_init "); /* the disk before what mounts it */
	if (*touch) {
		before("redwood_input_init ", touch);
		before("hakios_board_init ", touch);
	}
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

static void
board_with_ft3658u(void)
{
	board = "phone-cmd";
	boot_args = "touchscreen=1";
	hakios_drivers_init();
	all_once("renoir_fts_init ");
}

static void
board_with_goodix(void)
{
	board = "phone-video";
	boot_args = "";
	hakios_drivers_init();
	all_once("redwood_goodix_init ");
}

/* a board without a touch controller: no touch driver, and that is said when one was asked for */
static void
board_without_touch(void)
{
	board = "qemu-virt";
	boot_args = "touchscreen=1";
	hakios_drivers_init();
	all_once("");
	CHECK(logged("touch: touchscreen=1, but this board's board.toml lists no touch controller"), "said: %s", log_text);
}

static void
board_without_touch_not_asked(void)
{
	board = "phone-keys";
	boot_args = "";
	hakios_drivers_init();
	all_once("");
	CHECK(!logged("touch:"), "nothing is said when nothing was asked: %s", log_text);
}

/* a board the kernel does not know: every driver is still asked, each finds nothing to do */
static void
unknown_board(void)
{
	board = "nobody";
	boot_args = "";
	hakios_drivers_init();
	all_once("");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "board_with_ft3658u", board_with_ft3658u }, { "board_with_goodix", board_with_goodix },
		{ "board_without_touch", board_without_touch }, { "board_without_touch_not_asked", board_without_touch_not_asked },
		{ "unknown_board", unknown_board },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			if (freopen("/dev/null", "w", stdout) == NULL) {
				return 2;
			}
			scenarios[i].run();
			fprintf(stderr, "%-30s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
