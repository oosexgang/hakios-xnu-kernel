/*
 * A command database built in a test's own memory, the way Linux's cmd-db.c reads the one a boot loader
 * leaves: for the tests of the lookup (cmd_db_test.c) and of the drivers that use it. After the
 * SIZE bytes a driver may map come 64 that nothing may touch.
 */
#ifndef CMD_DB_IMAGE_H
#define CMD_DB_IMAGE_H

#include "board/cmd_db.h"

#define SIZE            0x2000u
static union {
	struct cmd_db_header header;
	uint8_t bytes[SIZE + 64];               /* (what lies after the mapping is the canary's) */
} image;
#define DB              (&image.header)
static unsigned entries_at, data_at;            /* from `data': where the next slave's entries and data go */

static inline void
empty(void)
{
	memset(&image, 0, sizeof(image));
	memset(image.bytes + SIZE, 0xee, 64);
	DB->magic[0] = 0xdb;
	DB->magic[1] = 0x30;
	DB->magic[2] = 0x03;
	DB->magic[3] = 0x0c;
	entries_at = 0;
	data_at = 0x800;
}

/* a slave of that kind with these resources: name, address, and the bytes of its own data */
struct resource {
	const char *name;
	uint32_t address;
	const void *data;
	uint16_t length;
};

static inline void
slave(unsigned index, unsigned kind, const struct resource *list, unsigned count)
{
	struct cmd_db_slave *s = &DB->slave[index];
	struct cmd_db_entry *entry = (struct cmd_db_entry *)(DB->data + entries_at);
	unsigned offset = 0;

	s->id = (uint16_t)kind;
	s->header_offset = (uint16_t)entries_at;
	s->data_offset = (uint16_t)data_at;
	s->count = (uint16_t)count;
	for (unsigned i = 0; i < count; i++) {
		memset(&entry[i], 0, sizeof(entry[i]));
		memcpy(entry[i].id, list[i].name, strlen(list[i].name) < 8 ? strlen(list[i].name) : 8);
		entry[i].address = list[i].address;
		entry[i].length = list[i].length;
		entry[i].offset = (uint16_t)offset;
		if (list[i].length) {
			memcpy(DB->data + data_at + offset, list[i].data, list[i].length);
		}
		offset += list[i].length;
	}
	entries_at += count * sizeof(struct cmd_db_entry);
	data_at += offset;
}

static inline bool
canary_whole(void)
{
	for (unsigned i = 0; i < 64; i++) {
		if (image.bytes[SIZE + i] != 0xee) {
			return false;
		}
	}
	return true;
}

#endif /* CMD_DB_IMAGE_H */
