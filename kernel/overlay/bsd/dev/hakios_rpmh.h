/*
 * A request to RPMh: up to sixteen writes to its resources, sent through the first command slot set
 * of the application processor's sequencer, and the wait until RPMh says it has done them. The
 * drivers that ask RPMh for a supply or a vote each had this sequence written out; this is it once.
 *
 * Where the sequencer is and where its slot sets begin comes from the SoC file's [rpmh]
 * (hakios_board()->rpmh.rsc and tcs_offset); a resource's address comes from the command database
 * (hakios_cmd_db.h). The first slot set is taken to be an "active" one, as on every chip so far.
 *
 * The slot set is one for all its users: whoever sends takes redwood_rpmh_lock() around it (not on
 * the way back to the loader, when the other cores are stopped). It is left as it was found, also
 * when RPMh does not answer.
 *
 * Register layout: Linux drivers/soc/qcom/rpmh-rsc.c. The file that includes this one has rd() and
 * wr() (hakios_mmio.h).
 */
#ifndef HAKIOS_RPMH_H
#define HAKIOS_RPMH_H

#include "hakios_time.h"

/* a slot set's registers, from where it begins */
#define TCS_IRQ_ENABLE          0x00            /* a bit per slot set of the sequencer; ours is the first */
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10            /* a bit per command: its answer is waited for */
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18            /* 0 while busy */
#define TCS_CMD_ENABLE          0x1c            /* a bit per command */
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100
#define RPMH_MOST_COMMANDS      16
#define RPMH_TRIES              2000            /* of 100 microseconds each: a fifth of a second */

#define RPMH_SAYS_BUSY          "the command slots stay busy"
#define RPMH_SAYS_NO_ANSWER     "no answer to the request"
#define RPMH_SAYS_NOT_ONE       "a request is one to sixteen commands"

/*
 * Send `count' writes (address[i] <- data[i]) through the slot set at `tcs' (mapped: the sequencer
 * plus tcs_offset) and wait for RPMh. NULL when RPMh has said done, else what went wrong; a slot
 * set that is in use is not written to at all.
 */
static inline const char *
hakios_rpmh_send(vm_offset_t tcs, const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable;
	bool done = false;

	if (count == 0 || count > RPMH_MOST_COMMANDS) {
		return RPMH_SAYS_NOT_ONE;
	}
	irq_enable = rd(tcs + TCS_IRQ_ENABLE);
	/* the slot set must be idle, and switched off the way the hardware wants it: trigger, then enable */
	for (int i = 0; rd(tcs + TCS_STATUS) == 0; i++) {
		if (i > RPMH_TRIES) {
			return RPMH_SAYS_BUSY;
		}
		sleep_us(100);
	}
	wr(tcs + TCS_CONTROL, rd(tcs + TCS_CONTROL) & ~TCS_CONTROL_TRIGGER);
	wr(tcs + TCS_CONTROL, rd(tcs + TCS_CONTROL) & ~TCS_CONTROL_ENABLE);
	wr(tcs + TCS_IRQ_CLEAR, 1);
	for (unsigned i = 0; i < count; i++) {
		wr(tcs + TCS_CMD_MSGID(i), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(tcs + TCS_CMD_ADDR(i), address[i]);
		wr(tcs + TCS_CMD_DATA(i), data[i]);
	}
	wr(tcs + TCS_WAIT_FOR_COMPLETION, (uint32_t)((1ull << count) - 1));
	wr(tcs + TCS_CMD_ENABLE, (uint32_t)((1ull << count) - 1));
	wr(tcs + TCS_IRQ_ENABLE, irq_enable | 1);       /* the status bit follows the enable; the interrupt itself stays masked */
	wr(tcs + TCS_CONTROL, TCS_CONTROL_ENABLE);
	wr(tcs + TCS_CONTROL, TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);
	for (int i = 0; i < RPMH_TRIES && !done; i++) {
		done = (rd(tcs + TCS_IRQ_STATUS) & 1) != 0;
		if (!done) {
			sleep_us(100);
		}
	}
	wr(tcs + TCS_CONTROL, rd(tcs + TCS_CONTROL) & ~TCS_CONTROL_TRIGGER);
	wr(tcs + TCS_CONTROL, rd(tcs + TCS_CONTROL) & ~TCS_CONTROL_ENABLE);
	wr(tcs + TCS_CMD_ENABLE, 0);
	wr(tcs + TCS_WAIT_FOR_COMPLETION, 0);
	wr(tcs + TCS_IRQ_CLEAR, 1);
	wr(tcs + TCS_IRQ_ENABLE, irq_enable);
	return done ? NULL : RPMH_SAYS_NO_ANSWER;
}

#endif /* HAKIOS_RPMH_H */
