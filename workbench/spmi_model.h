/*
 * A model of the SoC's PMIC arbiter, version 5, 7 or 8 (as Linux's drivers/spmi/spmi-pmic-arb.c has it), with
 * the peripherals a test puts behind it. For the tests of drivers that reach a PMIC: the driver's
 * registers are this model's (its test seam), so what a driver reads is what the test set up, and
 * what it writes is checked here.
 *
 *   core        0x000 the version, 0x004 the number of channels, 0x900 + 4n which peripheral channel n is
 *   cnfg        0x700 + 4n which system may write through channel n
 *   observer    a system's reads: 0x80 a channel; a command at 0x00, the status at 0x08, the bytes at 0x18
 *   channels    writes: 0x10000 a channel; the byte at 0x10, then a command at 0x00, the status at 0x08
 *
 * Version 7 (spmi_version_7() below) has two buses, whose channels the arbiter numbers in one row: the
 * numbers of both at 0x004 and 0x008 of its core, the table of channels at 0x2000 + 4n of it, a cnfg
 * block for each bus (the model has the one of the bus the test's SoC file names: 4 bytes a channel,
 * counted from the bus's first), the observer 0x8000 a system and 0x20 a channel, the write channels
 * 0x1000 a channel. Version 8 (channel_map) has its tables apart.
 *
 * A test fills in where the blocks are (from the board the driver reads, so that a wrong address in
 * the driver is an access to nothing), the channel table and the peripherals' registers.
 */
#ifndef SPMI_MODEL_H
#define SPMI_MODEL_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define SPMI_CHANNELS           64
#define SPMI_PERIPHERALS        8
#define SPMI_DONE               0x1
#define SPMI_FAILURE            0x2
#define SPMI_DENIED             0x4
#define SPMI_DROPPED            0x8

struct spmi_peripheral {
	uint16_t id;                    /* slave << 8 | peripheral */
	uint8_t reg[256];
	bool silent;                    /* nobody at that address: a transfer fails */
	/* a write the test wants to see or to change; NULL: the register takes the value */
	void (*written)(struct spmi_peripheral *p, uint8_t reg, uint8_t value);
	/* called before a read is answered: a test that wants a register to hold something for exactly one read */
	void (*looked_at)(struct spmi_peripheral *p, uint8_t reg);
	unsigned writes;
	uint32_t bus;                   /* version 7: which of the arbiter's buses it is on (the same address can be on both) */
};

static struct spmi_model {
	uint32_t core, cnfg, observer, channels;        /* observer: this system's part of the block */
	uint32_t channel_map;           /* version 8: where its table of channels is (its cnfg a word a channel, its observer
	                                 * and write channels closer together); 0: version 5 */
	bool v7;                        /* version 7: two buses, of which the test's SoC file names `bus' */
	uint32_t bus, bus_channels[2];  /* ... and how many channels the arbiter says each bus has; the second's follow the first's */
	uint32_t version, ee;
	uint32_t says_channels;         /* what the arbiter says of its number of channels, if not the model's own */
	uint32_t map[SPMI_CHANNELS], owner[SPMI_CHANNELS];
	struct spmi_peripheral *peripheral[SPMI_PERIPHERALS];
	uint32_t status[SPMI_CHANNELS], data0[SPMI_CHANNELS], data1[SPMI_CHANNELS];     /* a channel's read */
	uint32_t wstatus[SPMI_CHANNELS], wdata0[SPMI_CHANNELS];                         /* ... and its write */
	unsigned transfers, writes, denied;
	unsigned elsewhere;             /* accesses where nothing of the arbiter is, and commands that are none */
	unsigned past_the_table;        /* reads of a channel's entry past the number the arbiter says it has (version 7: of an
	                                 * entry that is not of the bus named, in the table of channels or past the bus's cnfg) */
	bool deaf;                      /* the arbiter takes commands and never ends one */
	unsigned drop_reads, drop_writes;       /* so many of the next reads, of the next writes, dropped (not made) */
	pthread_mutex_t lock;
} spmi = { .version = 0x50000000, .lock = PTHREAD_MUTEX_INITIALIZER };

