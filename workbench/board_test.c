/*
 * Which board the kernel takes itself for (drivers/board/board.c), against the table
 * generated from the device packages: the row is picked by the loader's name, a board that is not known
 * has no feature at all, and hw.board.* says what was found. One scenario a process (the lookup is
 * done once), as the driver tests.
 */
#include "fakekernel.h"

extern char log_text[];

#include "board/board.c"
#include "board/adreno_profile.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static void
known_phone(void)
{
	const struct hakios_board *b;

	board = "phone-cmd";
	b = hakios_board();
	CHECK(b != NULL && strcmp(b->name, "phone-cmd") == 0 && strcmp(b->soc, "sm7350") == 0, "the row of the phone");
	CHECK(hakios_board_has(HAKIOS_HAS_CPUFREQ_EPSS) && hakios_board_has(HAKIOS_HAS_ADSP), "its features");
	/* its own touch controller, and none of the parts only the other phone has */
	CHECK(hakios_board_has(HAKIOS_HAS_TOUCH_FT3658U) && !hakios_board_has(HAKIOS_HAS_TOUCH_GOODIX), "its touch controller");
	/* a command-mode panel, followed on the interface it hangs on */
	CHECK(hakios_board_has(HAKIOS_HAS_DISPLAY_SCAN) && b != NULL && b->display.intf == 0x0ae6b800 && !b->backlight.video_mode,
	    "its panel's readout: %#x", b ? b->display.intf : 0);
	CHECK(!hakios_board_has(HAKIOS_HAS_HAPTIC_AW86927) && !hakios_board_has(HAKIOS_HAS_TORCH_PM8350C) &&
	    !hakios_board_has(HAKIOS_HAS_SPEAKER_AW88261) && !hakios_board_has(HAKIOS_HAS_CAMERA_SM7325) &&
	    !hakios_board_has(HAKIOS_HAS_WIFI_WCN6750) && !hakios_board_has(HAKIOS_HAS_MIC_WCD938X) &&
	    !hakios_board_has(HAKIOS_HAS_IMU_LSM6DSO) && !hakios_board_has(HAKIOS_HAS_IMU_BMI160),
	    "no driver wired for another phone's parts");
	CHECK(b != NULL && b->backlight.type == HAKIOS_BACKLIGHT_DCS && b->backlight.dsi != 0 && b->clocks.dispcc != 0 &&
	    b->backlight.raw_min < b->backlight.raw_max, "a DCS panel has what the driver needs");
	CHECK(strcmp(get_string("name"), "phone-cmd") == 0, "hw.board.name %s", get_string("name"));
	CHECK(strcmp(get_string("soc"), "sm7350") == 0, "hw.board.soc %s", get_string("soc"));
	CHECK(strstr(get_string("features"), "cpufreq-epss") != NULL, "hw.board.features %s", get_string("features"));
	/* its keys: the PMK8350's power-on block and GPIO6 of the PM8350, through the arbiter of its SoC */
	CHECK(hakios_board_has(HAKIOS_HAS_BUTTONS_PMIC) && b != NULL && b->spmi.core == 0x0c440000 && b->spmi.cnfg == 0x0c40a000 &&
	    b->spmi.observer == 0x0e600000 && b->spmi.ee == 0, "the PMIC arbiter");
	CHECK(b != NULL && b->buttons.pon == 0x013 && b->buttons.volume_up == 0x18d && b->buttons.volume_up_name != NULL &&
	    strcmp(b->buttons.volume_up_name, "GPIO6 of the PM8350") == 0, "its buttons { }");
	CHECK(b != NULL && b->rtc.peripheral == 0x061 && b->rtc.name != NULL && strcmp(b->rtc.name, "the PMK8350's RTC") == 0,
	    "its PMIC's clock (rtc { })");
	CHECK(b != NULL && b->pins.tlmm == 0x0f100000, "the SoC's pins");
	CHECK(hakios_board_has(HAKIOS_HAS_ADSP) && b != NULL && b->ipc.aoss == 0x0c300000 && b->ipc.ipcc == 0x00408000 &&
	    b->ipc.smem == 0x80900000 && b->ipc.smem_size == 0x200000 && b->ipc.smem_lock == 0x01f43000,
	    "the way to the other processors, for its audio DSP");
	CHECK(hakios_board_has(HAKIOS_HAS_DDR_RPMH) && b != NULL && b->rpmh.cmd_db == 0x80860000 && b->rpmh.cmd_db_size == 0x20000 &&
	    b->rpmh.rsc == 0x18220000 && b->rpmh.tcs_offset == 0xd00, "RPMh for the memory vote");
	/* its audio DSP: its own firmware, 48 MiB, a gauge that counts in hundredths */
	CHECK(b != NULL && b->adsp.firmware != NULL && strcmp(b->adsp.firmware, "fw-audio-cmd") == 0 &&
	    b->adsp.memory == 0x86700000 && b->adsp.memory_size == 0x03000000 && b->adsp.charge_counter_divisor == 100, "its adsp { }");
	/* its GPU's blocks, as its SoC's file has them */
	CHECK(b != NULL && b->adreno.gpu == 0x03d00000 && b->adreno.gmu == 0x03d6a000 && b->adreno.gpucc == 0x03d90000 &&
	    b->adreno.cx_misc == 0x03d9e000 && b->adreno.rscc == 0x03de0000 && b->adreno.smmu == 0x03da0000 &&
	    b->adreno.pdc == 0x0b290000 && b->adreno.qfprom == 0x00780000 && b->adreno.tsens == 0x0c265000, "adreno { }");
	/* its radio: the one on PCIe, with its pool kept early */
	CHECK(b != NULL && b->wifi.radio_model == 1 && b->wifi.early_pool_mb == 80, "its wifi { }");
	/* its groups of cores: four small, three big, one prime */
	CHECK(hakios_board_cpu_groups() == 3, "%u groups", hakios_board_cpu_groups());
	CHECK(hakios_board_cpu_group_of(0) == 0 && hakios_board_cpu_group_of(3) == 0 && hakios_board_cpu_group_of(4) == 1 &&
	    hakios_board_cpu_group_of(6) == 1 && hakios_board_cpu_group_of(7) == 2, "which core is in which group");
	CHECK(hakios_board_cpu_group_of(8) == -1, "a core beyond the groups is in none");
	hakios_board_init();
	CHECK(logged("board: phone-cmd (Test phone, command-mode panel, sm7350): cpufreq-epss"), "said in the log: %s", log_text);
}

