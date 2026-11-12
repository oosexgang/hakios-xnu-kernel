/*
 * A processor's firmware as Qualcomm splits it (Linux: drivers/soc/qcom/mdt_loader.c): a 32-bit ELF whose
 * header, table of segments and block of hashes and certificates are the file NAME.mdt, and whose loadable
 * segments are a file each, NAME.bNN for segment NN. The checks that a header is one, that a segment lies
 * in the memory reserved for the processor, and that the header file is nothing but those three parts:
 * once, for the drivers that hand an image to TrustZone (the audio DSP's, the Wi-Fi DSP's). The files come
 * from a RAM disk: nothing here trusts a count, an address or a size in them.
 */
#ifndef HAKIOS_MDT_H
#define HAKIOS_MDT_H

#include <libkern/libkern.h>
#include <mach/mach_types.h>

struct elf_segment {
	uint32_t type, offset, vaddr, paddr, file_size, memory_size, flags, align;
};
#define MDT_ELF_HEADER          52                              /* the ELF header; the table of segments follows it */
#define MDT_SEGMENT             32                              /* one entry of the table */
#define MDT_LOAD                1                               /* a segment's type: one that is loaded */
#define MDT_HASH(flags)         (((flags) >> 24 & 7) == 2)      /* the segment that is the block of hashes */

/*
 * How many segments the header file describes, when it is one: a 32-bit ELF, at least three segments (the
 * header, the hashes, something to load) and at most `most', its table inside the file, the file no larger
 * than `largest' (what the driver hands TrustZone in one piece). 0 when it is not.
 */
static inline uint32_t
mdt_segment_count(const uint8_t *header, uint64_t size, uint32_t most, uint64_t largest)
{
	uint32_t count;

	if (header == NULL || size < MDT_ELF_HEADER + 3 * MDT_SEGMENT || size > largest ||
	    memcmp(header, "\177ELF\1", 5) != 0) {
		return 0;
	}
	count = header[44] | header[45] << 8;
	if (count < 3 || count > most || MDT_ELF_HEADER + (uint64_t)MDT_SEGMENT * count > size) {
		return 0;
	}
	return count;
}

/* Segment i of the table (i below the count above). */
static inline void
mdt_segment(const uint8_t *header, uint32_t i, struct elf_segment *segment)
{
	memcpy(segment, header + MDT_ELF_HEADER + MDT_SEGMENT * i, sizeof(*segment));
}

/* Is this a segment that is loaded (and not the block of hashes)? */
static inline bool
mdt_loaded(const struct elf_segment *segment)
{
	return segment->type == MDT_LOAD && !MDT_HASH(segment->flags) && segment->memory_size != 0;
}

/* Does a loaded segment lie wholly in the memory [base, base + size), with no more in its file than in memory? */
static inline bool
mdt_within(const struct elf_segment *segment, uint64_t base, uint64_t size)
{
	return segment->paddr >= base && segment->file_size <= segment->memory_size &&
	       (uint64_t)segment->paddr - base <= size && segment->memory_size <= size - ((uint64_t)segment->paddr - base);
}

/* Is the header file the ELF header, the table and the block of hashes, and nothing else? */
static inline bool
mdt_header_whole(uint32_t count, uint32_t hashes, uint64_t size)
{
	return MDT_ELF_HEADER + (uint64_t)MDT_SEGMENT * count + hashes == size;
}

#endif /* HAKIOS_MDT_H */