static inline struct spmi_peripheral *
spmi_peripheral_of(uint32_t channel)
{
	uint16_t id = (uint16_t)(spmi.map[channel] >> 8 & 0xfff);
	uint32_t bus = spmi.v7 && channel >= spmi.bus_channels[0];

	for (unsigned i = 0; i < SPMI_PERIPHERALS; i++) {
		if (spmi.peripheral[i] != NULL && spmi.peripheral[i]->id == id && (!spmi.v7 || spmi.peripheral[i]->bus == bus)) {
			return spmi.peripheral[i];
		}
	}
	return NULL;
}

/* channel n is that peripheral's, and written by that system */
static inline void
spmi_channel(uint32_t n, struct spmi_peripheral *p, uint32_t owner)
{
	spmi.map[n] = (uint32_t)p->id << 8;
	spmi.owner[n] = owner;
	for (unsigned i = 0; i < SPMI_PERIPHERALS; i++) {
		if (spmi.peripheral[i] == p) {
			return;
		}
	}
	for (unsigned i = 0; i < SPMI_PERIPHERALS; i++) {
		if (spmi.peripheral[i] == NULL) {
			spmi.peripheral[i] = p;
			return;
		}
	}
}

/*
 * The arbiter becomes one of version 7, and what the test has put into the table so far the channels of
 * its bus `bus', with their peripherals: after the `before' channels of the first bus if it is the
 * second (which a test then fills with spmi_channel(), by the arbiter's numbers, with peripherals whose
 * `bus' is 0). `own' is how many channels the bus named has, `observer' the whole observer block as the
 * SoC file has it.
 */
static inline void
spmi_version_7(uint32_t observer, uint32_t bus, uint32_t before, uint32_t own)
{
	uint32_t first = bus == 1 ? before : 0;

	spmi.v7 = true;
	spmi.version = 0x70000000;
	spmi.bus = bus;
	spmi.bus_channels[0] = bus == 1 ? before : own;
	spmi.bus_channels[1] = bus == 1 ? own : before;
	spmi.observer = observer + 0x8000 * spmi.ee;
	for (unsigned i = 0; i < SPMI_PERIPHERALS; i++) {
		if (spmi.peripheral[i] != NULL) {
			spmi.peripheral[i]->bus = bus;
		}
	}
	for (uint32_t n = SPMI_CHANNELS; first != 0 && n-- > 0;) {
		spmi.map[n] = n >= first ? spmi.map[n - first] : 0;
		spmi.owner[n] = n >= first ? spmi.owner[n - first] : 0;
	}
}

/* (version 7) the arbiter's number of the first channel of the bus named */
static inline uint32_t
spmi_v7_first(void)
{
	return spmi.bus == 1 ? spmi.bus_channels[0] : 0;
}

/*
 * Is that address in one of the arbiter's blocks, as wide as any version this models has them: for a test
 * whose driver reaches other blocks as well, which sends what is here to spmi_model_rd() and
 * spmi_model_wr(). So an access at another version's distances (a channel 0x80 of the observer after the
 * last where a version 7 arbiter has them 0x20 apart) is the model's to count (`elsewhere'), not lost
 * among the test's other registers.
 */
static inline bool
spmi_model_has(uintptr_t address)
{
	uint32_t tables = spmi.channel_map ? 0x8000 : 0x1000;

	return (spmi.core != 0 && address >= spmi.core && address < spmi.core + 0x2800) ||
	       (spmi.cnfg != 0 && address >= spmi.cnfg && address < spmi.cnfg + tables) ||
	       (spmi.channel_map != 0 && address >= spmi.channel_map && address < spmi.channel_map + tables) ||
	       (spmi.observer != 0 && address >= spmi.observer && address < spmi.observer + 0x80 * SPMI_CHANNELS) ||
	       (spmi.channels != 0 && address >= spmi.channels && address < spmi.channels + 0x10000ull * SPMI_CHANNELS);
}

