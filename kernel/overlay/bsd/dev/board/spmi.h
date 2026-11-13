/*
 * The SoC's PMIC arbiter, versions 5 and 8: the way to the SPMI bus the PMICs sit on. What every driver
 * that reaches a PMIC shares: where the arbiter's registers are, how a channel is found, how one
 * transfer is started and what its status says.
 *
 * What a driver keeps to itself: when it takes the bus (hakios_spmi_lock(), one transfer at a time
 * for all of them), whether it asks again after a dropped transfer, what it counts and what it says
 * in its log.
 *
 * Where the blocks are comes from the SoC file's spmi { } (hakios_board()->spmi); nothing here is one
 * phone's. Register layout: Linux drivers/spmi/spmi-pmic-arb.c.
 *
 *   core        0x000 the version, 0x004 the number of channels, 0x900 + 4n which peripheral channel n is
 *   cnfg        0x700 + 4n which system may write through channel n
 *   observer    reads: 0x10000 per system, 0x80 per channel in it; a command at 0x00, the status at
 *               0x08, the bytes read at 0x18 and 0x1c
 *   channels    writes: 0x10000 per channel; the bytes at 0x10, then a command at 0x00, the status at 0x08
 *
 * Version 8 (an SoC file whose spmi { } has channel-map) keeps its tables apart:
 *   channel-map 4n which peripheral channel n is ([12:0] slave << 8 | peripheral)
 *   cnfg        4n which system may write through channel n
 *   observer    0x20 per channel, the same registers as above (one system's block: EE 0 only)
 *   channels    0x200 per channel, the same registers as above
 *
 * The file that includes this one defines rd(address) and wr(address, value) first, as functions or
 * as macros: a driver's test puts a model of the arbiter behind them (workbench/spmi_model.h).
 */
#ifndef HAKIOS_SPMI_H
#define HAKIOS_SPMI_H

#include "board.h"

#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_SIZE       0x10000UL       /* one system's part of the observer block */
#define ARB_OBSERVER_CHANNEL    0x80UL          /* one channel's part of that */
#define ARB_WRITE_CHANNEL       0x10000UL       /* one channel's part of the channels block */
#define ARB_WRITE_SIZE          0x1000UL        /* ... of which its registers are the beginning */

#define ARB_VERSION             0x000
#define ARB_VERSION_5           0x50000000u     /* what is here is version 5's; 7 moved things again */
#define ARB_VERSION_7           0x70000000u
#define ARB_FEATURES            0x004           /* [10:0] the number of channels */
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))       /* core: [19:8] slave and peripheral, [24] interrupts go to the owner */
#define ARB_CHANNEL_INTERRUPTS  (1u << 24)
#define ARB_OWNER(n)            (0x700 + 4 * (n))       /* cnfg: [2:0] the system that may write through channel n */
#define ARB_MAX_CHANNELS        512
#define ARB_VERSION_8           0x80000000u
#define ARB_V8_TABLE_SIZE       0x8000UL        /* channel-map and cnfg: 4 bytes a channel */
#define ARB_V8_OBSERVER_CHANNEL 0x20UL
#define ARB_V8_WRITE_CHANNEL    0x200UL

/* a channel's registers, in the observer block (reads) and in the channels block (writes) alike */
#define CHANNEL_COMMAND         0x00
#define CHANNEL_STATUS          0x08
#define CHANNEL_WDATA0          0x10            /* the bytes to write, the first in the lowest byte */
#define OBSERVER_DATA0          0x18            /* the bytes read, the first in the lowest byte */
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_WRITE           (0u << 27)      /* "extended register write long"; [11:4] register, [2:0] bytes - 1 */
#define COMMAND_READ            (1u << 27)      /* "extended register read long", the same fields */
#define ARB_TRANSFER_TIMEOUT_US 2000            /* a transfer takes some tens of microseconds */

/* what hakios_spmi_problem() says; a driver that asks again after a dropped transfer compares with the last */
#define SPMI_SAYS_NO_ANSWER     "no answer from the arbiter"
#define SPMI_SAYS_DENIED        "the arbiter refuses"
#define SPMI_SAYS_FAILED        "the transfer failed (nobody at that address?)"
#define SPMI_SAYS_DROPPED       "the transfer was dropped"

struct hakios_spmi {
	vm_offset_t     core, cnfg;
	vm_offset_t     map;            /* version 8: the table of channels; 0 for version 5 */
	vm_offset_t     observer;       /* this system's part of the block */
	uint32_t        system;         /* who we are to the arbiter: the device tree's "qcom,ee" */
	uint32_t        version;
	uint32_t        channels;       /* how many to look through */
};

/*
 * Map the arbiter the board has and read its version. False if the board has none, or one of a
 * version this code does not know: `version' says which, 0 for no arbiter (a board nobody described
 * has none).
 */
