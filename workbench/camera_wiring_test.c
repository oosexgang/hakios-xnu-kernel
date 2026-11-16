/*
 * Where the camera driver (drivers/board/camera.c) takes its sensors from. The driver is
 * compiled as it is into a host program; nothing of the cameras is modelled. What is tested: each sensor
 * the driver works on is put together from the board's camera { } and its sensor@N nodes, with its chip's
 * ways and the SoC's clock registers for its MCLK and CSI receiver; for the test phone that has the first
 * phone's cameras that is, field for field, the table the driver used to carry; and a board that names a
 * chip or a clock the driver does not know, or no sensor, is refused with the reason; and the clocks' rates
 * and dividers the driver works out of the SoC file's PLL configurations are the ones it had written in.
 * Besides: the PM8008i's registers a write may reach (its chip enable, the sensor's own LDOs' set point, mode
 * and enable, nothing else), and a sensor's table (RWCTAB02) taken only when it is whole.
 *
 *   camera_wiring_test SCENARIO
 */
#include "fakekernel.h"

extern char log_text[];

/* (the PMIC arbiter, for the scenarios of the PM8008i's reset GPIO; the others read and write no register) */
#include "spmi_model.h"

static struct spmi_peripheral pgpio_model;
static bool arbiter;
static unsigned accesses;

uint32_t
hakios_test_rd(vm_offset_t address)
{
	accesses++;
	return arbiter ? spmi_model_rd(address) : 0;
}

void
hakios_test_wr(vm_offset_t address, uint32_t value)
{
	accesses++;
	if (arbiter) {
		spmi_model_wr(address, value);
	}
}
vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }
vm_offset_t ml_io_map_unmappable(vm_offset_t phys, vm_size_t size __unused, unsigned int flags __unused) { return phys; }
uint64_t kvtophys(vm_offset_t va) { return va; }
int kmem_alloc(void *map __unused, vm_offset_t *address __unused, vm_size_t size __unused, int flags __unused, int tag __unused) { return 1; }
void kmem_free(void *map __unused, vm_offset_t address __unused, vm_size_t size __unused) { }
/* the small image's files: none, but for a sensor's table a scenario makes (camera_table) */
static const uint8_t *table_file;
static uint64_t table_size;
const uint8_t *
hakios_wifi_file(const char *name __unused, uint64_t *size)
{
	*size = table_size;
	return table_file;
}
void hakios_rpmh_lock(void) { }
void hakios_rpmh_unlock(void) { }
void hakios_spmi_lock(void) { }
void hakios_spmi_unlock(void) { }
struct fake_lock hakios_smmu_lock = { PTHREAD_MUTEX_INITIALIZER };

#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#include "board/board.c"
#include "board/camera.c"
#undef printf

uint64_t loader_recording[3];          /* (the loader's, where the driver says which memory holds a recording) */

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* the table the driver carried before its sensors were the board's (phone-video has those cameras) */
static const struct {
	const char *name, *tab, *bus_name, *chip;
	unsigned sda, scl, reset, mclk_pin, csiphy;
	uint32_t mclk_cmd, mclk_cbcr, phy_cbcr, timer_cmd, timer_cbcr;
	uint8_t regs, bayer, pattern_bayer;
	uint16_t gain_unity, gain_max;
	struct hakios_camera_supply vana, vdig, vio;
	bool focus_motor, exposure;
} before[] = {
	{ "macro (OV02B10)", "camera-ov02b10.tab", "CCI 1 master 0", "OV02B10", 73, 74, 77, 66, 2, 0xe038, 0xe050, 0xe110, 0xe0f4,
	  0xe10c, REGS_OV_PAGED, 3, 2, 16, 0xf8, { 1, 7, 2800, 0, 0 }, { 1, 6, 1800, 0, 0 }, { 0, 0, 0, 0, 0 }, false, true },
	{ "wide (S5KHM2SD)", "camera-s5khm2sd.tab", "CCI 0 master 1", "S5KHM2SD", 71, 72, 21, 65, 1, 0xe01c, 0xe034, 0xe0ec, 0xe0d0,
	  0xe0e8, REGS_SMIA, 1, 2, 32, 0x800, { 2, 0, 2800, 106, 135 }, { 1, 1, 1000, 0, 0 }, { 1, 5, 1800, 0, 0 }, true, true },
	{ "front (OV16A1Q)", "camera-ov16a1q.tab", "CCI 0 master 0", "OV16A1Q", 69, 70, 20, 64, 0, 0xe000, 0xe018, 0xe0c8, 0xe0ac,
	  0xe0c4, REGS_OV16, 3, 0, 0x100, 0xf80, { 1, 3, 2800, 0, 0 }, { 1, 2, 1200, 0, 0 }, { 1, 5, 1800, 0, 0 }, false, false },
	{ "ultra-wide (S5K4H7)", "camera-s5k4h7.tab", "CCI 1 master 1", "S5K4H7", 75, 76, 78, 67, 3, 0xe054, 0xe06c, 0xe138, 0xe11c,
	  0xe134, REGS_SMIA, 1, 2, 32, 0x200, { 1, 4, 2800, 0, 0 }, { 2, 0, 1200, 108, 135 }, { 1, 5, 1800, 0, 0 }, false, false },
};

