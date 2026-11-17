/*
 * The memory's bandwidth, asked of RPMh: the feature "ddr-rpmh".
 *
 * The boot loader leaves the memory's bus clock at a modest rate; a system that wants more votes for
 * it on the memory's bus clock managers ("BCM"), through its RPMh sequencer. Where the command
 * database and the sequencer are comes from the SoC file's rpmh { } (board/board.h). What is taken for
 * granted beyond that, as on both phones so far: the sequencer's first slot set is an active one,
 * and the BCMs are named MC0, SH0 and ACV.
 *
 *   ddr=1      what the command database has for the memory, and what the sequencer's slots hold: read only
 *   ddr=2      (the default, with cpufreq=1) vote for the most
 *
 * Layouts: Linux drivers/soc/qcom/cmd-db.c, rpmh-rsc.c, drivers/interconnect/qcom/bcm-voter.c.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/board/input.h>
#include <sys/systm.h>

#include "kernel.h"
#include "board.h"
#include "cmd_db.h"
#include "log.h"
#include "mmio.h"
#include "rpmh.h"

/*
 * The loader's log as well: this runs early, and the kernel's own log of a run keeps only its end. With
 * interrupts off -- on the way back to the loader (hakios_ddr_quiesce()) -- the loader's log only: the
 * kernel's console may not be written there, and printf was a panic ("Console I/O from interrupt-disabled
 * context") at the end of every run once the memory vote was taken away on the way back.
 */
#define tell hakios_say

extern void hakios_cpufreq_quiesce(void);      /* board/cpufreq.c */

/*
 * Boot-arg ddr=1: the memory's bus clock managers ("BCM") in the command database the boot loader
 * left, with what is needed to vote for bandwidth on them -- only read. (membench=2 says the cores
 * together get no more from memory than one alone: 12 GB/s; nobody here votes for more.)
 */
#define CMD_DB_SIZE             (hakios_board()->rpmh.cmd_db_size)      /* where it is: the SoC file's rpmh { } */
struct cmd_db_bcm {                             /* a BCM's auxiliary data (Linux: struct bcm_db) */
	uint32_t unit;
	uint16_t width;
	uint8_t vcd, reserved;
};

static void
ddr_look(void)
{
	/* (memory, not registers: mapped so that reads of any size and alignment are allowed) */
	const struct cmd_db_header *db = (const struct cmd_db_header *)ml_io_map_wcomb(hakios_board()->rpmh.cmd_db, CMD_DB_SIZE);

	if (!hakios_cmd_db_valid(db)) {
		tell("ddr: no command database\n");
		return;
	}
	for (unsigned s = 0; s < CMD_DB_SLAVES; s++) {
		const struct cmd_db_entry *entry;

		if (db->slave[s].id != CMD_DB_BCM) {
			continue;
		}
		for (unsigned i = 0; (entry = hakios_cmd_db_entry(db, CMD_DB_SIZE, s, i)) != NULL; i++) {
			struct cmd_db_bcm aux = { 0 };
			char name[9] = { 0 };
			unsigned length;
			const uint8_t *data = hakios_cmd_db_data(db, CMD_DB_SIZE, s, entry, &length);

			memcpy(name, entry->id, 8);
			if (data != NULL && length >= sizeof(aux)) {
				memcpy(&aux, data, sizeof(aux));
			}
			tell("ddr: BCM %-8s at %#x: unit %u, width %u, vcd %u (aux %u bytes)\n", name, entry->address, aux.unit,
			    aux.width, aux.vcd, entry->length);
		}
	}
}

/*
 * ... and what the boot loader's firmware left in the application processor's RPMh command slots
 * ("DRV 2"; Linux: rpmh-rsc.c): 2 active, 3 sleep, 3 wake slot sets and a control one, of 16 commands
 * each (sc7280's qcom,tcs-config): every command with an address, and which of them go to the BCMs
 * above -- the bandwidth it asked for, and that is in force whenever the processors are awake.
 */
#define RSC_SIZE                0x10000UL       /* where it is: the SoC file's rpmh { } */
#define RSC_TCS(n, reg)         (hakios_board()->rpmh.tcs_offset + 0x2a0 * (n) + (reg))
#define RSC_TCS_COUNT           9
#define RSC_CMDS                16

