/*
 * What the drivers of PMICs share (drivers/board/spmi.h), by itself, against the
 * model of the arbiter (spmi_model.h), of versions 5, 7 and 8: that the arbiter is mapped where the
 * board's SoC file says and for the system it names, what a read and a write put on the bus, and what each status is said to
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
#include "board/board.c"
#include "board/spmi.h"

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

/* ---- version 8 ------------------------------------------------------------------------------------------ */

/* a SoC file whose spmi { } has a channel-map: the model's tables where it says, the arbiter of version 8 */
static void
version_8(void)
{
	phone("phone-scmi");
	spmi.channel_map = hakios_board()->spmi.channel_map;
	spmi.observer = hakios_board()->spmi.observer;
	spmi.version = 0x80000000;
}

/* taken: its channels from its own register (13 bits of it), whose each one is from the table apart */
static void
v8_mapped(void)
{
	version_8();
	spmi.says_channels = 1000 + 1;
	CHECK(hakios_spmi_map(&arb) && arb.version == 0x80000000 && arb.channels == 1000 && arb.map != 0, "taken: %#x, %u channels",
	    arb.version, arb.channels);
	CHECK(SPMI_ID_OF(hakios_spmi_channel_map(&arb, 9)) == chip.id && hakios_spmi_channel_map(&arb, 30) == 0,
	    "the table apart says whose a channel is: %#x", hakios_spmi_channel_map(&arb, 9));
	CHECK(hakios_spmi_channel_owner(&arb, 5) == 1 && hakios_spmi_channel_owner(&arb, 9) == 0, "... and its cnfg who writes through it");
	CHECK(spmi.elsewhere == 0 && spmi.past_the_table == 0, "%u accesses where nothing of the arbiter is", spmi.elsewhere);
}

/* reads through the observer at 0x20 a channel, writes at 0x200 a channel, as version 5's otherwise */
static void
v8_reads_and_writes(void)
{
	uint8_t bytes[2] = { 0 };
	vm_offset_t ours;

	version_8();
	CHECK(hakios_spmi_map(&arb), "mapped");
	CHECK(hakios_spmi_observer(&arb, 9) == arb.observer + 9 * 0x20, "channel 9 read at 0x120 into the observer");
	CHECK(hakios_spmi_read(hakios_spmi_observer(&arb, 9), 0x40, bytes, 2) == NULL && bytes[0] == (0x40 ^ 0xa5) &&
	    bytes[1] == (0x41 ^ 0xa5), "two registers read: %#x %#x", bytes[0], bytes[1]);
	ours = hakios_spmi_map_write(&arb, 9);
	CHECK(ours == hakios_board()->spmi.channels + 9 * 0x200, "channel 9 written at 0x1200 into the channels");
	CHECK(hakios_spmi_write(ours, 0x46, 0x80) == NULL && spmi_get(&chip, 0x46) == 0x80 && chip.writes == 1, "written");
	CHECK(SAME(hakios_spmi_write(hakios_spmi_map_write(&arb, 5), 0x46, 0x11), SPMI_SAYS_DENIED), "another system's: refused");
	CHECK(spmi.elsewhere == 0, "%u accesses where nothing of the arbiter is", spmi.elsewhere);
}

/* what this code does not know of version 8: an earlier version where the SoC file says 8, another system than
 * the first, an arbiter that says it has no channels */
static void
v8_not_taken(void)
{
	static struct hakios_board copy;

	version_8();
	spmi.version = 0x70000000;
	CHECK(!hakios_spmi_map(&arb), "version 7 with a table of channels: not taken");
	spmi.version = 0x80000000;
	spmi.says_channels = 0 + 1;
	CHECK(!hakios_spmi_map(&arb), "no channels: not taken");
	spmi.says_channels = 0x2000 + 1;
	CHECK(!hakios_spmi_map(&arb), "8192, which its 13 bits say as none: not taken");
	spmi.says_channels = 0x2000;
	CHECK(hakios_spmi_map(&arb) && arb.channels == 0x1fff, "8191, as many as the tables hold: taken (%u)", arb.channels);
	copy = *hakios_board();
	copy.spmi.ee = 1;
	found = &copy;
	CHECK(!hakios_spmi_map(&arb), "this system the arbiter's second: not taken");
	copy.spmi.ee = 0;
	CHECK(hakios_spmi_map(&arb), "(the first again: taken)");
	copy.spmi.bus = 1;
	CHECK(!hakios_spmi_map(&arb), "a second bus of a version 8 arbiter: not taken");
}

/* ---- version 7 ------------------------------------------------------------------------------------------ */

