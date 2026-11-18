/*
 * Where the microphones' driver (drivers/board/mic.c) takes the audio subsystem's blocks
 * and pins from. The driver is compiled as it is into a host program; the registers are memory that
 * keeps what is written, and the TX SoundWire master, where a scenario wants it, a model of its link and
 * command FIFO, and the devices on its bus as registers. What is tested, on a
 * made-up SoC whose lpass { } is no real chip's: the SoundWire pins the driver sets up are the SoC file's
 * swr-tx-pins, their slew fields its swr-tx-slew, and no other pin is touched; the hardware control
 * register it clears a bit of and puts back is the one tx-swr-hctl names, in its page. The audio DSP is a
 * script of answers: its refusals are named, the votes for the subsystem's blocks given back with their
 * handles. And the run's lines, kept, are said again at its end as they were said.
 *
 *   mic_driver_test SCENARIO
 */
#include "fakekernel.h"
#include <unistd.h>

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

/* ---- the TX SoundWire master, where a scenario wants it: its link, its command FIFO and a device's registers - */

#define SWR             0x04230000u     /* the made-up SoC's tx-swr */
static struct {
	bool modelled, answers, wrong_id, fifo_full, reads_unanswered;
	unsigned answer_after;                  /* an answer comes after that many looks at the FIFO's status */
	bool linked;
	uint32_t answer;                        /* the read FIFO's word, while one waits there */
	bool waiting;
	uint8_t device_reg[16][0x10000];        /* what each device's registers hold */
	uint8_t ever_set[16][0x10000];          /* every bit ever written into each */
	uint16_t stuck_reg;                     /* a register that keeps none of what is written (0: none) */
	unsigned status_reads, fifo_status_reads, fifo_reads, commands, slave_status_reads;
	uint8_t ids[32];                        /* the ids reads were sent with */
	unsigned id_count;
} swr;

static bool
swr_model_rd(uint64_t address, uint32_t *value)
{
	if (!swr.modelled || address < SWR || address >= SWR + 0x2000) {
		return false;
	}
	switch ((uint32_t)(address - SWR)) {
	case 0x0014:                            /* COMP_STATUS */
		swr.status_reads++;
		*value = swr.linked;
		return true;
	case 0x030c:                            /* CMD_FIFO_STATUS: [12:8] commands queued, [20:16] answers waiting */
		swr.fifo_status_reads++;
		if (swr.answer_after > 0) {
			swr.answer_after--;
		}
		*value = (swr.fifo_full ? 16u << 8 : 0) | (swr.waiting && swr.answer_after == 0 ? 1u << 16 : 0);
		return true;
	case 0x0318:                            /* CMD_FIFO_RD_FIFO */
		swr.fifo_reads++;
		*value = swr.answer;
		swr.waiting = false;
		return true;
	}
	return false;
}

static void
swr_model_wr(uint64_t address, uint32_t value)
{
	uint8_t device = value >> 20 & 0xf, id = value >> 16 & 0xf;
	uint16_t reg = (uint16_t)value;

	if (!swr.modelled || address < SWR || address >= SWR + 0x2000) {
		return;
	}
	switch ((uint32_t)(address - SWR)) {
	case 0x0004:                            /* COMP_CFG: 3, on; the link comes up if anybody is on the bus */
		swr.linked = value == 3 && swr.answers;
		break;
	case 0x0300:                            /* CMD_FIFO_WR_CMD */
		swr.commands++;
		if (reg != swr.stuck_reg || reg == 0) {
			swr.device_reg[device][reg] = (uint8_t)(value >> 24);
		}
		swr.ever_set[device][reg] |= (uint8_t)(value >> 24);
		break;
	case 0x0304:                            /* CMD_FIFO_RD_CMD */
		swr.commands++;
		if (swr.id_count < 32) {
			swr.ids[swr.id_count++] = id;
		}
		if (swr.answers && !swr.reads_unanswered) {
			swr.answer = swr.device_reg[device][reg] | (uint32_t)(swr.wrong_id ? (id + 1) & 0xf : id) << 8;
			swr.waiting = true;
		}
		break;
	}
}

uint32_t
hakios_test_rd(vm_offset_t address)
{
	uint32_t value;

	swr.slave_status_reads += address == SWR + 0x1090;     /* (MCP_SLV_STATUS, modelled or not) */
	return swr_model_rd(address, &value) ? value : *cell(address);
}

void
hakios_test_wr(vm_offset_t address, uint32_t value)
{
	swr_model_wr(address, value);
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
	if (answer == NULL) {
		/* (a command not waited for, a read: sent, or not when the scenario says the DSP is silent) */
		return !dsp_script[n].put || dsp_script[n].how != DSP_SILENT;
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

/* the memory the DSP sees, where a scenario gives it (none otherwise) */
static uint8_t dsp_memory[MIC_PERIODS * 4096];
static bool dsp_memory_there;

bool
hakios_audio_dsp_memory(uintptr_t *memory, uint64_t *dsp_address, uint32_t *bytes)
{
	if (!dsp_memory_there) {
		return false;
	}
	*memory = (uintptr_t)dsp_memory;
	*dsp_address = 0x9e000000u;
	*bytes = sizeof(dsp_memory);
	return true;
}
void hakios_audio_dsp_memory_put(void) { }
bool hakios_audio_recording_mic(void) { return false; }
void hakios_audio_record_fed(const int16_t *frames __unused, uint32_t count __unused) { }
void hakios_haptic_pretend(uint32_t peak __unused) { }

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited_ = 0; !(cond) && waited_ < (ms); waited_ += 5) { usleep(5000); } } while (0)

/* how many of the commands from the one numbered `from' on (of the last 16) were this one */
static unsigned
asked_since(unsigned from, uint32_t opcode)
{
	unsigned n = 0;

	for (unsigned i = from; i < dsp_asked; i++) {
		n += dsp_heard[i % 16].opcode == opcode;
	}
	return n;
}

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

/* ---- the SoundWire master -------------------------------------------------------------------------------- */

