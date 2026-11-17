/*
 * Bluetooth: the firmware a Qualcomm controller is given before it is used -- its patch to the ROM ("rampatch") and
 * its configuration (NVM) -- as the files the phone's Android has (vendor/firmware: msbtfw*.mbn or .tlv, msnv*.bin)
 * and as the commands that carry them. Pure: no hardware, no locks; board/bt_uart.c sends what is made here and
 * hands back what the controller answers, and a test runs these on a host (workbench/bt_qca_test.c).
 *
 * What the files are and how they are sent is Qualcomm's protocol as Linux's drivers/bluetooth/btqca.c speaks it
 * (written here from what that driver does, not from its code):
 *   - a TLV file: one header word, its type in the low byte (1 the patch, 2 the NVM) and the length of what follows in
 *     the three above; a patch's own header then says, among the rest, its "download mode": which answers the
 *     controller leaves out while the segments come;
 *   - an ELF file (.mbn, the newer chips' patch): sent as it is, every segment answered;
 *   - either goes in segments of at most 243 bytes, each the vendor command 0xfc00 with the sub-command 0x1e (TLV
 *     request), the segment's length, and the segment; the controller answers each with a vendor event (0xff) whose
 *     response type is that sub-command and whose status is 0 -- or with a Command Complete of 0xfc00, status 0 --
 *     unless the download mode says it leaves that answer out; the last segment is answered whatever the mode says.
 *
 * The NVM is sent as it is: Linux changes two of its tags to turn on the controller's in-band sleep and a faster UART,
 * which board/bt_uart.c, at 115200 baud without that sleep protocol, does not want.
 */
#ifndef BOARD_BT_QCA_H
#define BOARD_BT_QCA_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BT_QCA_SEGMENT          243u    /* the most a segment carries */
#define BT_QCA_TLV_REQUEST      0x1e    /* the sub-command of 0xfc00 that carries a segment */
#define BT_QCA_OPCODE           0xfc00u /* EDL: Qualcomm's vendor command */
#define BT_QCA_COMMAND_MOST     (4 + 2 + BT_QCA_SEGMENT)        /* a segment's H4 command, whole */

enum bt_qca_kind {
	BT_QCA_NOT_ONE = 0,             /* a file that is none of these, or one that says it is longer than it is */
	BT_QCA_PATCH,                   /* a TLV patch (type 1) */
	BT_QCA_NVM,                     /* a TLV configuration (type 2) */
	BT_QCA_ELF,                     /* an ELF patch (.mbn) */
};

/* Which answers the controller leaves out while a patch's segments come (a TLV patch's download mode) */
#define BT_QCA_ANSWERS_ALL      0       /* none: each segment is answered */
#define BT_QCA_NO_VENDOR_EVENT  1       /* no vendor event */
#define BT_QCA_NO_COMPLETE      2       /* no Command Complete */
#define BT_QCA_NO_EITHER        3       /* neither */

struct bt_qca_file {
	enum bt_qca_kind kind;
	uint8_t mode;                   /* BT_QCA_ANSWERS_ALL ... BT_QCA_NO_EITHER (a TLV patch's; ANSWERS_ALL otherwise) */
	uint16_t product, rom_build, patch_version;     /* a TLV patch's header says them; 0 otherwise */
};

static inline uint32_t
bt_qca_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/*
 * What DATA, SIZE bytes long, is: a TLV patch or configuration whose header's length is what follows it, or an ELF file
 * of at least its identification. A TLV patch's header (after the word: total size, data length, format version,
 * signature, download mode, reserved, product, ROM build, patch version) is read for the download mode and the
 * versions, which are said in the log.
 */