/* (the same address as the chip's, on the arbiter's other bus: what a read or a write through the wrong bus's channel finds) */
static struct spmi_peripheral namesake = { .id = 0x1c4 };
static struct hakios_board v7_board;

/*
 * A SoC file whose arbiter says it is of version 7, the phone's PMICs on its bus `bus', this system its `ee':
 * phone-buses' (the second bus, the second system), or that with the two changed. The bus named has 40
 * channels, the other 24; what phone() put in the table is the named bus's, and the other bus has a chip at
 * the same address, through channels of the same numbers.
 */
static void
version_7(uint32_t bus, uint32_t ee)
{
	board = "phone-buses";
	v7_board = *hakios_board();
	v7_board.spmi.bus = bus;
	v7_board.spmi.ee = ee;
	found = &v7_board;
	phone("phone-buses");
	spmi_version_7(v7_board.spmi.observer, bus, 24, 40);
	if (bus == 1) {
		spmi_channel(5, &namesake, ee);
		spmi_channel(9, &namesake, ee + 2);
		spmi_channel(11, &other, ee);
		namesake.bus = 0;
		for (unsigned i = 0; i < 256; i++) {
			namesake.reg[i] = (uint8_t)(i ^ 0x3c);
		}
	}
}

/* the second bus, as the fixture's SoC file has it: its channels counted from 0, after the first bus's in the arbiter's tables */
static void
v7_mapped(void)
{
	version_7(1, 1);
	CHECK(hakios_spmi_map(&arb) && arb.version == 0x70000000 && arb.channels == 40 && arb.first == 24 && arb.system == 1,
	    "taken: %#x, %u channels after %u", arb.version, arb.channels, arb.first);
	CHECK(SPMI_ID_OF(hakios_spmi_channel_map(&arb, 9)) == chip.id && SPMI_ID_OF(hakios_spmi_channel_map(&arb, 10)) == other.id &&
	    hakios_spmi_channel_map(&arb, 11) == 0 && hakios_spmi_channel_map(&arb, 30) == 0,
	    "the table says whose a channel of this bus is: %#x, %#x", hakios_spmi_channel_map(&arb, 9), hakios_spmi_channel_map(&arb, 11));
	CHECK(hakios_spmi_channel_owner(&arb, 5) == 2 && hakios_spmi_channel_owner(&arb, 9) == 1,
	    "... and the bus's cnfg who writes through it: %u, %u", hakios_spmi_channel_owner(&arb, 5), hakios_spmi_channel_owner(&arb, 9));
	for (uint32_t n = 0; n < arb.channels; n++) {
		(void)hakios_spmi_channel_map(&arb, n);
		(void)hakios_spmi_channel_owner(&arb, n);
	}
	CHECK(spmi.elsewhere == 0 && spmi.past_the_table == 0, "%u accesses where nothing of the arbiter is, %u of the other bus's",
	    spmi.elsewhere, spmi.past_the_table);
}

/* reads through this system's part of the observer (0x8000 a system) at 0x20 a channel, writes at 0x1000 a channel,
 * both by the arbiter's number of the channel; the chip of the same address on the first bus is not what answers */
static void
v7_reads_and_writes(void)
{
	uint8_t bytes[2] = { 0 };
	vm_offset_t ours;

	version_7(1, 1);
	CHECK(hakios_spmi_map(&arb), "mapped");
	CHECK(hakios_spmi_observer(&arb, 9) == hakios_board()->spmi.observer + 0x8000 + (24 + 9) * 0x20,
	    "channel 9 of the bus read at 0x8000 + 33 * 0x20 into the observer");
	CHECK(hakios_spmi_read(hakios_spmi_observer(&arb, 9), 0x40, bytes, 2) == NULL && bytes[0] == (0x40 ^ 0xa5) &&
	    bytes[1] == (0x41 ^ 0xa5), "two registers read, of this bus's chip: %#x %#x", bytes[0], bytes[1]);
	ours = hakios_spmi_map_write(&arb, 9);
	CHECK(ours == hakios_board()->spmi.channels + (24 + 9) * 0x1000, "channel 9 of the bus written at 33 * 0x1000 into the channels");
	CHECK(hakios_spmi_write(ours, 0x46, 0x80) == NULL && spmi_get(&chip, 0x46) == 0x80 && chip.writes == 1, "written");
	CHECK(namesake.writes == 0 && spmi_get(&namesake, 0x46) == (0x46 ^ 0x3c), "... and not to the other bus's chip");
	CHECK(SAME(hakios_spmi_write(hakios_spmi_map_write(&arb, 5), 0x46, 0x11), SPMI_SAYS_DENIED), "another system's: refused");
	CHECK(spmi.elsewhere == 0 && spmi.past_the_table == 0, "%u accesses where nothing of the arbiter is", spmi.elsewhere);
}

