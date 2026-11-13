/*
 * What the drivers of PMICs share (kernel/overlay/bsd/dev/hakios_spmi.h), by itself, against the
 * model of the arbiter (spmi_model.h): that the arbiter is mapped where the board's SoC file says and
 * for the system it names, what a read and a write put on the bus, and what each status is said to
 * mean. The drivers' own tests cover what each does with it.
 *
 *   spmi_test SCENARIO      one scenario a process: the model keeps its state in statics
 */
#include <unistd.h>

#include "fakekernel.h"
#include "spmi_model.h"

static uint32_t rd(vm_offset_t address) { return spmi_model_rd(address); }
static void wr(vm_offset_t address, uint32_t value) { spmi_model_wr(address, value); }
static unsigned maps;
vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { maps++; return phys; }

#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#include "hakios_board.c"
#include "hakios_spmi.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define SAME(a, b) ((a) != NULL && strcmp((a), (b)) == 0)

static struct spmi_peripheral chip = { .id = 0x1c4 }, other = { .id = 0x2ee };
static struct hakios_spmi arb;

/* the model where the board's SoC file has the arbiter; channel 5 is another system's, 9 this one's */
static void
phone(const char *name)
{
	const struct hakios_board *b;

	board = name;
	b = hakios_board();
	spmi.core = b->spmi.core;
	spmi.cnfg = b->spmi.cnfg;
	spmi.channels = b->spmi.channels;
	spmi.ee = b->spmi.ee;
	spmi.observer = b->spmi.observer + 0x10000 * spmi.ee;
	spmi_channel(5, &chip, spmi.ee + 1);
	spmi_channel(9, &chip, spmi.ee);
	spmi_channel(10, &other, spmi.ee);
	for (unsigned i = 0; i < 256; i++) {
		chip.reg[i] = (uint8_t)(i ^ 0xa5);
	}
}

/* the arbiter of a board: its version, how many channels, who we are to it */
static void
mapped(void)
{
	phone("phone-video");
	CHECK(hakios_spmi_map(&arb), "a version 5 arbiter is taken");
	CHECK(arb.version == 0x50000000 && arb.channels == SPMI_CHANNELS && arb.system == 0, "version %#x, %u channels, system %u",
	    arb.version, arb.channels, arb.system);
	CHECK(SPMI_ID_OF(hakios_spmi_channel_map(&arb, 9)) == chip.id && hakios_spmi_channel_map(&arb, 30) == 0, "the table says whose a channel is");
	CHECK(hakios_spmi_channel_owner(&arb, 5) == 1 && hakios_spmi_channel_owner(&arb, 9) == 0, "... and who writes through it");
	CHECK(spmi.elsewhere == 0, "%u accesses where nothing of the arbiter is", spmi.elsewhere);
}

/* an arbiter that says nothing sensible of its number of channels: the most there can be are looked through */
static void
channels_not_said(void)
{
	phone("phone-video");
	spmi.says_channels = 0 + 1;             /* none */
	CHECK(hakios_spmi_map(&arb) && arb.channels == 512, "no channels said: %u", arb.channels);
	spmi.says_channels = 0x7ff + 1;         /* more than an arbiter has */
	CHECK(hakios_spmi_map(&arb) && arb.channels == 512, "2047 said: %u", arb.channels);
	spmi.says_channels = 512 + 1;
	CHECK(hakios_spmi_map(&arb) && arb.channels == 512, "512 said: %u", arb.channels);
	spmi.says_channels = 40 + 1;
	CHECK(hakios_spmi_map(&arb) && arb.channels == 40, "40 said: %u", arb.channels);
	CHECK(spmi.elsewhere == 0, "%u accesses where nothing of the arbiter is", spmi.elsewhere);
}

/* a SoC whose arbiter is elsewhere and where this system is its third */
static void
another_soc(void)
{
	uint8_t byte = 0;

	phone("phone-keys");
	CHECK(hakios_spmi_map(&arb) && arb.system == 2, "system %u", arb.system);
	CHECK(hakios_spmi_channel_owner(&arb, 9) == 2, "the owner is read from that SoC's block");
	CHECK(hakios_spmi_read(hakios_spmi_observer(&arb, 9), 0x10, &byte, 1) == NULL && byte == (0x10 ^ 0xa5),
	    "a read goes through the third system's part of the observer: %#x", byte);
	CHECK(spmi.elsewhere == 0, "%u accesses where nothing of the arbiter is", spmi.elsewhere);
}

/* a board with no spmi { }: nothing is mapped, nothing is touched */
static void
no_arbiter(void)
{
	board = "nobody-described-this";
	CHECK(!hakios_spmi_map(&arb) && arb.version == 0, "no arbiter is taken (version %#x)", arb.version);
	CHECK(hakios_spmi_map_write(3) == 0, "... and no channel to write through");
	CHECK(maps == 0 && spmi.elsewhere == 0 && spmi.transfers == 0, "%u mappings, %u accesses", maps, spmi.elsewhere);
}

/* an arbiter of a version whose registers are elsewhere: said, with the version */
static void
another_version(void)
{
	phone("phone-video");
	spmi.version = 0x70000000;
	CHECK(!hakios_spmi_map(&arb) && arb.version == 0x70000000, "version 7 is not taken (%#x)", arb.version);
	spmi.version = 0x30000000;
	CHECK(!hakios_spmi_map(&arb) && arb.version == 0x30000000, "nor version 3 (%#x)", arb.version);
	spmi.version = 0x50010000;
	CHECK(hakios_spmi_map(&arb), "a later revision of 5 is");
}

