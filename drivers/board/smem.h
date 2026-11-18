/*
 * The memory the SoC's processors share ("SMEM"), as the boot loader laid it out: partitions, one
 * for each pair of processors that share items privately and a global one for the rest, and in
 * each the items, known by number. A driver that talks to another processor (the audio DSP, the
 * Wi-Fi's) finds the other's items here; this is that reading once.
 *
 * Where the memory is comes from the SoC file's ipc { } (hakios_board()->ipc.smem and smem_size).
 * The driver maps it -- memory, not registers -- and hands the mapping and its size to the
 * functions here. Other processors write there while we look: everything is copied out before it is
 * believed, and nothing is read outside the mapping whatever a header says of its own sizes.
 *
 * Layout: Linux drivers/soc/qcom/smem.c. In the last 4K the table of partitions ("$TOC"); a partition
 * begins with a header ("$PRT"); items are allocated from both ends of it: from the bottom an entry
 * and then its item, from the top an entry with its item in front of it.
 */
#ifndef HAKIOS_SMEM_H
#define HAKIOS_SMEM_H

#include <libkern/libkern.h>
#include <mach/mach_types.h>

#define SMEM_GLOBAL             0xfffe          /* both "hosts" of the global partition */
#define SMEM_CANARY             0xa5a5
#define SMEM_TABLE_SIZE         0x1000u         /* the table of partitions: the area's last 4K */
#define SMEM_MOST_PARTITIONS    64

struct smem_table_entry {
	uint32_t offset, size, flags;
	uint16_t host0, host1;
	uint32_t cacheline, reserved[7];
};
struct smem_table {
	uint8_t magic[4];                       /* "$TOC" */
	uint32_t version, count, reserved[5];
};
struct smem_partition {
	uint8_t magic[4];                       /* "$PRT" */
	uint16_t host0, host1;
	uint32_t size, free_uncached, free_cached, reserved[3];
};
struct smem_entry {
	uint16_t canary, item;
	uint32_t size;                          /* with the padding */
	uint16_t padding_data, padding_header;
	uint32_t reserved;
};
struct smem_item {
	uint32_t item, offset, size;            /* its number; where it is, from the area's start; how long */
};

struct hakios_smem {
	const volatile uint8_t *at;             /* the area, mapped */
	uint32_t size;
};

/* A piece of the area, copied. False, and nothing copied, if it is not all inside. */
static inline bool
hakios_smem_peek(const struct hakios_smem *area, uint64_t offset, void *to, uint32_t size)
{
	/* (not "offset + size > area->size": an offset near the end of all numbers makes that sum small) */
	if (area->at == NULL || offset > area->size || size > area->size - offset) {
		return false;
	}
	for (uint32_t i = 0; i < size; i++) {
		((uint8_t *)to)[i] = area->at[offset + i];
	}
	return true;
}

/*
 * The partition two processors share (in either order; SMEM_GLOBAL twice for the global one): where
 * it begins, from the area's start, or 0. `cacheline' is what its items from the top are aligned to.
 */
static inline uint32_t
hakios_smem_partition(const struct hakios_smem *area, uint16_t one, uint16_t other, uint32_t *cacheline, uint32_t *size)
{
	struct smem_table table;
	struct smem_table_entry entry;
	struct smem_partition header;
	uint32_t table_at;

	if (area->size < SMEM_TABLE_SIZE) {
		return 0;
	}
	table_at = area->size - SMEM_TABLE_SIZE;
	if (!hakios_smem_peek(area, table_at, &table, sizeof(table)) || memcmp(table.magic, "$TOC", 4) != 0 || table.version != 1 ||
	    table.count > SMEM_MOST_PARTITIONS) {
		return 0;
	}
	for (uint32_t i = 0; i < table.count; i++) {
		if (!hakios_smem_peek(area, table_at + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry)) || entry.offset == 0 ||
		    entry.size == 0) {
			continue;
		}
		if (!((entry.host0 == one && entry.host1 == other) || (entry.host0 == other && entry.host1 == one))) {
			continue;
		}
		if ((uint64_t)entry.offset + entry.size > table_at || !hakios_smem_peek(area, entry.offset, &header, sizeof(header)) ||
		    memcmp(header.magic, "$PRT", 4) != 0 || header.size != entry.size) {
			return 0;
		}
		*cacheline = entry.cacheline;
		*size = entry.size;
		return entry.offset;
	}
	return 0;
}

