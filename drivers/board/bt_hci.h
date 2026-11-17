/*
 * Bluetooth between Darwin's bluetoothd and a controller that is not Apple's: H4 packets, and what has to be
 * answered or changed on the way so that bluetoothd, which was written for Broadcom's and Apple's own
 * controllers, takes the controller it finds. Pure: no hardware, no locks; board/bt_host.c does the moving,
 * and a test runs these on a host (workbench/bt_hci_test.c).
 *
 * What bluetoothd asks and what its parsers read was found by HakiOS's MediaTek port (hakios-mt6768,
 * redwood_btsk.c / redwood_mtkbt_skel.c) and by the OnePlus Nord 2 port of iOS built on it
 * (frxncyyy/ios-denniz, piece 0721), both under the Apple Public Source License 2.0, from bluetoothd's own
 * code and the phones' logs. Darwin 27's bluetoothd opens the same things (com.apple.uart.sk.bluetooth,
 * "marconi-bt" or "bluetooth", transport-encoding, local-mac-address).
 */
#ifndef BOARD_BT_HCI_H
#define BOARD_BT_HCI_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* H4: a packet's type, then its header; 1 command, 2 ACL data, 3 SCO data, 4 event, 5 ISO data */
#define BT_H4_COMMAND           1
#define BT_H4_ACL               2
#define BT_H4_SCO               3
#define BT_H4_EVENT             4
#define BT_H4_ISO               5
#define BT_H4_MOST              (5 + 0x3fff)    /* the longest packet there is (ISO) */

/* What is answered or changed, a bit each (bt_quirks=, default BT_Q_DEFAULT) */
#define BT_Q_VENDOR             0x01u   /* Broadcom's and Apple's vendor commands (OGF 0x3f) answered done, with the data
                                         * bluetoothd's parsers read after the status; never sent to the controller */
#define BT_Q_ERRDATA            0x02u   /* Read/Write Default Erroneous Data Reporting answered (the controller may not have it:
                                         * bluetoothd's HCI initialisation ends with it, and fails without it) */
#define BT_Q_LE_COMMANDS        0x04u   /* Read Local Supported Commands: the LE commands bluetoothd counts on, and the two above, said */
#define BT_Q_VENDOR_EVENTS      0x08u   /* the controller's own vendor events (0xff) not handed on: bluetoothd takes them for Broadcom's */
#define BT_Q_LEGACY_LE          0x10u   /* LE Read Periodic Advertiser List Size answered: the one extended-advertising command of the
                                         * initialisation, after which a controller may refuse legacy LE scanning (Core 5.x, Vol 4 E 3.1.1) */
#define BT_Q_DEFAULT            (BT_Q_VENDOR | BT_Q_ERRDATA | BT_Q_LE_COMMANDS | BT_Q_VENDOR_EVENTS | BT_Q_LEGACY_LE)

/* A whole packet's length from its first bytes: 0 while there are too few of them to say, or for a type there is not */
static inline unsigned
bt_h4_length(const uint8_t *b, unsigned have)
{
	if (have < 1) {
		return 0;
	}
	switch (b[0]) {
	case BT_H4_COMMAND:
	case BT_H4_SCO:
		return have >= 4 ? 4u + b[3] : 0;
	case BT_H4_ACL:
		return have >= 5 ? 5u + (b[3] | ((unsigned)b[4] << 8)) : 0;
	case BT_H4_EVENT:
		return have >= 3 ? 3u + b[2] : 0;
	case BT_H4_ISO:
		return have >= 5 ? 5u + ((b[3] | ((unsigned)b[4] << 8)) & 0x3fff) : 0;
	default:
		return 0;
	}
}

/*
 * What a host wrote, a run of bytes in a queue of ROOM: the length of the whole packet at its head; 0 while it is not
 * all there yet; -1 when the run cannot be a host's packets (an event, which only a controller sends; a type there is
 * not; a packet longer than the queue, which would never be whole there) and is to be thrown away.
 */
static inline int
bt_h4_from_host(const uint8_t *q, unsigned have, unsigned room)
{
	unsigned n = bt_h4_length(q, have);

	if (have == 0) {
		return 0;
	}
	if (q[0] == BT_H4_EVENT || (have >= 5 && n == 0) || n > room) {
		return -1;
	}
	return n <= have ? (int)n : 0;           /* (n is 0 while its header is not all there) */
}

/* a command's opcode (H4 type 1), 0 for anything else */
static inline uint16_t
bt_opcode(const uint8_t *c, unsigned n)
{
	return n >= 4 && c[0] == BT_H4_COMMAND ? (uint16_t)(c[1] | (c[2] << 8)) : 0;
}