/* reads: one register, eight, the first in the lowest byte; a read that fails leaves the bytes alone */
static void
reads(void)
{
	uint8_t bytes[8] = { 0 }, kept[3] = { 1, 2, 3 };
	const char *problem;

	phone("phone-video");
	CHECK(hakios_spmi_map(&arb), "mapped");
	CHECK(hakios_spmi_read(hakios_spmi_observer(&arb, 9), 0x40, bytes, 1) == NULL && bytes[0] == (0x40 ^ 0xa5) && bytes[1] == 0,
	    "one register: %#x %#x", bytes[0], bytes[1]);
	CHECK(hakios_spmi_read(hakios_spmi_observer(&arb, 5), 0x41, bytes, 1) == NULL && bytes[0] == (0x41 ^ 0xa5),
	    "a read needs no channel of our own: %#x", bytes[0]);
	CHECK(hakios_spmi_read(hakios_spmi_observer(&arb, 9), 0x08, bytes, 8) == NULL, "eight registers");
	for (unsigned i = 0; i < 8; i++) {
		CHECK(bytes[i] == ((0x08 + i) ^ 0xa5), "byte %u is register %#x's: %#x", i, 0x08 + i, bytes[i]);
	}
	chip.silent = true;
	problem = hakios_spmi_read(hakios_spmi_observer(&arb, 9), 0x08, kept, 3);
	CHECK(SAME(problem, SPMI_SAYS_FAILED), "nobody at the address: %s", problem ? problem : "no problem");
	CHECK(kept[0] == 1 && kept[1] == 2 && kept[2] == 3, "the bytes are as they were");
	CHECK(spmi.transfers == 4 && spmi.writes == 0 && spmi.elsewhere == 0, "%u transfers, %u writes, %u elsewhere", spmi.transfers,
	    spmi.writes, spmi.elsewhere);
}

/* writes: one register through this system's channel; through another's the arbiter refuses, and that is said */
static void
writes(void)
{
	const char *problem;
	vm_offset_t ours, theirs;

	phone("phone-video");
	CHECK(hakios_spmi_map(&arb), "mapped");
	ours = hakios_spmi_map_write(9);
	theirs = hakios_spmi_map_write(5);
	CHECK(ours != 0 && theirs != 0 && ours != theirs, "each channel has its own registers for writing");
	CHECK(hakios_spmi_write(ours, 0x46, 0x80) == NULL && spmi_get(&chip, 0x46) == 0x80, "the register has the byte: %#x",
	    spmi_get(&chip, 0x46));
	CHECK(spmi_get(&chip, 0x45) == (0x45 ^ 0xa5) && spmi_get(&chip, 0x47) == (0x47 ^ 0xa5) && other.writes == 0,
	    "and nothing beside it");
	problem = hakios_spmi_write(theirs, 0x46, 0x11);
	CHECK(SAME(problem, SPMI_SAYS_DENIED) && spmi.denied == 1, "another system's channel: %s", problem ? problem : "no problem");
	CHECK(spmi_get(&chip, 0x46) == 0x80 && chip.writes == 1, "which changed nothing");
	chip.silent = true;
	problem = hakios_spmi_write(ours, 0x46, 0x00);
	CHECK(SAME(problem, SPMI_SAYS_FAILED), "nobody at the address: %s", problem ? problem : "no problem");
	CHECK(spmi.elsewhere == 0, "%u accesses where nothing of the arbiter is", spmi.elsewhere);
}

/* an arbiter that never ends a transfer: given up on after milliseconds, not waited for for ever */
static void
no_answer(void)
{
	uint8_t byte = 0x5a;
	const char *problem;
	uint64_t start, taken;

	phone("phone-video");
	CHECK(hakios_spmi_map(&arb), "mapped");
	spmi.deaf = true;
	start = mach_absolute_time();
	problem = hakios_spmi_read(hakios_spmi_observer(&arb, 9), 0x08, &byte, 1);
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &taken);
	CHECK(SAME(problem, SPMI_SAYS_NO_ANSWER) && byte == 0x5a, "said, and the byte left alone: %s", problem ? problem : "no problem");
	CHECK(taken >= 2000000 && taken < 1000000000, "after the two milliseconds a transfer may take: %llu ns", (unsigned long long)taken);
}

/* what a status means, the worst first */
static void
statuses(void)
{
	CHECK(hakios_spmi_problem(STATUS_DONE) == NULL, "done, and nothing else: no problem");
	CHECK(SAME(hakios_spmi_problem(0), SPMI_SAYS_NO_ANSWER), "not done");
	CHECK(SAME(hakios_spmi_problem(STATUS_FAILURE | STATUS_DENIED | STATUS_DROPPED), SPMI_SAYS_NO_ANSWER), "not done, whatever else");
	CHECK(SAME(hakios_spmi_problem(STATUS_DONE | STATUS_DENIED), SPMI_SAYS_DENIED), "refused");
	CHECK(SAME(hakios_spmi_problem(STATUS_DONE | STATUS_FAILURE), SPMI_SAYS_FAILED), "failed");
	CHECK(SAME(hakios_spmi_problem(STATUS_DONE | STATUS_DROPPED), SPMI_SAYS_DROPPED), "dropped");
	CHECK(SAME(hakios_spmi_problem(STATUS_DONE | STATUS_DENIED | STATUS_FAILURE), SPMI_SAYS_DENIED), "refused comes before failed");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "mapped", mapped }, { "channels_not_said", channels_not_said }, { "another_soc", another_soc }, { "no_arbiter", no_arbiter }, { "another_version", another_version },
		{ "reads", reads }, { "writes", writes }, { "no_answer", no_answer }, { "statuses", statuses },
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
			fprintf(stderr, "%-20s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
