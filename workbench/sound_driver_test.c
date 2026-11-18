/*
 * Which pins the sound driver (drivers/board/sound.c) sets up for the speakers' TDM
 * interface, which it watches to see the interface's clock run, and how it gives the audio DSP its view of
 * the buffers through the apps SMMU. The driver is compiled as it is into a host program; of the hardware
 * the pin controller's registers and the SMMU's are modelled. What is tested: the four pins it sets up are
 * the board's speaker { } sound-pins, with the board's function; the bit clock and frame sync it watches
 * are the first two of them -- on a board whose pins are not the first phone's, too; a free match register
 * and context of the SMMU are found and set up with page tables of the buffers, and put back as found;
 * what the boot chain set up for somebody else is left alone, and so is what somebody took after it was put back;
 * once the amplifiers are given up on, none of what Darwin plays goes to the DSP; a period whose write could not
 * be sent is written again; and the amplifiers are neither stopped in the middle of the sound by a write that
 * comes as the watcher looks, nor turned up by Darwin turning its volume down.
 *
 *   sound_driver_test SCENARIO
 */
#include <unistd.h>

#include "fakekernel.h"

extern char log_text[];

/* ---- the pin controller: what was written where, and two pins that toggle ---------------------------- */

#define TLMM_SIZE       0x100000u
static uint32_t pin_regs[TLMM_SIZE / 4];
static vm_offset_t tlmm_at;             /* where the driver mapped it (the fake maps a block at its address) */
static unsigned toggling[2] = { ~0u, ~0u }, reads[2];
static unsigned written_cfg[256], written_io[256];

/* ---- the apps SMMU (the made-up SoC's smmu { } apps): a megabyte of registers, 128 pages of 4K each half ---- */

#define SMMU_AT         0x16000000u
#define SMMU_BYTES      0x100000u
#define CONTEXT_AT(n)   (SMMU_AT + (128u + (n)) * 0x1000u)
static uint32_t smmu_regs[SMMU_BYTES / 4];
static unsigned smmu_writes, tlb_looks;
static bool tlb_stuck;                  /* a context's TLB sync that never ends */
static bool smr_deaf;                   /* the match registers keep none of what is written */

static uint32_t *
smmu(uint32_t address)
{
	return &smmu_regs[(address - SMMU_AT) / 4];
}