/* the first bus, this system the arbiter's first: nothing before its channels, the second bus's number not asked for */
static void
v7_first_bus(void)
{
	uint8_t byte = 0;

	version_7(0, 0);
	CHECK(hakios_spmi_map(&arb) && arb.channels == 40 && arb.first == 0 && arb.system == 0, "taken: %u channels after %u",
	    arb.channels, arb.first);
	CHECK(SPMI_ID_OF(hakios_spmi_channel_map(&arb, 9)) == chip.id && hakios_spmi_channel_owner(&arb, 5) == 1 &&
	    hakios_spmi_channel_owner(&arb, 9) == 0, "its tables");
	CHECK(hakios_spmi_observer(&arb, 9) == hakios_board()->spmi.observer + 9 * 0x20, "channel 9 read at 9 * 0x20 into the observer");
	CHECK(hakios_spmi_read(hakios_spmi_observer(&arb, 9), 0x10, &byte, 1) == NULL && byte == (0x10 ^ 0xa5), "read: %#x", byte);
	CHECK(hakios_spmi_map_write(&arb, 9) == hakios_board()->spmi.channels + 9 * 0x1000, "channel 9 written at 9 * 0x1000");
	CHECK(hakios_spmi_write(hakios_spmi_map_write(&arb, 9), 0x46, 0x80) == NULL && spmi_get(&chip, 0x46) == 0x80, "written");
	spmi.version = 0x70010000;
	CHECK(hakios_spmi_map(&arb) && arb.channels == 40, "a later revision of 7 is taken too");
	CHECK(spmi.elsewhere == 0 && spmi.past_the_table == 0, "%u accesses where nothing of the arbiter is", spmi.elsewhere);
}

/* what this code does not take of version 7: a third bus, a bus the arbiter says has no channels, more channels
 * than its tables hold; and a second bus of an arbiter that is of version 5 */
static void
v7_not_taken(void)
{
	version_7(1, 1);
	spmi.bus_channels[1] = 0;
	CHECK(!hakios_spmi_map(&arb) && arb.version == 0x70000000, "a second bus without channels: not taken");
	spmi.bus_channels[1] = 40;
	spmi.bus_channels[0] = 473;
	CHECK(!hakios_spmi_map(&arb), "473 + 40 channels, more than 512: not taken");
	spmi.bus_channels[0] = 472;
	CHECK(hakios_spmi_map(&arb) && arb.first == 472, "472 + 40: taken");
	spmi.bus_channels[0] = 24;
	v7_board.spmi.bus = 2;
	CHECK(!hakios_spmi_map(&arb), "a third bus: not taken");
	v7_board.spmi.bus = 0;
	spmi.bus_channels[0] = 0;
	CHECK(!hakios_spmi_map(&arb), "a first bus without channels: not taken");
	CHECK(spmi.transfers == 0, "nothing asked of a PMIC meanwhile");
}

/* a SoC file that names the second bus, and an arbiter of version 5, which has one */
static void
second_bus_of_version_5(void)
{
	phone("phone-buses");
	CHECK(hakios_board()->spmi.bus == 1, "(the SoC file names the second bus)");
	CHECK(!hakios_spmi_map(&arb) && arb.version == 0x50000000, "not taken (%#x)", arb.version);
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
	CHECK(hakios_spmi_map_write(&arb, 3) == 0, "... and no channel to write through");
	CHECK(maps == 0 && spmi.elsewhere == 0 && spmi.transfers == 0, "%u mappings, %u accesses", maps, spmi.elsewhere);
}

/* an arbiter of a version whose registers are elsewhere: said, with the version */
static void
another_version(void)
{
	phone("phone-video");
	spmi.version = 0x80000000;
	CHECK(!hakios_spmi_map(&arb) && arb.version == 0x80000000, "version 8 without its table of channels is not taken (%#x)", arb.version);
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
	ours = hakios_spmi_map_write(&arb, 9);
	theirs = hakios_spmi_map_write(&arb, 5);
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
		{ "v8_mapped", v8_mapped }, { "v8_reads_and_writes", v8_reads_and_writes }, { "v8_not_taken", v8_not_taken },
		{ "v7_mapped", v7_mapped }, { "v7_reads_and_writes", v7_reads_and_writes }, { "v7_first_bus", v7_first_bus },
		{ "v7_not_taken", v7_not_taken }, { "second_bus_of_version_5", second_bus_of_version_5 },
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
