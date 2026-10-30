/*
 * Writing into APFS: the contents of an existing file, in place.
 *
 * The driver beside this file reads APFS and does not write it, and that one fact shapes the
 * whole port: /private/var has to be a filesystem in memory, remade empty at every boot, and
 * what is to outlive a boot has to be packed into an archive and put somewhere that is not a
 * filesystem at all (userland/src/var_store.c).  This file is the first step out of that, and
 * it is a deliberately small one.
 *
 * What a write has to touch, and in what order
 * -------------------------------------------
 * APFS never overwrites a structure it has published.  A change is a transaction: fresh copies
 * of everything from the changed record up to the container superblock, written into free
 * space, and then one block -- a new container superblock in the checkpoint descriptor ring,
 * with a transaction identifier one larger -- that makes all of them live at once.  For a
 * single record of a single file that is, from the bottom up:
 *
 *   1. the data blocks of the file                     (not in any tree; this file writes these)
 *   2. the leaf of the file-system tree holding the record, and every node above it to the root
 *      (apfs_btree_node_phys, virtual objects: a new block each)
 *   3. a new entry per new node in the volume's object map, keyed (oid, xid)  (apfs_omap_*)
 *      -- which is itself a B-tree of physical nodes, so its own path to the root as well
 *   4. the extent-reference tree, if the set of physical extents changed  (apfs_j_phys_ext_val)
 *   5. a new copy of the volume superblock, and an entry for it in the container's object map
 *      (apfs_superblock, apfs_omap_phys at nx_omap_oid)
 *   6. the space manager, for every block allocated and freed above: the chunk bitmaps
 *      (APFS_OBJECT_TYPE_SPACEMAN_BITMAP) in the internal pool, the chunk-info blocks that
 *      count their free blocks, the totals in the space manager itself, and the two free
 *      queues that hold freed blocks until no older checkpoint can want them
 *   7. the ephemeral objects -- the space manager and the reaper -- copied into the checkpoint
 *      data ring, and a checkpoint map (apfs_checkpoint_map_phys) in the descriptor ring that
 *      says where in the ring each of them landed
 *   8. the container superblock (apfs_nx_superblock) last, into the next block of the
 *      descriptor ring, with nx_next_xid raised and its Fletcher-64 covering the lot
 *
 * Step 8 is the commit, and it is a commit because of how the reader finds the container:
 * apfs_nx_open() scans the whole descriptor ring and takes the superblock with the largest
 * transaction identifier that passes its checksum, passing over without a word anything that
 * does not (apfs_lib.c).  So up to and including a torn step 8 the previous checkpoint is still
 * the live one and nothing of steps 1-7 is reachable; after an intact step 8 all of it is.
 * That is the whole of APFS's crash safety and it is cheap -- but only if every one of steps
 * 1-7 went to blocks that the previous checkpoint does not use, which is what the space
 * manager is for, and the space manager is the part of APFS this driver does not model at all.
 *
 * What the reader here already understands, and what it does not
 * -------------------------------------------------------------
 * Understood: the object header and its Fletcher-64; the descriptor ring and picking the live
 * superblock out of it; both object maps, looked up by (oid, xid); B-tree nodes of every shape
 * the volumes use (virtual and physical children, fixed and variable key/value sizes, sealed
 * nodes with hashes); the file-system tree's records -- inodes, directory records, attributes,
 * data streams, file extents; the separate extent tree of a sealed volume; holes; and the
 * contents of compressed files.
 *
 * Not modelled, not even as a structure definition: the space manager and every part of it
 * (chunk bitmaps, chunk-info blocks and their blocks, the internal pool and its ring of bitmap
 * copies, the free queues) -- the word appears in apfs_format.h twice, both times as an object
 * type constant; the checkpoint data ring; the checkpoint map, which the header says outright
 * is skipped because only the space manager needs it; the snapshot metadata tree; the integrity
 * metadata and per-file hashes of a sealed volume.  Also absent is any notion of a dirty object,
 * an allocation, or a transaction: nothing in apfs_lib.c holds a block to be written later.
 *
 * So the smallest change to one metadata record needs the space manager, the checkpoint rings
 * and a transaction, which is most of a writable APFS.  That is the honest cost, and it is why
 * this file does something else.
 *
 * What this file does
 * -------------------
 * It replaces bytes inside a file that already exists, without changing its length, its extents
 * or any other structure.  Nothing in steps 2-8 above happens, because nothing in them has
 * anything to say about the bytes in a data block: the only blocks written are blocks that the
 * file's own extent records, and the extent-reference tree, both say belong to that one file.
 *
 * It is therefore safe against a power failure in the strong sense, and not by being atomic:
 * every structure of the container is exactly as it was, so the volume mounts, every tree and
 * every checksum still checks, and every other file is byte for byte what it was.  The one
 * thing a failure can leave behind is a mixture of the old and the new contents of the file
 * being written -- which is a loss the caller is in a position to notice and handle, as
 * var_store.c already does for the raw blocks it writes today: two slots, and a checksum
 * written last.
 *
 * What it refuses, and why.  Every one of these is read off the volume before a byte goes out:
 *
 *   the device cannot write                 dev.write is NULL, as it is at every mount
 *   a sealed volume                         its integrity metadata holds a hash of each file's
 *                                           contents, and we do not update them
 *   a volume with snapshots                 a snapshot refers to these very blocks
 *   no extent-reference tree                then nothing can say the blocks are not shared
 *   not a regular file, or no data stream
 *   a compressed file                       its bytes are in an attribute or a resource fork,
 *                                           and its decmpfs header states the size
 *   past the end of the file                a write that grows a file changes its records
 *   a hole in the range                     filling one means allocating, which means step 6
 *   an extent whose extent-reference record does not begin at the same block, does not cover
 *       the whole extent, is not owned by this stream, or has more than one reference
 *                                           -- a clone, a snapshot or a tree we misread
 *   a block outside the container, or inside block zero, the checkpoint rings or the container's
 *       object map                          -- the bound that holds even if a tree was misread
 *
 * The bounds are deliberately three deep, after the fashion of the writes into the phone's
 * flash (bsd/dev/redwood_ufs.c): the file's own extents say which blocks are its contents, the
 * extent-reference tree says the same blocks are that stream's and nobody else's, and a plain
 * range check refuses anything near the container's own structures whatever the trees said.
 * Then every block is read back and compared before the next one goes out.
 *
 * What is left out that a filesystem would do: the file's modification time is not touched (a
 * stale mtime is a lie, but not damage, and setting it is a transaction), and in the kernel the
 * pages of the file in the unified buffer cache would have to be invalidated by the caller --
 * there is no caller in the kernel yet, which is why that is not done here.
 */