uint32_t
hakios_test_rd(vm_offset_t address)
{
	uint32_t offset = (uint32_t)(address - tlmm_at);

	if (address >= SMMU_AT && address < SMMU_AT + SMMU_BYTES) {
		if (address >= CONTEXT_AT(0) && (address & 0xfff) == 0x7f4) {        /* (SMMU_CB_TLBSTATUS) */
			tlb_looks++;
			return tlb_stuck;
		}
		return *smmu((uint32_t)address);
	}
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

	if (address >= SMMU_AT && address < SMMU_AT + SMMU_BYTES) {
		smmu_writes++;
		if (address >= CONTEXT_AT(0) && (address & 0xfff) == 0x58) {       /* (a context's FSR: what is written is cleared) */
			*smmu((uint32_t)address) &= ~value;
		} else if (smr_deaf && address >= SMMU_AT + 0x800 && address < SMMU_AT + 0xc00) {
			/* (kept nothing) */
		} else {
			*smmu((uint32_t)address) = value;
		}
		return;
	}
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

/* memory the driver asks for: pages of the host's, at the addresses they have here */
uint64_t kvtophys(vm_offset_t va) { return va; }

int
kmem_alloc(void *map __unused, vm_offset_t *address, vm_size_t size, int flags __unused, int tag __unused)
{
	void *memory = NULL;

	if (posix_memalign(&memory, 4096, size) != 0) {
		return 1;
	}
	memset(memory, 0, size);
	*address = (vm_offset_t)memory;
	return 0;
}

void kmem_free(void *map __unused, vm_offset_t address __unused, vm_size_t size __unused) { }
struct fake_lock hakios_smmu_lock = { PTHREAD_MUTEX_INITIALIZER };

#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#include "board/board.c"
#define get sound_get
#include "board/sound.c"
#undef get
#undef printf

/* (the audio DSP's driver, the amplifiers', the guard's and Darwin's audio device: these scenarios reach none of them) */
/* the audio DSP: answers as a scenario puts them (none put: "done"), and what it was asked */
enum dsp_answer { DSP_RESULT, DSP_SILENT, DSP_OWN_ANSWER, DSP_ANOTHER_RESULT };
static struct { enum dsp_answer how; uint32_t status; } dsp_script[16];
static unsigned dsp_put, dsp_asked;
static struct { uint8_t service; uint16_t port; uint32_t opcode; } dsp_heard[16];

static void
dsp_will(enum dsp_answer how, uint32_t status)
{
	dsp_script[dsp_put].how = how;
	dsp_script[dsp_put].status = status;
	dsp_put++;
}

/* a period's write (not waited for): sent, unless a scenario says the link takes none now; both counted */
static volatile bool writes_refused;
static volatile unsigned writes_tried, writes_sent;

bool
hakios_adsp_apr(uint8_t service, uint16_t port, uint32_t token __unused, uint32_t opcode, const void *payload __unused,
    uint32_t length __unused, uint32_t wait_ms __unused, struct hakios_apr_answer *answer)
{
	unsigned n;

	if (opcode == ASM_DATA_CMD_WRITE_V2) {
		writes_tried++;
		writes_sent += !writes_refused;
		return !writes_refused;
	}
	n = dsp_asked++ % 16;

	dsp_heard[n].service = service;
	dsp_heard[n].port = port;
	dsp_heard[n].opcode = opcode;
	memset(answer, 0, sizeof(*answer));
	switch (dsp_script[n].how) {
	case DSP_SILENT:
		return false;
	case DSP_OWN_ANSWER:
		answer->opcode = opcode + 1;
		answer->length = 4;
		return true;
	case DSP_RESULT:
	case DSP_ANOTHER_RESULT:
		answer->opcode = APR_BASIC_RSP_RESULT;
		answer->length = 8;
		answer->words[0] = dsp_script[n].how == DSP_ANOTHER_RESULT ? opcode + 1 : opcode;
		answer->words[1] = dsp_script[n].status;
		return true;
	}
	return false;
}
void hakios_adsp_apr_events(hakios_apr_events_t handler __unused) { }
const uint8_t *hakios_adsp_fw_file(const char *name __unused, uint64_t *size __unused) { return NULL; }
void hakios_adsp_fw_free(const uint8_t *data __unused, uint64_t size __unused) { }
/*
 * the amplifiers never start, unless a scenario says they play (then they start, take any volume and stay healthy); a
 * start can be held until a scenario lets it go on, the stops are counted, and the least level under Android's asked for kept
 */
static volatile bool start_held, in_guard, speakers_play;
static volatile unsigned speaker_stops;
static volatile uint32_t least_asked = UINT32_MAX;
bool hakios_speaker_prepare(const uint8_t *acf __unused, uint64_t size __unused) { return false; }
bool
hakios_speaker_start(void)
{
	for (int ms = 0; start_held && !in_guard && ms < 5000; ms++) {
		usleep(1000);
	}
	return speakers_play;
}
bool
hakios_speaker_volume(uint32_t level_below, uint32_t step_ms __unused)
{
	if (level_below < least_asked) {
		least_asked = level_below;
	}
	return speakers_play;
}
bool hakios_speaker_healthy(void) { return speakers_play; }
void hakios_speaker_stop(void) { speaker_stops++; }
void hakios_speaker_quiesce(void) { }
uint32_t hakios_speaker_monitor(bool tell __unused) { return 0; }
void hakios_guard_configure(bool hpf __unused, uint32_t gain __unused) { }
/* the level the guard was last told; a period in the guard can be held there until the guard is told it is silent */
static volatile uint32_t guard_attenuation;
static bool guard_holds;
void hakios_guard_attenuation(uint32_t below) { guard_attenuation = below; }
void hakios_guard_monitor(uint32_t below __unused) { }
void
hakios_guard_process(int16_t *frames __unused, uint32_t count __unused)
{
	in_guard = true;
	for (int ms = 0; guard_holds && guard_attenuation != HAKIOS_GUARD_SILENT && ms < 5000; ms++) {
		usleep(1000);
	}
}
/* the guard's report: as many blocks as a scenario says it has worked on since it was last taken */
static uint32_t guard_blocks;
void hakios_guard_take_report(struct hakios_guard_report *report) { memset(report, 0, sizeof(*report)); report->blocks = guard_blocks; guard_blocks = 0; }
int hakios_guard_decibels(uint64_t ratio __unused) { return 0; }
void hakios_audio_set_sink(const char *name __unused, hakios_audio_sink_t sink __unused) { }
/* the volume Darwin has set (HAKIOS_AUDIO_SET_VOLUME): none, unless a scenario sets one */
static volatile uint32_t darwin_below, darwin_generation;
bool
hakios_audio_volume(uint32_t *level_below, bool *muted, uint32_t *generation)
{
	*level_below = darwin_below;
	*muted = false;
	*generation = darwin_generation;
	return darwin_generation != 0;
}
/* what is played, fed to a recording (board/audio.c) while a scenario says one is fed */
static bool recording;
static unsigned recorded_calls, recorded_frames;
bool hakios_audio_recording_fed(void) { return recording; }
void hakios_audio_record_fed(const int16_t *frames __unused, uint32_t count) { recorded_calls++; recorded_frames += count; }

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited = 0; !(cond) && waited < (ms); waited += 5) usleep(5000); } while (0)

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

/* ... nor when the bit clock runs and the frame sync does not */
static void
no_frames(void)
{
	start();
	pins_on();
	toggling[0] = 120;
	CHECK(!tdm_clock_runs(), "a bit clock without frames is no interface running: %s", log_text);
}

/* ---- the SMMU: the DSP's view of the buffers ---------------------------------------------------------------- */

#define AUDIO           0x1801u         /* the made-up SoC's audio-stream */

/* an SMMU of 16 match registers and 8 contexts, of which the boot chain has used two of each for other streams */
static void
an_smmu(void)
{
	start();
	apps_smmu.phys = hakios_board()->smmu.apps;
	*smmu(SMMU_AT + SMMU_IDR0) = 16;
	*smmu(SMMU_AT + SMMU_IDR1) = 6u << 28 | 8;
	*smmu(SMMU_AT + SMMU_SMR(0)) = 1u << 31 | 0x0100;
	*smmu(SMMU_AT + SMMU_S2CR(0)) = 0;
	*smmu(SMMU_AT + SMMU_SMR(1)) = 1u << 31 | 0x0200;
	*smmu(SMMU_AT + SMMU_S2CR(1)) = 1;
	*smmu(SMMU_AT + SMMU_SMR(2)) = 0x0333;          /* (not valid: free, whatever it says) */
	*smmu(SMMU_AT + SMMU_S2CR(2)) = 0x0444;
}

