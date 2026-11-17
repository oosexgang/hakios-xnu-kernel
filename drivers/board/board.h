/*
 * The board this kernel runs on, as data.
 *
 * One kernel serves every board. Which one it is, the loader says (/chosen/darwin-board); what that
 * board has and where, the repository's arch/arm64/boot/dts/NAME/board.dts and soc.dtsi say, and
 * tools/boards.py turns them into the table of board_table.h. A driver asks here instead of
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
 * directory under arch/arm64/boot/dts/ (Documentation/porting.md), not a change to the drivers.
 *
 * Compiled by the host tests as well (workbench/), so nothing of the kernel's is included here.
 */
#ifndef _HAKIOS_BOARD_H
#define _HAKIOS_BOARD_H

#include <stdbool.h>
#include <stdint.h>

/*
 * What a board can have a driver started for. The names in board.dts's `features` are these in
 * lower case with dashes ("cpufreq-epss"); tools/boards.py reads this list, so a new one is added
 * here and nowhere else.
 */
enum {
	HAKIOS_HAS_CPUFREQ_EPSS = 1u << 0,      /* CPU frequency through Qualcomm's EPSS (bsd/dev/board/cpufreq.c) */
	HAKIOS_HAS_DDR_RPMH     = 1u << 1,      /* memory bandwidth votes through RPMh (board/ddr.c) */
	HAKIOS_HAS_BATTERY_UEFI = 1u << 2,      /* the battery as the firmware reports it to the loader (board/battery.c) */
	HAKIOS_HAS_BUTTONS_PMIC = 1u << 3,      /* power and volume keys on the PMIC, through the SPMI arbiter (board/buttons.c) */
	HAKIOS_HAS_ADSP         = 1u << 4,      /* the audio DSP: sound, and the battery manager behind it (board/adsp.c) */
	HAKIOS_HAS_DISPLAY_SCAN = 1u << 13,     /* a command-mode panel whose readout is followed on the DSI interface's
	                                         * counters, for frames without tearing (board/adreno.c, fb_scan) */
	HAKIOS_HAS_VIRTIO       = 1u << 15,     /* an emulator's virtio devices on its MMIO transport: network, tablet
	                                         * and keyboard (board/virtio_net.c, board/virtio_input.c) */
	HAKIOS_HAS_GPU_A660     = 1u << 14,     /* an Adreno of the A660 family behind a GMU; what differs from one to
	                                         * the next is the board's gpu { } (board/adreno.c, gpu=1) */
	/*
	 * Drivers of one part, wired the way one phone has it (the pins, the bus, the chip): a board lists
	 * one only if it has that part wired that way. Asked for on any other board, the driver says so and
	 * does nothing, rather than write to another phone's pins.
	 */
	HAKIOS_HAS_TOUCH_GOODIX    = 1u << 5,   /* Goodix GT9916 or FocalTech FT3680 on SPI, wired as touch { } says (board/goodix.c) */
	HAKIOS_HAS_TOUCH_FT3658U   = 1u << 6,   /* FocalTech FT3658U on SPI, wired as touch { } says (board/fts.c) */
	HAKIOS_HAS_SPEAKER_AW88261 = 1u << 7,   /* two Awinic AW88261 amplifiers on I2C (board/speaker.c, board/sound.c) */
	HAKIOS_HAS_HAPTIC_AW86927  = 1u << 8,   /* an Awinic AW86927 vibration motor driver (board/haptic.c) */
	HAKIOS_HAS_TORCH_PM8350C   = 1u << 9,   /* the flash LED module of a PM8350C (board/torch.c) */
	HAKIOS_HAS_CAMERA_SM7325   = 1u << 10,  /* cameras on an SM7325's camera blocks, powered by a PM8008, wired as
	                                         * the board's camera { } says (board/camera.c) */
	HAKIOS_HAS_WIFI_WCN6750    = 1u << 11,  /* a WCN6750 radio on the SoC's own Wi-Fi DSP (board/wifi.c) */
	HAKIOS_HAS_MIC_WCD938X     = 1u << 12,  /* the microphones on a WCD938x codec, through the audio DSP (board/mic.c) */
	HAKIOS_HAS_BLUETOOTH_WCN_UART = 1u << 17, /* Qualcomm Bluetooth over a GENI UART (board/bt_uart.c) */
	HAKIOS_HAS_MODEM_SM7325    = 1u << 18,  /* an SM7325's modem, its packet engine and the SIM as this phone has them
	                                         * (board/mpss.c, board/ipa.c, board/nas.c, board/cell.c) */
	HAKIOS_HAS_CPUFREQ_SCMI = 1u << 16,     /* CPU performance queries via SCMI */
};