static bool
same_supply(const struct hakios_camera_supply *a, const struct hakios_camera_supply *b)
{
	return a->kind == b->kind && a->ldo == b->ldo && a->mv == b->mv && a->gpio == b->gpio && a->ramp_us == b->ramp_us;
}

/* the board, to be changed for a scenario: a copy of its row, which hakios_board() answers with from now on */
static struct hakios_board *
board_to_change(void)
{
	static struct hakios_board copy;

	copy = *hakios_board();
	found = &copy;
	return &copy;
}

/* ---- scenarios ---------------------------------------------------------------------------------------- */

/* the four sensors of the test phone, as the driver had them in its own table */
static void
as_before(void)
{
	CHECK(sensors_from_board(), "taken: %s", log_text);
	CHECK(sensor_count == 4, "four sensors: %u", sensor_count);
	for (unsigned i = 0; i < 4 && i < sensor_count; i++) {
		const struct sensor_hw *s = &sensors[i];

		CHECK(strcmp(s->name, before[i].name) == 0 && strcmp(s->tab, before[i].tab) == 0 && strcmp(s->bus_name, before[i].bus_name) == 0 &&
		    strcmp(s->chip, before[i].chip) == 0, "%u: %s, %s, %s, %s", i, s->name, s->tab, s->bus_name, s->chip);
		CHECK(s->sda == before[i].sda && s->scl == before[i].scl && s->reset == before[i].reset && s->mclk_pin == before[i].mclk_pin &&
		    s->csiphy == before[i].csiphy, "%u: pins %u %u %u %u, csiphy %u", i, s->sda, s->scl, s->reset, s->mclk_pin, s->csiphy);
		CHECK(s->mclk_cmd == before[i].mclk_cmd && s->mclk_cbcr == before[i].mclk_cbcr && s->phy_cbcr == before[i].phy_cbcr &&
		    s->timer_cmd == before[i].timer_cmd && s->timer_cbcr == before[i].timer_cbcr, "%u: clocks %#x %#x %#x %#x %#x", i,
		    s->mclk_cmd, s->mclk_cbcr, s->phy_cbcr, s->timer_cmd, s->timer_cbcr);
		CHECK(s->regs == before[i].regs && s->bayer == before[i].bayer && s->pattern_bayer == before[i].pattern_bayer &&
		    s->gain_unity == before[i].gain_unity && s->gain_max == before[i].gain_max, "%u: its chip's ways", i);
		CHECK(same_supply(&s->vana, &before[i].vana) && same_supply(&s->vdig, &before[i].vdig) && same_supply(&s->vio, &before[i].vio),
		    "%u: supplies", i);
		CHECK(s->focus_motor == before[i].focus_motor && s->exposure == before[i].exposure, "%u: focus %d, exposure %d", i,
		    s->focus_motor, s->exposure);
	}
	CHECK(hakios_board()->cameras.test_sensor == 0 && hakios_board()->cameras.darwin_sensor == 1, "the macro tested, the wide for Darwin");
	CHECK(hakios_board()->cameras.darwin_front == 2, "the front one as Darwin's front camera: %u", hakios_board()->cameras.darwin_front);
	for (unsigned i = 0; i < 4; i++) {
		CHECK(hakios_board()->cameras.sensors[i].upside_down == (i == 2), "%u: upside down only the front one", i);
	}
	CHECK(strcmp(hakios_board()->cameras.sensors[1].focus_supply, "ldoc9") == 0 && hakios_board()->cameras.sensors[1].focus_mv == 2800,
	    "the wide one's lens motor supply");
	CHECK(accesses == 0, "no register touched: %u", accesses);
	select_sensor(2);
	CHECK(hw == &sensors[2] && cci_bus.pins.sda == 69 && cci_bus.pins.scl == 70 && strcmp(cci_bus.name, "CCI 0 master 0") == 0,
	    "a sensor chosen is the bus the driver talks on");
}