/* for the test's own thread: a peripheral's register, as the hardware would change it */
static inline void
spmi_set(struct spmi_peripheral *p, uint8_t reg, uint8_t value)
{
	pthread_mutex_lock(&spmi.lock);
	p->reg[reg] = value;
	pthread_mutex_unlock(&spmi.lock);
}

static inline uint8_t
spmi_get(struct spmi_peripheral *p, uint8_t reg)
{
	uint8_t value;

	pthread_mutex_lock(&spmi.lock);
	value = p->reg[reg];
	pthread_mutex_unlock(&spmi.lock);
	return value;
}

/* version 8: the version and the number of channels in its core, the tables of channels apart, a word each; the
 * observer 0x20 a channel, the write channels 0x200 (the lock held) */
static inline uint32_t
spmi_v8_rd(uintptr_t address)
{
	uint32_t said = spmi.says_channels ? spmi.says_channels - 1 : SPMI_CHANNELS;

	if (address >= spmi.core && address < spmi.core + 0x1100) {
		uint32_t reg = (uint32_t)(address - spmi.core);

		return reg == 0x000 ? spmi.version : reg == 0x004 ? said : 0;
	}
	if (address >= spmi.channel_map && address < spmi.channel_map + 0x8000) {
		uint32_t n = (uint32_t)(address - spmi.channel_map) / 4;

		spmi.past_the_table += n >= said;
		return n < SPMI_CHANNELS ? spmi.map[n] >> 8 : 0;
	}
	if (address >= spmi.cnfg && address < spmi.cnfg + 0x8000) {
		uint32_t n = (uint32_t)(address - spmi.cnfg) / 4;

		spmi.past_the_table += n >= said;
		return n < SPMI_CHANNELS ? spmi.owner[n] : 0;
	}
	if (address >= spmi.observer && address < spmi.observer + 0x20 * SPMI_CHANNELS) {
		uint32_t channel = (uint32_t)(address - spmi.observer) / 0x20, reg = (uint32_t)(address - spmi.observer) % 0x20;

		return reg == 0x08 ? spmi.status[channel] : reg == 0x18 ? spmi.data0[channel] : reg == 0x1c ? spmi.data1[channel] : 0;
	}
	if (spmi.channels != 0 && address >= spmi.channels && address < spmi.channels + 0x200ull * SPMI_CHANNELS) {
		uint32_t channel = (uint32_t)((address - spmi.channels) / 0x200), reg = (uint32_t)((address - spmi.channels) % 0x200);

		return reg == 0x08 ? spmi.wstatus[channel] : 0;
	}
	spmi.elsewhere++;
	fprintf(stderr, "MODEL: a read at %#lx, where nothing of the version 8 arbiter is\n", (unsigned long)address);
	return 0;
}

/* version 7: the version and both buses' numbers of channels in its core, the table of channels further into it; the
 * cnfg of the bus named a word a channel; the observer 0x20 a channel, the write channels 0x1000 (the lock held) */
