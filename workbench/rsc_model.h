/*
 * A model of the first command slot set of an RPMh sequencer (as Linux's drivers/soc/qcom/rpmh-rsc.c
 * has it), for the tests of what sends requests through it: the registers are this model's (the
 * driver's test seam), what is triggered is recorded, and a test can make RPMh deaf or the slot set
 * busy. A test says where the slot set is (the sequencer's address plus the board's tcs_offset), so
 * that a wrong address in a driver is an access to nothing.
 */
#ifndef RSC_MODEL_H
#define RSC_MODEL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define COMMANDS        16
static struct {
	uint32_t base;                  /* the slot set: the sequencer's address + rpmh { } tcs_offset of the board */
	uint32_t irq_enable, irq_status, wait, control, enabled;
	uint32_t message[COMMANDS], address[COMMANDS], data[COMMANDS];
	bool deaf;                      /* RPMh never says a request is done */
	bool busy;                      /* the slot set never becomes idle */
	/* what was sent: each trigger's commands */
	struct { uint32_t address, data, message; } sent[8];
	unsigned sends, triggers, writes, elsewhere;
	uint32_t wait_at_trigger;       /* which commands' answers were to be waited for, when the last was sent */
	unsigned changed_while_sent;    /* a command or which are enabled, written while the trigger was set */
} rsc;

static inline uint32_t
rsc_model_rd(vm_offset_t address)
{
	uint32_t reg = (uint32_t)(address - rsc.base);

	if (rsc.base != 0 && address >= rsc.base && reg < 0x2a0) {
		unsigned c = reg >= 0x30 ? (reg - 0x30) / 20 : 0, field = reg >= 0x30 ? (reg - 0x30) % 20 : 0;

		return reg == 0x00 ? rsc.irq_enable : reg == 0x04 ? rsc.irq_status : reg == 0x10 ? rsc.wait : reg == 0x14 ? rsc.control :
		       reg == 0x18 ? !rsc.busy : reg == 0x1c ? rsc.enabled : reg < 0x30 ? 0 :
		       field == 0 ? rsc.message[c] : field == 4 ? rsc.address[c] : field == 8 ? rsc.data[c] : 0;
	}
	/* (a look at every slot set reads the eight after this one: nothing in them) */
	if (rsc.base != 0 && address >= rsc.base && reg < 0x2a0 * 9) {
		return 0;
	}
	rsc.elsewhere++;
	fprintf(stderr, "MODEL: a read at %#lx, where nothing of the sequencer is\n", (unsigned long)address);
	return 0;
}

static inline void
rsc_model_wr(vm_offset_t address, uint32_t value)
{
	uint32_t reg = (uint32_t)(address - rsc.base);

	if (rsc.base == 0 || address < rsc.base || reg >= 0x2a0) {
		rsc.elsewhere++;
		fprintf(stderr, "MODEL: a write of %#x at %#lx, which is not the first slot set\n", value, (unsigned long)address);
		return;
	}
	rsc.writes++;
	if ((rsc.control & 1u << 24) && (reg == 0x10 || reg == 0x1c || reg >= 0x30)) {
		rsc.changed_while_sent++;
		fprintf(stderr, "MODEL: register %#x of the slot set written while its trigger is set\n", reg);
	}
	if (reg == 0x00) {
		rsc.irq_enable = value;
	} else if (reg == 0x08) {
		rsc.irq_status &= ~value;
	} else if (reg == 0x10) {
		rsc.wait = value;
	} else if (reg == 0x1c) {
		rsc.enabled = value;
	} else if (reg == 0x14) {
		bool was = rsc.control & 1u << 24;

		rsc.control = value;
		if (!was && (value & 1u << 24) && (value & 1u << 16)) {
			/* triggered: every enabled command goes to RPMh */
			rsc.triggers++;
			rsc.wait_at_trigger = rsc.wait;
			for (unsigned c = 0; c < COMMANDS; c++) {
				if ((rsc.enabled & 1u << c) && rsc.sends < 8) {
					rsc.sent[rsc.sends].address = rsc.address[c];
					rsc.sent[rsc.sends].data = rsc.data[c];
					rsc.sent[rsc.sends++].message = rsc.message[c];
				}
			}
			if (!rsc.deaf) {
				rsc.irq_status |= 1;
			}
		}
	} else if (reg >= 0x30 && (reg - 0x30) / 20 < COMMANDS) {
		unsigned c = (reg - 0x30) / 20, field = (reg - 0x30) % 20;

		if (field == 0) {
			rsc.message[c] = value;
		} else if (field == 4) {
			rsc.address[c] = value;
		} else if (field == 8) {
			rsc.data[c] = value;
		}
	}
}

#endif /* RSC_MODEL_H */