/* a chip the driver does not know: refused, and said which */
static void
unknown_chip(void)
{
	board_to_change()->cameras.sensors[2].chip = "IMX766";
	CHECK(!sensors_from_board(), "refused");
	CHECK(logged("the board's front camera is IMX766, which this driver does not know"), "said: %s", log_text);
}

/* an MCLK or a CSI receiver the SoC has not: refused */
static void
no_such_clock(void)
{
	board_to_change()->cameras.sensors[0].mclk = 4;
	CHECK(!sensors_from_board(), "refused");
	CHECK(logged("the board's macro camera is on an MCLK or a CSIPHY the SoC has not"), "said: %s", log_text);
}

/* the clocks of the SoC file's PLL configurations: the rates, dividers and sources the driver had written in */
static void
clock_plan(void)
{
	static const uint32_t dividers[] = { 14, 7, 2, 3, 2, 1 }, sources[] = { 2, 2, 2, 2, 2, 3 };
	struct hakios_board *board;

	CHECK(pll0_khz() == 1200000 && pll0_output_khz(false) == 600000 && pll0_output_khz(true) == 400000,
	    "PLL0 %u kHz, even %u, odd %u", pll0_khz(), pll0_output_khz(false), pll0_output_khz(true));
	CHECK(pll2_khz() == 1440000 && mclk_24mhz_n() == 6, "PLL2 %u kHz, MCLK n %u", pll2_khz(), mclk_24mhz_n());
	CHECK(rcg_plan(), "planned: %s", log_text);
	for (unsigned i = 0; i < sizeof(rcgs) / sizeof(rcgs[0]); i++) {
		CHECK(rcgs[i].divider == dividers[i] && rcgs[i].source == sources[i], "%s: source %u, divider %u", rcgs[i].name,
		    rcgs[i].source, rcgs[i].divider);
	}
	CHECK(accesses == 0, "no register touched: %u", accesses);
	/* a PLL0 that makes none of those rates, and a PLL2 that makes no 24 MHz: refused, with the rate */
	board = board_to_change();
	board->camera.pll0_config[0] = 0x30;
	board->camera.pll0_config[2] = 0;
	CHECK(!rcg_plan() && logged("slow_ahb at 80000 kHz is no whole number of halves of PLL0's even output (460800 kHz)"),
	    "refused: %s", log_text);
	board->camera.pll2_config[0] = 0x4c;
	CHECK(mclk_24mhz_n() == 0 && logged("PLL2 at 1459200 kHz"), "refused: %s", log_text);
}

/* camera { } without a sensor: nothing to work on, and said so */
static void
no_sensor(void)
{
	board_to_change()->cameras.sensor_count = 0;
	CHECK(!sensors_from_board() && sensor_count == 0, "refused");
	CHECK(logged("the board's camera { } names no sensor"), "said: %s", log_text);
}

/* ---- the PM8008i's reset: a GPIO of a PMIC, behind the arbiter --------------------------------------------- */