#include "apfs_write.h"

/* ------------------------------------------------------------------ little-endian reads */

static inline uint32_t
w_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint64_t
w_le64(const uint8_t *p)
{
	return (uint64_t)w_le32(p) | (uint64_t)w_le32(p + 4) << 32;
}

/* ------------------------------------------------------------------ the volume */

int
apfs_write_volume_ok(struct apfs_vol *vol, const char **why)
{
	const char *reason = NULL;

	if (vol->nx->dev.write == NULL) {
		reason = "the device does not take writes";
	} else if (vol->nx->block_size < APFS_NX_MINIMUM_BLOCK_SIZE) {
		reason = "the container's block size makes no sense";
	} else if (vol->incompat & APFS_INCOMPAT_SEALED_VOLUME) {
		reason = "a sealed volume: its integrity metadata hashes the contents";
	} else if (vol->integrity_meta_oid != 0) {
		reason = "the volume has integrity metadata: the contents are hashed";
	} else if (vol->num_snapshots != 0) {
		reason = "the volume has snapshots, which share its blocks";
	} else if (vol->role == APFS_VOL_ROLE_SYSTEM || vol->role == APFS_VOL_ROLE_PREBOOT) {
		/*
		 * Not an APFS rule but this port's: the system volume of iOS is not ours to
		 * change.  It is worth saying out loud because the gates above do not catch it --
		 * on an iPhone that volume is sealed, but the copy this port boots from was
		 * unpacked out of an IPSW and is not, so it would otherwise pass every check
		 * here.  What keeps it safe on the phone is where a write may go at all (the
		 * loader's BLOCK_WRITE_FIRST and a kernel whose block devices refuse every
		 * write); this is one more fence in front of that, not instead of it.
		 */
		reason = "the volume is iOS's own system or preboot volume";
	} else if (vol->extentref_oid == 0) {
		reason = "no extent-reference tree: nothing can say a block is not shared";
	}
	if (why != NULL) {
		*why = reason;
	}
	return reason == NULL ? 0 : EROFS;
}

/*
 * Blocks [start, start + count) may hold the contents of a file and nothing else.  This knows
 * nothing of any tree: it is the bound that stands even if every tree was read wrongly.
 */
