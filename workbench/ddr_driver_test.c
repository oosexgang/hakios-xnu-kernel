/*
 * The memory bandwidth vote itself (drivers/board/ddr.c), run as a host program against
 * a model of the application processor's RPMh sequencer and a command database built here. This
 * driver writes to RPMh, which every part of the SoC depends on: what is tested first is what it
 * sends, to which resources, and that the command slots are left as they were found, also when RPMh
 * does not answer.
 *
 *   ddr_driver_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <unistd.h>

#include "fakekernel.h"

#define KERNEL 1
#include "cmd_db_image.h"
#include "rsc_model.h"

extern char log_text[];

static uint32_t sequencer_at;           /* the sequencer's own address: its version is read there (RSC_DRV_ID) */
uint32_t hakios_test_rd(vm_offset_t address) { return sequencer_at != 0 && address == sequencer_at ? 0x00020000 : rsc_model_rd(address); }
void hakios_test_wr(vm_offset_t address, uint32_t value) { rsc_model_wr(address, value); }

/* ---- the kernel around the driver ---------------------------------------------------------------- */

static unsigned database_maps, locks, unlocks;
static uint32_t database_at, database_size;

vm_offset_t
ml_io_map_wcomb(vm_offset_t phys, vm_size_t size)
{
	if (phys != database_at || size != database_size) {
		rsc.elsewhere++;
		fprintf(stderr, "MODEL: %#lx bytes mapped at %#lx, which is not the command database\n", (unsigned long)size, (unsigned long)phys);
	}
	database_maps++;
	return (vm_offset_t)&image;
}

vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }
void hakios_rpmh_lock(void) { locks++; }
void hakios_rpmh_unlock(void) { unlocks++; }
/* the CPU frequency driver's own quiet (board/cpufreq.c), asked for before DDR's */
static unsigned cpufreq_quiesced;
void hakios_cpufreq_quiesce(void);
void hakios_cpufreq_quiesce(void) { cpufreq_quiesced++; }
void hakios_ddr_init(bool with_cpufreq);
void hakios_ddr_quiesce(void);

#pragma GCC diagnostic ignored "-Wformat"
#define HAKIOS_MMIO_TEST 1
#include "board/board.c"
#include "board/ddr.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

#define VOTE(n)         (1u << 30 | 1u << 29 | (uint32_t)(n) << 14 | (uint32_t)(n))
#define KEEP_ON         (1u << 30 | 1u << 29 | 8u)      /* ACV: the memory kept on for this processor */

static const struct { uint32_t unit; uint16_t width; uint8_t vcd, reserved; } mc0 = { 1000, 14, 0, 0 };
/* (an aux of four bytes, shorter than a bus clock manager's: not read as one) */
static const uint8_t cn0_short[4] = { 0x11, 0x22, 0x33, 0x44 };

/* which of the three the database lacks, where a phone is asked for without all of them */
static const char *left_out = "SH0";

/* a phone: the sequencer and the database where its SoC file has them; the slots as a boot loader leaves them */
static void
phone(const char *name, bool with_sh0)
{
	static const struct resource rails[] = { { "cx.lvl", 0x30000, NULL, 0 }, { "MC0", 0x3ffff, NULL, 0 } };
	static const struct resource buses[] = {
		{ "MC0", 0x50000, &mc0, sizeof(mc0) }, { "SH0", 0x50004, NULL, 0 }, { "ACV", 0x50008, NULL, 0 }, { "CN0", 0x5000c, cn0_short, sizeof(cn0_short) },
	};
	static struct resource without[3];
	unsigned kept = 0;
	const struct hakios_board *b;

	board = name;
	fake_delay_divisor = 20;
	empty();
	slave(0, CMD_DB_ARC, rails, 2);         /* (a rail that happens to be named as a bus clock manager is: not it) */
	if (with_sh0) {
		slave(1, CMD_DB_BCM, buses, 4);
	} else {
		for (unsigned i = 0; i < 4; i++) {
			if (strcmp(buses[i].name, left_out) != 0 && strcmp(buses[i].name, "CN0") != 0) {
				without[kept++] = buses[i];
			}
		}
		slave(1, CMD_DB_BCM, without, kept);
	}
	b = hakios_board();
	if (b != NULL) {
		rsc.base = b->rpmh.rsc + b->rpmh.tcs_offset;
		sequencer_at = b->rpmh.rsc;     /* (a version 2 sequencer, as the model's slot set is laid out) */
		database_at = b->rpmh.cmd_db;
		database_size = b->rpmh.cmd_db_size;
	}
	rsc.irq_enable = 0x6;                   /* (other slot sets' interrupts, which are not ours to change) */
	rsc.control = 0;
}