static void
ddr_slots(void)
{
	vm_offset_t rsc = ml_io_map(hakios_board()->rpmh.rsc, RSC_SIZE);

	for (unsigned t = 0; t < RSC_TCS_COUNT; t++) {
		uint32_t enabled = rd(rsc + RSC_TCS(t, 0x1c));

		for (unsigned c = 0; c < RSC_CMDS; c++) {
			uint32_t msgid = rd(rsc + RSC_TCS(t, 0x30 + 20 * c));
			uint32_t address = rd(rsc + RSC_TCS(t, 0x34 + 20 * c));
			uint32_t data = rd(rsc + RSC_TCS(t, 0x38 + 20 * c));

			if (address == 0 && data == 0) {
				continue;
			}
			tell("ddr: slot set %u (%s) command %u%s: message %#x, address %#x, data %#x%s\n", t,
			    t < 2 ? "active" : t < 5 ? "sleep" : t < 8 ? "wake" : "control", c, enabled & (1u << c) ? " (on)" : "",
			    msgid, address, data, (address >> 16 & 0xf) == CMD_DB_BCM ? " <- a BCM" : "");
		}
	}
}

/*
 * The memory's bandwidth (on by default; boot-arg ddr=0: not): a vote through RPMh -- the most there
 * is -- on the bus clock managers of the memory controller (MC0: the memory's clock) and of the
 * shared hub in front of it with the system cache (SH0), as Linux's interconnect driver casts for
 * a busy GPU (icc-bcm-voter.c: a BCM command is commit, valid, the average and the peak, 14 bits each,
 * in the BCM's units; the BCM takes the highest of its levels the vote reaches), and the memory kept
 * on for this processor (ACV, bit 3, which the boot loader's firmware had set already, the same).
 * All cores together got 12.4 GB/s writing without it (membench=2: what one core gets alone), 14.8
 * with it; frames over 16.7 ms 28% instead of 34-36%. ddr_vote=N: N instead of the most (16383).
 * When the kernel goes back to the loader (hakios_ddr_quiesce(), on every way back, a panic's too)
 * the vote goes down to ddr_rest=N (default 1024). What the firmware had voted cannot be read back,
 * but what the cores get from memory can be measured (membench=2, writing): with any vote from 4
 * to 4096 12.0 to 12.4 GB/s, as before the first vote (12.4); with the most, 15.4; with none at all
 * ("no request", valid clear: what Linux sends for nothing) 0.2 -- the memory at its floor, and the
 * loader's USB transfers at 8 MB/s instead of 30. So the firmware had a vote of its own in our name,
 * of that middle range, and one from the middle of it is left.
 */
#define BCM_COMMIT              (1u << 30)
#define BCM_VALID               (1u << 29)
#define BCM_VOTE(x, y)          (BCM_COMMIT | BCM_VALID | ((uint32_t)(x) & 0x3fff) << 14 | ((uint32_t)(y) & 0x3fff))
#define BCM_ACV_APPS            8
#define DDR_REST                1024            /* the vote left for the loader (ddr_rest=N); see hakios_ddr_quiesce() */

static vm_offset_t ddr_rsc;
static uint32_t ddr_mc0, ddr_sh0, ddr_acv;
static bool ddr_voted;

/* (bsd/dev/board/input.c: the RPMh command slots are the touch driver's and the Wi-Fi's too) */
extern void hakios_rpmh_lock(void);
extern void hakios_rpmh_unlock(void);

/* What RPMh is told for the bus clock manager of that name; 0: the command database has none. */
static uint32_t
cmd_db_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)ml_io_map_wcomb(hakios_board()->rpmh.cmd_db, CMD_DB_SIZE);
	const struct cmd_db_entry *entry = hakios_cmd_db_find(db, CMD_DB_SIZE, name, CMD_DB_BCM, NULL);

	return entry != NULL ? entry->address : 0;
}

/* A request through command slot set 0, and the wait until RPMh has done it (board/rpmh.h); what went wrong is said, and returned. */
static const char *
ddr_rpmh_send(const uint32_t *address, const uint32_t *data, unsigned count)
{
	const char *problem = hakios_rpmh_send_rsc(ddr_rsc, hakios_board()->rpmh.tcs_offset, address, data, count);

	if (problem != NULL) {
		tell("ddr: RPMh: %s\n", problem);
	}
	return problem;
}

