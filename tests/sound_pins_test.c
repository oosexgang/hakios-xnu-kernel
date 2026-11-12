/*
 * Which pins the sound driver (kernel/overlay/bsd/dev/hakios_sound.c) sets up for the speakers' TDM
 * interface, and which it watches to see the interface's clock run. The driver is compiled as it is into
 * a host program; of the hardware only the pin controller's registers are modelled. What is tested: the
 * four pins it sets up are the board's speaker { } sound-pins, with the board's function; and the bit
 * clock and frame sync it watches are the first two of them -- on a board whose pins are not the first
 * phone's, too.
 *
 *   sound_pins_test SCENARIO
 */
#include "fakekernel.h"

extern char log_text[];

/* ---- the pin controller: what was written where, and two pins that toggle ---------------------------- */

#define TLMM_SIZE       0x100000u
static uint32_t pin_regs[TLMM_SIZE / 4];
static vm_offset_t tlmm_at;             /* where the driver mapped it (the fake maps a block at its address) */
static unsigned toggling[2] = { ~0u, ~0u }, reads[2];
static unsigned written_cfg[256], written_io[256];

uint32_t
hakios_test_rd(vm_offset_t address)
{
	uint32_t offset = (uint32_t)(address - tlmm_at);

	for (unsigned i = 0; i < 2; i++) {
		if (toggling[i] != ~0u && offset == 0x1000u * toggling[i] + 4) {
			return reads[i]++ & 1;          /* a pin that changes at every read */
		}
	}
	return offset < TLMM_SIZE ? pin_regs[offset / 4] : 0;
}

void
hakios_test_wr(vm_offset_t address, uint32_t value)
{
	uint32_t offset = (uint32_t)(address - tlmm_at);

	if (offset < TLMM_SIZE) {
		pin_regs[offset / 4] = value;
		if (offset % 0x1000 == 0) {
			written_cfg[offset / 0x1000]++;
		} else if (offset % 0x1000 == 4) {
			written_io[offset / 0x1000]++;
		}
	}
}

vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }

/* (what the driver links against and these scenarios never reach) */
uint64_t kvtophys(vm_offset_t va) { return va; }
int kmem_alloc(void *map __unused, vm_offset_t *address __unused, vm_size_t size __unused, int flags __unused, int tag __unused) { return 1; }
void kmem_free(void *map __unused, vm_offset_t address __unused, vm_size_t size __unused) { }
struct fake_lock hakios_smmu_lock = { PTHREAD_MUTEX_INITIALIZER };

#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#include "hakios_board.c"
#define get sound_get
#include "hakios_sound.c"
#undef get
#undef printf

/* (the audio DSP's driver, the amplifiers', the guard's and iOS's audio device: these scenarios reach none of them) */
bool
hakios_adsp_apr(uint8_t service __unused, uint16_t port __unused, uint32_t token __unused, uint32_t opcode __unused,
    const void *payload __unused, uint32_t length __unused, uint32_t wait_ms __unused, struct hakios_apr_answer *answer __unused)
{
	return false;
}
void hakios_adsp_apr_events(hakios_apr_events_t handler __unused) { }
const uint8_t *hakios_adsp_fw_file(const char *name __unused, uint64_t *size __unused) { return NULL; }
void hakios_adsp_fw_free(const uint8_t *data __unused, uint64_t size __unused) { }
bool hakios_speaker_prepare(const uint8_t *acf __unused, uint64_t size __unused) { return false; }
bool hakios_speaker_start(void) { return false; }
bool hakios_speaker_volume(uint32_t below __unused, uint32_t step_ms __unused) { return false; }
bool hakios_speaker_healthy(void) { return false; }
void hakios_speaker_stop(void) { }
void hakios_speaker_quiesce(void) { }
uint32_t hakios_speaker_monitor(bool tell __unused) { return 0; }
void hakios_guard_configure(bool hpf __unused, uint32_t gain __unused) { }
void hakios_guard_attenuation(uint32_t below __unused) { }
void hakios_guard_monitor(uint32_t below __unused) { }
void hakios_guard_process(int16_t *frames __unused, uint32_t count __unused) { }
void hakios_guard_take_report(struct hakios_guard_report *report) { memset(report, 0, sizeof(*report)); }
int hakios_guard_decibels(uint64_t ratio __unused) { return 0; }
void hakios_audio_set_sink(const char *name __unused, hakios_audio_sink_t sink __unused) { }
bool hakios_audio_volume(uint32_t *below __unused, bool *muted __unused, uint32_t *generation __unused) { return false; }
bool hakios_audio_recording_fed(void) { return false; }
void hakios_audio_record_fed(const int16_t *frames __unused, uint32_t count __unused) { }

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static void
start(void)
{
	tlmm.phys = hakios_board()->pins.tlmm;
	tlmm_at = tlmm.phys;
}

/* ---- scenarios ---------------------------------------------------------------------------------------- */

/* the four pins set up are the board's, each with the board's function, and no other pin is touched */
static void
the_boards_pins(void)
{
	const unsigned *wanted = (const unsigned[]){ 120, 121, 122, 123 };

	start();
	pins_on();
	for (unsigned pin = 0; pin < 256; pin++) {
		bool ours = pin >= wanted[0] && pin <= wanted[3];

		CHECK(written_cfg[pin] == (ours ? 1u : 0u) && written_io[pin] == (ours ? 1u : 0u), "pin %u: %u, %u", pin,
		    written_cfg[pin], written_io[pin]);
	}
	for (unsigned i = 0; i < 4; i++) {
		CHECK((pin_regs[0x1000 * wanted[i] / 4] >> 2 & 0xf) == 3, "pin %u's function: %#x", wanted[i], pin_regs[0x1000 * wanted[i] / 4]);
	}
	CHECK(logged("gpio120 (bit clock)") && logged("gpio121 (frame sync)"), "named by the board's numbers: %s", log_text);
}

/* the clock the driver watches is at the board's bit clock and frame sync pins: seen to run there ... */
static void
the_clock_watched_at_the_boards_pins(void)
{
	start();
	pins_on();
	toggling[0] = 120;
	toggling[1] = 121;
	CHECK(tdm_clock_runs(), "the clock runs at the board's pins: %s", log_text);
	CHECK(!logged("gpio97") && !logged("gpio100"), "not the first phone's pins: %s", log_text);
}

/* ... and not taken for running when only the first phone's pins toggle */
static void
not_at_the_first_phones(void)
{
	start();
	pins_on();
	toggling[0] = 97;
	toggling[1] = 100;
	CHECK(!tdm_clock_runs(), "the board's pins are still: %s", log_text);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "the_boards_pins", the_boards_pins }, { "the_clock_watched_at_the_boards_pins", the_clock_watched_at_the_boards_pins },
		{ "not_at_the_first_phones", not_at_the_first_phones },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	board = "phone-keys";
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			if (freopen("/dev/null", "w", stdout) == NULL) {
				return 2;
			}
			scenarios[i].run();
			fprintf(stderr, "%-36s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