static bool
memory(uintptr_t *at, uint64_t *dsp, uint32_t *bytes)
{
	return hakios_audio_dsp_memory(at, dsp, bytes);
}

/* nothing set up for the stream: the first free match register and the first unused context are ours, with
 * page tables that put our buffers at the DSP's addresses; given back, all is as it was */
static void
smmu_set_up(void)
{
	uintptr_t at = 0;
	uint64_t dsp = 0, ttbr, *root, *second, *third;
	uint32_t bytes = 0;

	an_smmu();
	CHECK(memory(&at, &dsp, &bytes), "given: %s", log_text);
	CHECK(*smmu(SMMU_AT + SMMU_SMR(2)) == (1u << 31 | AUDIO) && *smmu(SMMU_AT + SMMU_S2CR(2)) == 2,
	    "match register 2 sends the stream to context 2: %#x %#x", *smmu(SMMU_AT + SMMU_SMR(2)), *smmu(SMMU_AT + SMMU_S2CR(2)));
	CHECK((*smmu(CONTEXT_AT(2) + SMMU_CB_SCTLR) & 1) && *smmu(CONTEXT_AT(2) + SMMU_CB_TCR) == SMMU_TCR_39_BITS,
	    "context 2 on, of 39 bits: %#x", *smmu(CONTEXT_AT(2) + SMMU_CB_SCTLR));
	ttbr = (uint64_t)*smmu(CONTEXT_AT(2) + SMMU_CB_TTBR0 + 4) << 32 | *smmu(CONTEXT_AT(2) + SMMU_CB_TTBR0);
	CHECK(ttbr >> 48 == 2, "its tables tagged with its number: %#llx", (unsigned long long)ttbr);
	root = (uint64_t *)(uintptr_t)(ttbr & PTE_ADDRESS);
	second = (uint64_t *)(uintptr_t)(root[0] & PTE_ADDRESS);
	third = (uint64_t *)(uintptr_t)(second[0x80] & PTE_ADDRESS);
	CHECK((root[0] & 3) == 3 && (second[0x80] & 3) == 3, "0x10000000 through the root's first and the second's 128th");
	for (unsigned page = 0; page < SHARED_BYTES / 4096; page++) {
		CHECK(third[page] == ((uint64_t)(buffer + 4096 * page) | AUDIO_PTE), "page %u", page);
	}
	CHECK(third[SHARED_BYTES / 4096] == 0, "and no page past the buffers");
	CHECK(buffer != 0 && at == buffer + BUFFER_BYTES && dsp == (AUDIO_ADDRESSES | 1ull << 32) + BUFFER_BYTES && bytes == MIC_BYTES,
	    "the microphones' part, as the DSP sees it: %#llx", (unsigned long long)dsp);
	hakios_audio_dsp_memory_put();
	CHECK(*smmu(SMMU_AT + SMMU_SMR(2)) == 0x0333 && *smmu(SMMU_AT + SMMU_S2CR(2)) == 0x0444 &&
	    *smmu(CONTEXT_AT(2) + SMMU_CB_SCTLR) == 0, "put back as found");
	CHECK(logged("the SMMU has refused the DSP nothing"), "and nothing was refused: %s", log_text);
	/* a second user after the last has left (a recording after the sound): the same tables, not new ones every time */
	CHECK(memory(&at, &dsp, &bytes), "given again");
	CHECK(((uint64_t)*smmu(CONTEXT_AT(2) + SMMU_CB_TTBR0 + 4) << 32 | *smmu(CONTEXT_AT(2) + SMMU_CB_TTBR0)) == ttbr,
	    "the same tables: %#x", *smmu(CONTEXT_AT(2) + SMMU_CB_TTBR0));
	hakios_audio_dsp_memory_put();
}

/* the boot chain lets the stream through untranslated already (a match register whose mask covers it): used as it is */
static void
smmu_let_through(void)
{
	uintptr_t at = 0;
	uint64_t dsp = 0;
	uint32_t bytes = 0;

	an_smmu();
	*smmu(SMMU_AT + SMMU_SMR(1)) = 1u << 31 | 0x1u << 16 | 0x1800;       /* 0x1800 and 0x1801 ... */
	*smmu(SMMU_AT + SMMU_S2CR(1)) = 1u << 16;                               /* ... let through */
	smmu_writes = 0;
	CHECK(memory(&at, &dsp, &bytes), "given: %s", log_text);
	CHECK(dsp == ((uint64_t)buffer | 1ull << 32) + BUFFER_BYTES, "at its own address: %#llx", (unsigned long long)dsp);
	CHECK(smmu_writes == 0, "nothing written: %u", smmu_writes);
}