/* Command Complete for OPCODE with status 0 and DATA zero bytes after it, the first FIRST: into EVENT, its length */
static inline unsigned
bt_command_complete(uint8_t *event, unsigned room, uint16_t opcode, unsigned data, int first)
{
	if (room < 7 + data || data > 255 - 4) {
		return 0;
	}
	event[0] = BT_H4_EVENT;
	event[1] = 0x0e;
	event[2] = (uint8_t)(4 + data);
	event[3] = 1;                           /* (the controller may take one more command) */
	event[4] = (uint8_t)opcode;
	event[5] = (uint8_t)(opcode >> 8);
	event[6] = 0;
	memset(event + 7, 0, data);
	if (data != 0 && first >= 0) {
		event[7] = (uint8_t)first;
	}
	return 7 + data;
}

/*
 * A command bluetoothd wrote: the answer it gets from the kernel instead of the controller, into EVENT, its length;
 * 0: the command goes to the controller.
 */
static inline unsigned
bt_answer_here(const uint8_t *c, unsigned n, uint32_t quirks, uint8_t *event, unsigned room)
{
	uint16_t op = bt_opcode(c, n);

	if (op == 0) {
		return 0;
	}
	if ((op == 0x0c5a || op == 0x0c5b) && (quirks & BT_Q_ERRDATA)) {
		/* Read (0x0c5a): status, then the mode, 0, reporting off -- which is what a controller without it does */
		return bt_command_complete(event, room, op, op == 0x0c5a ? 1 : 0, 0);
	}
	if (op == 0x204a && (quirks & BT_Q_LEGACY_LE)) {
		return bt_command_complete(event, room, op, 1, 0x10);   /* (sixteen, what one such controller said itself) */
	}
	if ((op >> 10) != 0x3f || !(quirks & BT_Q_VENDOR)) {
		return 0;
	}
	/*
	 * A vendor command: done. Its answer has what the command's parser in bluetoothd reads after the status, zeros:
	 * 0xfd1b (calibration) 64 bytes, the first 0x51; 0xfce2 (a family) by its sub-command -- 0x15 and 0x36 (antennas) 16,
	 * 0x0d (profiling statistics, parsed as four 16-bit numbers: bluetoothd aborts with fewer) 8, 0x2d 4, 0x05 and 0x0a 1;
	 * 0xfd48 1; every other none. More than a parser reads is not harmless: 0xfce9's stalled the initialisation.
	 * (Write_BD_ADDR, 0xfc01, is among them: the controller keeps the address it has.)
	 */
	{
		uint8_t sub = n >= 5 ? c[4] : 0;
		unsigned data = 0;
		int first = -1;

		if (op == 0xfd1b) {
			data = 64;
			first = 0x51;
		} else if (op == 0xfce2) {
			data = sub == 0x15 || sub == 0x36 ? 16 : sub == 0x0d ? 8 : sub == 0x2d ? 4 : sub == 0x05 || sub == 0x0a ? 1 : 0;
		} else if (op == 0xfd48) {
			data = 1;
		}
		return bt_command_complete(event, room, op, data, first);
	}
}

/*
 * A packet from the controller, changed in place for bluetoothd: its length after, 0 when it is not to be handed on.
 */
static inline unsigned
bt_rewrite(uint8_t *p, unsigned n, uint32_t quirks)
{
	if (n >= 3 && p[0] == BT_H4_EVENT && p[1] == 0xff && (quirks & BT_Q_VENDOR_EVENTS)) {
		return 0;
	}
	/* Read Local Supported Commands (0x1002): 64 bytes of bits after the status */
	if ((quirks & BT_Q_LE_COMMANDS) && n >= 7 + 64 && p[0] == BT_H4_EVENT && p[1] == 0x0e && p[2] >= 4 + 64 && p[4] == 0x02 &&
	    p[5] == 0x10 && p[6] == 0) {
		static const uint8_t le[4] = { 0xf7, 0xff, 0xff, 0x7f };        /* octets 25 to 28: the LE commands of 4.0 to 4.2 */

		for (unsigned i = 0; i < 4; i++) {
			p[7 + 25 + i] |= le[i];
		}
		p[7 + 24] |= 0x60;                      /* LE Read Buffer Size, LE Read Local Supported Features */
		if (quirks & BT_Q_ERRDATA) {
			p[7 + 18] |= 0x0c;              /* Read/Write Default Erroneous Data Reporting: answered by bt_answer_here() */
		}
	}
	return n;
}

#endif /* BOARD_BT_HCI_H */
