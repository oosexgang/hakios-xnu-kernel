/*
 * A store of a few fixed-length files of an APFS volume, overwritten where they lie.
 *
 * What this is instead of
 * -----------------------
 * The data of iOS survives a boot today by being packed into an archive and written into the
 * phone's flash at a number in a comment: two slots of two gigabytes each at byte 8589934592 of
 * the partition darwin, straight onto a character device (userland/src/var_store.c,
 * bsd/dev/redwood_ufs.c).  It works, and its weakness is not the writing but the addressing --
 * nothing on the medium says those bytes are a store, how long a slot is, or that something else
 * may not have them.  Only the constant does, in two places that have to agree.
 *
 * Here the same bytes live in named files of an APFS volume, and where they are comes from the
 * volume's own records: the slots are two files of the volume's root, a slot's length is its
 * inode's length, and every write goes through apfs_write_in_place(), which will not touch a
 * block unless the file's extents and the extent-reference tree both say it is that file's.
 *
 * Nothing here needs the transaction machinery.  A slot file is made once, of a fixed length,
 * and from then on only the bytes inside it ever change: no record of the volume is ever
 * rewritten, so there is nothing to order and no window in which the volume is inconsistent.
 *
 * Who makes the volume
 * --------------------
 * Not this code, and not the kernel: a writable APFS would be needed for that and it is a piece
 * of work of its own (tools/apfs/txn-probe.py measures why).  The volume is built on the build
 * machine by tools/apfs/mkstore.py -- which is tools/apfs/mkapfs.py, the same program that makes
 * every other image this port uses -- and put on the flash once, in the free space of darwin past
 * the blocks the loader and the kernel refuse to write before.  After that its structure never
 * changes again.  That is the whole escape from needing a writable APFS, and it costs one
 * preparation step on a computer.
 *
 * Finding the newest good slot, without a transaction
 * ---------------------------------------------------
 * Unchanged from var_store.c, because it never depended on the medium:
 *
 *   - a save goes into the slot that does not hold the newest good header;
 *   - that slot's header block is zeroed first, so from then until the commit the slot holds
 *     nothing good and a reader takes the other one;
 *   - the payload next, from the front;
 *   - the header last, in one block of its own, carrying the generation, the payload's length
 *     and a sum of it, and a sum of itself.
 *
 * So the worst a save that does not finish can do is lose itself.  A reader takes the slot with
 * the highest generation whose header sums right and whose payload sums to what the header says;
 * a torn one fails one of those and the slot before it, which a save never touches, is still
 * whole.  The header is one block and the payload starts at the next, so no write ever touches
 * both.
 *
 * Sums are FNV-1a over 64 bits, which is what this port uses everywhere else it wants to know
 * whether a lot of bytes are the bytes it meant (tools/apfs/fnvsum.c, the census lines, the
 * walks); it is here to catch a torn or half-written slot, not an adversary.
 */

#include "apfs_store.h"

#define FNV_INIT  0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL

static uint64_t
fnv1a(const void *data, size_t len, uint64_t h)
{
	const uint8_t *p = data;
	size_t i;

	for (i = 0; i < len; i++) {
		h ^= p[i];
		h *= FNV_PRIME;
	}
	return h;
}

/* ---------------------------------------------------------------- the header on disk */