/* the SMMU's state that is somebody else's: left alone, and no memory for the DSP */
static void
smmu_left_alone(void)
{
	const char *which = getenv("SOUND_CASE");
	unsigned i = which ? (unsigned)atoi(which) : 0;
	uintptr_t at;
	uint64_t dsp;
	uint32_t bytes;
	static const char *const said[] = {
		"does not let it through untranslated", "numbers its streams with 16 bits", "not a megabyte",
		"no match register or no context left", "no match register or no context left", "IS SWITCHED ON",
		"does not finish forgetting old translations", "DID NOT TAKE WHAT WAS WRITTEN",
	};

	an_smmu();
	switch (i) {
	case 0: *smmu(SMMU_AT + SMMU_SMR(0)) = 1u << 31 | AUDIO; break;         /* translated, through somebody's context */
	case 1: *smmu(SMMU_AT + SMMU_SCR0) = 1u << 3; break;
	case 2: *smmu(SMMU_AT + SMMU_IDR1) = 7u << 28 | 8; break;
	case 3: for (unsigned n = 2; n < 16; n++) { *smmu(SMMU_AT + SMMU_SMR(n)) = 1u << 31 | n; } break;
	case 4: *smmu(SMMU_AT + SMMU_IDR1) = 6u << 28 | 2; break;                /* two contexts, both used */
	case 5: *smmu(CONTEXT_AT(2) + SMMU_CB_SCTLR) = 1; break;
	case 6: tlb_stuck = true; break;
	default: smr_deaf = true; break;
	}
	smmu_writes = 0;
	CHECK(!memory(&at, &dsp, &bytes), "case %u: none: %s", i, log_text);
	CHECK(logged(said[i]), "case %u: said: %s", i, log_text);
	CHECK(i >= 6 ? *smmu(SMMU_AT + SMMU_SMR(2)) == 0x0333 : smmu_writes == 0, "case %u: nothing of it changed (%u written)", i,
	    smmu_writes);
	CHECK(i != 6 || tlb_looks == 101, "a hundred milliseconds of looks, then given up: %u", tlb_looks);
	CHECK(i != 7 || (*smmu(SMMU_AT + SMMU_S2CR(2)) == 0x0444 && *smmu(CONTEXT_AT(2) + SMMU_CB_SCTLR) == 0),
	    "what was written put back, not left programmed with nobody using it: s2cr %#x, sctlr %#x",
	    *smmu(SMMU_AT + SMMU_S2CR(2)), *smmu(CONTEXT_AT(2) + SMMU_CB_SCTLR));
}

/* what the SMMU refused the DSP while it was on is said when it is put back */
static void
smmu_faults_said(void)
{
	uintptr_t at;
	uint64_t dsp;
	uint32_t bytes;

	an_smmu();
	CHECK(memory(&at, &dsp, &bytes), "given");
	*smmu(CONTEXT_AT(2) + SMMU_CB_FSR) = 1u << 1;
	*smmu(CONTEXT_AT(2) + SMMU_CB_FAR) = 0x10008000;
	hakios_audio_dsp_memory_put();
	CHECK(logged("THE SMMU HAS REFUSED A REQUEST: context 2: status 0x2, address 0x10008000"), "said: %s", log_text);
}

/* ---- the DSP's events about the stream -------------------------------------------------------------------- */

/* a write's "done" gives its period back, with an error if its status says one; a refused write too; underflows
 * counted; and what is not the stream's, or not about a write of ours, is somebody else's */
static void
the_streams_events(void)
{
	const uint32_t ok[4] = { 0x1000, 0, 3840, 0 }, failed[4] = { 0x1000, 0, 3840, 2 };

	CHECK(!events(APR_SVC_AFE, 0, ASM_DATA_EVENT_WRITE_DONE_V2, WRITE_TOKEN | 1, ok, 16), "another service's: not taken");
	CHECK(!events(APR_SVC_ASM, 0, ASM_DATA_EVENT_WRITE_DONE_V2, 1, ok, 16), "a write's done without our token: not taken");
	CHECK(written_done == 0 && returned == 0, "nothing counted");
	CHECK(events(APR_SVC_ASM, 0, ASM_DATA_EVENT_WRITE_DONE_V2, WRITE_TOKEN | 3, ok, 16), "period 3 done");
	CHECK(written_done == 1 && returned == 1u << 3 && write_errors == 0, "given back, no error: %#x", returned);
	CHECK(events(APR_SVC_ASM, 0, ASM_DATA_EVENT_WRITE_DONE_V2, WRITE_TOKEN | 5, failed, 16), "period 5 done, badly");
	CHECK(written_done == 2 && returned == (1u << 3 | 1u << 5) && write_errors == 1, "given back, an error: %u", write_errors);
	CHECK(events(APR_SVC_ASM, 0, ASM_DATA_EVENT_WRITE_DONE_V2, WRITE_TOKEN | 6, failed, 12), "a short one");
	CHECK(write_errors == 1 && (returned & 1u << 6), "its status, not there, is no error: %u", write_errors);
	CHECK(events(APR_SVC_ASM, 0, APR_BASIC_RSP_RESULT, WRITE_TOKEN | 7, failed, 8) && write_errors == 2 && (returned & 1u << 7),
	    "a write refused outright: an error, and its period back");
	CHECK(!events(APR_SVC_ASM, 0, APR_BASIC_RSP_RESULT, 7, failed, 8) && write_errors == 2, "a command's answer is not ours");
	CHECK(events(APR_SVC_ASM, 0, ASM_SESSION_EVENT_RX_UNDERFLOW, 0, ok, 0) && underflows == 1, "an underflow");
	CHECK(!events(APR_SVC_ASM, 0, 0x12345, WRITE_TOKEN, ok, 16) && underflows == 1 && written_done == 4, "anything else: not taken");
}

/* ---- the DSP's answers to the driver's commands ------------------------------------------------------- */