/* how the panel's light is set (bsd/dev/board/backlight.c) */
enum {
	HAKIOS_BACKLIGHT_NONE = 0,
	HAKIOS_BACKLIGHT_DCS  = 1,              /* MIPI DCS 0x51 through the SoC's DSI controller */
	HAKIOS_BACKLIGHT_SOFT = 2,              /* the display driver darkens the picture */
};

/* how a camera sensor's supply is switched (board: sensor@N { } vana, vdig, vio) */
enum {
	HAKIOS_SUPPLY_NONE = 0,                 /* not switched: always on, or not there */
	HAKIOS_SUPPLY_LDO  = 1,                 /* "ldoN MVmV": a regulator of the sensors' PMIC, at a set point */
	HAKIOS_SUPPLY_GPIO = 2,                 /* "gpioN MVmV USus": a fixed regulator that a pin of the SoC switches */
};

#define HAKIOS_CPU_GROUPS       4               /* groups of cores with a clock of their own, at most */
#define HAKIOS_TSENS_MAX        8               /* TSENS controllers of a SoC, at most (SM8845 has seven) */
#define HAKIOS_CAMERA_CLOCKS    8               /* MCLKs, and CSI receivers, of a SoC's camera, at most */

struct hakios_board {
	const char *name;                       /* /chosen/darwin-board */
	const char *description;
	const char *soc;
	uint32_t features;                      /* HAKIOS_HAS_* */

	/* the clocks the display block's registers need (soc: clocks { }) */
	struct {
		uint32_t gcc, gcc_disp_ahb_cbcr;
		uint32_t dispcc, dispcc_mdss_gdscr, dispcc_mdss_ahb_cbcr;
		uint32_t dispcc_mdss_pclk0_cbcr, dispcc_mdss_mdp_cbcr;  /* only looked at (display_probe=1); 0: not known */
	} clocks;

	/* the display controller (soc: display { }) */
	struct {
		uint32_t intf;                  /* the interface the panel hangs on (INTF_*): its timing and counters */
	} display;

	/* the panel's light (board: backlight { }; soc: display { }) */
	struct {
		uint8_t type;                   /* HAKIOS_BACKLIGHT_* */
		uint8_t video_mode;             /* a video-mode panel: commands go between frames */
		uint8_t bytes;                  /* DCS 0x51 takes 1 or 2 bytes */
		uint8_t lsb_first;              /* (2 bytes) least significant first */
		uint32_t dsi;                   /* the DSI controller's registers */
		uint32_t raw_min, raw_max;      /* the panel's numbers for the lowest and highest brightness */
		uint32_t raw_restore;           /* what the panel is left at for the loader */
	} backlight;

	/* the Adreno's blocks (soc: adreno { }); what differs from one phone to the next is gpu { } below */
	struct {
		uint32_t gpu, gmu, gpucc, cx_misc, rscc, smmu, pdc;
		uint32_t qfprom;                /* the fuses, where the speed and gaming bins are */
		uint32_t tsens;                 /* the TSENS controller with the GPU's two sensors; 0: not read */
	} adreno;

	/* the GPU (board: gpu { }) */
	struct {
		const char *model;              /* "Adreno 642" */
		const char *firmware;           /* the small image's directory with a660_gmu.bin, a660_sqe.fw, a660_zap.* */
		const char *alt_model;          /* what the parts with the speed bins alt_bins are sold as; NULL: none */
		uint32_t alt_bins[2];
		uint32_t chip_id, gmem_bytes;
		uint32_t primitive_fifo, hang_cycles, uche_cmdq_config;         /* register values; 0: left alone */
		uint32_t speed_fuse, gaming_fuse;       /* where in the fuses the speed and gaming bins are */
		uint32_t highest_bank_bit;
		uint32_t gcc_debug_gpucc;       /* the GPU clock controller's place in GCC's debug multiplexer */
		uint8_t zap_must_authenticate;  /* stop when TrustZone does not confirm the zap shader */
		uint8_t speed_count, default_speed;     /* how many of speeds_khz; the one to run at, counted from 1 */
		uint32_t speeds_khz[5], speed_levels[5];        /* the core clock's steps and their supply levels */
	} gpu;

	/* the PMICs' bus (soc: spmi { }): the blocks of the SPMI arbiter this system reads and writes through */
	struct {
		uint32_t core, cnfg, observer;
		uint32_t channel_map;           /* version 8: the table of channels; 0: version 5 (in core) */
		uint32_t channels;              /* writes; 0: not given, and nothing here writes to a PMIC */
		uint32_t ee;                    /* who this system is to the arbiter (the device tree's qcom,ee) */
	} spmi;