/* the link comes up: the frame is 50 rows and 2 columns at 48 frames a sync, the FIFO's retries and its flag set */
static void
the_link_up(void)
{
	blocks_from_board();
	swr.modelled = swr.answers = true;
	CHECK(master_on(), "on: %s", log_text);
	CHECK(*cell(SWR + 0x101c) == (1u << 3 | 47u << 16), "MCP_FRAME_CTRL_BANK0: %#x", *cell(SWR + 0x101c));
	CHECK((*cell(SWR + 0x1048) & 0x3e0000) == 0x3e0000, "31 pings: %#x", *cell(SWR + 0x1048));
	CHECK(*cell(SWR + 0x0314) == 0x80000003, "CMD_FIFO_CFG: %#x", *cell(SWR + 0x0314));
	CHECK(written(SWR + 0x0008) == 2, "reset twice, as Android does: %u", written(SWR + 0x0008));
	CHECK(strstr(log_text, "the SoundWire link IS UP") != NULL, "said");
	master_off();
	CHECK(*cell(SWR + 0x0004) == 0 && *cell(SWR + 0x0204) == 0 && written(SWR + 0x0008) == 3, "off, interrupts off, reset");
}

/* nobody on the bus: a hundred looks, about 50 ms, and no more */
static void
the_link_down(void)
{
	blocks_from_board();
	swr.modelled = true;
	CHECK(!master_on(), "not on");
	CHECK(swr.status_reads == 101, "COMP_STATUS read %u times (a hundred looks and once to say it)", swr.status_reads);
	CHECK(strstr(log_text, "the SoundWire link DID NOT COME UP") != NULL, "said: %s", log_text);
	CHECK(written(SWR + 0x0314) == 1, "the FIFO's flag not set on a link that is not there");
}

/* a register of a device read over the bus, each read with the next id, 1 to 14 and then 0 */
static void
reads_over_the_bus(void)
{
	uint8_t value = 0;

	blocks_from_board();
	swr.modelled = swr.answers = true;
	CHECK(master_on(), "on");
	swr.device_reg[2][0x41] = 0x5a;
	for (unsigned i = 0; i < 17; i++) {
		CHECK(swr_read(2, 0x41, &value) && value == 0x5a, "read %u: %#x", i, value);
	}
	for (unsigned i = 0; i < 17; i++) {
		CHECK(swr.ids[i] == (i + 1) % 15, "read %u went with id %u", i, swr.ids[i]);
	}
	CHECK(swr_write(2, 0x41, 0x33, 0) && swr.device_reg[2][0x41] == 0x33, "a write: %#x", swr.device_reg[2][0x41]);
}

/* an answer with another read's id is not this read's: four of them, and it is said */
static void
an_answer_for_another_read(void)
{
	uint8_t value = 0;

	blocks_from_board();
	swr.modelled = swr.answers = swr.wrong_id = true;
	CHECK(master_on(), "on");
	CHECK(!swr_read(2, 0x41, &value), "no value");
	CHECK(swr.fifo_reads == 4, "the read FIFO looked at %u times", swr.fifo_reads);
	CHECK(strstr(log_text, "device 2 register 0x41: no answer") != NULL, "said: %s", log_text);
}

/* an answer a while after the read: waited for, and the read FIFO looked at once, when it is there */
static void
an_answer_a_while_after(void)
{
	uint8_t value = 0;

	blocks_from_board();
	swr.modelled = swr.answers = true;
	CHECK(master_on(), "on");
	swr.device_reg[2][0x41] = 0x5a;
	swr.answer_after = 5;
	swr.fifo_status_reads = 0;
	CHECK(swr_read(2, 0x41, &value) && value == 0x5a, "read: %#x", value);
	CHECK(swr.fifo_reads == 1 && swr.fifo_status_reads == 5, "the FIFO read %u times, its status %u", swr.fifo_reads,
	    swr.fifo_status_reads);
}

/* no answer at all: twenty looks at the FIFO's status for each of four tries, then it is said */
static void
no_answer_at_all(void)
{
	uint8_t value = 0;

	blocks_from_board();
	swr.modelled = swr.answers = swr.reads_unanswered = true;
	CHECK(master_on(), "on");
	swr.fifo_status_reads = 0;
	CHECK(!swr_read(2, 0x41, &value), "no value");
	CHECK(swr.fifo_status_reads == 4 * 20 + 1 && swr.fifo_reads == 4, "the status looked at %u times, the FIFO %u",
	    swr.fifo_status_reads, swr.fifo_reads);
}

/* a full command FIFO is waited for, twenty looks, and the command written then */
static void
a_full_fifo(void)
{
	blocks_from_board();
	swr.modelled = swr.answers = true;
	CHECK(master_on(), "on");
	swr.fifo_full = true;
	swr.fifo_status_reads = 0;
	CHECK(swr_write(2, 0x41, 0x33, 0), "written");
	CHECK(swr.fifo_status_reads == 20 && swr.commands == 1, "%u looks, %u commands", swr.fifo_status_reads, swr.commands);
}

/* ---- the codec, on the bus ------------------------------------------------------------------------------- */

#define CODEC_ID        0x000d01170223ull       /* the WCD938x's TX device */

/* what the master's enumerator says is device d: its ID, and its status (0 gone, 1 attached, 2 alert) */
static void
enumerated(uint8_t d, uint64_t id, uint32_t status)
{
	*cell(SWR + 0x0530 + 8 * d) = (uint32_t)id;
	*cell(SWR + 0x0534 + 8 * d) = (uint32_t)(id >> 32);
	*cell(SWR + 0x1090) = (*cell(SWR + 0x1090) & ~(3u << 2 * d)) | status << 2 * d;
}

/* the codec's TX device among others on the bus: found by its ID, attached or asking for attention */
static void
the_codec_found(void)
{
	blocks_from_board();
	enumerated(1, 0x000d01170224ull, 1);            /* its RX device */
	enumerated(4, CODEC_ID, 1);
	CHECK(codec_wait() == 4, "device 4: %s", log_text);
	enumerated(4, CODEC_ID, 2);
	CHECK(codec_wait() == 4, "asking for attention, still found");
	enumerated(4, CODEC_ID, 0);
	enumerated(5, CODEC_ID | 0x1000000000000ull, 1);       /* (the ID's top bits are not the part's: 48 bits are) */
	CHECK(codec_wait() == 5, "by the 48 bits of its ID");
}