static void
as_found(void)
{
	CHECK(rsc.irq_enable == 0x6 && rsc.wait == 0 && rsc.enabled == 0 && (rsc.control & (1u << 24 | 1u << 16)) == 0 &&
	    rsc.irq_status == 0, "the slot set is not as it was found: interrupts %#x, wait %#x, commands %#x, control %#x, status %#x",
	    rsc.irq_enable, rsc.wait, rsc.enabled, rsc.control, rsc.irq_status);
	CHECK(rsc.elsewhere == 0, "%u accesses where nothing of the sequencer or the database is", rsc.elsewhere);
	CHECK(rsc.changed_while_sent == 0, "%u writes to the commands while the slot set was triggered", rsc.changed_while_sent);
	CHECK(canary_whole(), "something behind the command database was written");
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

/* with the cores' clocks raised: the most there is, on the memory's two bus clock managers, and the memory kept on */
static void
the_most(void)
{
	phone("phone-cmd", true);
	boot_args = "";
	hakios_ddr_init(true);
	CHECK(rsc.triggers == 1 && rsc.sends == 3, "one request of three commands: %u, %u", rsc.triggers, rsc.sends);
	CHECK(rsc.sent[0].address == 0x50000 && rsc.sent[0].data == VOTE(0x3fff), "MC0: %#x <- %#x", rsc.sent[0].address, rsc.sent[0].data);
	CHECK(rsc.sent[1].address == 0x50004 && rsc.sent[1].data == VOTE(0x3fff), "SH0: %#x <- %#x", rsc.sent[1].address, rsc.sent[1].data);
	CHECK(rsc.sent[2].address == 0x50008 && rsc.sent[2].data == KEEP_ON, "ACV: %#x <- %#x", rsc.sent[2].address, rsc.sent[2].data);
	CHECK(rsc.sent[0].message == 0x10108, "a write of four bytes that is answered: %#x", rsc.sent[0].message);
	CHECK(logged("ddr: memory bandwidth vote 16383 on MC0 (0x50000) and SH0 (0x50004)\n"), "said: %s", log_text);
	CHECK(locks == 1 && unlocks == 1, "the slot set is taken and given back (%u, %u)", locks, unlocks);
	as_found();
}

/* on the way back to the loader: a vote from the middle is left, on the two; then nothing more */
static void
given_back(void)
{
	phone("phone-cmd", true);
	boot_args = "";
	hakios_ddr_init(true);
	hakios_ddr_quiesce();
	CHECK(rsc.triggers == 2 && rsc.sends == 5, "a second request, of two commands: %u, %u", rsc.triggers, rsc.sends);
	CHECK(rsc.sent[3].address == 0x50000 && rsc.sent[3].data == VOTE(1024) && rsc.sent[4].address == 0x50004 &&
	    rsc.sent[4].data == VOTE(1024), "1024 on MC0 and SH0: %#x <- %#x, %#x <- %#x", rsc.sent[3].address, rsc.sent[3].data,
	    rsc.sent[4].address, rsc.sent[4].data);
	CHECK(logged("ddr: memory bandwidth vote 1024 for the loader: done"), "said: %s", log_text);
	CHECK(locks == 1, "nothing is waited for on the way out: the others are stopped (%u)", locks);
	hakios_ddr_quiesce();
	CHECK(rsc.triggers == 2, "once: a second way out sends nothing");
	as_found();
}

/* ddr_vote=N and ddr_rest=N; ddr_rest=0 is "no request", which is not a vote of nothing */
static void
other_votes(void)
{
	phone("phone-cmd", true);
	boot_args = "ddr_vote=5000 ddr_rest=0";
	hakios_ddr_init(true);
	CHECK(rsc.sent[0].data == VOTE(5000) && rsc.sent[1].data == VOTE(5000) && rsc.sent[2].data == KEEP_ON, "5000: %#x", rsc.sent[0].data);
	hakios_ddr_quiesce();
	CHECK(rsc.sent[3].data == 1u << 30 && rsc.sent[4].data == 1u << 30, "no request: commit alone (%#x)", rsc.sent[3].data);
	as_found();
}

/* what is left for the loader is no more than the fourteen bits either: asked for more, it is the most */
static void
rest_too_much(void)
{
	phone("phone-cmd", true);
	boot_args = "ddr_rest=99999";
	hakios_ddr_init(true);
	hakios_ddr_quiesce();
	CHECK(rsc.sent[3].data == VOTE(0x3fff) && rsc.sent[4].data == VOTE(0x3fff), "the most there is: %#x", rsc.sent[3].data);
	as_found();
}

/* a vote that does not fit the fourteen bits is not sent as something else */
static void
too_much(void)
{
	phone("phone-cmd", true);
	boot_args = "ddr_vote=16384";
	hakios_ddr_init(true);
	CHECK(rsc.writes == 0 && logged("ddr: no vote"), "refused: %s", log_text);
	boot_args = "ddr_vote=0";
	hakios_ddr_init(true);
	CHECK(rsc.writes == 0, "a vote of nothing is not sent either");
	hakios_ddr_quiesce();
	CHECK(rsc.writes == 0, "... and nothing is given back that was not taken");
}

/* without the cores' clocks raised nobody wants more memory; ddr=0 is off */
static void
not_asked(void)
{
	phone("phone-cmd", true);
	boot_args = "";
	hakios_ddr_init(false);
	CHECK(rsc.writes == 0 && database_maps == 0 && !logged("ddr:"), "nothing: %s", log_text);
	boot_args = "ddr=0";
	hakios_ddr_init(true);
	CHECK(rsc.writes == 0 && !logged("ddr:"), "ddr=0: nothing");
	as_found();
}

/* ddr=1: what the database has for the memory and what the slots hold, and not one write */
static void
look_only(void)
{
	phone("phone-cmd", true);
	boot_args = "ddr=1";
	rsc.address[2] = 0x50000;               /* (as a boot loader leaves a vote of its own in a slot) */
	rsc.data[2] = VOTE(2000);
	rsc.address[3] = 0x50004;               /* (and one whose vote is 0: a command all the same) */
	rsc.data[3] = 0;
	rsc.enabled = 0x4;
	hakios_ddr_init(true);
	CHECK(logged("ddr: BCM MC0      at 0x50000: unit 1000, width 14, vcd 0 (aux 8 bytes)"), "the memory's bus clock manager: %s", log_text);
	CHECK(logged("ddr: BCM CN0      at 0x5000c: unit 0, width 0, vcd 0 (aux 4 bytes)"), "every bus clock manager is shown, one whose aux "
	    "is too short for what it says as nothing: %s", log_text);
	CHECK(logged("ddr: slot set 0 (active) command 3: message 0, address 0x50004, data 0"), "a command whose vote is 0 is one: %s", log_text);
	CHECK(!logged("cx.lvl") && !logged("0x3ffff"), "and only those: a rail is not one, whatever its name");
	CHECK(logged("ddr: slot set 0 (active) command 2 (on): message 0, address 0x50000,") && logged("<- a BCM"), "what the slots hold");
	CHECK(!logged(": message 0, address 0, data 0"), "and not the empty commands: %s", log_text);
	CHECK(rsc.writes == 0 && rsc.triggers == 0, "a look writes nothing (%u)", rsc.writes);
	CHECK(rsc.elsewhere == 0 && canary_whole(), "nothing touched elsewhere");
}

/* RPMh does not answer: said, the slots cleaned up, and nothing given back later that was never taken */
static void
no_answer(void)
{
	phone("phone-cmd", true);
	boot_args = "";
	rsc.deaf = true;
	hakios_ddr_init(true);
	CHECK(logged("ddr: RPMh: no answer to the request") && logged("SH0 (0x50004): FAILED, or perhaps not"), "said: %s", log_text);
	CHECK(locks == 1 && unlocks == 1, "the slot set is given back all the same");
	as_found();
	hakios_ddr_quiesce();
	/* (the request was triggered: it may be in force, and is given back like one that was answered) */
	CHECK(rsc.triggers == 2, "the vote for the loader is sent on the way out all the same: %u triggers", rsc.triggers);
}

/* the slot set is somebody's and stays so: nothing is written into it */
static void
slots_busy(void)
{
	phone("phone-cmd", true);
	boot_args = "";
	rsc.busy = true;
	hakios_ddr_init(true);
	CHECK(logged("ddr: RPMh: the command slots stay busy"), "said: %s", log_text);
	CHECK(rsc.writes == 0 && rsc.triggers == 0, "not one write into a slot set that is in use (%u)", rsc.writes);
	CHECK(unlocks == 1, "the lock is given back");
}

/* a database without one of the three: no vote on the others alone */
static void
one_missing(void)
{
	phone("phone-cmd", false);
	boot_args = "";
	hakios_ddr_init(true);
	CHECK(logged("ddr: no vote (MC0 0x50000, SH0 0, ACV 0x50008, vote 16383)"), "said, with which: %s", log_text);
	CHECK(rsc.writes == 0, "nothing sent");
	/* ... and so for each of the other two */
	left_out = "MC0";
	phone("phone-cmd", false);
	hakios_ddr_init(true);
	CHECK(logged("ddr: no vote (MC0 0, SH0 0x50004, ACV 0x50008, vote 16383)") && rsc.writes == 0, "without MC0: %s", log_text);
	left_out = "ACV";
	phone("phone-cmd", false);
	hakios_ddr_init(true);
	CHECK(logged("ddr: no vote (MC0 0x50000, SH0 0x50004, ACV 0, vote 16383)") && rsc.writes == 0, "without ACV: %s", log_text);
	hakios_ddr_quiesce();
	CHECK(rsc.writes == 0, "and nothing is given back where nothing was asked for");
}

/* what is where the database should be is not one */
static void
no_database(void)
{
	phone("phone-cmd", true);
	DB->magic[0] = 0;
	boot_args = "";
	hakios_ddr_init(true);
	CHECK(logged("ddr: no vote (MC0 0, SH0 0, ACV 0,") && rsc.writes == 0, "no vote: %s", log_text);
	boot_args = "ddr=1";
	hakios_ddr_init(true);
	CHECK(logged("ddr: no command database"), "a look says so");
}

/* a chip with its sequencer and database elsewhere and its slot sets further in */
static void
another_soc(void)
{
	phone("phone-keys", true);
	boot_args = "";
	hakios_ddr_init(true);
	CHECK(rsc.base == 0x07220000 + 0x1c00 && database_at == 0x90860000 && database_size == 0x2000, "that SoC file's: %#x, %#x, %#x",
	    rsc.base, database_at, database_size);
	CHECK(rsc.triggers == 1 && rsc.sends == 3 && rsc.sent[0].address == 0x50000, "voted there: %u commands", rsc.sends);
	as_found();
}

/* a board without the feature: said if asked for, and nothing touched */
static void
not_listed(void)
{
	phone("qemu-virt", true);
	rsc.base = 0;
	boot_args = "";
	hakios_ddr_init(true);
	CHECK(logged("ddr: no RPMh vote on this board (its board.dts lists no \"ddr-rpmh\")"), "said: %s", log_text);
	CHECK(rsc.writes == 0 && rsc.elsewhere == 0 && database_maps == 0, "nothing mapped, read or written");
}

/* ... and where nothing asked for the vote (no ddr=, the CPU's clocks left alone), not a word: every boot of the emulator is this */
static void
not_listed_not_asked(void)
{
	phone("qemu-virt", true);
	rsc.base = 0;
	boot_args = "";
	hakios_ddr_init(false);
	boot_args = "ddr=0";
	hakios_ddr_init(true);
	CHECK(!logged("ddr:"), "nothing is said: %s", log_text);
	boot_args = "ddr=1";
	hakios_ddr_init(false);
	CHECK(logged("ddr: no RPMh vote on this board"), "a look that was asked for is answered: %s", log_text);
	CHECK(rsc.writes == 0 && rsc.elsewhere == 0 && database_maps == 0, "nothing mapped, read or written");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "not_listed_not_asked", not_listed_not_asked }, { "the_most", the_most }, { "given_back", given_back }, { "other_votes", other_votes }, { "rest_too_much", rest_too_much }, { "too_much", too_much },
		{ "not_asked", not_asked }, { "look_only", look_only }, { "no_answer", no_answer }, { "slots_busy", slots_busy },
		{ "one_missing", one_missing }, { "no_database", no_database }, { "another_soc", another_soc }, { "not_listed", not_listed },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			if (freopen("/dev/null", "w", stdout) == NULL) {
				return 2;
			}
			scenarios[i].run();
			fprintf(stderr, "%-20s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