	/* the SoC's own pins (soc: pins { }) */
	struct {
		uint32_t tlmm;                  /* where GPIO 0's registers are; GPIO n's are 0x1000 n further */
	} pins;

	/* the SMMU that stands between the SoC's devices and memory (soc: smmu { }); the GPU has its own (adreno { }) */
	struct {
		uint32_t apps;
		/* the streams parts' DMA comes in on: ID and mask (the device tree's iommus); 0, 0: not said */
		uint16_t audio_stream[2], wifi_stream[2], camera_stream[2];
	} smmu;

	/* the camera subsystem's blocks (soc: camera { }) */
	struct {
		uint32_t camcc;                 /* its clock controller */
		uint32_t ife, csid;             /* the image front end the sensors' data goes through, and its CSI decoder */
		uint32_t csiphy;                /* the first CSI receiver; the others follow, 0x2000 apart */
		/* where the clock controller has what the driver switches: offsets in camcc, as Linux's
		 * camcc-CODENAME.c has them (a GDSC's register, a PLL's first, an RCG's command register, a branch's) */
		uint32_t top_gdsc, pll0, pll2;
		/* how the PLLs are configured (camcc-CODENAME.c's cam_cc_pllN_config): PLL0's L, CAL_L, alpha, config
		 * (three words), user (three words); PLL2's L, config (three words), user. And which source of an
		 * RCG each output is: PLL0's even and odd ones, PLL2's early one (the parent maps' positions) */
		uint32_t pll0_config[9], pll2_config[5];
		uint8_t pll0_even_source, pll0_odd_source, pll2_source;
		uint8_t mclk_count, csiphy_count;
		uint32_t mclk_rcg[HAKIOS_CAMERA_CLOCKS], mclk_branch[HAKIOS_CAMERA_CLOCKS];
		uint32_t csiphy_branch[HAKIOS_CAMERA_CLOCKS], csiphy_timer_rcg[HAKIOS_CAMERA_CLOCKS],
		    csiphy_timer_branch[HAKIOS_CAMERA_CLOCKS];
		uint32_t slow_ahb_rcg, camnoc_axi_rcg, cphy_rx_rcg, ife_csid_rcg, ife_rcg;
		uint32_t cpas_ahb_branch, camnoc_axi_branch, ife_csid_branch, ife_cphy_rx_branch, ife_branch;
		/* ... and in the global clock controller (clocks { } gcc): the camera's bus and crystal clocks */
		uint32_t gcc_ahb_branch, gcc_xo_branch, gcc_hf_axi_branch, gcc_sf_axi_branch;
	} camera;

	/* the low-power audio subsystem's blocks the microphones' path goes through (soc: lpass { }) */
	struct {
		uint32_t tx_macro, va_macro;    /* the codec macros for capture */
		uint32_t tx_swr, tx_swr_hctl;   /* the SoundWire master of the capture side, and its hardware control register */
		uint32_t lpi, lpi_slew;         /* the subsystem's own pins, and their slew rates */
		uint8_t swr_tx_pin_count;
		uint8_t swr_tx_pins[4];         /* the TX SoundWire's pins among them: its clock, then its data lines */
		uint8_t swr_tx_slew[4];         /* ... each one's field (two bits) in the slew register */
	} lpass;

	/* the way to the SoC's other processors (soc: ipc { }) */
	struct {
		uint32_t aoss;                  /* the always-on processor's mailbox (QMP) */
		uint32_t ipcc;                  /* the block the processors ring each other through */
		uint32_t smem, smem_size;       /* the memory they share, as the boot loader laid it out */
		uint32_t smem_lock;             /* the hardware lock that guards its allocations */
	} ipc;

	/* RPMh (soc: rpmh { }): the command database the firmware left, and this system's sequencer */
	struct {
		uint32_t cmd_db, cmd_db_size;
		uint32_t rsc, tcs_offset;       /* the application processor's "drv-2"; where its slot sets begin */
	} rpmh;

	/* the keys on the PMICs (board: buttons { }); a peripheral is slave << 8 | the high byte of its registers */
	struct {
		uint16_t pon;                   /* the power-on block: power, and volume down on its RESIN_N */
		uint16_t volume_up;             /* the GPIO volume up pulls low */
		const char *pon_name, *volume_up_name;  /* for the log: "GPIO6 of the PM8350" */
	} buttons;