/* not on the bus: some 200 ms, then what the master has seen, the devices it has an ID of */
static void
the_codec_not_there(void)
{
	blocks_from_board();
	enumerated(1, 0x000d01170224ull, 1);
	enumerated(4, CODEC_ID, 0);                     /* (seen once, gone) */
	CHECK(codec_wait() == 0, "none");
	CHECK(swr.slave_status_reads == 200 * 11 + 1, "two hundred looks at each of the 11 devices, and once to say it: %u",
	    swr.slave_status_reads);
	CHECK(strstr(log_text, "did not show up") != NULL && strstr(log_text, "device 1: ID 0xd01170224") != NULL &&
	    strstr(log_text, "device 4: ID 0xd01170223") != NULL && strstr(log_text, "device 2:") == NULL, "said: %s", log_text);
}

/* the run ended (hakios_mic_dsp_down()) before the driver's thread took its lock: nothing is brought up after it */
static void
nothing_after_the_run_ends(void)
{
	thread_t thread;

	blocks_from_board();
	level = 2;
	running = true;
	hakios_mic_dsp_down();
	CHECK(kernel_thread_start(mic_thread, NULL, &thread) == KERN_SUCCESS, "started");
	for (int waited = 0; running && waited < 2000; waited += 5) {
		usleep(5000);
	}
	CHECK(!running, "the thread is done");
	CHECK(logged("mic: the run has ended: nothing brought up"), "said: %s", log_text);
	CHECK(write_count == 0, "nothing written: %u writes, the first at %#llx", write_count,
	    write_count ? (unsigned long long)writes[0] : 0ull);
}

/* a codec whose registers hold something else than what is wanted, set up as wcd938x_init_reg() sets one up */
static void
codec(void)
{
	blocks_from_board();
	swr.modelled = swr.answers = true;
	CHECK(master_on(), "on");
	codec_device = 4;
	memset(swr.device_reg[4], 0xa5, sizeof(swr.device_reg[4]));
	swr.device_reg[4][0x34ce] = 0x05;               /* EFUSE_REG_30 */
}

static void
the_codec_set_up(void)
{
	static const struct { uint16_t reg; uint8_t mask, value; } wanted[] = {
		{ 0x3474, 0xff, 0x00 }, { 0x3475, 0xff, 0x00 }, { 0x3476, 0xff, 0x00 },         /* INTR_LEVEL_0..2 */
		{ 0x3022, 0x3f, 35 }, { 0x3023, 0x3f, 35 }, { 0x3025, 0x3f, 35 }, { 0x3026, 0x3f, 35 }, /* MICB1..4: 2750 mV */
		{ 0x3103, 0xce, 0xca },                         /* SLEEP_CTL: 0x80, 0x40, the efuse's 5 << 1 */
		{ 0x312a, 0x10, 0x00 }, { 0x3029, 0xf0, 0x80 }, /* LDORXTX_CONFIG, BIAS_VBG_FINE_ADJ */
		{ 0x3001, 0xc0, 0x80 },                         /* ANA_BIAS: 0x40 set and taken away again */
		{ 0x3132, 0xf0, 0x00 }, { 0x3140, 0x1f, 0x15 }, { 0x3141, 0x1f, 0x15 }, { 0x30dd, 0xc0, 0x80 },
		{ 0x345b, 0x02, 0x02 }, { 0x31e1, 0xff, 0x14 }, { 0x31df, 0x1f, 0x08 },
		{ 0x34d0, 0xff, 0x55 }, { 0x34d1, 0xff, 0x44 }, { 0x34d2, 0xff, 0x11 }, { 0x34d3, 0xff, 0x00 }, { 0x34d4, 0xff, 0x00 },
		{ 0x306b, 0xe0, 0xe0 }, { 0x306e, 0xe0, 0xe0 }, { 0x3071, 0xe0, 0xe0 }, { 0x3074, 0xe0, 0xe0 },
		{ 0x308f, 0x01, 0x00 }, { 0x30e2, 0xc0, 0x00 },
	};

	codec();
	CHECK(codec_init(), "set up: %s", log_text);
	for (size_t i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
		uint8_t now = swr.device_reg[4][wanted[i].reg];

		CHECK((now & wanted[i].mask) == wanted[i].value && (now & ~wanted[i].mask) == (0xa5 & ~wanted[i].mask),
		    "%#x: %#x, its bits %#x wanted %#x and the others left", wanted[i].reg, now, wanted[i].mask, wanted[i].value);
	}
	CHECK(strstr(log_text, "(efuse 30: 0x5)") != NULL, "the efuse said");
	CHECK(swr.ever_set[4][0x3001] & 0x40, "ANA_BIAS's 0x40 was set before it was taken away: %#x", swr.ever_set[4][0x3001]);
}

/* ... and from registers that hold nothing: every bit that is wanted set is written, not found there */
static void
the_codec_set_up_from_nothing(void)
{
	codec();
	memset(swr.device_reg[4], 0, sizeof(swr.device_reg[4]));
	swr.device_reg[4][0x34ce] = 0x05;
	CHECK(codec_init(), "set up: %s", log_text);
	CHECK(swr.device_reg[4][0x3103] == 0xca && swr.device_reg[4][0x3001] == 0x80 && swr.device_reg[4][0x3029] == 0x80,
	    "SLEEP_CTL %#x, ANA_BIAS %#x, BIAS_VBG_FINE_ADJ %#x", swr.device_reg[4][0x3103], swr.device_reg[4][0x3001],
	    swr.device_reg[4][0x3029]);
}