/* a board is said with every feature it has, however many: the line is not cut */
static void
every_feature_said(void)
{
	char all[1024] = "";
	const struct hakios_board *b;

	board = "phone-video";
	b = hakios_board();
	CHECK(b != NULL, "the board is known");
	CHECK(b && hakios_board_has(HAKIOS_HAS_BLUETOOTH_WCN_UART) && b->bluetooth.serial_engine == 0x898000 &&
	    b->bluetooth.clock_source == 0x1e77c && b->bluetooth.wrapper == 0x8c0000 && b->bluetooth.enable_ppid == 0x38a &&
	    b->bluetooth.supply_count == 6 && b->bluetooth.supply_mv[5] == 2156 &&
	    !strcmp(b->bluetooth.supplies[0], "L2D_E0") && !strcmp(b->bluetooth.rf_clock, "C1A_E0"), "Bluetooth board data");
	/* the motion sensor's mounting as the board file writes it ("-y", "x", "-z"): which of the chip's axes, and the sign */
	CHECK(b && hakios_board_has(HAKIOS_HAS_IMU_LSM6DSO) && b->imu.sda == 116 && b->imu.scl == 117 && b->imu.address == 0x6a &&
	    b->imu.full_scale == 4 && b->imu.axis[0] == 1 && b->imu.axis[1] == 0 && b->imu.axis[2] == 2 && b->imu.sign[0] == -1 &&
	    b->imu.sign[1] == 1 && b->imu.sign[2] == -1, "the motion sensor's board data");
	hakios_board_init();
	for (unsigned bit = 0; b != NULL && bit < hakios_feature_count; bit++) {
		if (b->features & (1u << bit)) {
			char name[64];

			/* (as a word of its own: the name, then a space or the line's end) */
			snprintf(name, sizeof(name), " %s%s", hakios_feature_names[bit], "");
			CHECK(logged(name), "%s is not in the line: %s", hakios_feature_names[bit], log_text);
		}
	}
	CHECK(log_text[strlen(log_text) - 1] == '\n', "the line has its end");
	/* ... and so would a board that has all of them: the buffer the tool sized takes every name */
	for (unsigned bit = 0; bit < hakios_feature_count; bit++) {
		snprintf(all + strlen(all), sizeof(all) - strlen(all), "%s%s", bit ? " " : "", hakios_feature_names[bit]);
	}
	CHECK(strlen(all) + 1 == HAKIOS_FEATURES_TEXT, "room for all the names is %u, they take %zu", HAKIOS_FEATURES_TEXT, strlen(all) + 1);
}

