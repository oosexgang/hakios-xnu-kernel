/*
 * The registers of the way to the SoC's other processors: the always-on processor's mailbox and the
 * block the processors ring each other through. Where the two are comes from the SoC file's ipc { }
 * (hakios_board()->ipc.aoss and ipcc). The layouts, once, for the drivers that talk to another
 * processor; what is said through them is each driver's.
 *
 * The mailbox (Linux: drivers/soc/qcom/qcom_aoss.c, "QMP"): a few words of message memory with
 * two directions -- the always-on processor's and ours -- each with a link state, a channel state,
 * the other side's acknowledgement of both, and where and how big its mailbox is. A message is text;
 * it goes into our mailbox behind its length, the doorbell is rung, and the always-on processor has
 * taken it when the length reads zero.
 *
 * The doorbells (Linux: drivers/mailbox/qcom-ipcc.c): a doorbell is a processor ("client") and one of
 * its signals. Which client a driver's processor is, the driver says (the device tree's mboxes).
 */
#ifndef HAKIOS_IPC_H
#define HAKIOS_IPC_H

#include <stdbool.h>
#include <stdint.h>

/* the mailbox's words */
#define QMP_MAGIC               0x00
#define QMP_MAGIC_VALUE         0x4d41494c      /* "LIAM" */
#define QMP_VERSION             0x04
#define QMP_FEATURES            0x08
#define QMP_THEIR_LINK          0x0c
#define QMP_THEIR_LINK_ACK      0x10            /* ours to write: we have seen their link state */
#define QMP_THEIR_CHANNEL       0x14
#define QMP_THEIR_CHANNEL_ACK   0x18            /* ours to write */
#define QMP_THEIR_BOX_SIZE      0x1c
#define QMP_THEIR_BOX           0x20
#define QMP_OUR_LINK            0x24            /* ours to write */
#define QMP_OUR_LINK_ACK        0x28
#define QMP_OUR_CHANNEL         0x2c            /* ours to write */
#define QMP_OUR_CHANNEL_ACK     0x30
#define QMP_OUR_BOX_SIZE        0x34
#define QMP_OUR_BOX             0x38
#define QMP_UP                  0x0000ffffu
#define QMP_DOWN                0xffff0000u
#define QMP_MESSAGE             64              /* what Linux sends, whatever the text's length */
#define QMP_BOXES_FROM          0x3c            /* the mailboxes lie after the words above */

/*
 * Is our mailbox, as the always-on processor says where it is and how big (QMP_OUR_BOX, QMP_OUR_BOX_SIZE),
 * one a message fits in, after the words above and inside the block of `block' bytes? Both numbers are the
 * other processor's: they are not added, which could wrap.
 */
static inline bool
qmp_box_usable(uint32_t box, uint32_t size, uint64_t block)
{
	return size >= QMP_MESSAGE + 4 && box >= QMP_BOXES_FROM && !(box & 3) && box <= block && size <= block - box;
}

/* the doorbells' registers */
#define IPCC_SEND               0x0c            /* write client << 16 | signal: ring it */
#define IPCC_RECEIVED           0x10            /* who rang us, the same way; all ones: nobody */
#define IPCC_ENABLE             0x14            /* write: hear that client's signal */
#define IPCC_DISABLE            0x18            /* write: no longer */
#define IPCC_CLEAR              0x1c            /* write: that one has been heard */
#define IPCC_NOBODY             0xffffffffu
#define IPCC_AOP                0u              /* the client that is the always-on processor */
#define IPCC_GLINK              0u              /* signals: the mailbox, or GLINK */
#define IPCC_SMP2P              2u              /* ... and SMP2P */

#endif /* HAKIOS_IPC_H */