/* ADC1 on (mic bias 1 and the ADC's request), from registers that hold nothing: each of its bits there */
static void
adc1_turned_on(void)
{
	static const struct { uint16_t reg; uint8_t mask, value; } wanted[] = {
		{ WCD_CDC_DIG_CLK_CTL, 0xf0, 0xf0 }, { WCD_CDC_ANA_CLK_CTL, 0x18, 0x18 }, { WCD_CDC_ANA_TX_CLK_CTL, 0x01, 0x01 },
		{ WCD_MICB1_TEST_CTL_2, 0x01, 0x01 }, { WCD_MICB2_TEST_CTL_2, 0x01, 0x01 }, { WCD_MICB3_TEST_CTL_2, 0x01, 0x01 },
		{ WCD_MICB4_TEST_CTL_2, 0x01, 0x01 }, { WCD_ANA_MICB1, 0xc0, 0x40 }, { WCD_ANA_TX_CH1, 0x1f, 6 },
		{ WCD_CDC_REQ_CTL, 0x03, 0x02 }, { WCD_ANA_TX_CH2, 0x40, 0x00 }, { WCD_CDC_TX_ANA_MODE_0_1, 0x0f, 0x05 },
	};

	codec();
	memset(swr.device_reg[4], 0, sizeof(swr.device_reg[4]));
	CHECK(adc1_on(), "on: %s", log_text);
	for (size_t i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
		CHECK((swr.device_reg[4][wanted[i].reg] & wanted[i].mask) == wanted[i].value, "%#x: %#x, its bits %#x wanted %#x",
		    wanted[i].reg, swr.device_reg[4][wanted[i].reg], wanted[i].mask, wanted[i].value);
	}
	CHECK(swr.ever_set[4][WCD_ANA_TX_CH2] & 0x40, "the ADC's input held (HPF) before it is let go: %#x", swr.ever_set[4][WCD_ANA_TX_CH2]);
}

/* a register that holds what is wanted already is read, not written */
static void
the_codec_left_as_it_is(void)
{
	unsigned before;

	codec();
	swr.device_reg[4][0x3022] = 0xa3;               /* MICB1 at 2750 mV already */
	before = swr.commands;
	CHECK(codec_bits(0x3022, 0x3f, 35) && swr.commands == before + 1, "one read, no write: %u", swr.commands - before);
	CHECK(codec_bits(0x3022, 0x3f, 36) && swr.commands == before + 4 && swr.device_reg[4][0x3022] == 0xa4,
	    "then a read, a write and its read back: %u, %#x", swr.commands - before - 1, swr.device_reg[4][0x3022]);
}