/* the line of features into a buffer too small for it: cut where it ends, never past it; and a board whose
 * features word has bits past the names there are says the names there are and no more */
static void
features_cut_short(void)
{
	struct hakios_board all = { .features = 0xffffffffu };
	char text[64];
	size_t first = strlen(hakios_feature_names[0]);

	for (size_t size = 1; size < sizeof(text); size++) {
		memset(text, 0x5a, sizeof(text));
		features_text(&all, text, size);
		CHECK(strlen(text) < size && text[size] == 0x5a, "a buffer of %zu: %zu bytes of text, nothing past it", size,
		    strlen(text));
	}
	features_text(&all, text, first + 1);
	CHECK(strcmp(text, hakios_feature_names[0]) == 0, "room for the first name and its end: it alone: %s", text);
	{
		static char line[HAKIOS_FEATURES_TEXT];

		features_text(&all, line, sizeof(line));
		CHECK(strlen(line) + 1 == HAKIOS_FEATURES_TEXT, "every name, and none past the last: %zu", strlen(line));
	}
}

/* the GPU as its driver is handed it: the board's gpu { }, with "off" before the speeds */
static void
gpu_a642(void)
{
	const struct adreno_board_profile *p;

	board = "phone-cmd";
	p = adreno_profile_for_board();
	CHECK(p != NULL, "the phone has a GPU profile");
	if (p == NULL) {
		return;
	}
	CHECK(strcmp(p->board, "phone-cmd") == 0 && strcmp(p->model, "Adreno 642") == 0 &&
	    strcmp(p->firmware_directory, "fw-cmd") == 0, "%s, %s, %s", p->board, p->model, p->firmware_directory);
	CHECK(p->chip_id == 0x06060010 && p->gmem_bytes == 1536 * 1024, "chip %#x, GMEM %u", p->chip_id, p->gmem_bytes);
	CHECK(adreno642_gmu_chip_id(p->chip_id) == 0x16060000, "as the GMU wants it: %#x", adreno642_gmu_chip_id(p->chip_id));
	CHECK(p->primitive_fifo == 0x00300000 && p->hang_cycles == 0x3ffff && p->uche_cmdq_config == 0x66906, "register values");
	CHECK(p->speed_fuse == 0x1e1 && p->gaming_fuse == 0x1ed && p->highest_bank_bit == 14 && p->gcc_debug_gpucc == 0x13c,
	    "fuses, bank bit, debug multiplexer");
	CHECK(p->zap_must_authenticate, "nothing to fall back on when the zap shader is refused");
	CHECK(p->alt_model == NULL, "one name");
	CHECK(p->speed_count == 5 && p->default_speed == 4, "%u speeds, default %u", p->speed_count, p->default_speed);
	CHECK(p->speeds[0].khz == 0 && p->speeds[0].level == 0, "speed 0 is off");
	CHECK(p->speeds[1].khz == 285000 && p->speeds[1].level == 64 && p->speeds[2].khz == 365000 && p->speeds[2].level == 80 &&
	    p->speeds[3].khz == 443000 && p->speeds[3].level == 128 && p->speeds[4].khz == 490000 && p->speeds[4].level == 144,
	    "the speeds and their supply levels");
}

