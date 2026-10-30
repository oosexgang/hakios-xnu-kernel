/*
 * Writing into APFS: the contents of an existing file, in place.
 *
 * See apfs_write.c for what this does and, more to the point, what it does not.
 */

#ifndef _MISCFS_APFS_APFS_WRITE_H_
#define _MISCFS_APFS_APFS_WRITE_H_

#include "apfs_lib.h"

/* What a write did, or why it was refused.  Filled in whether it went well or not. */
struct apfs_write_report {
	const char *why;                /* NULL when the write went through */
	uint64_t    first_block;        /* the first block written, 0 if none was */
	uint64_t    blocks;             /* blocks written */
	uint64_t    bytes;              /* bytes of the file the caller asked to replace */
	uint64_t    partial;            /* blocks read, patched and written back whole */
	uint64_t    calls;              /* calls to dev.write */
};

/*
 * May this volume be written at all?  Answered from the volume alone, before any file is
 * named: 0, or EROFS with the reason in *why.  apfs_write_in_place() asks this itself; it
 * is here so that a caller can say at mount time whether writing is possible.
 */
int apfs_write_volume_ok(struct apfs_vol *vol, const char **why);

/*
 * Replace `len` bytes at offset `off` of the file `obj_id` with `buf`, leaving its length,
 * its extents and every other structure of the container exactly as they were.  Refuses
 * (EROFS, with the reason in rep->why) anything it cannot do that way -- see apfs_write.c.
 *
 * The blocks go out in ascending order of offset and each is read back and compared before
 * the next, so a caller that puts its own checksum in the last bytes of what it writes can
 * tell afterwards whether a write that was cut short got there.
 */
int apfs_write_in_place(struct apfs_vol *vol, uint64_t obj_id, uint64_t off,
    const void *buf, size_t len, struct apfs_write_report *rep);

#endif /* _MISCFS_APFS_APFS_WRITE_H_ */