#define SMEM_ANY_ITEM           0xffffffffu     /* hakios_smem_items_of(): every item, not one */

/*
 * The items of the partition at `partition': at most `max' go into `found'; returns how many
 * there are (which can be more). An item that is given out lies inside the partition's part for items
 * of its kind, and so inside the area: a driver may read all of it. An entry that says otherwise of
 * itself ends the walk it is in.
 */
static inline unsigned
hakios_smem_items_of(const struct hakios_smem *area, uint32_t partition, uint32_t cacheline, struct smem_item *found,
    unsigned max, uint32_t want)
{
	struct smem_partition header;
	struct smem_entry entry;
	uint32_t at, step_back;
	unsigned count = 0;

	if (partition == 0 || !hakios_smem_peek(area, partition, &header, sizeof(header)) || header.free_uncached > header.size ||
	    header.free_cached > header.size || (uint64_t)partition + header.size > area->size) {
		return 0;
	}
	/* from the bottom: an entry, then its item */
	for (at = sizeof(header); at + sizeof(entry) <= header.free_uncached;) {
		if (!hakios_smem_peek(area, partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY ||
		    entry.size > header.size || entry.padding_data > entry.size ||
		    (uint64_t)at + sizeof(entry) + entry.padding_header + entry.size > header.free_uncached) {
			break;
		}
		if (want != SMEM_ANY_ITEM && entry.item != want) {
			at += sizeof(entry) + entry.padding_header + entry.size;
			continue;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at + sizeof(entry) + entry.padding_header;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	/* from the top: an entry, with its item in front of it; entries are aligned as the table says */
	if (cacheline == 0 || (cacheline & (cacheline - 1))) {
		return count;
	}
	step_back = ((uint32_t)sizeof(entry) + cacheline - 1) & ~(cacheline - 1);
	for (at = header.size - step_back; at > header.free_cached && at < header.size;) {
		if (!hakios_smem_peek(area, partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > at ||
		    entry.padding_data > entry.size || at - entry.size < header.free_cached) {
			break;
		}
		if (count < max && (want == SMEM_ANY_ITEM || entry.item == want)) {
			found[count].item = entry.item;
			found[count].offset = partition + at - entry.size;
			found[count].size = entry.size - entry.padding_data;
		}
		count += want == SMEM_ANY_ITEM || entry.item == want;
		if (entry.size + step_back > at) {
			break;
		}
		at -= entry.size + step_back;
	}
	return count;
}

static inline unsigned
hakios_smem_items(const struct hakios_smem *area, uint32_t partition, uint32_t cacheline, struct smem_item *found, unsigned max)
{
	return hakios_smem_items_of(area, partition, cacheline, found, max, SMEM_ANY_ITEM);
}

/*
 * An item two processors share: in the partition of the two, or else in the global one. Where it
 * is, from the area's start, and how long; 0: nowhere.
 */
static inline uint32_t
hakios_smem_item(const struct hakios_smem *area, uint16_t us, uint16_t them, uint32_t item, uint32_t *size)
{
	const uint16_t hosts[2][2] = { { us, them }, { SMEM_GLOBAL, SMEM_GLOBAL } };

	/* (the walk keeps only that item: one past the 256th of a partition is found as well as the first) */
	for (unsigned h = 0; h < 2; h++) {
		struct smem_item found;
		uint32_t cacheline = 0, partition_size = 0;
		uint32_t partition = hakios_smem_partition(area, hosts[h][0], hosts[h][1], &cacheline, &partition_size);

		if (hakios_smem_items_of(area, partition, cacheline, &found, 1, item) != 0) {
			*size = found.size;
			return found.offset;
		}
	}
	return 0;
}

#endif /* HAKIOS_SMEM_H */
