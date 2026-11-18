/*
 * Where the kernel's drivers are started: one list, in the order they start.
 *
 * bsd_init() calls hakios_drivers_init() once, when /dev is there. Every driver's start function is
 * called from here and from nowhere else, whatever the board: a driver decides for itself whether it
 * has anything to do -- its boot argument asks for it, the board lists its feature
 * (hakios_board_has()), the SoC file says where its blocks are -- and returns at once if not. So the
 * list has no conditions but one: which touch controller's driver feeds /dev/hakios-touch, since a
 * board has one.
 *
 * A new driver is one more line here, after what it needs: the board first, then the processor, then
 * what a person touches and sees, then what talks to the other processors -- and a symbol of its own in
 * drivers/board/Kconfig, which the kernel's configuration chooses (Documentation/kconfig.md): a driver
 * not chosen is compiled all the same, and never started.
 */
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/board/apfs_check.h>
#include <sys/board/audio.h>
#include <sys/board/input.h>
#include <sys/board/sparse.h>
#include <sys/board/virtio_blk.h>

#include "board.h"
#include "kconfig.h"                    /* (the configuration: CONFIG_NAME 1 or 0, tools/gen_headers.py) */
#include "log.h"

extern void hakios_bt_uart_init(void);
extern void hakios_scmi_perf_init(void);
extern void hakios_prof_start(void);            /* osfmk/arm64/prof.c */
extern void hakios_backlight_init(void);        /* bsd/dev/board/backlight.c */
extern void hakios_thermal_init(void);          /* bsd/dev/board/thermal.c */
extern void hakios_imu_init(void);              /* bsd/dev/board/imu.c */
extern void hakios_rtc_init(void);              /* bsd/dev/board/rtc.c */

/* The touch screen: the driver of the controller this board has, if it lists one. */
static void
touch_controller_init(void)
{
	int32_t asked = 0;

	if (hakios_board_has(HAKIOS_HAS_TOUCH_FT3658U)) {
		if (CONFIG_TOUCH_FTS) {
			hakios_fts_init();
		} else {
			hakios_say("touch: this board's FocalTech FT3658U has no driver in this kernel (CONFIG_TOUCH_FTS)\n");
		}
	} else if (hakios_board_has(HAKIOS_HAS_TOUCH_GOODIX)) {
		if (CONFIG_TOUCH_GOODIX) {
			hakios_goodix_init();
		} else {
			hakios_say("touch: this board's Goodix touch screen has no driver in this kernel (CONFIG_TOUCH_GOODIX)\n");
		}
	} else if (PE_parse_boot_argn("touchscreen", &asked, sizeof(asked)) && asked != 0) {
		/* boot-arg touchscreen=1 asks for the touch screen; read here only to say that this board has none */
		hakios_say("touch: touchscreen=%d, but this board's board.dts lists no touch controller (\"touch-...\")\n", asked);
	}
}

void
hakios_drivers_init(void)
{
	/* (a kernel made without some drivers says so first: a part that does nothing for that is not a fault) */
	if (KCONFIG_OFF[0] != 0) {
		hakios_say("kconfig: made of %s, without %s\n", KCONFIG_MADE_OF, KCONFIG_OFF);
	}
	/* what everything else reads */
	hakios_input_init();           /* /dev/hakios-touch: the queue every source of touches and keys feeds */
	hakios_board_init();            /* which board this is and what it has, in the log (board/board.c) */

	/* the processor */
	if (CONFIG_QCOM_SCMI_PERF) {
		hakios_scmi_perf_init();  /* firmware performance levels and limits */
	}
	if (CONFIG_QCOM_CPUFREQ) {
		hakios_cpufreq_init();  /* boot-arg cpufreq=1: full speed; with it the memory's vote (ddr=) and cpuload=, membench= */
	}

	/* what a person touches and sees */
	if (CONFIG_VIRTIO_INPUT) {
		hakios_virtio_input_init();  /* boot-arg virtio_input=1: QEMU's pointer and keyboard */
	}
	touch_controller_init();        /* boot-arg touchscreen=1: the touch screen, by the driver of the controller the board lists */
	if (CONFIG_PROFILER) {
		hakios_prof_start();  /* boot-arg hakios_prof=NAME: a sampling profiler */
	}
	if (CONFIG_QCOM_BUTTONS) {
		hakios_buttons_init();  /* boot-arg buttons=1: the power and volume keys */
	}
	if (CONFIG_QCOM_RTC) {
		hakios_rtc_init();  /* the PMIC's clock, read once for process 1: hw.rtc_seconds (rtc=0: not read) */
	}
	if (CONFIG_BATTERY_FROM_LOADER) {
		hakios_battery_init();  /* boot-arg battery=1: the charger's state, from the loader */
	}
	if (CONFIG_QCOM_UFS) {
		hakios_ufs_init();  /* the phone's storage, read only */
	}
	hakios_net_init();             /* en0, if something carries its frames */
	if (CONFIG_ADRENO_A6XX) {
		hakios_adreno_init();  /* boot-arg adreno=N: the GPU */
	}
	if (CONFIG_BACKLIGHT) {
		hakios_backlight_init();  /* boot-arg backlight=N: the panel's light, hw.backlight.* */
	}
	if (CONFIG_QCOM_THERMAL) {
		hakios_thermal_init();  /* boot-arg thermal=N: held back when hot, hw.thermal.* */
	}

	/* what talks to the other processors */
	if (CONFIG_WIFI_WCN6750) {
		hakios_wifi_init();  /* boot-arg wifi=1 */
	}
	if (CONFIG_AUDIO_DSP) {
		hakios_adsp_init();  /* boot-arg adsp=1: the audio DSP; sound, the microphones and the battery's details follow it */
	}
	if (CONFIG_MODEM_SM7325) {
		hakios_mpss_init();  /* boot-arg modem=1: the modem started and linked (board feature "modem-sm7325") */
	}

	/* sound, and the parts that are looked at or switched by hand */
	hakios_audio_init();           /* /dev/hakios-audio: what Darwin plays, to a sink */
	if (CONFIG_VIRTIO_SOUND) {
		hakios_virtio_sound_init();  /* boot-arg virtio_sound=1: QEMU's sound device */
	}
	if (CONFIG_SPEAKER_AMPS) {
		hakios_speaker_init();  /* boot-arg speaker_probe=N: a look at the speaker amplifiers */
	}
	if (CONFIG_HAPTIC) {
		hakios_haptic_init();  /* boot-arg haptic=N: the vibration motor */
	}
	if (CONFIG_IMU_LSM6DSO) {
		hakios_imu_init();  /* boot-arg sensors=N: the motion sensor, the phone's orientation */
	}
	if (CONFIG_TORCH) {
		hakios_torch_init();  /* boot-arg torch=N: the flashlight */
	}
	if (CONFIG_CAMERA_SM7325) {
		hakios_camera_init();  /* boot-arg cam=N: the cameras */
	}
	if (CONFIG_BT_WCN_UART) {
		hakios_bt_uart_init();  /* boot-arg bt=1: Bluetooth UART identification, after RPMh/SPMI users */
	}

	/* storage that is not the phone's own */
	if (CONFIG_VIRTIO_BLK) {
		hakios_virtio_blk_init();  /* /dev/rvblk0: a disk image of the host (QEMU) */
	}
	if (CONFIG_SPARSE_IMAGE) {
		hakios_sparse_init();  /* /dev/rsp0: part of an image too big to hold */
	}
#if APFS
	if (CONFIG_APFS_CHECK) {
		hakios_apfs_check_init();  /* mounts and walks /dev/rvblk0 once userland has /private/var up */
	}
#endif
}