static bool
ddr_rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	return ddr_rpmh_send(address, data, count) == NULL;
}

static const char *
ddr_vote(uint32_t vote)
{
	uint32_t address[3] = { ddr_mc0, ddr_sh0, ddr_acv };
	uint32_t data[3] = { BCM_VOTE(vote, vote), BCM_VOTE(vote, vote), BCM_VOTE(0, BCM_ACV_APPS) };

	return ddr_rpmh_send(address, data, 3);
}

static void
ddr_raise(void)
{
	uint32_t vote = 0x3fff;
	const char *problem;

	PE_parse_boot_argn("ddr_vote", &vote, sizeof(vote));
	ddr_mc0 = cmd_db_address("MC0");
	ddr_sh0 = cmd_db_address("SH0");
	ddr_acv = cmd_db_address("ACV");
	if (ddr_mc0 == 0 || ddr_sh0 == 0 || ddr_acv == 0 || vote == 0 || vote > 0x3fff) {
		tell("ddr: no vote (MC0 %#x, SH0 %#x, ACV %#x, vote %u)\n", ddr_mc0, ddr_sh0, ddr_acv, vote);
		return;
	}
	ddr_rsc = ml_io_map(hakios_board()->rpmh.rsc, RSC_SIZE);
	hakios_rpmh_lock();
	problem = ddr_vote(vote);
	hakios_rpmh_unlock();
	/*
	 * A request without an answer was triggered all the same, and may well be in force: it is given back at the run's end
	 * like one that was answered (else the loader and the next kernel run with the memory at its top)
	 */
	ddr_voted = problem == NULL || strcmp(problem, RPMH_SAYS_NO_ANSWER) == 0;     /* (the words, not where they are) */
	tell("ddr: memory bandwidth vote %u on MC0 (%#x) and SH0 (%#x)%s\n", vote, ddr_mc0, ddr_sh0,
	    problem == NULL ? "" : ddr_voted ? ": FAILED, or perhaps not: given back at the run's end all the same" : ": FAILED");
}

/* On the way back to the loader (osfmk/arm64/loader_return.c: loader_return(), whatever the reason; the other processors are stopped) */
void
hakios_ddr_quiesce(void)
{
	uint32_t address[2] = { ddr_mc0, ddr_sh0 };
	uint32_t rest = DDR_REST, data[2];
	bool done;

	hakios_cpufreq_quiesce();       /* the cores' groups at their top rows, the cache slices as they were (cpufreq.c) */
	if (!ddr_voted) {
		return;
	}
	ddr_voted = false;
	PE_parse_boot_argn("ddr_rest", &rest, sizeof(rest));
	rest = MIN(rest, 0x3fff);
	data[0] = data[1] = rest != 0 ? BCM_VOTE(rest, rest) : BCM_COMMIT;     /* 0: no request */
	done = ddr_rpmh_write(address, data, 2);
	tell("ddr: memory bandwidth vote %u for the loader: %s\n", rest, done ? "done" : "FAILED");
}

/* `with_cpufreq': the cores' clocks are raised as well (boot-arg cpufreq=1), which is when more memory is wanted. */
void
hakios_ddr_init(bool with_cpufreq)
{
	uint32_t level = 2;

	PE_parse_boot_argn("ddr", &level, sizeof(level));
	if (level == 0 || (level == 2 && !with_cpufreq)) {
		return;                 /* not asked for: nothing read, nothing said */
	}
	if (!hakios_board_has(HAKIOS_HAS_DDR_RPMH)) {
		tell("ddr: no RPMh vote on this board (its board.dts lists no \"ddr-rpmh\")\n");
	} else if (hakios_board()->rpmh.cmd_db == 0 || hakios_board()->rpmh.rsc == 0) {
		tell("ddr: no RPMh vote: the SoC file has no rpmh { }\n");
	} else if (level == 1) {
		ddr_look();
		ddr_slots();
	} else if (level == 2) {
		ddr_raise();
	}
}