/* done, refused by name, not answered, answered for another command; its own answer when one is waited for */
static void
the_dsps_answers(void)
{
	dsp_will(DSP_RESULT, 0);
	CHECK(command("a command", APR_SVC_ASM, 0x12, 0x100, NULL, 0, 0, NULL) && dsp_heard[0].service == APR_SVC_ASM &&
	    dsp_heard[0].port == 0x12, "done, to the service and port asked");
	CHECK(logged("sound: a command: done"), "said: %s", log_text);
	dsp_will(DSP_RESULT, 13);
	CHECK(!command("a refused one", APR_SVC_AFE, 0, 0x101, NULL, 0, 0, NULL) && logged("a refused one: REFUSED: aborted"),
	    "refused by name: %s", log_text);
	dsp_will(DSP_SILENT, 0);
	CHECK(!command("unanswered", APR_SVC_AFE, 0, 0x102, NULL, 0, 0, NULL) && logged("unanswered (opcode 0x102 to service 4, port 0): NO ANSWER"),
	    "no answer: %s", log_text);
	dsp_will(DSP_ANOTHER_RESULT, 0);
	CHECK(!command("for another", APR_SVC_AFE, 0, 0x103, NULL, 0, 0, NULL) && logged("for another: an answer not expected"),
	    "a result for another command: %s", log_text);
	dsp_will(DSP_OWN_ANSWER, 0);
	CHECK(command("asked", APR_SVC_ADM, 0, 0x200, NULL, 0, 0x201, NULL), "its own answer, waited for");
	dsp_will(DSP_RESULT, 0);
	CHECK(!command("asked again", APR_SVC_ADM, 0, 0x200, NULL, 0, 0x201, NULL), "a good result is not the answer waited for");
	dsp_will(DSP_OWN_ANSWER, 0);
	CHECK(!command("not waited for", APR_SVC_ADM, 0, 0x200, NULL, 0, 0, NULL), "nor an answer to a command that waits for none");
	dsp_will(DSP_OWN_ANSWER, 0);
	CHECK(!command("another's", APR_SVC_ADM, 0, 0x200, NULL, 0, 0x300, NULL), "nor another command's answer");
}

/* sound=0, or none: the DSP's coming up starts nothing and says nothing */
static void
said_off(void)
{
	boot_args = getenv("SOUND_CASE")[0] == '0' ? "sound=0" : "";
	hakios_sound_dsp_up();
	CHECK(!logged("sound:") && level == 0, "nothing said or kept: %s", log_text);
	CHECK(tlmm.phys == 0 && apps_smmu.phys == 0, "nothing looked for: the pins at %#llx, the SMMU at %#llx",
	    (unsigned long long)tlmm.phys, (unsigned long long)apps_smmu.phys);
}

/* a board whose board.dts lists no amplifiers: what the DSP would be told is not for it */
static void
no_amplifiers(void)
{
	board = "phone-cmd";
	boot_args = "sound=1";
	hakios_sound_dsp_up();
	CHECK(logged("sound: disabled: this board's board.dts lists no \"speaker-aw88261\"") && level == 0, "said: %s", log_text);
}

/*
 * What Darwin writes, through the ring to the periods the DSP plays: each frame's left and right in their
 * places and in order, its loudest sample kept, silence (no more than 8) not taken for sound, no more taken
 * than there is room for, and the indices running on past 2^32.
 */
static void
darwins_frames_through_the_ring(void)
{
	static int16_t periods[4096 * 2 / 2] __attribute__((aligned(4096)));
	const int16_t frames[] = { 100, -200, 300, 5, -5, 7, 30000, -31000 }, quiet[] = { 8, -8, 0, 3 };
	const int16_t *samples = periods;
	bool in_order = true;

	buffer = (vm_offset_t)periods;
	CHECK(darwin_sink(frames, 4) == 4 && ring_head == 4, "four frames taken: head %u", ring_head);
	CHECK(darwin_peak == 31000, "the loudest sample kept: %u", darwin_peak);
	CHECK(last_sound_us != 0, "sound heard");
	period_fill_darwin(0);
	for (unsigned i = 0; i < 8; i++) {
		in_order &= samples[i] == frames[i];
	}
	CHECK(in_order, "the period has them as written: %d %d %d %d", samples[0], samples[1], samples[2], samples[3]);
	CHECK(samples[8] == 0 && samples[2 * PERIOD_FRAMES - 1] == 0, "the rest of the period silence");
	CHECK(ring_tail == 4 && fed_frames == 4 && silent_frames == PERIOD_FRAMES - 4, "tail %u, fed %llu, silent %llu",
	    ring_tail, (unsigned long long)fed_frames, (unsigned long long)silent_frames);

	last_sound_us = 0;
	darwin_peak = 0;
	CHECK(darwin_sink(quiet, 2) == 2 && last_sound_us == 0, "8 and less is silence, not sound");
	darwin_peak = 200;
	CHECK(darwin_sink(frames + 2, 1) == 1 && darwin_peak == 300, "a louder left alone raises the peak: %u", darwin_peak);

	ring_tail = ring_head = 0u - 2;         /* (two frames before the indices wrap) */
	CHECK(darwin_sink(frames, 4) == 4 && ring_head == 2, "taken across the wrap: head %u", ring_head);
	period_fill_darwin(1);
	in_order = true;
	for (unsigned i = 0; i < 8; i++) {
		in_order &= periods[4096 / 2 + i] == frames[i];      /* (a period every 4096 bytes) */
	}
	CHECK(in_order && ring_tail == 2, "and given out in order across it");

	ring_head = ring_tail + RING_FRAMES - 2;
	CHECK(darwin_sink(frames, 4) == 2 && ring_head == ring_tail + RING_FRAMES, "no more than the room: head %u", ring_head);
}