	/* the vibration motor's chip (board: haptic { }): which of the SoC's pins it is on, and this motor's calibration */
	struct {
		uint8_t sda, scl;               /* its I2C bus's two lines */
		uint8_t reset;                  /* high: the chip runs */
		uint8_t interrupt;              /* low when the chip has something to say (only looked at) */
		uint8_t f0_calibration;         /* what Android's driver found for this motor's resonance (its trim register) */
	} haptic;

	/*
	 * The touch controller's wiring (board: touch { }): the serial engine it is on, that engine's clocks in
	 * the clock controller, its pins and its supplies. One driver for a controller, any wiring.
	 */
	struct {
		uint32_t serial_engine;         /* the SPI engine's registers */
		uint32_t m_ahb_clock, s_ahb_clock, se_clock;    /* offsets in the clock controller: the wrapper's two bus clocks, the engine's own
		                                 * (its rate generator's registers follow it) */
		uint32_t vote_register;         /* the clock controller's register the application processor votes in, as an offset */
		uint8_t vote[5];                /* its bits for: core 2x, core, the two bus clocks, the engine */
		uint8_t data_in, data_out, clock, chip_select;  /* the bus's four pins of the SoC */
		uint8_t interrupt, reset;       /* low: the chip has a report; low: in reset */
		int16_t analog_enable;          /* a pin that switches the chip's analog supply on, -1: none */
		int16_t vendor_pin[2];          /* two straps that say which maker's controller is fitted, -1: none */
		const char *io_supply, *analog_supply;  /* regulators by their names in RPMh's command database; NULL: none */
		uint32_t max_x, max_y;          /* the largest coordinates the controller reports; 0: not said */
	} touch;

	/* Bluetooth H4 wiring (board: bluetooth { }); GCC offsets are explicit, including the RCG. */
	struct {
		uint32_t serial_engine, wrapper;
		uint32_t clocks[3], clock_source, vote_register;
		uint8_t votes[5], uart_pins[4], uart_function, sw_ctrl;
		uint16_t enable_ppid;
		uint8_t supply_count;
		const char *supplies[8], *rf_clock;
		uint16_t supply_mv[8];
		/* the controller's firmware in the small image: its directory (NULL: "fw"), its patch and its configuration
		 * (NULL: none, the controller runs its ROM) */
		const char *firmware, *rampatch, *nvm;
	} bluetooth;

	/* the speaker's two amplifiers (board: speaker { }): the I2C bus they share and what tells them apart */
	struct {
		uint8_t sda, scl;               /* the bus's two lines, pins of the SoC */
		uint8_t address[2];             /* each one's I2C address; the first plays the sound's slot 0 */
		uint8_t interrupt[2];           /* each one's interrupt pin (only looked at) */
		uint8_t sound_pin[4];           /* the sound's own pins: bit clock, frame sync, data in, data out */
		uint8_t sound_function;         /* which of those pins' functions is the sound's (the SoC's first MI2S: 1) */
		uint8_t tdm;                    /* which of the SoC's TDM interfaces they are on: 0 the primary ... 4 the quinary */
	} speaker;

	/*
	 * The one partition of the phone's storage a write may reach, the port's own (board: storage { }):
	 * where it begins on the first logical unit, how many blocks of 4096 it has, and the first of them
	 * a write may reach. blocks 0: the board names none, and nothing is ever written. (The loader has
	 * the same from the same file, and the partition's GUIDs besides.)
	 */
	struct {
		uint64_t first_lba, blocks, write_first;
	} storage;

	/* the flashlight's part on the PMIC (board: torch { }); a peripheral as in buttons { } */
	struct {
		uint16_t flash;                 /* the flash LED module */
		uint16_t temp_alarm;            /* that PMIC's temperature alarm, which the driver watches while the light is on */
	} torch;

	/* the audio DSP (board: adsp { }) */
	struct {
		const char *firmware;           /* the root's directory with the DSP's image (adsp.mdt and its parts) */
		uint32_t memory;                /* where its reserved memory is */
		uint32_t memory_size;           /* how much of it the image may take */
		uint32_t charge_counter_divisor;        /* the battery manager's charge counter, in this many parts of a uAh */
	} adsp;

	/* the microphones' codec (board: mic { }) */
	struct {
		uint8_t wired;                  /* the board gives the section */
		uint8_t codec_reset;            /* the SoC's pin the codec's reset is on */
	} mic;

