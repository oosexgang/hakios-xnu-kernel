#include <assert.h>
#include <stdio.h>

#include "board/synaptics_report.h"

int
main(void)
{
	uint8_t report[SYNAPTICS_CONTACTS * SYNAPTICS_CONTACT_BYTES] = { 0 };
	struct synaptics_contact contacts[SYNAPTICS_CONTACTS];

	for (unsigned i = 0; i < SYNAPTICS_CONTACTS; i++) {
		report[i * SYNAPTICS_CONTACT_BYTES] = (uint8_t)(SYNAPTICS_CONTACTS - 1 - i);
	}
	report[0] |= 0x10;
	report[1] = 0xe8;
	report[2] = 0x03;
	report[3] = 0xf4;
	report[4] = 0x01;
	assert(synaptics_parse_contacts(report, sizeof(report), 1000, 1000, contacts));
	assert(contacts[9].active && contacts[9].x == UINT16_MAX && contacts[9].y == 32767);
	assert(!contacts[0].active);

	assert(!synaptics_parse_contacts(report, sizeof(report) - 1, 1000, 1000, contacts));
	assert(!synaptics_parse_contacts(report, sizeof(report), 999, 1000, contacts));
	report[sizeof(report) - SYNAPTICS_CONTACT_BYTES] = 9;
	assert(!synaptics_parse_contacts(report, sizeof(report), 1000, 1000, contacts));
	assert(contacts[9].active && contacts[9].x == UINT16_MAX);

	puts("Synaptics report: valid, truncated, out-of-range and duplicate contacts checked");
	return 0;
}