/* the arbiter where the SoC file has it, the GPIO on channel 4 (another system's) and channel 9 (this one's) */
static void
arbiter_and_gpio(void)
{
	const struct hakios_board *b = hakios_board();

	arbiter = true;
	spmi.core = b->spmi.core;
	spmi.cnfg = b->spmi.cnfg;
	spmi.channels = b->spmi.channels;
	spmi.ee = b->spmi.ee;
	spmi.observer = b->spmi.observer + 0x10000 * spmi.ee;
	pgpio_model.id = b->cameras.pmic_reset;
	spmi_channel(4, &pgpio_model, 3);
	spmi_channel(9, &pgpio_model, spmi.ee);
	pgpio_model.reg[PGPIO_TYPE] = 0x10;
	CHECK(hakios_spmi_map(&arb), "the arbiter mapped");
}

/* of its two channels the one this system writes through; a write to one of its five registers goes there */
static void
pgpio_through_our_channel(void)
{
	arbiter_and_gpio();
	pgpio_find();
	CHECK(pgpio_found && pgpio_writable && pgpio_channel == 9, "channel 9, ours: %u", pgpio_channel);
	CHECK(logged("channel 4 is the PM8008i's reset GPIO (0x28a00); written by system 3\n") &&
	    logged("channel 9 is the PM8008i's reset GPIO (0x28a00); written by system 0, which is us"), "both said: %s", log_text);
	CHECK(pgpio_put(PGPIO_MODE_CTL, 1, false) && pgpio_model.reg[PGPIO_MODE_CTL] == 1 && spmi.denied == 0, "written, read back");
}

/* its only channel another system's: read through it, never written */
static void
pgpio_only_anothers_channel(void)
{
	uint8_t type = 0;

	arbiter_and_gpio();
	spmi.map[9] = 0;
	pgpio_find();
	CHECK(pgpio_found && !pgpio_writable && pgpio_channel == 4, "channel 4, not ours: %u", pgpio_channel);
	CHECK(pgpio_read(PGPIO_TYPE, &type) == NULL && type == 0x10, "read: %#x", type);
	CHECK(!pgpio_put(PGPIO_MODE_CTL, 1, false) && spmi.writes == 0 && spmi.denied == 0, "not written, not even tried");
	CHECK(broken && logged("a write to the PMIC that may not be made"), "and given up: %s", log_text);
}

/* of all its registers five may be written, and only those */
static void
pgpio_five_registers(void)
{
	unsigned allowed = 0;

	arbiter_and_gpio();
	pgpio_find();
	for (unsigned reg = 0; reg < 256; reg++) {
		bool put;

		broken = false;
		put = pgpio_put((uint8_t)reg, 0, false);
		allowed += put;
		CHECK(put == (reg == 0x40 || reg == 0x41 || reg == 0x42 || reg == 0x44 || reg == 0x46), "register %#x", reg);
	}
	CHECK(allowed == 5 && pgpio_model.writes == 5, "five written: %u", pgpio_model.writes);
}

/* a transfer the arbiter drops is made again, up to three times */
static void
pgpio_dropped(void)
{
	uint8_t type = 0;

	arbiter_and_gpio();
	pgpio_find();
	spmi.drop_reads = 2;
	CHECK(pgpio_read(PGPIO_TYPE, &type) == NULL && type == 0x10, "two reads dropped, the third made");
	spmi.drop_reads = 3;
	CHECK(pgpio_read(PGPIO_TYPE, &type) != NULL, "three dropped: not read");
	spmi.drop_reads = 0;
	spmi.drop_writes = 2;
	CHECK(pgpio_put(PGPIO_EN_CTL, 0x80, false) && pgpio_model.reg[PGPIO_EN_CTL] == 0x80, "two writes dropped, the third made");
	spmi.drop_writes = 3;
	CHECK(!pgpio_put(PGPIO_EN_CTL, 0x00, false) && pgpio_model.reg[PGPIO_EN_CTL] == 0x80 && broken, "three dropped: not written");
}

