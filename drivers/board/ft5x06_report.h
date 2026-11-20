/*
 * What a FocalTech controller of the FT5x06 register family says in its working mode, apart from the bus it says it
 * on: which registers hold a report, and a report made into fingers. For board/ft5x06.c and the host's tests.
 *
 * The layout, as remembered from the family's data sheets and application notes; none of it has met a chip here. How
 * sure each line is, is said with it:
 *
 *   0x00        the mode: bits 6:4 are 0 while the chip works, 4 in its factory test mode             (sure)
 *   0x02        how many fingers this report has, in the low four bits                               (sure)
 *   0x03 ...    six bytes a finger, one slot after another:                                          (sure)
 *                 [0]  bits 7:6 what happened: 0 it landed, 1 it lifted, 2 it stays, 3 nothing
 *                      bits 3:0 x, bits 11:8
 *                 [1]  x, bits 7:0
 *                 [2]  bits 7:4 which finger (its number, kept while it stays), bits 3:0 y, bits 11:8
 *                 [3]  y, bits 7:0
 *                 [4]  its weight, [5] its area: not used here
 *   a slot nobody is in reads 0xff: a finger's number of 15                                          (fairly sure)
 *   a finger that lifted is said once with "lifted", in a slot the count of 0x02 no longer counts    (NOT SURE: so a
 *               finger is also lifted here when a report simply does not have it any more, and when 0x02 says none)
 *   the upper four bits of 0x02 are not the count's                                                  (NOT SURE what
 *               they are; they are not looked at)
 *   0xa3, 0x9f  the chip's identity, its high and its low byte (0x9f: the parts from the FT5x46 on)  (fairly sure)
 *   0xa6        its firmware's version                                                               (sure)
 *
 * Coordinates are twelve bits, in the panel's own points: a board says the largest (touch-i2c { } coordinates).
 */
#ifndef HAKIOS_FT5X06_REPORT_H
#define HAKIOS_FT5X06_REPORT_H

#include <stdbool.h>
#include <stdint.h>

#define FT5X06_CONTACTS         10      /* the most a chip of the family reports (an FT5x46); a board says its chip's */
#define FT5X06_REG_MODE         0x00
#define FT5X06_MODE_MASK        0x70    /* 0: working */
#define FT5X06_REG_STATUS       0x02
#define FT5X06_SLOT_BYTES       6       /* a finger's, from 0x03 on */
#define FT5X06_REG_ID_LOW       0x9f
#define FT5X06_REG_ID_HIGH      0xa3
#define FT5X06_REG_FIRMWARE     0xa6
#define FT5X06_COORDINATE_MAX   0xfff   /* a report's coordinates are 12 bits */

#define FT5X06_EVENT_LANDED     0
#define FT5X06_EVENT_LIFTED     1
#define FT5X06_EVENT_STAYS      2
#define FT5X06_EVENT_NONE       3

struct ft5x06_frame {
	uint16_t active;                        /* a bit for each finger that is down, by its number */
	uint16_t x[FT5X06_CONTACTS], y[FT5X06_CONTACTS];
};

/*
 * How many slots are worth reading after a status byte: the fingers it counts, and one for each finger that was down
 * before -- a finger that lifted may still have a slot, after the counted ones or among them (this file's head). None
 * for a status no chip of `contacts' fingers says. Never more than the chip has: what follows its last slot is other
 * registers.
 */
static inline unsigned
ft5x06_slots(uint8_t status, unsigned down_before, unsigned contacts)
{
	unsigned count = status & 0x0f;

	if (count > contacts) {
		return 0;
	}
	return count + down_before < contacts ? count + down_before : contacts;
}

/*
 * A report -- the status byte and `slots' slots read after it -- into the fingers that are down. Decoded whole before
 * anything is taken from it: false, and `frame' as it was, for a report that cannot be one -- more fingers than the
 * chip has (a bus that reads all ones says 15), a finger said twice, a position off the panel whose largest
 * coordinates are max_x and max_y.
 */
static inline bool
ft5x06_decode(uint8_t status, const uint8_t *points, unsigned slots, unsigned contacts, uint32_t max_x, uint32_t max_y,
    struct ft5x06_frame *frame)
{
	struct ft5x06_frame result = { 0 };
	unsigned count = status & 0x0f;
	uint16_t said = 0;

	if (contacts > FT5X06_CONTACTS || count > contacts) {
		return false;
	}
	if (count == 0) {
		*frame = result;                /* no finger, whatever the slots still hold */
		return true;
	}
	for (unsigned i = 0; i < slots && i < contacts; i++) {
		const uint8_t *slot = points + FT5X06_SLOT_BYTES * i;
		unsigned id = slot[2] >> 4, event = slot[0] >> 6;

		if (id >= contacts || event == FT5X06_EVENT_NONE) {
			continue;               /* nobody's slot */
		}
		if (said & (1u << id)) {
			return false;
		}
		said |= 1u << id;
		if (event == FT5X06_EVENT_LIFTED) {
			continue;               /* (it need not carry a position: where it was last is what is posted) */
		}
		result.x[id] = (uint16_t)((slot[0] & 0x0f) << 8 | slot[1]);
		result.y[id] = (uint16_t)((slot[2] & 0x0f) << 8 | slot[3]);
		if (result.x[id] > max_x || result.y[id] > max_y) {
			return false;
		}
		result.active |= (uint16_t)(1u << id);
	}
	*frame = result;
	return true;
}

#endif /* HAKIOS_FT5X06_REPORT_H */
