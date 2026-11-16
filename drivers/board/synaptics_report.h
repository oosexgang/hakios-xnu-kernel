/* Synaptics TCM custom report configured by the Ace 6T reference driver. */
#ifndef _HAKIOS_SYNAPTICS_REPORT_H
#define _HAKIOS_SYNAPTICS_REPORT_H

#include <stdbool.h>
#include <stdint.h>

enum { SYNAPTICS_CONTACTS = 10, SYNAPTICS_CONTACT_BYTES = 5 };

struct synaptics_contact {
	uint16_t x, y;
	bool active;
};

static inline unsigned
synaptics_le16(const uint8_t *bytes)
{
	return bytes[0] | ((unsigned)bytes[1] << 8);
}

/* Commit the decoded frame only when every record is valid. */
static inline bool
synaptics_parse_contacts(const uint8_t *report, unsigned length, unsigned max_x, unsigned max_y,
    struct synaptics_contact out[SYNAPTICS_CONTACTS])
{
	if (length != SYNAPTICS_CONTACTS * SYNAPTICS_CONTACT_BYTES || !max_x || !max_y) {
		return false;
	}

	struct synaptics_contact contacts[SYNAPTICS_CONTACTS] = { 0 };
	unsigned seen = 0;
	for (unsigned i = 0; i < SYNAPTICS_CONTACTS; i++, report += SYNAPTICS_CONTACT_BYTES) {
		unsigned id = report[0] & 15;
		unsigned classification = report[0] >> 4;
		if (id >= SYNAPTICS_CONTACTS || (seen & (1u << id)) || classification > 3) {
			return false;
		}
		seen |= 1u << id;
		if (classification == 1 || classification == 2) {
			unsigned x = synaptics_le16(report + 1);
			unsigned y = synaptics_le16(report + 3);
			if (x > max_x || y > max_y) {
				return false;
			}
			contacts[id].active = true;
			contacts[id].x = (uint16_t)(x * UINT32_C(65535) / max_x);
			contacts[id].y = (uint16_t)(y * UINT32_C(65535) / max_y);
		}
	}

	for (unsigned i = 0; i < SYNAPTICS_CONTACTS; i++) {
		out[i] = contacts[i];
	}
	return true;
}

#endif /* _HAKIOS_SYNAPTICS_REPORT_H */