/* the tone the first level plays: the same sample on the left and on the right, every frame; silence all zeros */
static void
the_tone_in_both_channels(void)
{
	static int16_t periods[4096 * 2 / 2] __attribute__((aligned(4096)));
	bool both = true, silent = true;

	buffer = (vm_offset_t)periods;
	tone_at = 0;
	memset(periods, 0x55, sizeof(periods));
	period_fill(0, false);
	period_fill(1, true);
	for (unsigned f = 0; f < PERIOD_FRAMES; f++) {
		both &= periods[2 * f] == tone[f % 48] && periods[2 * f + 1] == tone[f % 48];
		silent &= periods[4096 / 2 + 2 * f] == 0 && periods[4096 / 2 + 2 * f + 1] == 0;
	}
	CHECK(both, "the tone in both channels: %d %d, %d %d", periods[2], periods[3], periods[4], periods[5]);
	CHECK(silent, "silence in both");
}

/* sound in one channel is sound */
static void
sound_in_one_channel(void)
{
	const int16_t right_only[] = { 0, 100 }, left_only[] = { -100, 0 };

	last_sound_us = 0;
	CHECK(darwin_sink(right_only, 1) == 1 && last_sound_us != 0, "the right alone");
	last_sound_us = 0;
	CHECK(darwin_sink(left_only, 1) == 1 && last_sound_us != 0, "the left alone");
}

/* tenths of a dB as the log says them */
static void
decibels_as_text(void)
{
	char t[16];

	CHECK(strcmp(db_text(t, 0), "0.0") == 0 && strcmp(db_text(t, -57), "-5.7") == 0 && strcmp(db_text(t, 31), "3.1") == 0 &&
	    strcmp(db_text(t, -3), "-0.3") == 0, "%s", db_text(t, 0));
}

/* the guard's line: said when it has worked on something, and not when quiet or when it has not */
static void
the_guards_line(void)
{
	guard_blocks = 5;
	guard_say(", 5 s", true);
	CHECK(!logged("sound: the guard"), "not when quiet: %s", log_text);
	guard_blocks = 5;
	guard_say(", 5 s", false);
	CHECK(logged("sound: the guard, 5 s: peaks in 0, out 0") && logged("of 5 periods"), "said: %s", log_text);
	guard_say(" until the amplifiers stopped", false);
	CHECK(!logged("until the amplifiers stopped"), "not for nothing: %s", log_text);
}

/* a recording fed what is played: Darwin's frames when there are any, not a period of silence */
static void
recording_fed_what_is_played(void)
{
	static int16_t periods[4096 * 2 / 2] __attribute__((aligned(4096)));
	const int16_t frames[] = { 100, -200, 300, 5 };

	buffer = (vm_offset_t)periods;
	recording = true;
	period_fill_darwin(0);
	CHECK(recorded_calls == 0, "nothing for an empty ring: %u", recorded_calls);
	CHECK(darwin_sink(frames, 2) == 2, "taken");
	period_fill_darwin(1);
	CHECK(recorded_calls == 1 && recorded_frames == 2, "Darwin's two frames: %u calls, %u frames", recorded_calls, recorded_frames);
	recording = false;
	CHECK(darwin_sink(frames, 2) == 2, "taken");
	period_fill_darwin(0);
	CHECK(recorded_calls == 1, "and none without a recording: %u", recorded_calls);
}

/* the watcher of sound=4 (what starts and stops the amplifiers), on a thread of its own as on the sound's */
static void *
watcher(void *unused __unused)
{
	watch();
	return NULL;
}

static bool
all_silence(const int16_t *samples)
{
	for (unsigned i = 0; i < 2 * PERIOD_FRAMES; i++) {
		if (samples[i] != 0) {
			return false;
		}
	}
	return true;
}

/*
 * The amplifiers do not start when Darwin plays, and are given up on: from then on none of what Darwin writes
 * goes to the DSP -- silence in its place, Darwin's frames taken from the ring all the same -- and the
 * amplifiers' stop is tried again (a power-down that did not read back is retried there).
 */
static void
given_up_darwin_not_fed(void)
{
	static int16_t periods[4096 * 2 / 2] __attribute__((aligned(4096)));
	const int16_t frames[] = { 1000, -1000, 2000, -2000, 3000, -3000 };
	pthread_t thread;

	buffer = (vm_offset_t)periods;
	CHECK(darwin_sink(frames, 3) == 3 && last_sound_us != 0, "Darwin plays");
	pthread_create(&thread, NULL, watcher, NULL);
	WAIT_FOR(logged("THE AMPLIFIERS DID NOT START") && guard_attenuation == HAKIOS_GUARD_SILENT, 5000);
	CHECK(logged("THE AMPLIFIERS DID NOT START") && guard_attenuation == HAKIOS_GUARD_SILENT, "given up on: %s", log_text);
	CHECK(darwin_sink(frames, 3) == 3, "Darwin goes on playing");
	memset(periods, 0x55, sizeof(periods));
	period_fill_darwin(0);
	CHECK(all_silence(periods), "the DSP is given silence, not Darwin's sound: %d %d %d", periods[0], periods[1], periods[2]);
	CHECK(ring_tail == ring_head, "Darwin's frames taken from the ring all the same: tail %u, head %u", ring_tail, ring_head);
	CHECK(fed_frames == 0, "none fed: %llu", (unsigned long long)fed_frames);
	WAIT_FOR(speaker_stops >= 2, 2000);
	CHECK(speaker_stops >= 2, "the amplifiers' power-down tried again while given up on: %u stops", speaker_stops);
	stopping = true;
	pthread_join(thread, NULL);
}

