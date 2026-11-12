/*
 * The command database: the list the boot loader leaves in memory of what RPMh can switch -- supply
 * rails, clocks, bus clock managers -- each with its name, its address for RPMh and some data of
 * its own (a rail's levels, a bus clock manager's unit). Every driver that asks RPMh for something
 * looks its resource up here by name; this is that lookup once.
 *
 * Where the database is comes from the SoC file's rpmh { } (hakios_board()->rpmh.cmd_db and
 * cmd_db_size). The driver maps it -- it is memory, not registers: mapped so that reads of any size
 * and alignment are allowed -- and hands the mapping and its size to the functions here, which
 * never read outside it whatever the database says of itself.
 *
 * Layout: Linux drivers/soc/qcom/cmd-db.c.
 */
#ifndef HAKIOS_CMD_DB_H
#define HAKIOS_CMD_DB_H

#include <libkern/libkern.h>
#include <mach/mach_types.h>

struct cmd_db_slave {                           /* the resources of one kind */
	uint16_t id;                            /* the kind; 0: unused */
	uint16_t header_offset;                 /* where its entries are, from `data' */
	uint16_t data_offset;                   /* where its entries' own data is, from `data' */
	uint16_t count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];                             /* the name, filled up with zeros: not ended by one when it has eight letters */
	uint32_t priority[2];
	uint32_t address;                       /* what RPMh is told */
	uint16_t length, offset;                /* its own data, from the slave's data_offset */
};

#define CMD_DB_SLAVES           8
#define CMD_DB_MOST_ENTRIES     512             /* more than any database has: one that says more is not believed */
#define CMD_DB_ANY              0
#define CMD_DB_ARC              3               /* kinds (a slave's id): rails asked for by level */
#define CMD_DB_VRM              4               /* regulators asked for by voltage */
#define CMD_DB_BCM              5               /* bus clock managers */

/* Is that a command database at all? */
static inline bool
hakios_cmd_db_valid(const struct cmd_db_header *db)
{
	return db != NULL && db->magic[0] == 0xdb && db->magic[1] == 0x30 && db->magic[2] == 0x03 && db->magic[3] == 0x0c;
}

/*
 * Entry `index' of slave `slave', or NULL: past the slave's last entry, a slave that is not used, or
 * one whose entries would lie outside the `size' bytes that are mapped.
 */
static inline const struct cmd_db_entry *
hakios_cmd_db_entry(const struct cmd_db_header *db, uint32_t size, unsigned slave, unsigned index)
{
	const struct cmd_db_slave *s;

	if (!hakios_cmd_db_valid(db) || slave >= CMD_DB_SLAVES || size < sizeof(*db)) {
		return NULL;
	}
	s = &db->slave[slave];
	if (s->id == 0 || s->count > CMD_DB_MOST_ENTRIES || index >= s->count ||
	    (uint64_t)s->header_offset + (uint64_t)s->count * sizeof(struct cmd_db_entry) > size - sizeof(*db)) {
		return NULL;
	}
	return (const struct cmd_db_entry *)(db->data + s->header_offset) + index;
}

/* An entry's own data and how long it is; NULL and 0 if there is none or it would lie outside the mapping. */
static inline const uint8_t *
hakios_cmd_db_data(const struct cmd_db_header *db, uint32_t size, unsigned slave, const struct cmd_db_entry *entry,
    unsigned *length)
{
	uint64_t at = (uint64_t)db->slave[slave].data_offset + entry->offset;

	*length = 0;
	if (entry->length == 0 || at + entry->length > size - sizeof(*db)) {
		return NULL;
	}
	*length = entry->length;
	return db->data + at;
}

/*
 * The resource of that name among those of `kind' (CMD_DB_ANY: of any kind), or NULL. `slave', if
 * not NULL, is told which slave it is in, for hakios_cmd_db_data().
 */
static inline const struct cmd_db_entry *
hakios_cmd_db_find(const struct cmd_db_header *db, uint32_t size, const char *name, unsigned kind, unsigned *slave)
{
	char id[8] = { 0 };

	if (!hakios_cmd_db_valid(db)) {
		return NULL;
	}
	/* (the database holds a name of eight letters with no zero after it) */
	for (unsigned i = 0; i < sizeof(id) && name[i] != 0; i++) {
		id[i] = name[i];
	}
	for (unsigned s = 0; s < CMD_DB_SLAVES; s++) {
		const struct cmd_db_entry *entry;

		if (kind != CMD_DB_ANY && db->slave[s].id != kind) {
			continue;
		}
		for (unsigned i = 0; (entry = hakios_cmd_db_entry(db, size, s, i)) != NULL; i++) {
			if (memcmp(entry->id, id, sizeof(id)) == 0) {
				if (slave != NULL) {
					*slave = s;
				}
				return entry;
			}
		}
	}
	return NULL;
}

/* What RPMh is told for the resource of that name; 0: the database has none. */
static inline uint32_t
hakios_cmd_db_address(const struct cmd_db_header *db, uint32_t size, const char *name)
{
	const struct cmd_db_entry *entry = hakios_cmd_db_find(db, size, name, CMD_DB_ANY, NULL);

	return entry != NULL ? entry->address : 0;
}

/*
 * A supply rail that is asked for by level: what RPMh is told for it (0: the database has no such
 * resource), and the levels it knows, in the order RPMh numbers them -- a request names a level by its
 * position in this list. At most `max' of them go into `levels'; `count' is how many did. Either
 * may be NULL for who wants the address alone.
 */
static inline uint32_t
hakios_cmd_db_levels(const struct cmd_db_header *db, uint32_t size, const char *name, uint16_t *levels, unsigned max,
    unsigned *count)
{
	unsigned slave = 0, length = 0;
	const struct cmd_db_entry *entry = hakios_cmd_db_find(db, size, name, CMD_DB_ANY, &slave);
	const uint8_t *data;

	if (count != NULL) {
		*count = 0;
	}
	if (entry == NULL) {
		return 0;
	}
	data = hakios_cmd_db_data(db, size, slave, entry, &length);
	for (unsigned l = 0; data != NULL && levels != NULL && count != NULL && l < length / 2u && l < max; l++) {
		levels[l] = (uint16_t)(data[2 * l] | data[2 * l + 1] << 8);
		if (l > 0 && levels[l] == 0) {
			break;                  /* (the list is filled up with zeros; the first level may be zero itself) */
		}
		*count = l + 1;
	}
	return entry->address;
}

#endif /* HAKIOS_CMD_DB_H */