/* a register that does not take what is written; a channel that is somebody else's by now: given up */
static void
keeps_nothing(struct spmi_peripheral *p __unused, uint8_t reg __unused, uint8_t value __unused) { }

static void
pgpio_given_up(void)
{
	arbiter_and_gpio();
	pgpio_find();
	pgpio_model.written = keeps_nothing;
	CHECK(!pgpio_put(PGPIO_DIG_PULL_CTL, 5, false) && broken, "not taken");
	CHECK(logged("the PM8008i's reset GPIO 0x42 <- 0x05: does not read back (reads 0x00)"), "said: %s", log_text);
	pgpio_model.written = NULL;
	CHECK(!pgpio_put(PGPIO_DIG_PULL_CTL, 5, false) && pgpio_model.reg[PGPIO_DIG_PULL_CTL] == 0, "once given up, nothing more");
	CHECK(pgpio_put(PGPIO_DIG_PULL_CTL, 5, true) && pgpio_model.reg[PGPIO_DIG_PULL_CTL] == 5, "but what puts things back, yes");
	spmi.map[9] = 0x3c000;
	CHECK(!pgpio_put(PGPIO_DIG_PULL_CTL, 1, true) && pgpio_model.reg[PGPIO_DIG_PULL_CTL] == 5, "a channel that is not its own now");
	CHECK(logged("the arbiter's channel is no longer the PM8008i's reset GPIO") || broken, "said");
}

/* how many times `text' is in the log */
static unsigned
said_times(const char *text)
{
	unsigned n = 0;

	for (const char *at = log_text; (at = strstr(at, text)) != NULL; at++) {
		n++;
	}
	return n;
}

/*
 * The PM8008i's registers a write may reach (pm8008_put()): its chip enable, and the set point, mode and enable
 * of the LDOs the sensor is on -- the front camera's 2, 3 and 5 on this board. Anything else is refused before
 * the bus is touched, said, and nothing more written in the run.
 */
static void
pm8008_whitelist(void)
{
	static const struct { uint8_t address; uint16_t reg; const char *what; } refused[] = {
		{ PM8008_CHIP, MISC_CHIP_ENABLE + 1, "the chip's register next to its enable" },
		{ PM8008_REGULATORS, MISC_CHIP_ENABLE, "the enable's number at the regulators' address" },
		{ 0x0a, MISC_CHIP_ENABLE, "another address" },
		{ PM8008_REGULATORS, LDO_BASE(1) + LDO_ENABLE, "an LDO that is not the sensor's (1)" },
		{ PM8008_REGULATORS, LDO_BASE(7) + LDO_ENABLE, "nor 7" },
		{ PM8008_REGULATORS, LDO_BASE(8) + LDO_ENABLE, "past the seventh" },
		{ PM8008_REGULATORS, LDO_BASE(3) + 0x44, "the sensor's LDO, another register of it" },
		{ PM8008_REGULATORS, LDO_BASE(3) + LDO_ENABLE + 1, "nor the one after its enable" },
	};

	board = "phone-video";
	CHECK(sensors_from_board() && sensor_count == 4, "the board's sensors");
	hw = &sensors[2];
	for (unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
		unsigned before = accesses, said = said_times("not to be written");

		broken = false;
		CHECK(!pm8008_put(refused[i].address, refused[i].reg, 0, false) && accesses == before &&
		    said_times("not to be written") == said + 1 && broken, "%s: refused, said, the bus untouched", refused[i].what);
	}
	static const uint16_t allowed[] = { LDO_BASE(2) + LDO_VSET_LB, LDO_BASE(2) + LDO_VSET_UB, LDO_BASE(3) + LDO_MODE_CTL1,
		LDO_BASE(5) + LDO_ENABLE };
	for (unsigned i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
		unsigned said = said_times("not to be written");

		broken = false;
		(void)pm8008_put(PM8008_REGULATORS, allowed[i], 0, false);
		CHECK(said_times("not to be written") == said, "register %#x of the sensor's LDOs: not refused", allowed[i]);
	}
	broken = false;
	(void)pm8008_put(PM8008_CHIP, MISC_CHIP_ENABLE, 0, false);
	CHECK(said_times("not to be written") == sizeof(refused) / sizeof(refused[0]), "the chip's enable: not refused");
	broken = true;
	CHECK(!pm8008_put(PM8008_CHIP, MISC_CHIP_ENABLE, 0, false), "after a fault: not even the enable");
}

