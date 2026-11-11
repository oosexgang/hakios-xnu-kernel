/*
 * Which board the kernel takes itself for (kernel/overlay/bsd/dev/hakios_board.c), against the table
 * generated from devices/ and soc/: the row is picked by the loader's name, a board that is not known
 * has no feature at all, and hw.board.* says what was found. One scenario a process (the lookup is
 * done once), as the driver tests.
 */
#include "fakekernel.h"

extern char log_text[];

#include "hakios_board.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static void
known_phone(void)
{
	const struct hakios_board *b;

	board = "renoir";
	b = hakios_board();
	CHECK(b != NULL && strcmp(b->name, "renoir") == 0 && strcmp(b->soc, "sm7350") == 0, "the row of renoir");
	CHECK(hakios_board_has(HAKIOS_HAS_CPUFREQ_EPSS) && hakios_board_has(HAKIOS_HAS_ADSP), "its features");
	/* its own touch controller, and none of the parts only the other phone has */
	CHECK(hakios_board_has(HAKIOS_HAS_TOUCH_FT3658U) && !hakios_board_has(HAKIOS_HAS_TOUCH_GOODIX), "its touch controller");
	CHECK(!hakios_board_has(HAKIOS_HAS_HAPTIC_AW86927) && !hakios_board_has(HAKIOS_HAS_TORCH_PM8350C) &&
	    !hakios_board_has(HAKIOS_HAS_SPEAKER_AW88261) && !hakios_board_has(HAKIOS_HAS_CAMERA_REDWOOD) &&
	    !hakios_board_has(HAKIOS_HAS_WIFI_WCN6750) && !hakios_board_has(HAKIOS_HAS_MIC_WCD938X),
	    "no driver wired for another phone's parts");
	CHECK(b != NULL && b->backlight.type == HAKIOS_BACKLIGHT_DCS && b->backlight.dsi != 0 && b->clocks.dispcc != 0 &&
	    b->backlight.raw_min < b->backlight.raw_max, "a DCS panel has what the driver needs");
	CHECK(strcmp(get_string("name"), "renoir") == 0, "hw.board.name %s", get_string("name"));
	CHECK(strcmp(get_string("soc"), "sm7350") == 0, "hw.board.soc %s", get_string("soc"));
	CHECK(strstr(get_string("features"), "cpufreq-epss") != NULL, "hw.board.features %s", get_string("features"));
	/* its groups of cores: four small, three big, one prime */
	CHECK(hakios_board_cpu_groups() == 3, "%u groups", hakios_board_cpu_groups());
	CHECK(hakios_board_cpu_group_of(0) == 0 && hakios_board_cpu_group_of(3) == 0 && hakios_board_cpu_group_of(4) == 1 &&
	    hakios_board_cpu_group_of(6) == 1 && hakios_board_cpu_group_of(7) == 2, "which core is in which group");
	CHECK(hakios_board_cpu_group_of(8) == -1, "a core beyond the groups is in none");
	hakios_board_init();
	CHECK(logged("board: renoir (Xiaomi Mi 11 Lite 5G, sm7350): cpufreq-epss"), "said in the log: %s", log_text);
}

static void
emulator(void)
{
	board = "qemu-virt";
	CHECK(hakios_board() != NULL && hakios_board()->backlight.type == HAKIOS_BACKLIGHT_SOFT, "the row of QEMU");
	for (unsigned bit = 0; bit < 32; bit++) {
		CHECK(!hakios_board_has(1u << bit), "QEMU has no feature of a phone (bit %u)", bit);
	}
	CHECK(strcmp(get_string("features"), "") == 0, "hw.board.features \"%s\"", get_string("features"));
	CHECK(hakios_board_cpu_groups() == 0 && hakios_board_cpu_group_of(0) == -1, "no groups of cores to clock");
}

static void
unknown(void)
{
	board = "a-phone-nobody-described";
	CHECK(hakios_board() == NULL, "no row");
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
		    "%s: cpufreq-epss without [cpufreq]", b->name);
		CHECK(b->backlight.type != HAKIOS_BACKLIGHT_DCS || (b->backlight.bytes >= 1 && b->backlight.bytes <= 2),
		    "%s: DCS bytes %u", b->name, b->backlight.bytes);
		CHECK(!((b->features & HAKIOS_HAS_TOUCH_GOODIX) && (b->features & HAKIOS_HAS_TOUCH_FT3658U)),
		    "%s lists two touch controllers", b->name);
		for (unsigned j = 0; j < i; j++) {
			CHECK(strcmp(b->name, hakios_boards[j].name) != 0, "%s is there twice", b->name);
		}
	}
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "known_phone", known_phone }, { "emulator", emulator }, { "unknown", unknown }, { "table", table },
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