/*
 * Given up on while a period of Darwin's is in the guard: that period may have gone through it as silent -- held back
 * by nothing -- and is not sent as it is, but as silence.
 */
static void
given_up_while_a_period_is_guarded(void)
{
	static int16_t periods[4096 * 2 / 2] __attribute__((aligned(4096)));
	const int16_t frames[] = { 1000, -1000, 2000, -2000, 3000, -3000 };
	pthread_t thread;

	buffer = (vm_offset_t)periods;
	start_held = true;                      /* (the start goes on, and fails, once the period is in the guard) */
	guard_holds = true;                     /* (... which it leaves once it is told the amplifiers are given up on) */
	CHECK(darwin_sink(frames, 3) == 3 && last_sound_us != 0, "Darwin plays");
	pthread_create(&thread, NULL, watcher, NULL);
	memset(periods, 0x55, sizeof(periods));
	period_fill_darwin(0);
	CHECK(guard_attenuation == HAKIOS_GUARD_SILENT, "(the guard was told the amplifiers were given up on while it had the period)");
	CHECK(all_silence(periods), "that period is silence: %d %d %d", periods[0], periods[1], periods[2]);
	CHECK(fed_frames == 0, "none fed: %llu", (unsigned long long)fed_frames);
	stopping = true;
	pthread_join(thread, NULL);
}

/*
 * The match register and the context, put back when the last user left, are taken by somebody else before the next
 * user comes (the Wi-Fi's start, board/wifi.c, finds them free): that one looks again, takes the next free ones, and
 * leaves the Wi-Fi's alone -- its match register, its context's registers, through a mapping of the right context.
 */
static void
smmu_taken_meanwhile(void)
{
	uintptr_t at;
	uint64_t dsp;
	uint32_t bytes;

	an_smmu();
	CHECK(memory(&at, &dsp, &bytes) && *smmu(SMMU_AT + SMMU_SMR(2)) == (1u << 31 | AUDIO), "given, through match register 2");
	hakios_audio_dsp_memory_put();
	CHECK(*smmu(SMMU_AT + SMMU_SMR(2)) == 0x0333 && *smmu(CONTEXT_AT(2) + SMMU_CB_SCTLR) == 0, "put back as found");
	/* the Wi-Fi's start: its stream through match register 2 and context 2, on, with its own tables */
	*smmu(SMMU_AT + SMMU_SMR(2)) = 1u << 31 | 0x0c80;
	*smmu(SMMU_AT + SMMU_S2CR(2)) = 2;
	*smmu(CONTEXT_AT(2) + SMMU_CB_TTBR0) = 0xabc000;
	*smmu(CONTEXT_AT(2) + SMMU_CB_SCTLR) = SMMU_SCTLR_ON;
	CHECK(memory(&at, &dsp, &bytes), "given again: %s", log_text);
	CHECK(*smmu(SMMU_AT + SMMU_SMR(2)) == (1u << 31 | 0x0c80) && *smmu(SMMU_AT + SMMU_S2CR(2)) == 2,
	    "the Wi-Fi's match register left alone: %#x %#x", *smmu(SMMU_AT + SMMU_SMR(2)), *smmu(SMMU_AT + SMMU_S2CR(2)));
	CHECK(*smmu(CONTEXT_AT(2) + SMMU_CB_TTBR0) == 0xabc000 && *smmu(CONTEXT_AT(2) + SMMU_CB_SCTLR) == SMMU_SCTLR_ON,
	    "the Wi-Fi's context left alone: ttbr0 %#x", *smmu(CONTEXT_AT(2) + SMMU_CB_TTBR0));
	CHECK(*smmu(SMMU_AT + SMMU_SMR(3)) == (1u << 31 | AUDIO) && *smmu(SMMU_AT + SMMU_S2CR(3)) == 3 &&
	    (*smmu(CONTEXT_AT(3) + SMMU_CB_SCTLR) & 1), "the next free ones taken: match register 3, context 3: %#x %#x %#x",
	    *smmu(SMMU_AT + SMMU_SMR(3)), *smmu(SMMU_AT + SMMU_S2CR(3)), *smmu(CONTEXT_AT(3) + SMMU_CB_SCTLR));
	hakios_audio_dsp_memory_put();
	CHECK(*smmu(SMMU_AT + SMMU_SMR(3)) == 0 && *smmu(CONTEXT_AT(3) + SMMU_CB_SCTLR) == 0 &&
	    *smmu(SMMU_AT + SMMU_SMR(2)) == (1u << 31 | 0x0c80) && *smmu(CONTEXT_AT(2) + SMMU_CB_SCTLR) == SMMU_SCTLR_ON,
	    "and those put back as found, the Wi-Fi's still as it has them");
}

/*
 * The writes of Darwin's periods cannot be sent for a while (the link finds no room at the DSP): the DSP never gives
 * those periods back, and they are written again once the link takes them -- not lost for good, which after eight
 * would leave nothing queued at the DSP and the stream silent until the run ends.
 */
static void
a_write_not_sent_is_tried_again(void)
{
	static int16_t periods[PERIODS * 4096 / 2] __attribute__((aligned(4096)));
	thread_t thread;

	buffer = (vm_offset_t)periods;
	writes_refused = true;
	feeder_runs = true;
	CHECK(kernel_thread_start(feeder_thread, NULL, &thread) == KERN_SUCCESS, "the feeder");
	thread_deallocate(thread);
	WAIT_FOR(writes_tried >= PERIODS, 2000);
	CHECK(writes_tried >= PERIODS && writes_sent == 0, "none sent: %u tried", writes_tried);
	writes_refused = false;
	WAIT_FOR(writes_sent >= PERIODS, 2000);
	usleep(50000);
	CHECK(writes_sent == PERIODS, "every period written again once the link takes them, and once: %u", writes_sent);
	stopping = true;
	WAIT_FOR(!feeder_runs, 2000);
	CHECK(!feeder_runs, "the feeder stops");
}

