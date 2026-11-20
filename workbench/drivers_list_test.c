/*
 * The list of drivers itself (drivers/board/drivers.c), run as a host program with a
 * stand-in for every driver's start function that only writes its name down. What is tested: each
 * start function is called once, in the order the drivers depend on each other, on every board; and
 * a board gets the driver of the touch controller it lists, and no other (one of three).
 *
 *   drivers_list_test SCENARIO      one scenario a process
 */
#include <unistd.h>

#include "fakekernel.h"

extern char log_text[];

static char started[1024];
#define STARTS(name) void name(void); void name(void) { strcat(started, #name " "); }
STARTS(hakios_bt_uart_init)
STARTS(hakios_scmi_perf_init)
STARTS(hakios_input_init) STARTS(hakios_cpufreq_init) STARTS(hakios_virtio_input_init) STARTS(hakios_fts_init)
STARTS(hakios_ft5x06_init)
STARTS(hakios_goodix_init) STARTS(hakios_prof_start) STARTS(hakios_buttons_init) STARTS(hakios_rtc_init) STARTS(hakios_battery_init)
STARTS(hakios_ufs_init) STARTS(hakios_net_init) STARTS(hakios_adreno_init) STARTS(hakios_backlight_init)
STARTS(hakios_thermal_init) STARTS(hakios_wifi_init) STARTS(hakios_adsp_init) STARTS(hakios_mpss_init) STARTS(hakios_audio_init)
STARTS(hakios_virtio_sound_init) STARTS(hakios_speaker_init) STARTS(hakios_haptic_init) STARTS(hakios_imu_init) STARTS(hakios_torch_init)
STARTS(hakios_camera_init) STARTS(hakios_virtio_blk_init) STARTS(hakios_sparse_init) STARTS(hakios_apfs_check_init)

#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define APFS 1
#define hakios_board_init real_board_init
#include "board/board.c"
#undef hakios_board_init
/* (the board is looked up as on a phone; that it was asked for is written down like the rest) */
void hakios_board_init(void);
void hakios_board_init(void) { strcat(started, "hakios_board_init "); real_board_init(); }
#include "board/drivers.c"

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
		"hakios_bt_uart_init ", "hakios_input_init ", "hakios_board_init ", "hakios_scmi_perf_init ", "hakios_cpufreq_init ", "hakios_virtio_input_init ", "hakios_prof_start ",
		"hakios_buttons_init ", "hakios_rtc_init ", "hakios_battery_init ", "hakios_ufs_init ", "hakios_net_init ", "hakios_adreno_init ",
		"hakios_backlight_init ", "hakios_thermal_init ", "hakios_wifi_init ", "hakios_adsp_init ", "hakios_mpss_init ", "hakios_audio_init ",
		"hakios_virtio_sound_init ", "hakios_speaker_init ", "hakios_haptic_init ", "hakios_imu_init ", "hakios_torch_init ", "hakios_camera_init ",
		"hakios_virtio_blk_init ", "hakios_sparse_init ", "hakios_apfs_check_init ",
	};

	for (unsigned i = 0; i < sizeof(every) / sizeof(every[0]); i++) {
		CHECK(times(every[i]) == 1, "%sis started %u times", every[i], times(every[i]));
	}
	CHECK(times("hakios_fts_init ") == (strcmp(touch, "hakios_fts_init ") == 0), "the FT3658U's driver: %s", started);
	CHECK(times("hakios_goodix_init ") == (strcmp(touch, "hakios_goodix_init ") == 0), "the Goodix driver: %s", started);
	CHECK(times("hakios_ft5x06_init ") == (strcmp(touch, "hakios_ft5x06_init ") == 0), "the FT5x06 family's driver: %s", started);
	/* what the order is for */
	before("hakios_input_init ", "hakios_virtio_input_init ");    /* the queue before whoever feeds it */
	before("hakios_input_init ", "hakios_buttons_init ");
	before("hakios_board_init ", "hakios_cpufreq_init ");          /* the board before whoever asks what it has */
	before("hakios_cpufreq_init ", "hakios_thermal_init ");        /* the clock tables before what holds them back */
	before("hakios_buttons_init ", "hakios_wifi_init ");          /* the PMIC arbiter's lock before the others that use the bus */
	before("hakios_adsp_init ", "hakios_audio_init ");
	before("hakios_adsp_init ", "hakios_mpss_init ");             /* the modem after the audio DSP, as drivers.c has it */
	before("hakios_buttons_init ", "hakios_bt_uart_init ");
	before("hakios_board_init ", "hakios_rtc_init ");            /* the board says where the clock is */
	before("hakios_torch_init ", "hakios_bt_uart_init ");
	before("hakios_camera_init ", "hakios_bt_uart_init ");
	before("hakios_audio_init ", "hakios_virtio_sound_init ");    /* the device before its sinks */
	before("hakios_virtio_blk_init ", "hakios_apfs_check_init "); /* the disk before what mounts it */
	if (*touch) {
		before("hakios_input_init ", touch);
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
	all_once("hakios_fts_init ");
}

static void
board_with_ft5x06(void)
{
	board = "phone-i2c";
	boot_args = "touchscreen=-1";
	hakios_drivers_init();
	all_once("hakios_ft5x06_init ");
	CHECK(!logged("touch:"), "nothing is said of a board that has its driver: %s", log_text);
}

static void
board_with_goodix(void)
{
	board = "phone-video";
	boot_args = "";
	hakios_drivers_init();
	all_once("hakios_goodix_init ");
}

/* a board without a touch controller: no touch driver, and that is said when one was asked for */
static void
board_without_touch(void)
{
	board = "qemu-virt";
	boot_args = "touchscreen=1";
	hakios_drivers_init();
	all_once("");
	CHECK(logged("touch: touchscreen=1, but this board's board.dts lists no touch controller"), "said: %s", log_text);
}

/* ... and touchscreen=-1, the look, is said as it was asked for */
static void
board_without_touch_looked_at(void)
{
	board = "qemu-virt";
	boot_args = "touchscreen=-1";
	hakios_drivers_init();
	CHECK(logged("touch: touchscreen=-1, but this board's board.dts lists no touch controller"), "said: %s", log_text);
}

/* touchscreen=0 is not asking for it: nothing said either */
static void
board_without_touch_turned_off(void)
{
	board = "phone-keys";
	boot_args = "touchscreen=0";
	hakios_drivers_init();
	all_once("");
	CHECK(!logged("touch:"), "nothing is said: %s", log_text);
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
		{ "board_with_ft5x06", board_with_ft5x06 },
		{ "board_without_touch", board_without_touch }, { "board_without_touch_looked_at", board_without_touch_looked_at }, { "board_without_touch_not_asked", board_without_touch_not_asked },
		{ "board_without_touch_turned_off", board_without_touch_turned_off },
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