static inline uint32_t
spmi_v7_rd(uintptr_t address)
{
	uint32_t first = spmi_v7_first(), own = spmi.bus_channels[spmi.bus & 1];

	if (address >= spmi.core && address < spmi.core + 0x2800) {
		uint32_t reg = (uint32_t)(address - spmi.core), n = (reg - 0x2000) / 4;

		if (reg == 0x000 || reg == 0x004 || reg == 0x008) {
			return reg == 0x000 ? spmi.version : spmi.bus_channels[reg / 4 - 1];
		}
		if (reg >= 0x2000) {
			spmi.past_the_table += n < first || n >= first + own;
			return n < SPMI_CHANNELS ? spmi.map[n] : 0;
		}
	} else if (address >= spmi.cnfg && address < spmi.cnfg + 0x800) {
		uint32_t n = (uint32_t)(address - spmi.cnfg) / 4;

		spmi.past_the_table += n >= own;
		return first + n < SPMI_CHANNELS ? spmi.owner[first + n] : 0;
	} else if (address >= spmi.observer && address < spmi.observer + 0x20 * SPMI_CHANNELS) {
		uint32_t channel = (uint32_t)(address - spmi.observer) / 0x20, reg = (uint32_t)(address - spmi.observer) % 0x20;

		return reg == 0x08 ? spmi.status[channel] : reg == 0x18 ? spmi.data0[channel] : reg == 0x1c ? spmi.data1[channel] : 0;
	} else if (spmi.channels != 0 && address >= spmi.channels && address < spmi.channels + 0x1000ull * SPMI_CHANNELS) {
		uint32_t channel = (uint32_t)((address - spmi.channels) / 0x1000), reg = (uint32_t)((address - spmi.channels) % 0x1000);

		return reg == 0x08 ? spmi.wstatus[channel] : 0;
	}
	/* (version 5's tables at 0x900 of the core and 0x700 of cnfg are nothing of this arbiter's either) */
	spmi.elsewhere++;
	fprintf(stderr, "MODEL: a read at %#lx, where nothing of the version 7 arbiter is\n", (unsigned long)address);
	return 0;
}

static inline uint32_t
spmi_model_rd(uintptr_t address)
{
	uint32_t value = 0;

	pthread_mutex_lock(&spmi.lock);
	if (spmi.v7) {
		value = spmi_v7_rd(address);
		pthread_mutex_unlock(&spmi.lock);
		return value;
	}
	if (spmi.channel_map != 0) {
		value = spmi_v8_rd(address);
		pthread_mutex_unlock(&spmi.lock);
		return value;
	}
	if (address >= spmi.core && address < spmi.core + 0x1100) {
		uint32_t reg = (uint32_t)(address - spmi.core);

		uint32_t said = spmi.says_channels ? spmi.says_channels - 1 : SPMI_CHANNELS;

		if (reg >= 0x900 && (reg - 0x900) / 4 >= said) {
			spmi.past_the_table++;
		}
		value = reg == 0x000 ? spmi.version : reg == 0x004 ? said :
		    reg >= 0x900 && reg < 0x900 + 4 * SPMI_CHANNELS ? spmi.map[(reg - 0x900) / 4] : 0;
	} else if (address >= spmi.cnfg && address < spmi.cnfg + 0x1000) {
		uint32_t reg = (uint32_t)(address - spmi.cnfg);

		if (reg >= 0x700 && reg < 0x700 + 4 * 0x200 &&
		    (reg - 0x700) / 4 >= (spmi.says_channels ? spmi.says_channels - 1 : SPMI_CHANNELS)) {
			spmi.past_the_table++;
		}
		value = reg >= 0x700 && reg < 0x700 + 4 * SPMI_CHANNELS ? spmi.owner[(reg - 0x700) / 4] : 0;
	} else if (address >= spmi.observer && address < spmi.observer + 0x80 * SPMI_CHANNELS) {
		uint32_t channel = (uint32_t)(address - spmi.observer) / 0x80, reg = (uint32_t)(address - spmi.observer) % 0x80;

		value = reg == 0x08 ? spmi.status[channel] : reg == 0x18 ? spmi.data0[channel] : reg == 0x1c ? spmi.data1[channel] : 0;
	} else if (spmi.channels != 0 && address >= spmi.channels && address < spmi.channels + 0x10000ull * SPMI_CHANNELS) {
		uint32_t channel = (uint32_t)((address - spmi.channels) / 0x10000), reg = (uint32_t)((address - spmi.channels) % 0x10000);

		value = reg == 0x08 ? spmi.wstatus[channel] : 0;
	} else {
		spmi.elsewhere++;
		fprintf(stderr, "MODEL: a read at %#lx, where nothing of the arbiter is\n", (unsigned long)address);
	}
	pthread_mutex_unlock(&spmi.lock);
	return value;
}