static void
gpu_a642l(void)
{
	const struct adreno_board_profile *p;

	board = "phone-video";
	p = adreno_profile_for_board();
	CHECK(p != NULL, "the phone has a GPU profile");
	if (p == NULL) {
		return;
	}
	CHECK(strcmp(p->model, "Adreno 642L") == 0 && strcmp(p->firmware_directory, "fw-video") == 0, "%s, %s", p->model,
	    p->firmware_directory);
	CHECK(p->alt_model != NULL && strcmp(p->alt_model, "Adreno 643") == 0 && p->alt_bins[0] == 0xac && p->alt_bins[1] == 0xbe,
	    "the speed bins sold as the 643");
	CHECK(p->chip_id == 0x06030500 && p->gmem_bytes == 512 * 1024, "chip %#x, GMEM %u", p->chip_id, p->gmem_bytes);
	CHECK(p->primitive_fifo == 0x00200000 && p->hang_cycles == 0x1fffff && p->uche_cmdq_config == 0, "register values");
	CHECK(p->speed_fuse == 0x1e9 && p->gaming_fuse == 0x1f5 && p->highest_bank_bit == 14 && p->gcc_debug_gpucc == 0x146,
	    "fuses, bank bit, debug multiplexer");
	CHECK(!p->zap_must_authenticate, "the firmware's zap shader is fallen back on");
	CHECK(hakios_board()->wifi.radio_model == 0 && hakios_board()->wifi.early_pool_mb == 0 &&
	    hakios_board()->wifi.dsp_memory == 0x9ae00000 && hakios_board()->wifi.dsp_memory_size == 0x01900000,
	    "a radio on the SoC's own DSP, and where that DSP's memory is");
	CHECK(strcmp(hakios_board()->adsp.firmware, "fw-audio-video") == 0 && hakios_board()->adsp.memory_size == 0x04000000 &&
	    hakios_board()->adsp.charge_counter_divisor == 1, "its adsp { }");
	CHECK(p->speed_count == 4 && p->default_speed == 3, "%u speeds, default %u", p->speed_count, p->default_speed);
	CHECK(p->speeds[1].khz == 315000 && p->speeds[1].level == 64 && p->speeds[2].khz == 450000 && p->speeds[2].level == 128 &&
	    p->speeds[3].khz == 550000 && p->speeds[3].level == 192, "the speeds and their supply levels");
}

static void
emulator(void)
{
	board = "qemu-virt";
	CHECK(adreno_profile_for_board() == NULL, "no GPU to start");
	CHECK(hakios_board() != NULL && hakios_board()->backlight.type == HAKIOS_BACKLIGHT_SOFT, "the row of QEMU");
	for (unsigned bit = 0; bit < 32; bit++) {
		CHECK(hakios_board_has(1u << bit) == ((1u << bit) == HAKIOS_HAS_VIRTIO), "QEMU has its virtio devices and no "
		    "feature of a phone (bit %u)", bit);
	}
	CHECK(strcmp(get_string("features"), "virtio") == 0, "hw.board.features \"%s\"", get_string("features"));
	CHECK(hakios_board_cpu_groups() == 0 && hakios_board_cpu_group_of(0) == -1, "no groups of cores to clock");
}

static void
unknown(void)
{
	board = "a-phone-nobody-described";
	CHECK(hakios_board() == NULL, "no row");
	CHECK(adreno_profile_for_board() == NULL, "no GPU to start");
	for (unsigned bit = 0; bit < 32; bit++) {
		CHECK(!hakios_board_has(1u << bit), "no feature (bit %u)", bit);
	}
	CHECK(strcmp(get_string("name"), "unknown") == 0, "hw.board.name %s", get_string("name"));
	hakios_board_init();
	CHECK(logged("not one of the"), "said in the log");
}

