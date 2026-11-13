/*
 * Where the camera driver (kernel/overlay/bsd/dev/hakios_camera.c) takes its sensors from. The driver is
 * compiled as it is into a host program; nothing of the cameras is modelled. What is tested: each sensor
 * the driver works on is put together from the board's camera { } and its sensor@N nodes, with its chip's
 * ways and the SoC's clock registers for its MCLK and CSI receiver; for the test phone that has the first
 * phone's cameras that is, field for field, the table the driver used to carry; and a board that names a
 * chip or a clock the driver does not know, or no sensor, is refused with the reason.
 *
 *   camera_wiring_test SCENARIO
 */
#include "fakekernel.h"

extern char log_text[];

/* (what the driver links against and these scenarios never reach: no register is read or written) */
static unsigned accesses;
uint32_t hakios_test_rd(vm_offset_t address __unused) { accesses++; return 0; }
void hakios_test_wr(vm_offset_t address __unused, uint32_t value __unused) { accesses++; }
vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }
vm_offset_t ml_io_map_unmappable(vm_offset_t phys, vm_size_t size __unused, unsigned int flags __unused) { return phys; }
uint64_t kvtophys(vm_offset_t va) { return va; }
int kmem_alloc(void *map __unused, vm_offset_t *address __unused, vm_size_t size __unused, int flags __unused, int tag __unused) { return 1; }
void kmem_free(void *map __unused, vm_offset_t address __unused, vm_size_t size __unused) { }
const uint8_t *hakios_wifi_file(const char *name __unused, uint64_t *size __unused) { return NULL; }
void hakios_rpmh_lock(void) { }
void hakios_rpmh_unlock(void) { }
void hakios_spmi_lock(void) { }
void hakios_spmi_unlock(void) { }
struct fake_lock hakios_smmu_lock = { PTHREAD_MUTEX_INITIALIZER };

#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#include "hakios_board.c"
#include "hakios_camera.c"
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
	CHECK(hakios_board()->cameras.test_sensor == 0 && hakios_board()->cameras.ios_sensor == 1, "the macro tested, the wide for Darwin");
	CHECK(hakios_board()->cameras.ios_front == 2, "the front one as Darwin's front camera: %u", hakios_board()->cameras.ios_front);
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

/* camera { } without a sensor: nothing to work on, and said so */
static void
no_sensor(void)
{
	board_to_change()->cameras.sensor_count = 0;
	CHECK(!sensors_from_board() && sensor_count == 0, "refused");
	CHECK(logged("the board's camera { } names no sensor"), "said: %s", log_text);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "as_before", as_before }, { "unknown_chip", unknown_chip }, { "no_such_clock", no_such_clock }, { "no_sensor", no_sensor },
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