static inline void
spmi_model_wr(uintptr_t address, uint32_t value)
{
	/* (version 8: the observer 0x20 a channel, the write channels 0x200; version 7: 0x20 and 0x1000) */
	uint32_t observer_channel = spmi.channel_map || spmi.v7 ? 0x20 : 0x80;
	uint32_t write_channel = spmi.v7 ? 0x1000 : spmi.channel_map ? 0x200 : 0x10000;

	pthread_mutex_lock(&spmi.lock);
	if (address >= spmi.observer && address < spmi.observer + observer_channel * SPMI_CHANNELS &&
	    (address - spmi.observer) % observer_channel == 0) {
		/* a system's read: "count bytes from register reg on" */
		uint32_t channel = (uint32_t)(address - spmi.observer) / observer_channel, reg = value >> 4 & 0xff, count = (value & 7) + 1;
		struct spmi_peripheral *p = spmi_peripheral_of(channel);
		uint8_t bytes[8] = { 0 };

		spmi.transfers++;
		if (!(value & 1u << 27)) {
			spmi.elsewhere++;
			fprintf(stderr, "MODEL: a command through the observer that is not a read: %#x\n", value);
		} else if (spmi.deaf) {
			spmi.status[channel] = 0;
		} else if (p == NULL || p->silent) {
			spmi.status[channel] = SPMI_DONE | SPMI_FAILURE;
		} else if (spmi.drop_reads > 0) {
			spmi.drop_reads--;
			spmi.status[channel] = SPMI_DONE | SPMI_DROPPED;
		} else {
			if (p->looked_at != NULL) {
				p->looked_at(p, (uint8_t)reg);
			}
			for (uint32_t i = 0; i < count; i++) {
				bytes[i] = p->reg[(reg + i) & 0xff];
			}
			spmi.data0[channel] = bytes[0] | bytes[1] << 8 | bytes[2] << 16 | (uint32_t)bytes[3] << 24;
			spmi.data1[channel] = bytes[4] | bytes[5] << 8 | bytes[6] << 16 | (uint32_t)bytes[7] << 24;
			spmi.status[channel] = SPMI_DONE;
		}
	} else if (spmi.channels != 0 && address >= spmi.channels && address < spmi.channels + (uint64_t)write_channel * SPMI_CHANNELS) {
		uint32_t channel = (uint32_t)((address - spmi.channels) / write_channel), reg = (uint32_t)((address - spmi.channels) % write_channel);
		struct spmi_peripheral *p = spmi_peripheral_of(channel);

		if (reg == 0x10) {
			spmi.wdata0[channel] = value;
		} else if (reg == 0x00 && !(value & 1u << 27) && (value & 7) == 0) {
			/* a write of one byte, by the system the channel is of */
			spmi.transfers++;
			if (spmi.deaf) {
				spmi.wstatus[channel] = 0;
			} else if (spmi.owner[channel] != spmi.ee) {
				spmi.denied++;
				spmi.wstatus[channel] = SPMI_DONE | SPMI_DENIED;
			} else if (p == NULL || p->silent) {
				spmi.wstatus[channel] = SPMI_DONE | SPMI_FAILURE;
			} else if (spmi.drop_writes > 0) {
				spmi.drop_writes--;
				spmi.wstatus[channel] = SPMI_DONE | SPMI_DROPPED;
			} else {
				uint8_t which = (uint8_t)(value >> 4 & 0xff), byte = (uint8_t)spmi.wdata0[channel];

				spmi.writes++;
				p->writes++;
				if (p->written != NULL) {
					p->written(p, which, byte);
				} else {
					p->reg[which] = byte;
				}
				spmi.wstatus[channel] = SPMI_DONE;
			}
		} else {
			spmi.elsewhere++;
			fprintf(stderr, "MODEL: a write of %#x at +%#x of channel %u: not a one-byte write\n", value, reg, channel);
		}
	} else {
		spmi.elsewhere++;
		fprintf(stderr, "MODEL: a write of %#x at %#lx, where nothing is to be written\n", value, (unsigned long)address);
	}
	pthread_mutex_unlock(&spmi.lock);
}

#endif /* SPMI_MODEL_H */