/* every row: the names are what a loader can pass, and a feature has the addresses it needs */
static void
table(void)
{
	CHECK(hakios_board_count >= 3, "%u boards", hakios_board_count);
	CHECK(hakios_feature_count >= 12 && strcmp(hakios_feature_names[0], "cpufreq-epss") == 0 &&
	    strcmp(hakios_feature_names[11], "wifi-wcn6750") == 0, "the features' names, by bit");
	for (unsigned i = 0; i < hakios_board_count; i++) {
		const struct hakios_board *b = &hakios_boards[i];

		CHECK(b->name[0] != 0 && b->description[0] != 0 && b->soc[0] != 0, "row %u is named", i);
		CHECK(!(b->features & HAKIOS_HAS_CPUFREQ_EPSS) || (b->cpufreq.epss != 0 && b->cpufreq.epss_domain_size != 0),
		    "%s: cpufreq-epss without cpufreq { }", b->name);
		CHECK(b->backlight.type != HAKIOS_BACKLIGHT_DCS || (b->backlight.bytes >= 1 && b->backlight.bytes <= 2),
		    "%s: DCS bytes %u", b->name, b->backlight.bytes);
		CHECK(!(b->features & HAKIOS_HAS_DISPLAY_SCAN) || (b->display.intf != 0 && b->clocks.dispcc != 0 &&
		    !b->backlight.video_mode), "%s: display-scan without an interface, or on a video panel", b->name);
		CHECK(!(b->features & HAKIOS_HAS_BUTTONS_PMIC) || (b->spmi.core != 0 && b->spmi.cnfg != 0 && b->spmi.observer != 0 &&
		    b->buttons.pon != 0 && b->buttons.volume_up != 0), "%s: buttons-pmic without spmi { } or buttons { }", b->name);
		CHECK(b->rtc.peripheral == 0 || b->spmi.core != 0, "%s: rtc { } without spmi { }", b->name);
		CHECK(!(b->features & (HAKIOS_HAS_TORCH_PM8350C | HAKIOS_HAS_CAMERA_SM7325)) || (b->spmi.core != 0 &&
		    b->spmi.channels != 0), "%s: a driver that writes to a PMIC without the arbiter's channels", b->name);
		CHECK(!(b->features & (HAKIOS_HAS_TOUCH_GOODIX | HAKIOS_HAS_TOUCH_FT3658U | HAKIOS_HAS_SPEAKER_AW88261 |
		    HAKIOS_HAS_HAPTIC_AW86927 | HAKIOS_HAS_CAMERA_SM7325 | HAKIOS_HAS_MIC_WCD938X | HAKIOS_HAS_WIFI_WCN6750 |
		    HAKIOS_HAS_IMU_LSM6DSO | HAKIOS_HAS_IMU_BMI160 | HAKIOS_HAS_TOUCH_FT5X06)) || b->pins.tlmm != 0,
		    "%s: a driver that drives the SoC's pins without pins { }", b->name);
		CHECK(!(b->features & (HAKIOS_HAS_TOUCH_GOODIX | HAKIOS_HAS_TOUCH_FT3658U | HAKIOS_HAS_SPEAKER_AW88261 |
		    HAKIOS_HAS_CAMERA_SM7325)) || b->clocks.gcc != 0, "%s: a driver that switches a bus's clocks on without clocks { }",
		    b->name);
		CHECK(!(b->features & (HAKIOS_HAS_TOUCH_GOODIX | HAKIOS_HAS_TOUCH_FT3658U | HAKIOS_HAS_SPEAKER_AW88261 |
		    HAKIOS_HAS_CAMERA_SM7325 | HAKIOS_HAS_ADSP | HAKIOS_HAS_WIFI_WCN6750)) || (b->rpmh.cmd_db != 0 &&
		    b->rpmh.cmd_db_size != 0 && b->rpmh.rsc != 0 && b->rpmh.tcs_offset != 0),
		    "%s: a driver that asks RPMh for a supply without rpmh { }", b->name);
		CHECK(!(b->features & (HAKIOS_HAS_SPEAKER_AW88261 | HAKIOS_HAS_CAMERA_SM7325 | HAKIOS_HAS_WIFI_WCN6750)) ||
		    b->smmu.apps != 0, "%s: a driver that gives a device its addresses without smmu { }", b->name);
		CHECK(!(b->features & (HAKIOS_HAS_ADSP | HAKIOS_HAS_WIFI_WCN6750)) || (b->ipc.aoss != 0 && b->ipc.ipcc != 0 &&
		    b->ipc.smem != 0 && b->ipc.smem_size != 0 && b->ipc.smem_lock != 0),
		    "%s: a driver that talks to another processor without ipc { }", b->name);
		CHECK(!(b->features & HAKIOS_HAS_WIFI_WCN6750) || (b->wifi.dsp_memory != 0 && b->wifi.dsp_memory_size != 0),
		    "%s: a radio on the SoC's DSP without the DSP's memory", b->name);
		CHECK(!(b->features & HAKIOS_HAS_CAMERA_SM7325) || (b->camera.camcc != 0 && b->camera.ife != 0 && b->camera.csid != 0 &&
		    b->camera.csiphy != 0), "%s: the cameras' driver without the camera subsystem's blocks", b->name);
		CHECK(!(b->features & HAKIOS_HAS_MIC_WCD938X) || (b->lpass.tx_macro != 0 && b->lpass.va_macro != 0 &&
		    b->lpass.tx_swr != 0 && b->lpass.tx_swr_hctl != 0 && b->lpass.lpi != 0 && b->lpass.lpi_slew != 0),
		    "%s: the microphones' driver without the audio subsystem's blocks", b->name);
		CHECK(!(b->features & HAKIOS_HAS_HAPTIC_AW86927) || (b->haptic.sda != b->haptic.scl && b->haptic.reset != b->haptic.sda &&
		    b->haptic.reset != b->haptic.scl), "%s: the vibration motor's driver without its pins", b->name);
		CHECK(!(b->features & (HAKIOS_HAS_IMU_LSM6DSO | HAKIOS_HAS_IMU_BMI160)) || (b->imu.sda != b->imu.scl && b->imu.address != 0 &&
		    b->imu.axis[0] + b->imu.axis[1] + b->imu.axis[2] == 3 && b->imu.axis[0] != b->imu.axis[1] &&
		    b->imu.axis[1] != b->imu.axis[2] && b->imu.axis[0] != b->imu.axis[2]), "%s: the motion sensor's driver without its "
		    "pins, its address or a turn of the chip", b->name);
		CHECK((b->features & (HAKIOS_HAS_IMU_LSM6DSO | HAKIOS_HAS_IMU_BMI160)) != (HAKIOS_HAS_IMU_LSM6DSO | HAKIOS_HAS_IMU_BMI160),
		    "%s: two motion sensors' chips for the one imu { }", b->name);
		CHECK(!(b->features & (HAKIOS_HAS_TOUCH_GOODIX | HAKIOS_HAS_TOUCH_FT3658U)) || (b->touch.serial_engine != 0 &&
		    b->touch.se_clock != 0 && b->touch.io_supply != NULL && b->touch.data_in != b->touch.data_out &&
		    b->touch.clock != b->touch.chip_select && b->touch.interrupt != b->touch.reset),
		    "%s: a touch controller's driver without its wiring", b->name);
		CHECK(!(b->features & HAKIOS_HAS_TOUCH_GOODIX) || (b->touch.analog_enable >= 0 && b->touch.vendor_pin[0] >= 0 &&
		    b->touch.vendor_pin[1] >= 0), "%s: the Goodix driver without its analog supply's pin and the two straps", b->name);
		CHECK(!(b->features & HAKIOS_HAS_TOUCH_FT3658U) || b->touch.analog_supply != NULL,
		    "%s: the FT3658U's driver without its analog supply", b->name);
		CHECK(!(b->features & HAKIOS_HAS_TOUCH_FT5X06) || (b->touch_i2c.sda != b->touch_i2c.scl && b->touch_i2c.address != 0 &&
		    b->touch_i2c.interrupt != b->touch_i2c.reset && b->touch_i2c.contacts >= 1 && b->touch_i2c.contacts <= 10 &&
		    b->touch_i2c.max_x != 0 && b->touch_i2c.max_x <= 0xfff && b->touch_i2c.max_y != 0 && b->touch_i2c.max_y <= 0xfff),
		    "%s: the FT5x06 family's driver without its bus, address, fingers or coordinates", b->name);
		CHECK(!(b->features & HAKIOS_HAS_TOUCH_FT5X06) || (b->touch_i2c.io_supply == NULL && b->touch_i2c.analog_supply == NULL) ||
		    (b->rpmh.cmd_db != 0 && b->rpmh.rsc != 0), "%s: a touch controller's supplies named without rpmh { }", b->name);
		CHECK((b->touch_i2c.analog_supply != NULL) == (b->touch_i2c.analog_mv != 0), "%s: touch-i2c { }: an analog supply and its "
		    "voltage go together", b->name);
		CHECK(b->storage.blocks == 0 || (b->storage.first_lba != 0 && b->storage.write_first != 0 &&
		    b->storage.write_first < b->storage.blocks), "%s: a partition to write into that begins at 0, or from a block outside it", b->name);
		CHECK(!(b->features & HAKIOS_HAS_SPEAKER_AW88261) || (b->speaker.sda != b->speaker.scl &&
		    b->speaker.address[0] != 0 && b->speaker.address[1] != 0 && b->speaker.address[0] != b->speaker.address[1]),
		    "%s: the speaker amplifiers' driver without their bus and addresses", b->name);
		CHECK(!(b->features & HAKIOS_HAS_TORCH_PM8350C) || (b->torch.flash != 0 && b->torch.temp_alarm != 0),
		    "%s: the flashlight's driver without its peripherals", b->name);
		CHECK(!(b->features & HAKIOS_HAS_DDR_RPMH) || (b->rpmh.cmd_db != 0 && b->rpmh.cmd_db_size != 0 && b->rpmh.rsc != 0 &&
		    b->rpmh.tcs_offset != 0), "%s: ddr-rpmh without rpmh { }", b->name);
		CHECK(!(b->features & HAKIOS_HAS_ADSP) || (b->adsp.firmware != NULL && b->adsp.memory != 0 && b->adsp.memory_size != 0 &&
		    b->adsp.charge_counter_divisor != 0), "%s: adsp without adsp { }", b->name);
		CHECK(!(b->features & HAKIOS_HAS_GPU_A660) || (b->adreno.gpu != 0 && b->adreno.gmu != 0 && b->adreno.gpucc != 0 &&
		    b->adreno.cx_misc != 0 && b->adreno.rscc != 0 && b->adreno.smmu != 0 && b->adreno.pdc != 0 &&
		    b->adreno.qfprom != 0 && b->clocks.gcc != 0 && b->rpmh.cmd_db != 0), "%s: gpu-a660 without the GPU's blocks",
		    b->name);
		if (b->features & HAKIOS_HAS_GPU_A660) {
			CHECK(b->gpu.chip_id != 0 && b->gpu.model != NULL && b->gpu.firmware != NULL, "%s: gpu-a660 without gpu { }", b->name);
			CHECK(b->gpu.speed_count >= 1 && b->gpu.speed_count <= 5 && b->gpu.default_speed >= 1 &&
			    b->gpu.default_speed <= b->gpu.speed_count, "%s: %u GPU speeds, default %u", b->name, b->gpu.speed_count,
			    b->gpu.default_speed);
			for (unsigned s = 1; s < b->gpu.speed_count && s < 5; s++) {
				CHECK(b->gpu.speeds_khz[s - 1] < b->gpu.speeds_khz[s], "%s: GPU speeds from the slowest up", b->name);
			}
		}
		CHECK(!(b->features & HAKIOS_HAS_VIRTIO) || !(b->features & ~HAKIOS_HAS_VIRTIO), "%s: virtio beside a phone's parts",
		    b->name);
		CHECK(__builtin_popcount(b->features & (HAKIOS_HAS_TOUCH_GOODIX | HAKIOS_HAS_TOUCH_FT3658U | HAKIOS_HAS_TOUCH_FT5X06)) <= 1,
		    "%s lists two touch controllers", b->name);
		for (unsigned j = 0; j < i; j++) {
			CHECK(strcmp(b->name, hakios_boards[j].name) != 0, "%s is there twice", b->name);
		}
	}
}

/* a loader that named no board, and one whose name only begins like a board's */
static void
unnamed(void)
{
	board = NULL;
	CHECK(hakios_board() == NULL && !hakios_board_has(HAKIOS_HAS_CPUFREQ_EPSS), "no name, no board");
	CHECK(strcmp(get_string("name"), "unknown") == 0, "hw.board.name %s", get_string("name"));
}

static void
almost_named(void)
{
	board = "phone-cmd-2";          /* (not "phone-cmd": the whole name is compared, with its end) */
	CHECK(hakios_board() == NULL, "a name that begins like a board's is not that board");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "known_phone", known_phone }, { "every_feature_said", every_feature_said }, { "features_cut_short", features_cut_short }, { "gpu_a642", gpu_a642 }, { "gpu_a642l", gpu_a642l },
		{ "emulator", emulator }, { "unknown", unknown }, { "unnamed", unnamed }, { "almost_named", almost_named }, { "table", table },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			scenarios[i].run();
			printf("%-20s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures != 0;
		}
	}
	return 2;
}