static bool
blocks_are_free_ground(struct apfs_nx *nx, uint64_t start, uint64_t count)
{
	uint64_t end;

	if (count == 0 || start == 0 || start >= nx->block_count) {
		return false;
	}
	if (count > nx->block_count - start) {
		return false;
	}
	end = start + count;
	/* the two checkpoint rings, as the live container superblock names them */
	if (nx->xp_desc_blocks != 0 && start < nx->xp_desc_base + nx->xp_desc_blocks &&
	    end > nx->xp_desc_base) {
		return false;
	}
	if (nx->xp_data_blocks != 0 && start < nx->xp_data_base + nx->xp_data_blocks &&
	    end > nx->xp_data_base) {
		return false;
	}
	/* the container's object map, which is at a block address and not behind one */
	if (nx->omap_addr >= start && nx->omap_addr < end) {
		return false;
	}
	return true;
}

/*
 * The extent-reference tree's word on an extent: one record, beginning at this very block,
 * covering the whole of it, owned by this stream, referred to once.  Anything else -- a record
 * that begins elsewhere, a shorter one, another owner, a second reference -- is a clone, a
 * snapshot, or a tree we did not read the way the writer of it meant, and is refused.
 */
static int
extent_is_ours(struct apfs_btree *er, uint64_t stream_id, uint64_t phys, uint64_t blocks,
    const char **why)
{
	struct apfs_cursor cur;
	struct apfs_rec rec;
	uint64_t len, owner;
	int32_t refcnt;
	int error;

	error = apfs_seek(er, phys, APFS_TYPE_EXTENT, &cur);
	if (error) {
		*why = "the extent-reference tree cannot be read";
		return error;
	}
	error = apfs_cursor_next(&cur, &rec);
	if (error != 0 || rec.obj_id != phys || rec.type != APFS_TYPE_EXTENT) {
		*why = "no extent-reference record begins at this extent's first block";
		return EROFS;
	}
	if (rec.val == NULL || rec.val_len < 20) {
		*why = "an extent-reference record too short to be one";
		return EIO;
	}
	len = w_le64(rec.val) & APFS_PEXT_LEN_MASK;
	owner = w_le64(rec.val + 8);
	refcnt = (int32_t)w_le32(rec.val + 16);
	if (len < blocks) {
		*why = "the extent-reference record covers less than the extent";
		return EROFS;
	}
	if (owner != stream_id) {
		*why = "the extent-reference record names another stream as the owner";
		return EROFS;
	}
	if (refcnt != 1) {
		*why = "the extent is referred to more than once: a clone or a snapshot";
		return EROFS;
	}
	return 0;
}

/* ------------------------------------------------------------------ the write */

/*
 * One block out and read back again.  A device that says a write went through and then hands
 * back something else is the one failure no amount of checking above can catch, so every block
 * is compared before the next goes out -- the same bargain the writes into the phone's flash
 * make (bsd/dev/redwood_ufs.c).
 */
static int
block_out(struct apfs_nx *nx, uint64_t block, const uint8_t *data, uint8_t *scratch)
{
	uint64_t at = block * (uint64_t)nx->block_size;
	int error;

	error = nx->dev.write(nx->dev.ctx, at, data, nx->block_size);
	if (error) {
		return error;
	}
	error = nx->dev.read_data != NULL ?
	    nx->dev.read_data(nx->dev.ctx, at, scratch, nx->block_size) :
	    nx->dev.read(nx->dev.ctx, at, scratch, nx->block_size);
	if (error) {
		return error;
	}
	if (memcmp(scratch, data, nx->block_size) != 0) {
		apfs_log("block %llu read back as something else than it was written\n",
		    (unsigned long long)block);
		return EIO;
	}
	return 0;
}