/* the watcher of sound=4 on a thread of its own, the amplifiers playing as asked, Darwin playing */
static void
playing(pthread_t *thread)
{
	static int16_t periods[4096 * 2 / 2] __attribute__((aligned(4096)));
	const int16_t frames[] = { 1000, -1000, 2000, -2000, 3000, -3000 };

	buffer = (vm_offset_t)periods;
	speakers_play = true;
	CHECK(darwin_sink(frames, 3) == 3 && last_sound_us != 0, "Darwin plays");
	pthread_create(thread, NULL, watcher, NULL);
	WAIT_FOR(logged("the amplifiers are up"), 5000);
	CHECK(logged("the amplifiers are up"), "started: %s", log_text);
}

/*
 * A write that comes between the watcher's look at the clock and its look at when the latest sound came is, to the
 * watcher, sound from after its now: not three seconds of silence, and the amplifiers are not stopped in the middle
 * of the sound. (Here a fifth of a second after, so that the watcher sees it at more than one look; the race itself
 * makes it a few microseconds.)
 */
static void
a_write_as_the_watcher_looks(void)
{
	pthread_t thread;

	playing(&thread);
	last_sound_us = now_us() + 200000;
	usleep(100000);
	CHECK(!logged("three seconds of silence") && speaker_stops == 0, "the amplifiers were stopped: %u stops, %s", speaker_stops,
	    log_text);
	stopping = true;
	pthread_join(thread, NULL);
}

/*
 * sound_below past 32 bits' end as a negative number reads (sound_below=-8: 0xfffffff8), and Darwin's own volume 2 dB
 * under the loudest: the amplifiers are asked for no level nearer Android's than the lowest, not for a sum that
 * wrapped to 1 dB under it.
 */
static void
far_under_and_darwin_under_that(void)
{
	pthread_t thread;

	boot_args = "sound=4 sound_below=-8";
	hakios_sound_dsp_up();
	/* (no configuration file for the amplifiers here: the level ends there, and is taken down) */
	WAIT_FOR(logged("STOPPED SHORT"), 5000);
	CHECK(logged("STOPPED SHORT"), "the level ended: %s", log_text);
	stopping = false;
	darwin_below = 16;
	darwin_generation = 1;
	playing(&thread);
	WAIT_FOR(logged("Darwin sets the volume"), 2000);
	CHECK(least_asked >= 90 * 8, "the amplifiers asked for %u eighths of a dB under Android's level", least_asked);
	stopping = true;
	pthread_join(thread, NULL);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); unsigned cases; } scenarios[] = {
		{ "the_boards_pins", the_boards_pins, 1 }, { "the_clock_watched_at_the_boards_pins", the_clock_watched_at_the_boards_pins, 1 },
		{ "not_at_the_first_phones", not_at_the_first_phones, 1 }, { "no_frames", no_frames, 1 },
		{ "smmu_set_up", smmu_set_up, 1 }, { "smmu_let_through", smmu_let_through, 1 },
		{ "smmu_left_alone", smmu_left_alone, 8 }, { "smmu_faults_said", smmu_faults_said, 1 },
		{ "the_streams_events", the_streams_events, 1 }, { "the_dsps_answers", the_dsps_answers, 1 },
		{ "darwins_frames_through_the_ring", darwins_frames_through_the_ring, 1 }, { "said_off", said_off, 2 },
		{ "no_amplifiers", no_amplifiers, 1 }, { "the_tone_in_both_channels", the_tone_in_both_channels, 1 },
		{ "sound_in_one_channel", sound_in_one_channel, 1 }, { "decibels_as_text", decibels_as_text, 1 },
		{ "the_guards_line", the_guards_line, 1 }, { "recording_fed_what_is_played", recording_fed_what_is_played, 1 },
		{ "given_up_darwin_not_fed", given_up_darwin_not_fed, 1 },
		{ "given_up_while_a_period_is_guarded", given_up_while_a_period_is_guarded, 1 },
		{ "smmu_taken_meanwhile", smmu_taken_meanwhile, 1 }, { "a_write_not_sent_is_tried_again", a_write_not_sent_is_tried_again, 1 },
		{ "a_write_as_the_watcher_looks", a_write_as_the_watcher_looks, 1 },
		{ "far_under_and_darwin_under_that", far_under_and_darwin_under_that, 1 },
	};
	char name[64];
	const char *colon;

	if (argc < 2) {
		/* a scenario with several cases is listed once for each: NAME:N (the driver's state is a process's) */
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			for (unsigned c = 0; c < scenarios[i].cases; c++) {
				if (scenarios[i].cases == 1) {
					printf("%s\n", scenarios[i].name);
				} else {
					printf("%s:%u\n", scenarios[i].name, c);
				}
			}
		}
		return 0;
	}
	snprintf(name, sizeof(name), "%s", argv[1]);
	if ((colon = strchr(argv[1], ':')) != NULL) {
		name[colon - argv[1]] = 0;
		setenv("SOUND_CASE", colon + 1, 1);
	}
	board = "phone-keys";
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(name, scenarios[i].name) == 0) {
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
