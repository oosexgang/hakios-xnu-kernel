/*
 * QRTR's datagrams as they arrive from another processor (Linux: net/qrtr/af_qrtr.c): the header in either
 * of its two versions, and where the datagram's bytes are. Once, for the drivers that carry QRTR over GLINK
 * (the audio DSP's and the Wi-Fi's); what each does with a datagram is its own. The bytes are the other
 * processor's: nothing here trusts a size or an offset to be inside what arrived.
 */
#ifndef HAKIOS_QRTR_FRAME_H
#define HAKIOS_QRTR_FRAME_H

#include <libkern/libkern.h>
#include <mach/mach_types.h>

#define QRTR_US                 1u              /* our node */
#define QRTR_CONTROL            0xfffffffeu     /* the control port */
enum { QRTR_DATA = 1, QRTR_HELLO = 2, QRTR_BYE = 3, QRTR_NEW_SERVER = 4, QRTR_DEL_SERVER = 5, QRTR_DEL_CLIENT = 6, QRTR_RESUME_TX = 7,
       QRTR_NEW_LOOKUP = 10, QRTR_DEL_LOOKUP = 11 };

struct qrtr_header {                            /* version 1, which Linux sends; version 2 is read as well */
	uint32_t version, type, from_node, from_port, confirm, size, to_node, to_port;
};
struct qrtr_control {
	uint32_t command;
	uint32_t words[4];                      /* a server: service, instance, node, port; a client: node, port */
};

enum qrtr_frame_result { QRTR_FRAME_OK, QRTR_FRAME_NO_DATAGRAM, QRTR_FRAME_SIZE_WRONG };

/*
 * The header of what arrived, in version 1's shape whichever it came in, and where its datagram begins
 * (*at); OK only when the datagram is not empty and lies wholly inside the `length' bytes that arrived.
 */
static inline enum qrtr_frame_result
qrtr_frame(const uint8_t *data, uint32_t length, struct qrtr_header *header, uint32_t *at)
{
	if (length >= sizeof(*header) && data[0] == 1) {
		memcpy(header, data, sizeof(*header));
		*at = sizeof(*header);
	} else if (length >= 16 && data[0] == 3) {
		/* version 2: version, type, flags, a length of options; the size; then four numbers of 16 bits */
		bzero(header, sizeof(*header));
		header->version = 2;
		header->type = data[1];
		header->confirm = data[2] & 1;
		memcpy(&header->size, data + 4, 4);
		header->from_node = (uint32_t)(data[8] | data[9] << 8);
		header->from_port = (uint32_t)(data[10] | data[11] << 8);
		header->to_node = (uint32_t)(data[12] | data[13] << 8);
		header->to_port = (uint32_t)(data[14] | data[15] << 8);
		header->from_port = header->from_port == 0xfffe ? QRTR_CONTROL : header->from_port;
		header->to_port = header->to_port == 0xfffe ? QRTR_CONTROL : header->to_port;
		*at = 16u + data[3];
	} else {
		return QRTR_FRAME_NO_DATAGRAM;
	}
	/* (inside what arrived without adding the two: a size near 2^32 would make the sum small) */
	if (header->size == 0 || *at > length || header->size > length - *at) {
		return QRTR_FRAME_SIZE_WRONG;
	}
	return QRTR_FRAME_OK;
}

#endif /* HAKIOS_QRTR_FRAME_H */