static inline bool
hakios_spmi_map(struct hakios_spmi *arb)
{
	const struct hakios_board *board = hakios_board();

	arb->version = 0;
	arb->channels = 0;
	arb->map = 0;
	if (board == NULL || board->spmi.core == 0 || board->spmi.cnfg == 0 || board->spmi.observer == 0) {
		return false;
	}
	arb->system = board->spmi.ee;
	arb->core = ml_io_map(board->spmi.core, ARB_CORE_SIZE);
	if (board->spmi.channel_map != 0) {
		arb->version = rd(arb->core + ARB_VERSION);
		arb->channels = rd(arb->core + ARB_FEATURES) & 0x1fff;
		if (arb->version < ARB_VERSION_8 || arb->system != 0 || arb->channels == 0 ||
		    arb->channels > ARB_V8_TABLE_SIZE / 4) {
			return false;
		}
		arb->map = ml_io_map(board->spmi.channel_map, ARB_V8_TABLE_SIZE);
		arb->cnfg = ml_io_map(board->spmi.cnfg, ARB_V8_TABLE_SIZE);
		arb->observer = ml_io_map(board->spmi.observer, arb->channels * ARB_V8_OBSERVER_CHANNEL);
		return arb->map != 0 && arb->cnfg != 0 && arb->observer != 0;
	}
	arb->cnfg = ml_io_map(board->spmi.cnfg, ARB_CNFG_SIZE);
	arb->observer = ml_io_map(board->spmi.observer + ARB_OBSERVER_SIZE * arb->system, ARB_OBSERVER_SIZE);
	arb->version = rd(arb->core + ARB_VERSION);
	arb->channels = rd(arb->core + ARB_FEATURES) & 0x7ff;
	if (arb->channels == 0 || arb->channels > ARB_MAX_CHANNELS) {
		arb->channels = ARB_MAX_CHANNELS;
	}
	return arb->version >= ARB_VERSION_5 && arb->version < ARB_VERSION_7;
}

/* which peripheral channel n is (slave << 8 | peripheral), or 0 for a channel that is nobody's */
static inline uint32_t
hakios_spmi_channel_map(const struct hakios_spmi *arb, uint32_t n)
{
	if (arb->map != 0) {
		return (rd(arb->map + 4 * n) & 0x1fff) << 8;   /* (in version 5's place, for SPMI_ID_OF) */
	}
	return rd(arb->core + ARB_CHANNEL(n));
}

#define SPMI_ID_OF(map)         (((map) >> 8) & 0xfff)

/* the system that may write through channel n */
static inline uint32_t
hakios_spmi_channel_owner(const struct hakios_spmi *arb, uint32_t n)
{
	return rd(arb->cnfg + (arb->map != 0 ? 4 * n : ARB_OWNER(n))) & 7;
}

/* where channel n's registers for reading are (mapped with the arbiter) */
static inline vm_offset_t
hakios_spmi_observer(const struct hakios_spmi *arb, uint32_t n)
{
	return arb->observer + (arb->map != 0 ? ARB_V8_OBSERVER_CHANNEL : ARB_OBSERVER_CHANNEL) * n;
}

/* map channel n's registers for writing; 0 for a board whose SoC file does not say where they are */
static inline vm_offset_t
hakios_spmi_map_write(uint32_t n)
{
	const struct hakios_board *board = hakios_board();

	if (board == NULL || board->spmi.channels == 0) {
		return 0;
	}
	if (board->spmi.channel_map != 0) {
		return ml_io_map(board->spmi.channels + ARB_V8_WRITE_CHANNEL * n, ARB_V8_WRITE_CHANNEL);
	}
	return ml_io_map(board->spmi.channels + ARB_WRITE_CHANNEL * n, ARB_WRITE_SIZE);
}

/* what a transfer's status says went wrong, or NULL */
static inline const char *
hakios_spmi_problem(uint32_t status)
{
	return !(status & STATUS_DONE) ? SPMI_SAYS_NO_ANSWER : (status & STATUS_DENIED) ? SPMI_SAYS_DENIED :
	       (status & STATUS_FAILURE) ? SPMI_SAYS_FAILED : (status & STATUS_DROPPED) ? SPMI_SAYS_DROPPED : NULL;
}

/* wait for the transfer started on that channel; its status, without STATUS_DONE if it never ended */
static inline uint32_t
hakios_spmi_wait(vm_offset_t channel)
{
	uint64_t start = mach_absolute_time(), interval;
	uint32_t status;

	nanoseconds_to_absolutetime(ARB_TRANSFER_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	do {
		status = rd(channel + CHANNEL_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	return status;
}

/*
 * One read: `count' (1 to 8) registers of the channel's peripheral, from `reg' on. The caller holds
 * the bus. NULL, or what went wrong, and then `bytes' is as it was.
 */
static inline const char *
hakios_spmi_read(vm_offset_t channel, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	const char *problem;
	uint32_t data[2];

	wr(channel + CHANNEL_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	problem = hakios_spmi_problem(hakios_spmi_wait(channel));
	data[0] = rd(channel + OBSERVER_DATA0);
	data[1] = count > 4 ? rd(channel + OBSERVER_DATA1) : 0;
	if (problem == NULL) {
		for (uint32_t i = 0; i < count; i++) {
			bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
		}
	}
	return problem;
}

/* One write of one register, through a channel mapped for writing. The caller holds the bus. */
static inline const char *
hakios_spmi_write(vm_offset_t channel, uint8_t reg, uint8_t value)
{
	wr(channel + CHANNEL_WDATA0, value);
	wr(channel + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4);
	return hakios_spmi_problem(hakios_spmi_wait(channel));
}

#endif /* HAKIOS_SPMI_H */
