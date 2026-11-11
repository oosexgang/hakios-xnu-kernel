/*
 * A store of a few fixed-length files of an APFS volume, overwritten where they lie.
 *
 * See apfs_store.c for the shape of it and for what it is instead of.
 */

#ifndef _MISCFS_APFS_APFS_STORE_H_
#define _MISCFS_APFS_APFS_STORE_H_

#include "apfs_write.h"

/* The volume this will touch and no other: its name, and the names of the slots in its root. */
#define APFS_STORE_VOLNAME  "HakiosStore"
#define APFS_STORE_SLOTS    2
#define APFS_STORE_SLOT0    "slot0"
#define APFS_STORE_SLOT1    "slot1"

/* The first block of a slot is its header alone; the payload begins after it. */
#define APFS_STORE_PAYLOAD  4096
#define APFS_STORE_MAGIC    "RDWSTOR"
#define APFS_STORE_VERSION  1
#define APFS_STORE_HEAD_LEN 64

/*
 * What a slot says about itself.  On disk this is APFS_STORE_HEAD_LEN bytes at the front of the
 * slot's first block, little-endian, the rest of the block zero; in memory it is this.
 */
struct apfs_store_head {
	uint32_t version;
	uint32_t slot;
	uint64_t generation;            /* the newest good slot is the one with the highest */
	uint64_t bytes;                 /* the payload, as the writer handed it over */
	uint64_t seconds;               /* when it was written, by the writer's clock */
	uint64_t note;                  /* the writer's own number, kept and handed back */
	uint64_t sum;                   /* of the payload */
};

/* The store, as apfs_store_open() finds it.  The caller keeps this while it uses the store. */
struct apfs_store {
	struct apfs_vol *vol;
	uint64_t         slot_id[APFS_STORE_SLOTS];      /* the inodes of the slot files */
	uint64_t         slot_bytes;                     /* the length of a slot, from its own inode */
	uint64_t         payload_max;                    /* slot_bytes - APFS_STORE_PAYLOAD */
};

/* A save in progress: begun, written to, then committed. */
struct apfs_store_write {
	struct apfs_store *st;
	int                slot;
	uint64_t           generation;                   /* one more than the newest good one */
	uint64_t           at;                           /* payload bytes put so far */
	uint64_t           sum;
	uint64_t           blocks;                       /* blocks written, for the report */
	bool               open;
};

/*
 * Find the store on a volume: its name must be APFS_STORE_VOLNAME, and its root must hold the
 * slots, each a plain file of the same length and long enough for a header.  ENOENT with the
 * reason in *why; *why is NULL when all is well.  This says nothing about whether the store can
 * be written -- see apfs_store_writable().
 */
int apfs_store_open(struct apfs_vol *vol, struct apfs_store *st, const char **why);

/* Would a save go through on this volume at all?  0, or EROFS with the reason in *why. */
int apfs_store_writable(struct apfs_store *st, const char **why);

/*
 * The newest slot whose header and payload both check out, or -1 with *head left alone.  `scratch`
 * is where the payload is read in pieces to sum it; a bigger one costs fewer reads and nothing
 * else.  This is the whole of "finding the newest good one", and it needs no transaction: a slot
 * says what generation it is, and whether it is torn.
 */
int apfs_store_newest(struct apfs_store *st, void *scratch, size_t scratch_len,
    struct apfs_store_head *head);

/* The header of one slot as it reads now; ENOENT when that slot holds nothing good. */
int apfs_store_head(struct apfs_store *st, int slot, struct apfs_store_head *head);

/* Payload bytes of one slot, which the caller has already satisfied itself about. */
int apfs_store_read(struct apfs_store *st, int slot, uint64_t off, void *buf, size_t len);

/*
 * Begin a save.  It goes into the slot that does not hold the newest good header, and the first
 * thing written is that slot's header block zeroed -- so from here until the commit the slot
 * holds nothing good, and the other slot is what a reader will take.
 */
int apfs_store_begin(struct apfs_store *st, void *scratch, size_t scratch_len,
    struct apfs_store_write *w);

/* More payload, in order from the front.  ENOSPC when it would not fit the slot. */
int apfs_store_put(struct apfs_store_write *w, const void *data, size_t len);

/*
 * Finish it: the header block, written last, which is what makes everything before it good at
 * once.  After this the slot is the newest.
 */
int apfs_store_commit(struct apfs_store_write *w, uint64_t seconds, uint64_t note);

/* A whole payload in one call: begin, put, commit. */
int apfs_store_save(struct apfs_store *st, const void *data, uint64_t len, uint64_t seconds,
    uint64_t note, void *scratch, size_t scratch_len, struct apfs_store_write *w);

#endif /* _MISCFS_APFS_APFS_STORE_H_ */
