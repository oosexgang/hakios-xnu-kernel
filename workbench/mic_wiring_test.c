/*
 * Where the microphones' driver (kernel/overlay/bsd/dev/hakios_mic.c) takes the audio subsystem's blocks
 * and pins from. The driver is compiled as it is into a host program; the registers are memory that
 * keeps what is written, and nothing of the codec or of SoundWire is modelled. What is tested, on a
 * made-up SoC whose lpass { } is no real chip's: the SoundWire pins the driver sets up are the SoC file's
 * swr-tx-pins, their slew fields its swr-tx-slew, and no other pin is touched; the hardware control
 * register it clears a bit of and puts back is the one tx-swr-hctl names, in its page.
 *
 *   mic_wiring_test SCENARIO
 */
#include "fakekernel.h"

extern char log_text[];

/* ---- registers: memory that keeps what is written, and where writes went ------------------------------ */

#define CELLS   4096
static struct { uint64_t address; uint32_t value; } cells[CELLS];
static unsigned cell_count;
static uint64_t writes[CELLS];
static unsigned write_count;

static uint32_t *
cell(uint64_t address)
{
	for (unsigned i = 0; i < cell_count; i++) {
		if (cells[i].address == address) {
			return &cells[i].value;
		}
	}
	if (cell_count == CELLS) {
		abort();
	}
	cells[cell_count].address = address;
	return &cells[cell_count++].value;
}

uint32_t hakios_test_rd(vm_offset_t address) { return *cell(address); }

void
hakios_test_wr(vm_offset_t address, uint32_t value)
{
	*cell(address) = value;
	if (write_count < CELLS) {
		writes[write_count++] = address;
	}
}

static unsigned
written(uint64_t address)
{
	unsigned n = 0;

	for (unsigned i = 0; i < write_count; i++) {
		n += writes[i] == address;
	}
	return n;
}

vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }

#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#include "hakios_board.c"
#define get mic_get
#include "hakios_mic.c"
#undef get
#undef printf

/* (the audio DSP's driver, Darwin's audio device and the vibration motor's: these scenarios reach none of them) */
bool
hakios_adsp_apr(uint8_t service __unused, uint16_t port __unused, uint32_t token __unused, uint32_t opcode __unused,
    const void *payload __unused, uint32_t length __unused, uint32_t wait_ms __unused, struct hakios_apr_answer *answer __unused)
{
	return false;
}
void hakios_adsp_apr_events_second(hakios_apr_events_t handler __unused) { }
bool hakios_audio_dsp_memory(uintptr_t *memory __unused, uint64_t *dsp_address __unused, uint32_t *bytes __unused) { return false; }
void hakios_audio_dsp_memory_put(void) { }
bool hakios_audio_recording_mic(void) { return false; }
void hakios_audio_record_fed(const int16_t *frames __unused, uint32_t count __unused) { }
void hakios_haptic_pretend(uint32_t peak __unused) { }

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* the made-up SoC's (workbench/devices/phone-keys/soc.dtsi) */
#define LPI             0x043c0000u
#define LPI_SLEW        0x0455a000u
#define HCTL_REGISTER   0x042a90b0u

/* ---- scenarios ---------------------------------------------------------------------------------------- */

/* the SoundWire pins set up are the SoC file's, their slew fields its, and no other of the subsystem's pins */
static void
the_socs_pins(void)
{
	static const unsigned pins[] = { 3, 4, 5 }, fields[] = { 8, 10, 12 };

	blocks_from_board();
	*cell(LPI_SLEW) = 0xffffffff;
	CHECK(pins_on(), "set up: %s", log_text);
	for (unsigned pin = 0; pin < 16; pin++) {
		bool ours = pin >= 3 && pin <= 5;

		CHECK(written(LPI + 0x1000 * pin) == ours && written(LPI + 0x1000 * pin + 4) == ours, "pin %u: %u, %u", pin,
		    written(LPI + 0x1000 * pin), written(LPI + 0x1000 * pin + 4));
	}
	for (unsigned i = 0; i < 3; i++) {
		CHECK((*cell(LPI + 0x1000 * pins[i]) >> 2 & 0xf) == 1, "pin %u's function: %#x", pins[i], *cell(LPI + 0x1000 * pins[i]));
		CHECK((*cell(LPI_SLEW) >> fields[i] & 3) == 1, "pin %u's slew field at bit %u: %#x", pins[i], fields[i], *cell(LPI_SLEW));
	}
	CHECK((*cell(LPI_SLEW) & ~(0x3fu << 8)) == ~(0x3fu << 8), "no other slew field changed: %#x", *cell(LPI_SLEW));
	pins_restore();
	CHECK(*cell(LPI_SLEW) == 0xffffffff, "put back as found: %#x", *cell(LPI_SLEW));
}

/* the hardware control register is the one tx-swr-hctl names: its bit 1 cleared, and put back as found */
static void
the_socs_hctl(void)
{
	blocks_from_board();
	*cell(HCTL_REGISTER) = 0x3;
	(void)master_on();              /* (no link comes up: nothing answers on the bus) */
	CHECK(written(HCTL_REGISTER) == 1 && *cell(HCTL_REGISTER) == 0x1, "bit 1 cleared there: %u writes, %#x",
	    written(HCTL_REGISTER), *cell(HCTL_REGISTER));
	CHECK(written(HCTL_REGISTER & ~0xfffu) == 0 && written(HCTL_REGISTER - 0xb0 + 0xa8) == 0, "not at the page's start, "
	    "nor where an SM7325 has it");
	master_off();
	CHECK(written(HCTL_REGISTER) == 2 && *cell(HCTL_REGISTER) == 0x3, "put back as found: %#x", *cell(HCTL_REGISTER));
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "the_socs_pins", the_socs_pins }, { "the_socs_hctl", the_socs_hctl },
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
			fprintf(stderr, "%-24s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
