/*
 * Where the microphones' driver (kernel/overlay/bsd/dev/board/mic.c) takes the audio subsystem's blocks
 * and pins from. The driver is compiled as it is into a host program; the registers are memory that
 * keeps what is written, and nothing of the codec or of SoundWire is modelled. What is tested, on a
 * made-up SoC whose lpass { } is no real chip's: the SoundWire pins the driver sets up are the SoC file's
 * swr-tx-pins, their slew fields its swr-tx-slew, and no other pin is touched; the hardware control
 * register it clears a bit of and puts back is the one tx-swr-hctl names, in its page. The audio DSP is a
 * script of answers: its refusals are named, the votes for the subsystem's blocks given back with their
 * handles. And the run's lines, kept, are said again at its end as they were said.
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
#include "board/board.c"
#define get mic_get
#include "board/mic.c"
#undef get
#undef printf

/* ---- the audio DSP: answers as a scenario tells it to, and what it was asked -------------------------- */

enum dsp_answer { DSP_SILENT, DSP_RESULT, DSP_OWN_ANSWER, DSP_ANOTHER_RESULT };
static struct { bool put; enum dsp_answer how; uint32_t status, word; } dsp_script[16];
static unsigned dsp_asked;
static struct { uint32_t opcode, words[4]; } dsp_heard[16];

/* the next command's answer: as the scenario put it, or "done" when it put none */
static void
dsp_will(enum dsp_answer how, uint32_t status, uint32_t word)
{
	static unsigned put;

	dsp_script[put].put = true;
	dsp_script[put].how = how;
	dsp_script[put].status = status;
	dsp_script[put].word = word;
	put++;
}

