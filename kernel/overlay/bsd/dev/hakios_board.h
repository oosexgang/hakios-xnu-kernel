/*
 * The board this kernel runs on, as data.
 *
 * One kernel serves every board. Which one it is, the loader says (/chosen/darwin-board); what that
 * board has and where, the repository's devices/NAME/board.toml and soc/SOC.toml say, and
 * tools/boards.py turns them into the table of hakios_board_table.h. A driver asks here instead of
 * carrying its own list of phones:
 *
 *	const struct hakios_board *board = hakios_board();
 *
 *	if (!hakios_board_has(HAKIOS_HAS_CPUFREQ_EPSS)) {
 *		return;                 // this board has no such thing (or is not known at all)
 *	}
 *	base = ml_io_map(board->cpufreq.epss, board->cpufreq.epss_domain_size);
 *
 * A board that is not in the table (a new port's first boots, a loader that names none) gets NULL and
 * no feature: nothing of a phone is touched on a machine that was not described. Adding a device is a
 * directory under devices/ (docs/porting.md), not a change to the drivers.
 *
 * Compiled by the host tests as well (tests/), so nothing of the kernel's is included here.
 */
#ifndef _HAKIOS_BOARD_H
#define _HAKIOS_BOARD_H

#include <stdbool.h>
#include <stdint.h>

/*
 * What a board can have a driver started for. The names in board.toml's `features` are these in
 * lower case with dashes ("cpufreq-epss"); tools/boards.py reads this list, so a new one is added
 * here and nowhere else.
 */
enum {
	HAKIOS_HAS_CPUFREQ_EPSS = 1u << 0,      /* CPU frequency through Qualcomm's EPSS (bsd/dev/redwood_cpufreq.c) */
	HAKIOS_HAS_DDR_RPMH     = 1u << 1,      /* memory bandwidth votes through RPMh (redwood_cpufreq.c) */
	HAKIOS_HAS_BATTERY_UEFI = 1u << 2,      /* the battery as the firmware reports it to the loader (redwood_battery.c) */
	HAKIOS_HAS_BUTTONS_PMIC = 1u << 3,      /* power and volume keys on the PMIC, through the SPMI arbiter (redwood_buttons.c) */
	HAKIOS_HAS_ADSP         = 1u << 4,      /* the audio DSP: sound, and the battery manager behind it (redwood_adsp.c) */
	/*
	 * Drivers of one part, wired the way one phone has it (the pins, the bus, the chip): a board lists
	 * one only if it has that part wired that way. Asked for on any other board, the driver says so and
	 * does nothing, rather than write to another phone's pins.
	 */
	HAKIOS_HAS_TOUCH_GOODIX    = 1u << 5,   /* Goodix GT9916 or FocalTech FT3680 on redwood's SPI (redwood_goodix.c) */
	HAKIOS_HAS_TOUCH_FT3658U   = 1u << 6,   /* FocalTech FT3658U on renoir's SPI (renoir_fts.c) */
	HAKIOS_HAS_SPEAKER_AW88261 = 1u << 7,   /* two Awinic AW88261 amplifiers on I2C (redwood_speaker.c, redwood_sound.c) */
	HAKIOS_HAS_HAPTIC_AW86927  = 1u << 8,   /* an Awinic AW86927 vibration motor driver (redwood_haptic.c) */
	HAKIOS_HAS_TORCH_PM8350C   = 1u << 9,   /* the flash LED module of a PM8350C (redwood_torch.c) */
	HAKIOS_HAS_CAMERA_REDWOOD  = 1u << 10,  /* redwood's four cameras and their wiring (redwood_camera.c) */
	HAKIOS_HAS_WIFI_WCN6750    = 1u << 11,  /* a WCN6750 radio on the SoC's own Wi-Fi DSP (redwood_wifi.c) */
	HAKIOS_HAS_MIC_WCD938X     = 1u << 12,  /* the microphones on a WCD938x codec, through the audio DSP (redwood_mic.c) */
};

/* how the panel's light is set (bsd/dev/hakios_backlight.c) */
enum {
	HAKIOS_BACKLIGHT_NONE = 0,
	HAKIOS_BACKLIGHT_DCS  = 1,              /* MIPI DCS 0x51 through the SoC's DSI controller */
	HAKIOS_BACKLIGHT_SOFT = 2,              /* the display driver darkens the picture */
};

#define HAKIOS_CPU_GROUPS       4               /* groups of cores with a clock of their own, at most */

struct hakios_board {
	const char *name;                       /* /chosen/darwin-board */
	const char *description;
	const char *soc;
	uint32_t features;                      /* HAKIOS_HAS_* */

	/* the clocks the display block's registers need (soc: [clocks]) */
	struct {
		uint32_t gcc, gcc_disp_ahb_cbcr;
		uint32_t dispcc, dispcc_mdss_gdscr, dispcc_mdss_ahb_cbcr;
	} clocks;

	/* the panel's light (board: [backlight]; soc: [display]) */
	struct {
		uint8_t type;                   /* HAKIOS_BACKLIGHT_* */
		uint8_t video_mode;             /* a video-mode panel: commands go between frames */
		uint8_t bytes;                  /* DCS 0x51 takes 1 or 2 bytes */
		uint8_t lsb_first;              /* (2 bytes) least significant first */
		uint32_t dsi;                   /* the DSI controller's registers */
		uint32_t raw_min, raw_max;      /* the panel's numbers for the lowest and highest brightness */
		uint32_t raw_restore;           /* what the panel is left at for the loader */
	} backlight;

	/* CPU frequency (soc: [cpufreq]) */
	struct {
		uint32_t epss, epss_domain_size;        /* EPSS: a block of registers a group of cores */
		uint8_t domain_cores[HAKIOS_CPU_GROUPS]; /* how many cores each group has, in the cores' order; 0: no more */
	} cpufreq;

	/* temperature (soc: [thermal]; board: [thermal]) */
	struct {
		uint32_t tsens[2];              /* the TSENS controllers' readings, 0: none */
		uint32_t floor_mhz[HAKIOS_CPU_GROUPS];  /* the slowest each group of cores is held back to */
	} thermal;
};

/* The board this kernel runs on; NULL when the loader named none, or one the table has not got. */
const struct hakios_board *hakios_board(void);

/* Has this board the feature (one HAKIOS_HAS_*)? Never, on a board that is not known. */
bool hakios_board_has(uint32_t feature);

/*
 * The groups of cores with a clock of their own (soc: [cpufreq] domain_cores): how many there are, and
 * which one a core is in, by its number among the cores (0 up; -1 when the board does not say).
 */
unsigned hakios_board_cpu_groups(void);
int hakios_board_cpu_group_of(unsigned core);

/* The table itself (hakios_board_table.h), for who wants to list it. */
extern const struct hakios_board hakios_boards[];
extern const unsigned hakios_board_count;
extern const char *const hakios_feature_names[];        /* by bit: "cpufreq-epss", ... */
extern const unsigned hakios_feature_count;

/* Says in the log which board this is and what it has; called once while the system starts. */
void hakios_board_init(void);

#endif /* _HAKIOS_BOARD_H */