/* a register that does not take what is written stops it there: nothing after it is written */
static void
the_codec_does_not_take_it(void)
{
	codec();
	swr.stuck_reg = 0x3023;                         /* MICB2 */
	CHECK(!codec_init(), "not set up");
	CHECK(strstr(log_text, "the codec's 0x3023 <- 0xa3, but it reads 0xa5: STOPPING HERE") != NULL, "said: %s", log_text);
	CHECK(swr.device_reg[4][0x3022] == 0xa3 && swr.device_reg[4][0x3025] == 0xa5 && swr.device_reg[4][0x3103] == 0xa5,
	    "MICB1 written, MICB3 and SLEEP_CTL not");
	CHECK(!codec_put(0x3474, 1, 0) && !codec_bits(0x3474, 1, 1), "nothing more, once stopped");
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

/* ---- the ring Darwin's audio server reads (/dev/hakios-mic) ---- */

static int16_t heard[RING_FRAMES], got[RING_FRAMES];
static uint64_t put_so_far;

/* `count' more frames heard: each the number of the frame it is (mod 32768) */
static void
hear(uint32_t count)
{
	int16_t samples[1000];

	while (count > 0) {
		uint32_t n = count < 1000 ? count : 1000;

		for (uint32_t i = 0; i < n; i++) {
			samples[i] = (int16_t)((put_so_far + i) & 0x7fff);
		}
		ring_put(samples, n);
		put_so_far += n;
		count -= n;
	}
}

/* what a read of `frames' gives, in frames; whether they run on one from the other from `first' */
static uint32_t
read_frames(uint32_t frames, int first, bool *in_order)
{
	struct uio uio = { .base = (char *)got, .resid = frames * 2, .to_kernel = false };
	uint32_t n;

	mic_dev_read(0, &uio, 0);
	n = (uint32_t)(frames * 2 - uio.resid) / 2;
	*in_order = true;
	for (uint32_t i = 0; i < n; i++) {
		*in_order &= got[i] == (int16_t)((first + (int)i) & 0x7fff);
	}
	return n;
}

/*
 * Nothing is given until 40 ms are there; then what has come, in order, the reader kept 40 ms behind; a read
 * that finds too little takes it all and waits for 40 ms again; more than 100 ms waiting, and the oldest
 * is skipped; frames run on across the ring's end.
 */
static void
the_ring_darwin_reads(void)
{
	uint64_t skipped;
	bool in_order;

	ring = heard;
	hear(1000);
	CHECK(read_frames(480, 0, &in_order) == 0, "nothing before 40 ms are there");
	hear(1000);
	CHECK(read_frames(480, 0, &in_order) == 480 && in_order, "then from the first frame, in order");
	CHECK(read_frames(0, 0, &in_order) == 0, "a read of nothing is nothing");
	CHECK(read_frames(2000, 480, &in_order) == 1520 && in_order && ring_dry == 1, "too little: all of it, once dry (%llu)",
	    (unsigned long long)ring_dry);
	hear(1000);
	CHECK(read_frames(480, 2000, &in_order) == 0, "and nothing again until 40 ms are there");
	hear(5000);
	CHECK(read_frames(480, 8000 - RING_KEEP, &in_order) == 480 && in_order && ring_skipped == 6000 - RING_KEEP,
	    "more than 100 ms waiting: the oldest skipped, the reader 40 ms behind (%llu skipped)", (unsigned long long)ring_skipped);
	hear(16384 - 8000 + 1000);      /* (frames 15464 to 17383 waiting: across the ring's end) */
	CHECK(read_frames(RING_KEEP, 17384 - RING_KEEP, &in_order) == RING_KEEP && in_order, "across the ring's end, in order");
	CHECK(ring_dry == 1, "a read of just what was there did not run dry (%llu)", (unsigned long long)ring_dry);
	skipped = ring_skipped;
	hear(RING_MOST);
	CHECK(read_frames(480, 17384, &in_order) == 480 && in_order && ring_skipped == skipped,
	    "100 ms waiting, and not more: nothing skipped (%llu)", (unsigned long long)(ring_skipped - skipped));
}

/* ---- the digital codec: which bolero, and the decimator ------------------------------------------------- */

#define VA_AT   0x3370000u              /* (the VA and TX macros of the model: memory, wherever they are) */
#define TX_AT   0x3220000u

static void
macros(uint32_t id0, uint32_t id1, uint32_t cfg0)
{
	va_macro.phys = VA_AT;
	tx_macro.phys = TX_AT;
	*cell(VA_AT + VA_CORE_ID_0) = id0;
	*cell(VA_AT + VA_CORE_ID_1) = id1;
	for (unsigned i = 0; i < 3; i++) {
		*cell(VA_AT + VA_SWR_MIC_CTL0 + 4 * i) = 0x11;
	}
	*cell(TX_AT + TX1_PATH_CFG0) = cfg0;
}

static uint32_t
at(uint64_t address)
{
	return *cell(address);
}

/* bolero 2.1 (its ID 02 0E): the SoundWire mics' clock dividers are the VA macro's, as va_macro_init() sets them,
 * and nothing of them is the TX macro's */
static void
bolero_2_1(void)
{
	macros(0x02, 0x0e, 0x00);
	CHECK(bolero_look() && strstr(log_text, "the digital codec is bolero 2.1") != NULL, "taken: %s", log_text);
	for (unsigned i = 0; i < 3; i++) {
		CHECK((at(VA_AT + VA_SWR_MIC_CTL0 + 4 * i) & 0xee) == 0xcc && found_va_mic[i] == 0x11, "SWR_MIC_CTL%u: %#x, found %#x", i,
		    at(VA_AT + VA_SWR_MIC_CTL0 + 4 * i), found_va_mic[i]);
	}
	CHECK(va_mic_taken, "to be put back");
	CHECK(dec1_on(), "the decimator on: %s", log_text);
	CHECK(written(TX_AT + TX_TOP_CSR_SWR_CTRL) == 0 && written(TX_AT + TX_SWR_MIC0_CTL) == 0, "no clocks in the TX macro");
	CHECK((at(TX_AT + TX_ADC_MUX1_CFG0) & 0x0f) == 1, "SWR_MIC0 into it (mic_input=1): %#x", at(TX_AT + TX_ADC_MUX1_CFG0));
}

/* bolero 2.0 (01 0F): nothing in the VA macro, the clocks in the TX macro once the decimator is on */
static void
bolero_2_0(void)
{
	macros(0x01, 0x0f, 0x00);
	CHECK(bolero_look() && strstr(log_text, "the digital codec is bolero 2.0") != NULL, "taken: %s", log_text);
	CHECK(written(VA_AT + VA_SWR_MIC_CTL0) == 0 && !va_mic_taken, "nothing in the VA macro");
	CHECK(dec1_on(), "the decimator on: %s", log_text);
	CHECK((at(TX_AT + TX_TOP_CSR_SWR_CTRL) & 1) && (at(TX_AT + TX_SWR_MIC0_CTL) & 0x0e) == 0x0c &&
	    (at(TX_AT + TX_SWR_MIC1_CTL) & 0x0e) == 0x0c, "the TX macro's clocks: %#x %#x", at(TX_AT + TX_TOP_CSR_SWR_CTRL),
	    at(TX_AT + TX_SWR_MIC0_CTL));
}

/* an ID of neither, or half of one: not taken, said, nothing written */
static void
bolero_unknown(void)
{
	static const uint32_t ids[][2] = { { 0x03, 0x0e }, { 0x01, 0x0e }, { 0x02, 0x0f }, { 0x00, 0x00 } };

	for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		macros(ids[i][0], ids[i][1], 0x00);
		CHECK(!bolero_look(), "%02x %02x: not taken", ids[i][0], ids[i][1]);
	}
	CHECK(strstr(log_text, "the digital codec is bolero OF AN UNKNOWN VERSION") != NULL, "said: %s", log_text);
	CHECK(write_count == 0, "nothing written: %u", write_count);
}

/* the decimator's high-pass filter found elsewhere than at 150 Hz: put there while it settles, its gate opened and
 * closed, what it was kept to be put back */
static void
decimator_filter_moved(void)
{
	macros(0x02, 0x0e, 0x00);
	CHECK(bolero_look() && dec1_on(), "on: %s", log_text);
	CHECK((at(TX_AT + TX1_PATH_CFG0) & 0x60) == 0x40 && hpf_cut_off == 0, "at 150 Hz, 0 kept: %#x", at(TX_AT + TX1_PATH_CFG0));
	CHECK(written(TX_AT + TX1_PATH_CFG0) == 3 && written(TX_AT + TX1_PATH_SEC2) == 2, "the filter set, its gate opened and closed: %u, %u",
	    written(TX_AT + TX1_PATH_CFG0), written(TX_AT + TX1_PATH_SEC2));
}

/* ... found at 150 Hz already: left there, the gate not touched */
static void
decimator_filter_in_place(void)
{
	macros(0x02, 0x0e, 0x40);
	CHECK(bolero_look() && dec1_on(), "on: %s", log_text);
	CHECK((at(TX_AT + TX1_PATH_CFG0) & 0x60) == 0x40 && hpf_cut_off == 2, "as it was: %#x", at(TX_AT + TX1_PATH_CFG0));
	CHECK(written(TX_AT + TX1_PATH_CFG0) == 2 && written(TX_AT + TX1_PATH_SEC2) == 0, "nothing more written: %u, %u",
	    written(TX_AT + TX1_PATH_CFG0), written(TX_AT + TX1_PATH_SEC2));
}

