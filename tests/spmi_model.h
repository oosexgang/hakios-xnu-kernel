/*
 * A model of the SoC's PMIC arbiter, version 5 (as Linux's drivers/spmi/spmi-pmic-arb.c has it), with
 * the peripherals a test puts behind it. For the tests of drivers that reach a PMIC: the driver's
 * registers are this model's (its test seam), so what a driver reads is what the test set up, and
 * what it writes is checked here.
 *
 *   core        0x000 the version, 0x004 the number of channels, 0x900 + 4n which peripheral channel n is
 *   cnfg        0x700 + 4n which system may write through channel n
 *   observer    a system's reads: 0x80 a channel; a command at 0x00, the status at 0x08, the bytes at 0x18
 *   channels    writes: 0x10000 a channel; the byte at 0x10, then a command at 0x00, the status at 0x08
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

struct spmi_peripheral {
	uint16_t id;                    /* slave << 8 | peripheral */
	uint8_t reg[256];
	bool silent;                    /* nobody at that address: a transfer fails */
	/* a write the test wants to see or to change; NULL: the register takes the value */
	void (*written)(struct spmi_peripheral *p, uint8_t reg, uint8_t value);
	unsigned writes;
};

static struct spmi_model {
	uint32_t core, cnfg, observer, channels;        /* observer: this system's part of the block */
	uint32_t version, ee;
	uint32_t map[SPMI_CHANNELS], owner[SPMI_CHANNELS];
	struct spmi_peripheral *peripheral[SPMI_PERIPHERALS];
	uint32_t status[SPMI_CHANNELS], data0[SPMI_CHANNELS], data1[SPMI_CHANNELS];     /* a channel's read */
	uint32_t wstatus[SPMI_CHANNELS], wdata0[SPMI_CHANNELS];                         /* ... and its write */
	unsigned transfers, writes, denied;
	unsigned elsewhere;             /* accesses where nothing of the arbiter is, and commands that are none */
	pthread_mutex_t lock;
} spmi = { .version = 0x50000000, .lock = PTHREAD_MUTEX_INITIALIZER };

static inline struct spmi_peripheral *
spmi_peripheral_of(uint32_t channel)
{
	uint16_t id = (uint16_t)(spmi.map[channel] >> 8 & 0xfff);

	for (unsigned i = 0; i < SPMI_PERIPHERALS; i++) {
		if (spmi.peripheral[i] != NULL && spmi.peripheral[i]->id == id) {
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

static inline uint32_t
spmi_model_rd(uintptr_t address)
{
	uint32_t value = 0;

	pthread_mutex_lock(&spmi.lock);
	if (address >= spmi.core && address < spmi.core + 0x1100) {
		uint32_t reg = (uint32_t)(address - spmi.core);

		value = reg == 0x000 ? spmi.version : reg == 0x004 ? SPMI_CHANNELS :
		    reg >= 0x900 && reg < 0x900 + 4 * SPMI_CHANNELS ? spmi.map[(reg - 0x900) / 4] : 0;
	} else if (address >= spmi.cnfg && address < spmi.cnfg + 0x1000) {
		uint32_t reg = (uint32_t)(address - spmi.cnfg);

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
	pthread_mutex_lock(&spmi.lock);
	if (address >= spmi.observer && address < spmi.observer + 0x80 * SPMI_CHANNELS && (address - spmi.observer) % 0x80 == 0) {
		/* a system's read: "count bytes from register reg on" */
		uint32_t channel = (uint32_t)(address - spmi.observer) / 0x80, reg = value >> 4 & 0xff, count = (value & 7) + 1;
		struct spmi_peripheral *p = spmi_peripheral_of(channel);
		uint8_t bytes[8] = { 0 };

		spmi.transfers++;
		if (!(value & 1u << 27)) {
			spmi.elsewhere++;
			fprintf(stderr, "MODEL: a command through the observer that is not a read: %#x\n", value);
		} else if (p == NULL || p->silent) {
			spmi.status[channel] = SPMI_DONE | SPMI_FAILURE;
		} else {
			for (uint32_t i = 0; i < count; i++) {
				bytes[i] = p->reg[(reg + i) & 0xff];
			}
			spmi.data0[channel] = bytes[0] | bytes[1] << 8 | bytes[2] << 16 | (uint32_t)bytes[3] << 24;
			spmi.data1[channel] = bytes[4] | bytes[5] << 8 | bytes[6] << 16 | (uint32_t)bytes[7] << 24;
			spmi.status[channel] = SPMI_DONE;
		}
	} else if (spmi.channels != 0 && address >= spmi.channels && address < spmi.channels + 0x10000ull * SPMI_CHANNELS) {
		uint32_t channel = (uint32_t)((address - spmi.channels) / 0x10000), reg = (uint32_t)((address - spmi.channels) % 0x10000);
		struct spmi_peripheral *p = spmi_peripheral_of(channel);

		if (reg == 0x10) {
			spmi.wdata0[channel] = value;
		} else if (reg == 0x00 && !(value & 1u << 27) && (value & 7) == 0) {
			/* a write of one byte, by the system the channel is of */
			spmi.transfers++;
			if (spmi.owner[channel] != spmi.ee) {
				spmi.denied++;
				spmi.wstatus[channel] = SPMI_DONE | SPMI_DENIED;
			} else if (p == NULL || p->silent) {
				spmi.wstatus[channel] = SPMI_DONE | SPMI_FAILURE;
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