static inline struct bt_qca_file
bt_qca_identify(const uint8_t *data, uint32_t size)
{
	struct bt_qca_file f = { BT_QCA_NOT_ONE, BT_QCA_ANSWERS_ALL, 0, 0, 0 };
	uint32_t word, length;

	if (data == NULL) {
		return f;
	}
	if (size >= 16 && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F') {
		f.kind = BT_QCA_ELF;
		return f;
	}
	if (size < 4) {
		return f;
	}
	word = bt_qca_le32(data);
	length = word >> 8;
	if (length == 0 || length > size - 4) {
		return f;
	}
	switch (word & 0xff) {
	case 1:
		if (length < 22) {
			return f;       /* (no room for the patch's own header) */
		}
		f.kind = BT_QCA_PATCH;
		f.mode = data[4 + 10] <= BT_QCA_NO_EITHER ? data[4 + 10] : BT_QCA_ANSWERS_ALL;
		f.product = (uint16_t)(data[4 + 12] | data[4 + 13] << 8);
		f.rom_build = (uint16_t)(data[4 + 14] | data[4 + 15] << 8);
		f.patch_version = (uint16_t)(data[4 + 16] | data[4 + 17] << 8);
		return f;
	case 2:
		f.kind = BT_QCA_NVM;
		return f;
	default:
		return f;
	}
}

/* How many segments a file of SIZE bytes goes in */
static inline uint32_t
bt_qca_segments(uint32_t size)
{
	return (size + BT_QCA_SEGMENT - 1) / BT_QCA_SEGMENT;
}

/*
 * The H4 command that carries segment NUMBER of DATA (SIZE bytes), into COMMAND (ROOM bytes): its length, 0 when there
 * is no such segment or no room for it.
 */
static inline unsigned
bt_qca_segment(uint8_t *command, unsigned room, const uint8_t *data, uint32_t size, uint32_t number)
{
	uint32_t at = number * BT_QCA_SEGMENT, length;

	if (number >= bt_qca_segments(size)) {
		return 0;
	}
	length = size - at < BT_QCA_SEGMENT ? size - at : BT_QCA_SEGMENT;
	if (room < 6 + length) {
		return 0;
	}
	command[0] = 1;                                 /* H4: a command */
	command[1] = (uint8_t)BT_QCA_OPCODE;
	command[2] = (uint8_t)(BT_QCA_OPCODE >> 8);
	command[3] = (uint8_t)(2 + length);
	command[4] = BT_QCA_TLV_REQUEST;
	command[5] = (uint8_t)length;
	memcpy(command + 6, data + at, length);
	return 6 + length;
}

/* Is segment NUMBER of a file of SIZE bytes answered, in MODE? (the last always is) */
static inline bool
bt_qca_answered(uint8_t mode, uint32_t size, uint32_t number)
{
	return mode == BT_QCA_ANSWERS_ALL || mode == BT_QCA_NO_COMPLETE || number + 1 >= bt_qca_segments(size);
}

/*
 * What the controller's EVENT (an H4 packet of N bytes) says of a segment: 1 taken, -1 refused (its status), 0 not an
 * answer to a segment at all (another event, which the caller passes over).
 */
static inline int
bt_qca_segment_answer(const uint8_t *event, unsigned n)
{
	if (n >= 6 && event[0] == 4 && event[1] == 0xff && event[2] >= 3 && event[3] == 0x00 && event[4] == BT_QCA_TLV_REQUEST) {
		return event[5] == 0 ? 1 : -1;          /* a vendor event: response to a request, of the TLV request */
	}
	if (n >= 7 && event[0] == 4 && event[1] == 0x0e && event[2] >= 4 && event[4] == (uint8_t)BT_QCA_OPCODE &&
	    event[5] == (uint8_t)(BT_QCA_OPCODE >> 8)) {
		/* a Command Complete of 0xfc00: its status, then (as Linux's btqca reads it) the request it answers -- one of
		 * another request (the version's, 0x19) is not a segment's, nor taken for one */
		if (event[2] >= 5 && n >= 8 && event[7] != BT_QCA_TLV_REQUEST) {
			return 0;
		}
		return event[6] == 0 ? 1 : -1;
	}
	return 0;
}

#endif /* BOARD_BT_QCA_H */