/* half a frame is nothing; one frame is one, and the next the next */
static void
reads_at_the_edges(void)
{
	struct uio half = { .base = (char *)got, .resid = 1, .to_kernel = false };
	bool in_order;

	ring = heard;
	hear(2000);
	mic_dev_read(0, &half, 0);
	CHECK(half.resid == 1, "one byte: nothing (%lld left)", (long long)half.resid);
	CHECK(read_frames(1, 0, &in_order) == 1 && in_order, "one frame: the first");
	CHECK(read_frames(1, 1, &in_order) == 1 && in_order, "and then the next");
}

/*
 * What the DSP says of the reads it was given (ASM_DATA_EVENT_READ_DONE_V2, or a result for one): taken only from the
 * recording's service, for a read's token and a period there is; a read done with an error, or said in fewer than its
 * 24 bytes, is counted as one; each marks its period ready.
 */
static void
the_dsps_read_events(void)
{
	uint32_t done[6] = { 0, 0x1000, 0, 7, 1920, 0 };
	uint32_t errors = read_errors, finished = reads_done;

	CHECK(!mic_events(APR_SVC_AFE, 0, ASM_DATA_EVENT_READ_DONE_V2, READ_TOKEN | 1, done, 24), "another service's: not ours");
	CHECK(!mic_events(APR_SVC_ASM, 0, ASM_DATA_EVENT_READ_DONE_V2, 1, done, 24), "a token that is not a read's: not ours");
	CHECK(!mic_events(APR_SVC_ASM, 0, ASM_DATA_EVENT_READ_DONE_V2, READ_TOKEN | MIC_PERIODS, done, 24),
	    "a period past the last: not ours (and nothing written past read_sizes)");
	CHECK(!mic_events(APR_SVC_ASM, 0, 0x12345, READ_TOKEN | 1, done, 24), "another event: not ours");
	CHECK(read_ready == 0 && reads_done == finished && read_errors == errors, "none of these counted");

	CHECK(mic_events(APR_SVC_ASM, 0, ASM_DATA_EVENT_READ_DONE_V2, READ_TOKEN | 3, done, 24), "a read done");
	CHECK(read_sizes[3] == 1920 && (read_ready & 1u << 3) && reads_done == finished + 1 && read_errors == errors,
	    "its size, its period ready, no error: %u", read_sizes[3]);
	CHECK(mic_events(APR_SVC_ASM, 0, ASM_DATA_EVENT_READ_DONE_V2, READ_TOKEN | (MIC_PERIODS - 1), done, 24) &&
	    read_sizes[MIC_PERIODS - 1] == 1920, "the last period");
	done[0] = 2;
	CHECK(mic_events(APR_SVC_ASM, 0, ASM_DATA_EVENT_READ_DONE_V2, READ_TOKEN | 4, done, 24) && read_errors == errors + 1,
	    "done with an error: counted");
	done[0] = 0;
	CHECK(mic_events(APR_SVC_ASM, 0, ASM_DATA_EVENT_READ_DONE_V2, READ_TOKEN | 5, done, 20) && read_errors == errors + 2 &&
	    read_sizes[5] == 0 && (read_ready & 1u << 5), "said short: an error, nothing in it, ready all the same");
	read_sizes[6] = 99;
	CHECK(mic_events(APR_SVC_ASM, 0, APR_BASIC_RSP_RESULT, READ_TOKEN | 6, done, 8) && read_errors == errors + 3 &&
	    read_sizes[6] == 0 && (read_ready & 1u << 6), "a result for a read: it was refused, the period ready and empty");
}

/* ---- what is given back when the DSP does not answer -------------------------------------------------- */

/* a start of the port the DSP did not answer: it may have started, and it is stopped with the rest */
static void
an_unanswered_start_stopped(void)
{
	dsp_will(DSP_RESULT, 0, 0);     /* the port's format */
	dsp_will(DSP_SILENT, 0, 0);     /* its start */
	dsp_port_on();
	dsp_off();
	CHECK(asked_since(2, AFE_PORT_CMD_DEVICE_STOP) == 1, "stopped: %u commands, %u of them a stop", dsp_asked,
	    asked_since(2, AFE_PORT_CMD_DEVICE_STOP));
}

/* a clock the DSP did not answer for: turned off with the rest, and the votes given back */
static void
an_unanswered_clock_turned_off(void)
{
	unsigned from;

	dsp_will(DSP_OWN_ANSWER, 0, 11);
	dsp_will(DSP_OWN_ANSWER, 0, 12);
	dsp_will(DSP_SILENT, 0, 0);     /* the core clock, on */
	for (int i = 0; i < 3; i++) {
		dsp_will(DSP_RESULT, 0, 0);     /* (its off, the votes given back) */
	}
	clocks_on();
	from = dsp_asked;
	clocks_off();
	CHECK(asked_since(from, AFE_SVC_CMD_SET_PARAM) == 1 && asked_since(from, AFE_CMD_REMOTE_LPASS_CORE_HW_DEVOTE_REQUEST) == 2,
	    "the core clock off, the votes given back: %u clocks, %u votes", asked_since(from, AFE_SVC_CMD_SET_PARAM),
	    asked_since(from, AFE_CMD_REMOTE_LPASS_CORE_HW_DEVOTE_REQUEST));
	dsp_will(DSP_OWN_ANSWER, 0, 11);
	dsp_will(DSP_OWN_ANSWER, 0, 12);
	dsp_will(DSP_RESULT, 0, 0);     /* the core clock, on */
	dsp_will(DSP_SILENT, 0, 0);     /* the NPL clock, on */
	clocks_on();
	from = dsp_asked;
	clocks_off();
	CHECK(asked_since(from, AFE_SVC_CMD_SET_PARAM) == 2, "both clocks off: %u", asked_since(from, AFE_SVC_CMD_SET_PARAM));
}

