/*
 * The kernel's configuration as drivers/board/drivers.c follows it: a driver whose symbol is off is not started
 * (it is compiled all the same), a board whose touch controller's driver is off says so, and a kernel made without
 * some drivers says which first. The configuration is set here before kconfig.h is read, as tools/gen_headers.py
 * would have made it of a fragment that turns those drivers off.
 *
 *   drivers_kconfig_test SCENARIO
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

/* a kernel of a fragment that leaves the GPU, two of the touch screens' drivers, the modem and QEMU's disk out */
#define CONFIG_ADRENO_A6XX      0
#define CONFIG_TOUCH_GOODIX     0
#define CONFIG_TOUCH_FT5X06     0
#define CONFIG_MODEM_SM7325     0
#define CONFIG_VIRTIO_BLK       0
#define CONFIG_APFS_CHECK       0
#define KCONFIG_OFF             "TOUCH_GOODIX TOUCH_FT5X06 ADRENO_A6XX MODEM_SM7325 VIRTIO_BLK APFS_CHECK"
#define KCONFIG_MADE_OF         "test.config"

#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define APFS 1
#include "board/board.c"
#include "board/drivers.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* what is off is not started; the rest is, as without a configuration */
static void
off_is_not_started(void)
{
	board = "phone-video";
	boot_args = "";
	hakios_drivers_init();
	CHECK(strstr(started, "hakios_adreno_init") == NULL && strstr(started, "hakios_mpss_init") == NULL &&
	    strstr(started, "hakios_virtio_blk_init") == NULL && strstr(started, "hakios_apfs_check_init") == NULL,
	    "the GPU, the modem and the disk image not started: %s", started);
	CHECK(strstr(started, "hakios_wifi_init") != NULL && strstr(started, "hakios_ufs_init") != NULL &&
	    strstr(started, "hakios_cpufreq_init") != NULL && strstr(started, "hakios_bt_uart_init") != NULL,
	    "the others are: %s", started);
	CHECK(logged("kconfig: made of test.config, without TOUCH_GOODIX TOUCH_FT5X06 ADRENO_A6XX MODEM_SM7325 VIRTIO_BLK APFS_CHECK"),
	    "said first: %s", log_text);
}

/* a board whose touch controller's driver is not in the kernel: not started, and said */
static void
touch_driver_left_out(void)
{
	board = "phone-video";                  /* (its touch controller is a Goodix) */
	boot_args = "touchscreen=1";
	hakios_drivers_init();
	CHECK(strstr(started, "hakios_goodix_init") == NULL, "not started: %s", started);
	CHECK(logged("touch: this board's Goodix touch screen has no driver in this kernel (CONFIG_TOUCH_GOODIX)"), "said: %s", log_text);
}

/* ... nor the one of a controller on I2C; and the driver that is in the kernel is started on its board */
static void
touch_i2c_driver_left_out(void)
{
	board = "phone-i2c";
	boot_args = "touchscreen=1";
	hakios_drivers_init();
	CHECK(strstr(started, "hakios_ft5x06_init") == NULL, "not started: %s", started);
	CHECK(logged("touch: this board's FocalTech FT5x06-family touch screen has no driver in this kernel (CONFIG_TOUCH_FT5X06)"),
	    "said: %s", log_text);
}

static void
touch_driver_kept(void)
{
	board = "phone-cmd";                    /* (its touch controller is an FT3658U) */
	boot_args = "touchscreen=1";
	hakios_drivers_init();
	CHECK(strstr(started, "hakios_fts_init") != NULL && !logged("touch:"), "started, nothing said: %s / %s", started, log_text);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "off_is_not_started", off_is_not_started }, { "touch_driver_left_out", touch_driver_left_out },
		{ "touch_i2c_driver_left_out", touch_i2c_driver_left_out }, { "touch_driver_kept", touch_driver_kept },
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
			fprintf(stderr, "%-26s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