int
apfs_write_in_place(struct apfs_vol *vol, uint64_t obj_id, uint64_t off, const void *buf,
    size_t len, struct apfs_write_report *rep)
{
	struct apfs_nx *nx = vol->nx;
	struct apfs_inode ino;
	struct apfs_xattr xa;
	struct apfs_btree er;
	const uint8_t *src = buf;
	uint8_t *block = NULL, *scratch = NULL;
	uint32_t bs = nx->block_size;
	uint64_t at = off, left = len;
	const char *why = NULL;
	int error;

	memset(rep, 0, sizeof(*rep));
	rep->bytes = len;

	error = apfs_write_volume_ok(vol, &why);
	if (error) {
		goto refuse;
	}
	error = apfs_inode_get(vol, obj_id, &ino);
	if (error) {
		why = "there is no such inode";
		goto refuse;
	}
	if ((ino.mode & APFS_S_IFMT) != APFS_S_IFREG) {
		why = "not a regular file";
		error = EROFS;
		goto refuse;
	}
	if (!ino.has_dstream) {
		why = "the file has no data stream";
		error = EROFS;
		goto refuse;
	}
	/*
	 * A compressed file keeps its bytes in an attribute or a resource fork and its decmpfs
	 * header states the uncompressed size, so writing over its extents writes over nothing
	 * a reader looks at -- or over the resource fork, which is a different stream.
	 */
	error = apfs_xattr_lookup(vol, obj_id, APFS_DECMPFS_XATTR_NAME, &xa);
	if (error == 0) {
		why = "the file is compressed: its bytes are not in its extents";
		error = EROFS;
		goto refuse;
	}
	if (error != ENOENT) {
		why = "the file's attributes cannot be read";
		goto refuse;
	}
	if (len == 0) {
		error = 0;
		goto out;
	}
	if (off > ino.size || len > ino.size - off) {
		why = "the write would go past the end of the file";
		error = EROFS;
		goto refuse;
	}
	error = apfs_extentref_open(vol, &er);
	if (error) {
		why = "the extent-reference tree cannot be opened";
		goto refuse;
	}
	block = apfs_alloc(bs);
	scratch = apfs_alloc(bs);
	if (block == NULL || scratch == NULL) {
		why = "no memory";
		error = ENOMEM;
		goto refuse;
	}

	/*
	 * An extent at a time.  Looking one up walks the stream's extents from the front, so a
	 * write across a badly fragmented file costs more than it looks -- which is fine for what
	 * this is for (a file of a few extents written whole) and would not be for a filesystem.
	 */
	while (left > 0) {
		struct apfs_extent ext;
		uint64_t ext_blocks, piece;

		error = apfs_extent_at(vol, ino.private_id, at, &ext);
		if (error) {
			why = "no extent of the file covers this offset";
			error = error == ENOENT ? EROFS : error;
			goto refuse;
		}
		if (ext.phys == 0) {
			why = "a hole in the range: filling one means allocating blocks";
			error = EROFS;
			goto refuse;
		}
		ext_blocks = ext.length / bs;
		if (!blocks_are_free_ground(nx, ext.phys, ext_blocks)) {
			why = "the extent is not where a file's contents can be";
			error = EROFS;
			goto refuse;
		}
		error = extent_is_ours(&er, ino.private_id, ext.phys, ext_blocks, &why);
		if (error) {
			goto refuse;
		}
		/* how much of this extent the write still wants */
		piece = ext.length - (at - ext.logical);
		if (piece > left) {
			piece = left;
		}

		/*
		 * Whole blocks only: the write path on the phone takes nothing else, and a
		 * partial block is the file's own bytes either way, so it is read, patched and
		 * written back whole.
		 */
		while (piece > 0) {
			uint64_t in_block = (at - ext.logical) % bs;
			uint64_t take = bs - in_block;
			uint64_t b = ext.phys + (at - ext.logical) / bs;

			if (take > piece) {
				take = piece;
			}
			if (in_block == 0 && take == bs) {
				memcpy(block, src, bs);
			} else {
				error = nx->dev.read_data != NULL ?
				    nx->dev.read_data(nx->dev.ctx, b * (uint64_t)bs, block, bs) :
				    nx->dev.read(nx->dev.ctx, b * (uint64_t)bs, block, bs);
				if (error) {
					why = "the block to be patched cannot be read";
					goto refuse;
				}
				memcpy(block + in_block, src, (size_t)take);
				rep->partial++;
			}
			error = block_out(nx, b, block, scratch);
			rep->calls++;
			if (error) {
				why = "the device did not take the block";
				goto refuse;
			}
			if (rep->blocks == 0) {
				rep->first_block = b;
			}
			rep->blocks++;
			src += take;
			at += take;
			piece -= take;
			left -= take;
		}
	}
	error = 0;
	goto out;

refuse:
	if (why == NULL) {
		why = "the write failed";
	}
	rep->why = why;
	apfs_log("write of %llu bytes at %llu of inode %llu refused: %s\n",
	    (unsigned long long)len, (unsigned long long)off, (unsigned long long)obj_id, why);
out:
	if (block != NULL) {
		apfs_free(block, bs);
	}
	if (scratch != NULL) {
		apfs_free(scratch, bs);
	}
	return error;
}