/* a stream the DSP did not answer the open of: closed; and said so, not with what an earlier command was answered */
static void
an_unanswered_stream_closed(void)
{
	unsigned from;

	dsp_memory_there = true;
	dsp_will(DSP_OWN_ANSWER, 0, 0x77);      /* our memory mapped: its handle */
	dsp_will(DSP_SILENT, 0, 0);             /* the stream's open */
	dsp_path_on();
	CHECK(logged("mic: the stream, opened for reading: NO ANSWER") && !logged("0x10d93"), "said: %s", log_text);
	from = dsp_asked;
	dsp_off();
	CHECK(asked_since(from, ASM_STREAM_CMD_CLOSE) == 1 && asked_since(from, ASM_CMD_SHARED_MEM_UNMAP_REGIONS) == 1,
	    "closed and unmapped: %u, %u", asked_since(from, ASM_STREAM_CMD_CLOSE), asked_since(from, ASM_CMD_SHARED_MEM_UNMAP_REGIONS));
}

/*
 * Our memory's map and a device path the DSP did not answer: what it may hold cannot be given back without the
 * answer's handle, and it is said that the microphone stays off
 */
static void
unanswered_handles_said(void)
{
	unsigned from;

	dsp_memory_there = true;
	dsp_will(DSP_SILENT, 0, 0);             /* the map */
	dsp_path_on();
	CHECK(logged("our periods for the microphone, mapped for the DSP: NO ANSWER; if the DSP did it") &&
	    logged("stays off until the run ends"), "said: %s", log_text);
	from = dsp_asked;
	dsp_off();
	CHECK(dsp_asked == from, "nothing to unmap without the handle: %u commands", dsp_asked - from);
	dsp_will(DSP_OWN_ANSWER, 0, 0x77);      /* the map */
	dsp_will(DSP_RESULT, 0, 0);             /* the stream's open */
	dsp_will(DSP_RESULT, 0, 0);             /* its format */
	dsp_will(DSP_SILENT, 0, 0);             /* the device path's open */
	dsp_path_on();
	CHECK(logged("a device path from the port: NO ANSWER; if the DSP did it"), "said: %s", log_text);
	from = dsp_asked;
	dsp_off();
	CHECK(asked_since(from, ASM_STREAM_CMD_CLOSE) == 1 && asked_since(from, ADM_CMD_DEVICE_CLOSE_V5) == 0,
	    "the stream closed, no path without its handle: %u, %u", asked_since(from, ASM_STREAM_CMD_CLOSE),
	    asked_since(from, ADM_CMD_DEVICE_CLOSE_V5));
}

/* ---- mic=4: bring-ups that fail ---------------------------------------------------------------------------- */

/* the driver's thread at mic=4, Darwin's audio server with the device open */
static void
darwin_records(void)
{
	thread_t thread;

	blocks_from_board();
	level = 4;
	running = true;
	darwin_wants = 1;
	CHECK(kernel_thread_start(mic_thread, NULL, &thread) == KERN_SUCCESS, "started");
}

static void
the_run_ends(void)
{
	hakios_mic_dsp_down();
	EVENTUALLY(!running, 3000, "the thread is done");
}

/*
 * A bring-up that fails short of a write that does not read back (here the first vote not given): once while the
 * device stays open; again only once Darwin has closed and opened it, and not within a second; after the third, no more
 */
static void
darwins_bring_up_failed(void)
{
	darwin_records();
	usleep(300000);
	CHECK(dsp_asked == 1 && logged("mic: Darwin's recording 1 BROKE OFF"), "once while the device stays open: %u commands",
	    dsp_asked);
	darwin_wants = 0;
	usleep(200000);
	darwin_wants = 1;
	usleep(200000);
	CHECK(dsp_asked == 1, "closed and opened again: not again within a second (%u)", dsp_asked);
	EVENTUALLY(dsp_asked == 2, 2000, "and then again: %u", dsp_asked);
	darwin_wants = 0;
	usleep(200000);
	darwin_wants = 1;
	EVENTUALLY(!running, 3000, "after the third that broke off, no more: %u commands", dsp_asked);
	CHECK(dsp_asked == 3 && logged("three recordings broke off"), "%u commands: %s", dsp_asked, log_text);
	the_run_ends();
}

/* the first vote not answered: the DSP may hold it, and the microphone stays off for the rest of the run */
static void
darwins_vote_unanswered(void)
{
	dsp_will(DSP_SILENT, 0, 0);
	darwin_records();
	EVENTUALLY(!running, 2000, "no more after it: %u commands", dsp_asked);
	CHECK(dsp_asked == 1 && logged("NO ANSWER; if the DSP did it"), "%u commands: %s", dsp_asked, log_text);
	the_run_ends();
}

/* ---- a stream whose reads stop -------------------------------------------------------------------------- */

static volatile bool reads_go_on;
static volatile int after_the_first;    /* 0: nothing more comes back, 1: every read refused */

/* the DSP: the first period read, then nothing, or refusals */
static void *
the_dsps_reads(void *unused)
{
	uint32_t done[6] = { 0, 0, 0, 0, MIC_PERIOD_BYTES, 0 };
	unsigned p = 1;

	usleep(20000);
	mic_events(APR_SVC_ASM, 0, ASM_DATA_EVENT_READ_DONE_V2, READ_TOKEN | 0, done, 24);
	while (reads_go_on) {
		usleep(3000);
		if (after_the_first == 1) {
			mic_events(APR_SVC_ASM, 0, APR_BASIC_RSP_RESULT, READ_TOKEN | (p++ % MIC_PERIODS), done, 8);
		}
	}
	return unused;
}

/* listened to for 4 s: how long it went on */
static uint64_t
listened_ms(void)
{
	static int16_t samples[MIC_PERIODS * 4096 / 2];
	pthread_t dsp;
	uint64_t began = mach_absolute_time(), ns;

	codec();
	mic_memory = (uintptr_t)samples;
	mic_secs = 4;
	reads_go_on = true;
	pthread_create(&dsp, NULL, the_dsps_reads, NULL);
	listen(false);
	reads_go_on = false;
	pthread_join(dsp, NULL);
	absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
	return ns / NSEC_PER_MSEC;
}

/* the reads refused after the first: 1.5 s and it is stopped, and said */
static void
reads_refused_after_the_first(void)
{
	uint64_t ms;

	after_the_first = 1;
	ms = listened_ms();
	CHECK(ms >= 1500 && ms < 2500 && logged("mic: NOTHING HEARD in 1.5 s (1 reads came back, "), "%llu ms: %s",
	    (unsigned long long)ms, log_text);
}

