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
 * what a person touches and sees, then what talks to the other processors.
 */
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/hakios_apfs_check.h>
#include <sys/hakios_audio.h>
#include <sys/hakios_input.h>
#include <sys/hakios_sparse.h>
#include <sys/hakios_virtio_blk.h>

#include "hakios_board.h"
#include "hakios_log.h"

extern void hakios_scmi_perf_init(void);
extern void hakios_prof_start(void);            /* osfmk/arm64/hakios_prof.c */
extern void hakios_backlight_init(void);        /* bsd/dev/hakios_backlight.c */
extern void hakios_thermal_init(void);          /* bsd/dev/hakios_thermal.c */

/* The touch screen: the driver of the controller this board has, if it lists one. */
static void
touch_controller_init(void)
{
	uint32_t asked = 0;

	if (hakios_board_has(HAKIOS_HAS_TOUCH_FT3658U)) {
		hakios_fts_init();
	} else if (hakios_board_has(HAKIOS_HAS_TOUCH_GOODIX)) {
		hakios_goodix_init();
	} else if (PE_parse_boot_argn("touchscreen", &asked, sizeof(asked)) && asked != 0) {
		hakios_say("touch: touchscreen=1, but this board's board.dts lists no touch controller (\"touch-...\")\n");
	}
}

void
hakios_drivers_init(void)
{
	/* what everything else reads */
	hakios_input_init();           /* /dev/hakios-touch: the queue every source of touches and keys feeds */
	hakios_board_init();            /* which board this is and what it has, in the log (hakios_board.c) */

	/* the processor */
	hakios_scmi_perf_init();        /* firmware performance levels and limits */
	hakios_cpufreq_init();         /* boot-arg cpufreq=1: full speed; with it the memory's vote (ddr=) and cpuload=, membench= */

	/* what a person touches and sees */
	hakios_virtio_input_init();    /* boot-arg virtio_input=1: QEMU's pointer and keyboard */
	touch_controller_init();        /* boot-arg touchscreen=1: the touch screen, by the driver of the controller the board lists */
	hakios_prof_start();            /* boot-arg hakios_prof=NAME: a sampling profiler */
	hakios_buttons_init();         /* boot-arg buttons=1: the power and volume keys */
	hakios_battery_init();         /* boot-arg battery=1: the charger's state, from the loader */
	hakios_ufs_init();             /* the phone's storage, read only */
	hakios_net_init();             /* en0, if something carries its frames */
	hakios_adreno_init();          /* boot-arg adreno=N: the GPU */
	hakios_backlight_init();        /* boot-arg backlight=N: the panel's light, hw.backlight.* */
	hakios_thermal_init();          /* boot-arg thermal=N: held back when hot, hw.thermal.* */

	/* what talks to the other processors */
	hakios_wifi_init();            /* boot-arg wifi=1 */
	hakios_adsp_init();            /* boot-arg adsp=1: the audio DSP; sound, the microphones and the battery's details follow it */

	/* sound, and the parts that are looked at or switched by hand */
	hakios_audio_init();           /* /dev/hakios-audio: what iOS plays, to a sink */
	hakios_virtio_sound_init();    /* boot-arg virtio_sound=1: QEMU's sound device */
	hakios_speaker_init();         /* boot-arg speaker_probe=N: a look at the speaker amplifiers */
	hakios_haptic_init();          /* boot-arg haptic=N: the vibration motor */
	hakios_torch_init();           /* boot-arg torch=N: the flashlight */
	hakios_camera_init();          /* boot-arg cam=N: the cameras */

	/* storage that is not the phone's own */
	hakios_virtio_blk_init();      /* /dev/rvblk0: a disk image of the host (QEMU) */
	hakios_sparse_init();          /* /dev/rsp0: part of an image too big to hold */
#if APFS
	hakios_apfs_check_init();      /* mounts and walks /dev/rvblk0 once userland has /private/var up */
#endif
}