static void
put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void
put_le64(uint8_t *p, uint64_t v)
{
	put_le32(p, (uint32_t)v);
	put_le32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t
get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t
get_le64(const uint8_t *p)
{
	return (uint64_t)get_le32(p) | (uint64_t)get_le32(p + 4) << 32;
}

/* The header block as it goes out: head_sum covers the bytes before it and is written over zero. */
static void
head_pack(uint8_t *blk, const struct apfs_store_head *h)
{
	memset(blk, 0, APFS_STORE_PAYLOAD);
	memcpy(blk, APFS_STORE_MAGIC, 8);
	put_le32(blk + 8, h->version);
	put_le32(blk + 12, h->slot);
	put_le64(blk + 16, h->generation);
	put_le64(blk + 24, h->bytes);
	put_le64(blk + 32, h->seconds);
	put_le64(blk + 40, h->note);
	put_le64(blk + 48, h->sum);
	put_le64(blk + 56, fnv1a(blk, 56, FNV_INIT));
}

static bool
head_unpack(const uint8_t *blk, int slot, uint64_t payload_max, struct apfs_store_head *h)
{
	if (memcmp(blk, APFS_STORE_MAGIC, 8) != 0) {
		return false;
	}
	if (get_le64(blk + 56) != fnv1a(blk, 56, FNV_INIT)) {
		return false;           /* a header half written, or never written at all */
	}
	h->version = get_le32(blk + 8);
	h->slot = get_le32(blk + 12);
	h->generation = get_le64(blk + 16);
	h->bytes = get_le64(blk + 24);
	h->seconds = get_le64(blk + 32);
	h->note = get_le64(blk + 40);
	h->sum = get_le64(blk + 48);
	if (h->version != APFS_STORE_VERSION || h->slot != (uint32_t)slot || h->generation == 0 ||
	    h->bytes > payload_max) {
		return false;
	}
	return true;
}

/* ---------------------------------------------------------------- the slots */

static int
slot_read(struct apfs_store *st, int slot, uint64_t off, void *buf, size_t len)
{
	struct apfs_inode ino;
	struct apfs_cfile cf;
	size_t got = 0;
	int error;

	if (slot < 0 || slot >= APFS_STORE_SLOTS || len == 0) {
		return EINVAL;
	}
	if (off > st->slot_bytes || len > st->slot_bytes - off) {
		return EINVAL;
	}
	error = apfs_inode_get(st->vol, st->slot_id[slot], &ino);
	if (error) {
		return error;
	}
	memset(&cf, 0, sizeof(cf));
	error = apfs_read_file(st->vol, &ino, &cf, off, buf, len, &got);
	apfs_cfile_fini(&cf);
	if (error == 0 && got != len) {
		error = EIO;
	}
	return error;
}

static int
slot_write(struct apfs_store *st, int slot, uint64_t off, const void *buf, size_t len,
    uint64_t *blocks)
{
	struct apfs_write_report rep;
	int error;

	if (slot < 0 || slot >= APFS_STORE_SLOTS) {
		return EINVAL;
	}
	error = apfs_write_in_place(st->vol, st->slot_id[slot], off, buf, len, &rep);
	if (blocks != NULL) {
		*blocks += rep.blocks;
	}
	return error;
}

int
apfs_store_open(struct apfs_vol *vol, struct apfs_store *st, const char **why)
{
	static const char *const names[APFS_STORE_SLOTS] = { APFS_STORE_SLOT0, APFS_STORE_SLOT1 };
	struct apfs_xattr xa;
	const char *reason = NULL;
	uint64_t size = 0;
	int i, error = 0;

	memset(st, 0, sizeof(*st));
	st->vol = vol;
	/*
	 * The name is a gate, not decoration: it is the only thing that says this volume is the
	 * store, and without it a store pointed at the wrong volume would write over whatever
	 * files of it happened to be called slot0 and slot1.
	 */
	if (strncmp(vol->name, APFS_STORE_VOLNAME, sizeof(vol->name)) != 0) {
		reason = "the volume is not named " APFS_STORE_VOLNAME;
		error = ENOENT;
		goto out;
	}
	/*
	 * Whether the store can be written is not asked here.  A store is read at every boot off a
	 * volume mounted read-only -- which is every mount this driver makes -- and only the one
	 * program that saves ever has a device that takes writes.  apfs_write_in_place() asks the
	 * question itself, at the first block, and nothing between here and there can change the
	 * answer; apfs_store_writable() is for a caller that wants to say so up front.
	 */
	for (i = 0; i < APFS_STORE_SLOTS; i++) {
		struct apfs_inode ino;
		uint64_t id = 0;
		uint8_t type = 0;

		error = apfs_lookup(vol, APFS_ROOT_DIR_INO_NUM, names[i], strlen(names[i]), &id,
		    &type);
		if (error != 0 || type != APFS_DT_REG) {
			reason = "the volume's root has no such slot file";
			error = ENOENT;
			goto out;
		}
		error = apfs_inode_get(vol, id, &ino);
		if (error) {
			reason = "a slot file's inode cannot be read";
			goto out;
		}
		if ((ino.mode & APFS_S_IFMT) != APFS_S_IFREG) {
			reason = "a slot is not a plain file";
			error = ENOENT;
			goto out;
		}
		/*
		 * The likeliest way to get a store wrong is to build its volume with
		 * compression on: a compressed file keeps its bytes in an attribute or a
		 * resource fork and has no data stream at all, so it would be refused anyway --
		 * but by a message that does not say what to change (mkstore.py does not pass
		 * -z to mkapfs.py; something else did).
		 */
		if (apfs_xattr_lookup(vol, id, APFS_DECMPFS_XATTR_NAME, &xa) == 0) {
			reason = "a slot was stored compressed: its volume must be built without -z";
			error = ENOENT;
			goto out;
		}
		if (!ino.has_dstream) {
			reason = "a slot has no data stream";
			error = ENOENT;
			goto out;
		}
		if (ino.size <= APFS_STORE_PAYLOAD) {
			reason = "a slot is too short to hold a header and anything else";
			error = ENOENT;
			goto out;
		}
		if (i == 0) {
			size = ino.size;
		} else if (ino.size != size) {
			reason = "the slots are not the same length";
			error = ENOENT;
			goto out;
		}
		st->slot_id[i] = id;
	}
	if (st->slot_id[0] == st->slot_id[1]) {
		reason = "both slots are the same file";
		error = ENOENT;
		goto out;
	}
	st->slot_bytes = size;
	st->payload_max = size - APFS_STORE_PAYLOAD;
out:
	if (why != NULL) {
		*why = reason;
	}
	if (error != 0) {
		apfs_log("store: %s\n", reason != NULL ? reason : "cannot be opened");
	}
	return error;
}

int
apfs_store_writable(struct apfs_store *st, const char **why)
{
	return apfs_write_volume_ok(st->vol, why);
}

int
apfs_store_head(struct apfs_store *st, int slot, struct apfs_store_head *head)
{
	uint8_t *blk = apfs_alloc(APFS_STORE_PAYLOAD);
	int error;

	if (blk == NULL) {
		return ENOMEM;
	}
	error = slot_read(st, slot, 0, blk, APFS_STORE_PAYLOAD);
	if (error == 0 && !head_unpack(blk, slot, st->payload_max, head)) {
		error = ENOENT;
	}
	apfs_free(blk, APFS_STORE_PAYLOAD);
	return error;
}

/* Does the payload still sum to what the header says?  This is the second reading of it. */
static int
slot_payload_ok(struct apfs_store *st, int slot, const struct apfs_store_head *h, void *scratch,
    size_t scratch_len, bool *ok)
{
	uint64_t off = 0, sum = FNV_INIT;

	*ok = false;
	if (scratch == NULL || scratch_len == 0) {
		return EINVAL;
	}
	while (off < h->bytes) {
		size_t want = (size_t)(h->bytes - off);
		int error;

		if (want > scratch_len) {
			want = scratch_len;
		}
		error = slot_read(st, slot, APFS_STORE_PAYLOAD + off, scratch, want);
		if (error) {
			return error;
		}
		sum = fnv1a(scratch, want, sum);
		off += want;
	}
	*ok = (sum == h->sum);
	return 0;
}

int
apfs_store_newest(struct apfs_store *st, void *scratch, size_t scratch_len,
    struct apfs_store_head *head)
{
	int best = -1, slot;

	for (slot = 0; slot < APFS_STORE_SLOTS; slot++) {
		struct apfs_store_head h;
		bool ok = false;

		if (apfs_store_head(st, slot, &h) != 0) {
			continue;
		}
		if (best >= 0 && h.generation <= head->generation) {
			continue;       /* older than one we already have; do not pay to sum it */
		}
		if (slot_payload_ok(st, slot, &h, scratch, scratch_len, &ok) != 0 || !ok) {
			apfs_log("store: slot %d says generation %llu but its payload does not "
			    "sum right: passing over it\n", slot,
			    (unsigned long long)h.generation);
			continue;
		}
		*head = h;
		best = slot;
	}
	return best;
}

int
apfs_store_read(struct apfs_store *st, int slot, uint64_t off, void *buf, size_t len)
{
	if (off > st->payload_max || len > st->payload_max - off) {
		return EINVAL;
	}
	return slot_read(st, slot, APFS_STORE_PAYLOAD + off, buf, len);
}

/* ---------------------------------------------------------------- saving */

int
apfs_store_begin(struct apfs_store *st, void *scratch, size_t scratch_len,
    struct apfs_store_write *w)
{
	struct apfs_store_head newest;
	uint8_t *blk;
	int best, error;

	memset(w, 0, sizeof(*w));
	memset(&newest, 0, sizeof(newest));
	w->st = st;
	w->sum = FNV_INIT;
	best = apfs_store_newest(st, scratch, scratch_len, &newest);
	/* the slot that is not the newest good one; either, when neither is good */
	w->slot = (best == 0) ? 1 : 0;
	w->generation = newest.generation + 1;

	blk = apfs_alloc(APFS_STORE_PAYLOAD);
	if (blk == NULL) {
		return ENOMEM;
	}
	/*
	 * The old header out first, over a block of zeros.  Until the commit this slot says
	 * nothing, which is the point: a save that stops anywhere in the middle is not mistaken
	 * for the save before it.
	 */
	memset(blk, 0, APFS_STORE_PAYLOAD);
	error = slot_write(st, w->slot, 0, blk, APFS_STORE_PAYLOAD, &w->blocks);
	apfs_free(blk, APFS_STORE_PAYLOAD);
	if (error == 0) {
		w->open = true;
	}
	return error;
}

int
apfs_store_put(struct apfs_store_write *w, const void *data, size_t len)
{
	int error;

	if (!w->open) {
		return EINVAL;
	}
	if (len == 0) {
		return 0;
	}
	if (w->at > w->st->payload_max || len > w->st->payload_max - w->at) {
		apfs_log("store: %llu bytes more than the slot's %llu will hold\n",
		    (unsigned long long)len, (unsigned long long)w->st->payload_max);
		return ENOSPC;
	}
	error = slot_write(w->st, w->slot, APFS_STORE_PAYLOAD + w->at, data, len, &w->blocks);
	if (error) {
		return error;
	}
	w->sum = fnv1a(data, len, w->sum);
	w->at += len;
	return 0;
}

int
apfs_store_commit(struct apfs_store_write *w, uint64_t seconds, uint64_t note)
{
	struct apfs_store_head h;
	uint8_t *blk;
	int error;

	if (!w->open) {
		return EINVAL;
	}
	memset(&h, 0, sizeof(h));
	h.version = APFS_STORE_VERSION;
	h.slot = (uint32_t)w->slot;
	h.generation = w->generation;
	h.bytes = w->at;
	h.seconds = seconds;
	h.note = note;
	h.sum = w->sum;

	blk = apfs_alloc(APFS_STORE_PAYLOAD);
	if (blk == NULL) {
		return ENOMEM;
	}
	head_pack(blk, &h);
	error = slot_write(w->st, w->slot, 0, blk, APFS_STORE_PAYLOAD, &w->blocks);
	apfs_free(blk, APFS_STORE_PAYLOAD);
	if (error == 0) {
		w->open = false;
	}
	return error;
}

int
apfs_store_save(struct apfs_store *st, const void *data, uint64_t len, uint64_t seconds,
    uint64_t note, void *scratch, size_t scratch_len, struct apfs_store_write *w)
{
	int error = apfs_store_begin(st, scratch, scratch_len, w);

	if (error == 0 && len != 0) {
		error = apfs_store_put(w, data, (size_t)len);
	}
	if (error == 0) {
		error = apfs_store_commit(w, seconds, note);
	}
	return error;
}
