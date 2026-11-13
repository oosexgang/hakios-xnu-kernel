/*
 * A request to RPMh: up to sixteen writes to its resources, sent through the first command slot set
 * of the application processor's sequencer, and the wait until RPMh says it has done them. The
 * drivers that ask RPMh for a supply or a vote each had this sequence written out; this is it once.
 *
 * Where the sequencer is and where its slot sets begin comes from the SoC file's rpmh { }
 * (hakios_board()->rpmh.rsc and tcs_offset); a resource's address comes from the command database
 * (board/cmd_db.h). The first slot set is taken to be an "active" one, as on every chip so far.
 *
 * The slot set is one for all its users: whoever sends takes hakios_rpmh_lock() around it (not on
 * the way back to the loader, when the other cores are stopped). It is left as it was found, also
 * when RPMh does not answer.
 *
 * Register layout: Linux drivers/soc/qcom/rpmh-rsc.c. The file that includes this one has rd() and
 * wr() (board/mmio.h).
 */
#ifndef HAKIOS_RPMH_H
#define HAKIOS_RPMH_H

#include "time.h"

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
 * Where a slot set's registers are. Version 3 of the sequencer and later (Linux rpmh-rsc.c, "v3" register
 * offsets; read in RSC_DRV_ID at the sequencer's beginning, bits 23:16) moved all but the first three.
 */
struct hakios_rpmh_layout {
	uint32_t        wait, control, status, cmd_enable, msgid, stride;
};
#define RPMH_LAYOUT_V2          ((struct hakios_rpmh_layout){ 0x10, 0x14, 0x18, 0x1c, 0x30, 20 })
#define RPMH_LAYOUT_V3          ((struct hakios_rpmh_layout){ 0x20, 0x24, 0x28, 0x2c, 0x34, 24 })
#define RSC_DRV_ID              0x00
#define RSC_MAJOR(id)           (((id) >> 16) & 0xff)

/*
 * Send `count' writes (address[i] <- data[i]) through the slot set at `tcs' laid out as `l', and wait
 * for RPMh. NULL when RPMh has said done, else what went wrong; a slot set that is in use is not
 * written to at all.
 */
static inline const char *
hakios_rpmh_send_with(vm_offset_t tcs, struct hakios_rpmh_layout l, const uint32_t *address, const uint32_t *data,
    unsigned count)
{
	uint32_t irq_enable;
	bool done = false;

	if (count == 0 || count > RPMH_MOST_COMMANDS) {
		return RPMH_SAYS_NOT_ONE;
	}
	irq_enable = rd(tcs + TCS_IRQ_ENABLE);
	/* the slot set must be idle, and switched off the way the hardware wants it: trigger, then enable */
	for (int i = 0; rd(tcs + l.status) == 0; i++) {
		if (i > RPMH_TRIES) {
			return RPMH_SAYS_BUSY;
		}
		sleep_us(100);
	}
	wr(tcs + l.control, rd(tcs + l.control) & ~TCS_CONTROL_TRIGGER);
	wr(tcs + l.control, rd(tcs + l.control) & ~TCS_CONTROL_ENABLE);
	wr(tcs + TCS_IRQ_CLEAR, 1);
	for (unsigned i = 0; i < count; i++) {
		wr(tcs + l.msgid + l.stride * i, MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(tcs + l.msgid + l.stride * i + 4, address[i]);
		wr(tcs + l.msgid + l.stride * i + 8, data[i]);
	}
	wr(tcs + l.wait, (uint32_t)((1ull << count) - 1));
	wr(tcs + l.cmd_enable, (uint32_t)((1ull << count) - 1));
	wr(tcs + TCS_IRQ_ENABLE, irq_enable | 1);       /* the status bit follows the enable; the interrupt itself stays masked */
	wr(tcs + l.control, TCS_CONTROL_ENABLE);
	wr(tcs + l.control, TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);
	for (int i = 0; i < RPMH_TRIES && !done; i++) {
		done = (rd(tcs + TCS_IRQ_STATUS) & 1) != 0;
		if (!done) {
			sleep_us(100);
		}
	}
	wr(tcs + l.control, rd(tcs + l.control) & ~TCS_CONTROL_TRIGGER);
	wr(tcs + l.control, rd(tcs + l.control) & ~TCS_CONTROL_ENABLE);
	wr(tcs + l.cmd_enable, 0);
	wr(tcs + l.wait, 0);
	wr(tcs + TCS_IRQ_CLEAR, 1);
	wr(tcs + TCS_IRQ_ENABLE, irq_enable);
	return done ? NULL : RPMH_SAYS_NO_ANSWER;
}

/* the same through a sequencer of version 2 (every chip before SM8845): `tcs' is the sequencer plus tcs_offset */
static inline const char *
hakios_rpmh_send(vm_offset_t tcs, const uint32_t *address, const uint32_t *data, unsigned count)
{
	return hakios_rpmh_send_with(tcs, RPMH_LAYOUT_V2, address, data, count);
}

/* the same, the layout chosen by the sequencer's own version: `rsc' is the sequencer, mapped */
static inline const char *
hakios_rpmh_send_rsc(vm_offset_t rsc, uint32_t tcs_offset, const uint32_t *address, const uint32_t *data,
    unsigned count)
{
	return hakios_rpmh_send_with(rsc + tcs_offset, RSC_MAJOR(rd(rsc + RSC_DRV_ID)) >= 3 ? RPMH_LAYOUT_V3 : RPMH_LAYOUT_V2,
	           address, data, count);
}

#endif /* HAKIOS_RPMH_H */