	/* the Wi-Fi radio (board: wifi { }), for the driver's layer under it (board/lx.c) */
	struct {
		uint32_t radio_model;           /* which radio: 0 a WCN6750 on the SoC's own bus, 1 a WCN6855 on PCIe */
		uint32_t early_pool_mb;         /* memory in one piece kept for the radio's driver from early on; 0: none */
		uint32_t dsp_memory, dsp_memory_size;   /* the Wi-Fi DSP's reserved memory, for a radio on the SoC's own DSP */
		int16_t pin[3];                 /* the radio's enable, Bluetooth's reset and its sw-ctrl; -1: not said */
		char supply[3][8];              /* RPMh's names of the radio's vdd-cx-mx, 1.8 V and 1.3 V; "": not said */
		const char *board_data, *regdb; /* the radio's board data and regulatory database, in fw-wifi/ */
	} wifi;

	/* the cameras as the phone has them wired (board: camera { } and its sensor@N { }) */
	struct {
		uint8_t pmic_sda, pmic_scl;     /* the I2C bus of the PMIC whose regulators feed the sensors (a PM8008) */
		uint16_t pmic_reset;            /* the PMIC GPIO its reset is on: SPMI slave << 8 | the peripheral's high byte */
		uint8_t sensor_count;           /* 0: the board says nothing of its cameras */
		uint8_t test_sensor;            /* which the driver works on by itself (cam=1..4) */
		uint8_t darwin_sensor, darwin_front;  /* Darwin's back camera (cam=5) and its front one (0xff: none) */
		struct hakios_camera_sensor {
			const char *name;               /* "macro", "wide", ...: what cam_sensor= picks it by */
			const char *chip;               /* one of the camera driver's chips */
			uint8_t cci, cci_master;        /* the CCI the sensor is on and which of its two masters ... */
			uint8_t sda, scl;               /* ... and that master's pins */
			uint8_t reset;                  /* the sensor's reset pin */
			uint8_t mclk, mclk_pin;         /* which of camcc's MCLKs it runs on, and that clock's pin */
			uint8_t csiphy;                 /* the CSI receiver its lanes go to */
			uint8_t focus_motor;            /* 1: a DW9800 moves its lens */
			uint8_t upside_down;            /* 1: its pictures are turned half round from the way Darwin has that camera's */
			char focus_supply[8];           /* the motor's supply, by its RPMh name, when it has to be switched on; "": none */
			uint16_t focus_mv;              /* ... and its voltage */
			struct hakios_camera_supply {
				uint8_t kind;           /* HAKIOS_SUPPLY_NONE, _LDO (one of the PMIC's), _GPIO (a pin switches it) */
				uint8_t ldo;            /* _LDO: which */
				uint16_t mv;            /* the set point (for _GPIO: what the switched regulator gives) */
				uint16_t gpio, ramp_us; /* _GPIO: the pin, active high, and how long the regulator takes */
			} vana, vdig, vio;
		} sensors[4];
	} cameras;

	/* CPU frequency (soc: cpufreq { }) */
	struct {
		uint32_t epss, epss_domain_size;        /* EPSS: a block of registers a group of cores */
		uint8_t domain_cores[HAKIOS_CPU_GROUPS]; /* how many cores each group has, in the cores' order; 0: no more */
	} cpufreq;

	/* an emulator's virtio devices on its MMIO transports (soc: virtio { }) */
	struct {
		uint32_t mmio;                  /* the first transport; the others follow, 0x200 bytes apart */
		uint32_t transports;
	} virtio;

	/* CPU performance mailbox (soc: scmi-perf { }). */
	struct {
		uint32_t shmem, mailbox, doorbell;
	} scmi_perf;

	/* temperature (soc: thermal { }; board: thermal { }) */
	struct {
		uint32_t tsens[HAKIOS_TSENS_MAX];       /* the TSENS controllers' readings, 0: none */
		uint32_t floor_mhz[HAKIOS_CPU_GROUPS];  /* the slowest each group of cores is held back to */
	} thermal;
};

/* The board this kernel runs on; NULL when the loader named none, or one the table has not got. */
const struct hakios_board *hakios_board(void);

/* Has this board the feature (one HAKIOS_HAS_*)? Never, on a board that is not known. */
bool hakios_board_has(uint32_t feature);

/*
 * The groups of cores with a clock of their own (soc: cpufreq { } domain_cores): how many there are, and
 * which one a core is in, by its number among the cores (0 up; -1 when the board does not say).
 */
unsigned hakios_board_cpu_groups(void);
int hakios_board_cpu_group_of(unsigned core);

/* The table itself (board_table.h), for who wants to list it. */
extern const struct hakios_board hakios_boards[];
extern const unsigned hakios_board_count;
extern const char *const hakios_feature_names[];        /* by bit: "cpufreq-epss", ... */
extern const unsigned hakios_feature_count;

/* Says in the log which board this is and what it has; called once while the system starts. */
void hakios_board_init(void);

#endif /* _HAKIOS_BOARD_H */