/* a sensor's table (RWCTAB02): its head, `modes' modes, two power sequences of `power' entries, and `tables' */
static uint8_t table[4096];

static uint64_t
make_table(uint32_t modes, uint32_t power, uint32_t tables)
{
	struct ctab_head head = { .slave = 0x20, .address_bytes = 2, .data_bytes = 1, .id_address = 0x300b, .id = 0x16, .id_mask = 0xff,
		                  .modes = modes };
	uint8_t *at = table + sizeof(head) + modes * sizeof(struct ctab_mode);

	memset(table, 0, sizeof(table));
	memcpy(head.magic, "RWCTAB02", 8);
	memcpy(table, &head, sizeof(head));
	for (int p = 0; p < 2; p++) {
		memcpy(at, p ? "power-of" : "power-on", 8);
		memcpy(at + 8, &power, 4);
		at += 12 + power * sizeof(struct ctab_entry);
	}
	memcpy(at, &tables, 4);
	return (uint64_t)(at + 4 - table);
}

/* whether ctab_load() takes the table as made, `size' bytes of it */
static bool
takes(uint64_t size)
{
	table_file = table;
	table_size = size;
	return ctab_load();
}

/*
 * A sensor's table taken only when it is one: its magic, 1 to 16 modes, and the two power sequences and the
 * count of tables all within the file -- a file cut anywhere short of its end is refused, not read past
 */
static void
camera_table(void)
{
	uint64_t whole;

	board = "phone-video";
	CHECK(sensors_from_board(), "the board's sensors");
	hw = &sensors[2];
	whole = make_table(2, 3, 0);
	CHECK(takes(whole), "a table as it should be: %s", log_text);
	CHECK(ctab->modes == 2 && ctab_power == table + sizeof(struct ctab_head) + 2 * sizeof(struct ctab_mode) &&
	    ctab_tables == table + whole - 4, "its parts where they are");
	CHECK(!takes(whole - 1) && !takes(whole - 4 - 12) && !takes(sizeof(struct ctab_head) - 1) && !takes(0), "cut short: refused");
	CHECK(!takes(whole - 4 - 12 - 3 * sizeof(struct ctab_entry) - 1), "cut in the second power sequence: refused");
	CHECK(takes(make_table(16, 0, 0)), "16 modes");
	CHECK(!takes(make_table(17, 0, 0)) && !takes(make_table(0, 0, 0)), "17 modes, or none: refused");
	whole = make_table(1, 0, 0);
	table[7] = '3';
	CHECK(!takes(whole), "another magic: refused");
	whole = make_table(1, 0, 0);
	memcpy(table + sizeof(struct ctab_head) + sizeof(struct ctab_mode) + 8, &(uint32_t){ 0x40000000 }, 4);
	CHECK(!takes(whole), "a power sequence that says it is longer than the file: refused");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "as_before", as_before }, { "pgpio_through_our_channel", pgpio_through_our_channel },
		{ "pgpio_only_anothers_channel", pgpio_only_anothers_channel }, { "pgpio_five_registers", pgpio_five_registers },
		{ "pgpio_dropped", pgpio_dropped }, { "pgpio_given_up", pgpio_given_up }, { "unknown_chip", unknown_chip }, { "no_such_clock", no_such_clock }, { "no_sensor", no_sensor },
		{ "clock_plan", clock_plan }, { "pm8008_whitelist", pm8008_whitelist },
		{ "camera_table", camera_table },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	board = "phone-video";
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			if (freopen("/dev/null", "w", stdout) == NULL) {
				return 2;
			}
			scenarios[i].run();
			fprintf(stderr, "%-24s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