bool
hakios_adsp_apr(uint8_t service __unused, uint16_t port __unused, uint32_t token __unused, uint32_t opcode,
    const void *payload, uint32_t length, uint32_t wait_ms __unused, struct hakios_apr_answer *answer)
{
	unsigned n = dsp_asked++ % 16;

	dsp_heard[n].opcode = opcode;
	memset(dsp_heard[n].words, 0, sizeof(dsp_heard[n].words));
	if (payload != NULL) {
		memcpy(dsp_heard[n].words, payload, length < sizeof(dsp_heard[n].words) ? length : sizeof(dsp_heard[n].words));
	}
	memset(answer, 0, sizeof(*answer));
	switch (dsp_script[n].put ? dsp_script[n].how : DSP_RESULT) {
	case DSP_SILENT:
		return false;
	case DSP_RESULT:
	case DSP_ANOTHER_RESULT:
		answer->opcode = APR_BASIC_RSP_RESULT;
		answer->length = 8;
		answer->words[0] = dsp_script[n].how == DSP_ANOTHER_RESULT ? opcode + 1 : opcode;
		answer->words[1] = dsp_script[n].status;
		return true;
	case DSP_OWN_ANSWER:
		answer->opcode = opcode + 1;    /* (the vote's answer is the request's opcode and one) */
		answer->length = 4;
		answer->words[0] = dsp_script[n].word;
		return true;
	}
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

/* ---- the DSP's answers to the driver's commands ------------------------------------------------------- */

/* "done", a refusal by name (and by none, past the names known), no answer, an answer to something else */
static void
the_dsps_answers(void)
{
	dsp_will(DSP_RESULT, 0, 0);
	CHECK(command("a command", APR_SVC_AFE, 0x100, NULL, 0, 0, NULL), "done is done");
	CHECK(strstr(log_text, "mic: a command: done") != NULL, "said: %s", log_text);
	dsp_will(DSP_RESULT, 2, 0);
	CHECK(!command("a refused one", APR_SVC_AFE, 0x101, NULL, 0, 0, NULL) &&
	    strstr(log_text, "a refused one: REFUSED: bad parameter") != NULL, "refused, by name: %s", log_text);
	dsp_will(DSP_RESULT, 20, 0);
	CHECK(!command("the last name", APR_SVC_AFE, 0x102, NULL, 0, 0, NULL) &&
	    strstr(log_text, "the last name: REFUSED: not there") != NULL, "the last of the names: %s", log_text);
	dsp_will(DSP_RESULT, 21, 0);
	CHECK(!command("past the names", APR_SVC_AFE, 0x103, NULL, 0, 0, NULL) &&
	    strstr(log_text, "past the names: REFUSED: an error this does not know") != NULL, "past them: %s", log_text);
	dsp_will(DSP_SILENT, 0, 0);
	CHECK(!command("unanswered", APR_SVC_AFE, 0x104, NULL, 0, 0, NULL) &&
	    strstr(log_text, "unanswered (opcode 0x104): NO ANSWER") != NULL, "no answer: %s", log_text);
	dsp_will(DSP_ANOTHER_RESULT, 0, 0);
	CHECK(!command("answered for another", APR_SVC_AFE, 0x105, NULL, 0, 0, NULL) &&
	    strstr(log_text, "answered for another: an answer not expected") != NULL, "a result for another command: %s", log_text);
}

/* a command whose answer is its own: that answer, not a result, even a good one; and not another's */
static void
an_answer_of_its_own(void)
{
	struct hakios_apr_answer answer;

	dsp_will(DSP_OWN_ANSWER, 0, 7);
	CHECK(command("asked", APR_SVC_AFE, 0x200, NULL, 0, 0x201, &answer) && answer.words[0] == 7, "its answer");
	dsp_will(DSP_RESULT, 0, 0);
	CHECK(!command("asked again", APR_SVC_AFE, 0x200, NULL, 0, 0x201, &answer), "a good result is not the answer asked for");
	dsp_will(DSP_OWN_ANSWER, 0, 7);
	CHECK(!command("asked another", APR_SVC_AFE, 0x200, NULL, 0, 0x300, &answer), "nor another command's answer");
	dsp_will(DSP_OWN_ANSWER, 0, 7);
	CHECK(!command("answered unasked", APR_SVC_AFE, 0x200, NULL, 0, 0, &answer), "nor an answer to a command that waits for none");
}

/* the two votes, the handle each is given and given back with; the second not asked for once the first is refused */
static void
the_votes(void)
{
	dsp_will(DSP_OWN_ANSWER, 0, 11);
	dsp_will(DSP_OWN_ANSWER, 0, 12);
	CHECK(clocks_on(), "on: %s", log_text);
	CHECK(dsp_asked == 4 && dsp_heard[0].opcode == AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST && dsp_heard[0].words[0] == 3 &&
	    dsp_heard[1].words[0] == 4 && dsp_heard[2].opcode == AFE_SVC_CMD_SET_PARAM, "the votes, then the clocks: %u asked",
	    dsp_asked);
	clocks_off();
	CHECK(dsp_asked == 8, "two clocks off and two votes given back: %u", dsp_asked);
	CHECK(dsp_heard[6].opcode == AFE_CMD_REMOTE_LPASS_CORE_HW_DEVOTE_REQUEST && dsp_heard[6].words[0] == 4 &&
	    dsp_heard[6].words[1] == 12 && dsp_heard[7].words[0] == 3 && dsp_heard[7].words[1] == 11,
	    "each block's, with its handle: %u %u, %u %u", dsp_heard[6].words[0], dsp_heard[6].words[1], dsp_heard[7].words[0],
	    dsp_heard[7].words[1]);
	clocks_off();
	CHECK(dsp_asked == 8, "off again: nothing more (%u)", dsp_asked);
}

static void
the_first_vote_refused(void)
{
	dsp_will(DSP_RESULT, 3, 0);
	CHECK(!clocks_on(), "not on");
	CHECK(dsp_asked == 1, "the second vote not asked for: %u", dsp_asked);
	clocks_off();
	CHECK(dsp_asked == 1, "nothing to give back: %u", dsp_asked);
}

/* ---- the run's lines, said again at its end (hakios_mic_quiesce()) ------------------------------------- */

static unsigned
occurrences(const char *text)
{
	unsigned n = 0;

	for (const char *at = log_text; (at = strstr(at, text)) != NULL; at++) {
		n++;
	}
	return n;
}

/* nothing said: nothing said again, not even that this is the run's lines */
static void
nothing_to_say_again(void)
{
	hakios_mic_quiesce();
	CHECK(log_text[0] == 0, "the log: \"%s\"", log_text);
}

/* every line again, after a "> ", each on a line of its own: one without its newline, or empty, gets one */
static void
the_run_said_again(void)
{
	say("mic: first\n");
	say("mic: no newline");
	say("%s", "");
	say("mic: last\n");
	hakios_mic_quiesce();
	CHECK(strstr(log_text, "mic: what happened in this run, again:\n> mic: first\n> mic: no newline\n> \n> mic: last\n") != NULL &&
	    strcmp(log_text + strlen(log_text) - strlen("> mic: last\n"), "> mic: last\n") == 0, "the log: \"%s\"", log_text);
}

/* more lines than are kept: the latest of them, oldest first */
static void
the_latest_lines_said_again(void)
{
	char last[32];

	for (unsigned i = 0; i < KEPT_LINES + 50; i++) {
		say("mic: line %u\n", i);
	}
	hakios_mic_quiesce();
	snprintf(last, sizeof(last), "> mic: line %u\n", KEPT_LINES + 49);
	CHECK(occurrences("> mic: line ") == KEPT_LINES, "%u lines said again", occurrences("> mic: line "));
	CHECK(occurrences("> mic: line 49\n") == 0 && occurrences("again:\n> mic: line 50\n") == 1, "from the 51st on");
	CHECK(strcmp(log_text + strlen(log_text) - strlen(last), last) == 0, "to the last: %s", log_text + strlen(log_text) - 40);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "the_socs_pins", the_socs_pins }, { "the_socs_hctl", the_socs_hctl },
		{ "nothing_to_say_again", nothing_to_say_again }, { "the_run_said_again", the_run_said_again },
		{ "the_latest_lines_said_again", the_latest_lines_said_again }, { "the_dsps_answers", the_dsps_answers },
		{ "an_answer_of_its_own", an_answer_of_its_own }, { "the_votes", the_votes },
		{ "the_first_vote_refused", the_first_vote_refused },
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