/* the DSP stops answering the reads after the first: the same */
static void
reads_unanswered_after_the_first(void)
{
	uint64_t ms;

	after_the_first = 0;
	ms = listened_ms();
	CHECK(ms >= 1500 && ms < 2500 && logged("mic: NOTHING HEARD in 1.5 s (1 reads came back, 0 errors)"), "%llu ms: %s",
	    (unsigned long long)ms, log_text);
}

/* ---- the decimator, and the ring Darwin reads: put back, taken back -------------------------------------- */

/* a recording that ends before the filter has settled: the cut-off as found put back, and found so by the next */
static void
decimator_filter_put_back(void)
{
	macros(0x02, 0x0e, 0x00);
	*cell(TX_AT + TX1_PATH_SEC2) = 0x00;
	CHECK(bolero_look() && dec1_on(), "on: %s", log_text);
	dec1_off();                     /* (before dec1_settled()) */
	va_mic_off();
	CHECK((at(TX_AT + TX1_PATH_CFG0) & 0x60) == 0 && at(TX_AT + TX1_PATH_SEC2) == 0, "as found: CFG0 %#x, SEC2 %#x",
	    at(TX_AT + TX1_PATH_CFG0), at(TX_AT + TX1_PATH_SEC2));
	CHECK(bolero_look() && dec1_on(), "on again");
	CHECK(hpf_cut_off == 0, "the next finds the cut-off it was: %u", hpf_cut_off);
}

/* a read that faults takes nothing: what it would have given is the next read's, and it is not counted as one */
static void
a_read_that_faults(void)
{
	struct uio uio = { .base = (char *)got, .resid = 480 * 2, .to_kernel = false };
	bool in_order;

	ring = heard;
	hear(2000);
	fake_uiomove_fault_at = fake_uiomove_calls + 1;
	CHECK(mic_dev_read(0, &uio, 0) == EFAULT, "the fault");
	CHECK(ring_read == 0 && !ring_primed && ring_dry == 0, "nothing taken: %llu, primed %d, dry %llu",
	    (unsigned long long)ring_read, ring_primed, (unsigned long long)ring_dry);
	CHECK(read_frames(480, 0, &in_order) == 480 && in_order, "the next read from the first frame");
	fake_uiomove_fault_at = fake_uiomove_calls + 1;
	uio = (struct uio){ .base = (char *)got, .resid = 4000 * 2, .to_kernel = false };
	CHECK(mic_dev_read(0, &uio, 0) == EFAULT && ring_dry == 0 && ring_primed, "too little, and a fault: not counted dry");
	CHECK(read_frames(480, 480, &in_order) == 480 && in_order, "and the next from where the last good one ended");

	/* across the ring's end: the first part given, the second faults; the first is taken, the second not */
	put_so_far = ring_written = RING_FRAMES - 100;
	ring_read = RING_FRAMES - 100;
	hear(RING_KEEP);
	fake_uiomove_fault_at = fake_uiomove_calls + 2;
	uio = (struct uio){ .base = (char *)got, .resid = 480 * 2, .to_kernel = false };
	CHECK(mic_dev_read(0, &uio, 0) == EFAULT && ring_read == RING_FRAMES, "the part before the end taken: %llu",
	    (unsigned long long)ring_read);
	CHECK(read_frames(380, RING_FRAMES, &in_order) == 380 && in_order, "the rest the next read's");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "nothing_after_the_run_ends", nothing_after_the_run_ends },
		{ "the_socs_pins", the_socs_pins }, { "the_socs_hctl", the_socs_hctl },
		{ "nothing_to_say_again", nothing_to_say_again }, { "the_run_said_again", the_run_said_again },
		{ "the_latest_lines_said_again", the_latest_lines_said_again }, { "the_dsps_answers", the_dsps_answers },
		{ "an_answer_of_its_own", an_answer_of_its_own }, { "the_votes", the_votes },
		{ "the_first_vote_refused", the_first_vote_refused }, { "the_link_up", the_link_up }, { "the_link_down", the_link_down },
		{ "reads_over_the_bus", reads_over_the_bus }, { "an_answer_for_another_read", an_answer_for_another_read },
		{ "a_full_fifo", a_full_fifo }, { "an_answer_a_while_after", an_answer_a_while_after }, { "no_answer_at_all", no_answer_at_all }, { "the_codec_found", the_codec_found }, { "the_codec_not_there", the_codec_not_there },
		{ "the_codec_set_up", the_codec_set_up }, { "the_codec_set_up_from_nothing", the_codec_set_up_from_nothing },
		{ "adc1_turned_on", adc1_turned_on }, { "the_codec_left_as_it_is", the_codec_left_as_it_is },
		{ "the_codec_does_not_take_it", the_codec_does_not_take_it }, { "the_ring_darwin_reads", the_ring_darwin_reads },
		{ "reads_at_the_edges", reads_at_the_edges }, { "the_dsps_read_events", the_dsps_read_events },
		{ "bolero_2_1", bolero_2_1 }, { "bolero_2_0", bolero_2_0 }, { "bolero_unknown", bolero_unknown },
		{ "decimator_filter_moved", decimator_filter_moved }, { "decimator_filter_in_place", decimator_filter_in_place },
		{ "an_unanswered_start_stopped", an_unanswered_start_stopped },
		{ "an_unanswered_clock_turned_off", an_unanswered_clock_turned_off },
		{ "an_unanswered_stream_closed", an_unanswered_stream_closed }, { "unanswered_handles_said", unanswered_handles_said },
		{ "darwins_bring_up_failed", darwins_bring_up_failed }, { "darwins_vote_unanswered", darwins_vote_unanswered },
		{ "reads_refused_after_the_first", reads_refused_after_the_first },
		{ "reads_unanswered_after_the_first", reads_unanswered_after_the_first },
		{ "decimator_filter_put_back", decimator_filter_put_back }, { "a_read_that_faults", a_read_that_faults },
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
