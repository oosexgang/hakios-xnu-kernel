/*
 * Reading the Apple File System.  See apfs_lib.h for what this does and does not do, and
 * apfs_format.h for the layout.  Nothing here writes to the device.
 *
 * Everything on disk is little-endian and is read with memcpy through the le* helpers, so
 * none of this depends on the alignment of a field inside a block.
 *
 * Validation is the point of most of the code that is not a struct field being copied: a
 * block that fails its Fletcher-64, an offset that leaves its node, a length that does not
 * fit the record it came from, a tree deeper than APFS_MAX_LEVELS -- each ends the call
 * with EIO rather than being followed.
 */

#include "apfs_lib.h"

#if defined(KERNEL) || defined(APFS_HOST_UNICODE)
#define APFS_HAVE_UNICODE 1
#include <sys/unicode.h>                /* utf8_normalizeOptCaseFoldAndHash, ...AndCompare (bsd/vfs/vfs_unicode.c) */
#else
#define APFS_HAVE_UNICODE 0
#endif

#ifdef KERNEL
#include <libkern/zlib.h>
#else
#include <zlib.h>
#endif
#include "libdeflate/libdeflate.h"
#include "libzbitmap/libzbitmap.h"
#ifdef KERNEL
#include <libkern/crypto/sha2.h>
#else
#include <openssl/sha.h>
#endif

/* The pieces a resource-fork-compressed file is cut into. */
#define APFS_RSRC_PIECE APFS_DECMPFS_BLOCK_SIZE

/* ------------------------------------------------------------------ bytes */

static inline uint16_t
le16(const uint8_t *p)
{
	uint16_t v;
	memcpy(&v, p, sizeof(v));
	return v;
}

static inline uint32_t
le32(const uint8_t *p)
{
	uint32_t v;
	memcpy(&v, p, sizeof(v));
	return v;
}

static inline uint64_t
le64(const uint8_t *p)
{
	uint64_t v;
	memcpy(&v, p, sizeof(v));
	return v;
}

static inline uint32_t
be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/*
 * Fletcher-64 as APFS uses it: over the block with its own checksum field included, the
 * result is zero; over the block after the field, the result is what the field holds.
 */
uint64_t
apfs_fletcher64(const uint8_t *data, size_t len)
{
	const uint64_t mod = 0xFFFFFFFFULL;
	uint64_t sum1 = 0, sum2 = 0;
	size_t i;

	for (i = 0; i + 4 <= len; i += 4) {
		sum1 = (sum1 + le32(data + i)) % mod;
		sum2 = (sum2 + sum1) % mod;
	}
	{
		uint64_t c1 = mod - ((sum1 + sum2) % mod);
		uint64_t c2 = mod - ((sum1 + c1) % mod);
		return (c2 << 32) | c1;
	}
}

bool
apfs_checksum_ok(const uint8_t *block, uint32_t block_size)
{
	if (block_size < 8 || (block_size % 4) != 0) {
		return false;
	}
	return apfs_fletcher64(block + 8, block_size - 8) == le64(block);
}

uint16_t
apfs_mode_to_dtype(uint16_t mode)
{
	switch (mode & APFS_S_IFMT) {
	case APFS_S_IFIFO:  return APFS_DT_FIFO;
	case APFS_S_IFCHR:  return APFS_DT_CHR;
	case APFS_S_IFDIR:  return APFS_DT_DIR;
	case APFS_S_IFBLK:  return APFS_DT_BLK;
	case APFS_S_IFREG:  return APFS_DT_REG;
	case APFS_S_IFLNK:  return APFS_DT_LNK;
	case APFS_S_IFSOCK: return APFS_DT_SOCK;
	default:            return APFS_DT_UNKNOWN;
	}
}

/* ------------------------------------------------------------------ blocks */

/*
 * A block of metadata, from the cache or from the device.  The returned pointer stays
 * good while fewer than APFS_CACHE_RECENT further blocks have been fetched -- the clock
 * hand passes over a block used that recently -- which is why a B-tree descent holds
 * block addresses and not pointers.
 */
static inline uint32_t
cache_bucket(uint64_t addr)
{
	return (uint32_t)((addr * 0x9E3779B97F4A7C15ULL) >> 40) & (APFS_CACHE_HASH - 1);
}

/* A cached block's memory leaves the cache; it is freed by apfs_nx_reclaim once nobody can be reading it. */
static void
cache_retire(struct apfs_nx *nx, uint8_t *data)
{
	struct apfs_retired *r = apfs_alloc(sizeof(*r));

	if (r == NULL) {
		return;                 /* never freed then: that is safe, merely a block lost */
	}
	r->data = data;
	r->epoch = nx->epoch++;
	r->next = nx->retired;
	nx->retired = r;
	if (++nx->retired_now > nx->retired_most) {
		nx->retired_most = nx->retired_now;
	}
}

void
apfs_nx_reclaim(struct apfs_nx *nx, uint64_t oldest)
{
	struct apfs_retired **pp = &nx->retired, *r;

	/* newest first: past the first one old enough, all are */
	while (*pp != NULL && (*pp)->epoch >= oldest) {
		pp = &(*pp)->next;
	}
	while ((r = *pp) != NULL) {
		*pp = r->next;
		apfs_free(r->data, nx->block_size);
		apfs_free(r, sizeof(*r));
		nx->retired_now--;
	}
}

#ifndef KERNEL
void
apfs_nx_cache_flush(struct apfs_nx *nx)
{
	uint32_t i;

	for (i = 0; i < APFS_CACHE_BLOCKS; i++) {
		struct apfs_buf *b = &nx->cache[i];

		if (b->data != NULL && nx->unlock != NULL) {
			cache_retire(nx, b->data);
			b->data = NULL;
		}
		b->addr = APFS_BUF_EMPTY;
		b->next = -1;
		b->ref = false;
	}
	for (i = 0; i < APFS_CACHE_HASH; i++) {
		nx->cache_hash[i] = -1;
	}
	if (nx->omap_cache != NULL) {
		memset(nx->omap_cache, 0, APFS_OMAP_CACHE * sizeof(*nx->omap_cache));
	}
}
#endif

/* A free slot, or the first the clock hand finds neither used since it last passed nor recent. */
static struct apfs_buf *
cache_victim(struct apfs_nx *nx)
{
	struct apfs_buf *victim;
	uint32_t steps = 0;
	int32_t *pp;

	for (;;) {
		victim = &nx->cache[nx->hand];
		nx->hand = (nx->hand + 1) % APFS_CACHE_BLOCKS;
		if (victim->data == NULL) {
			victim->data = apfs_alloc(nx->block_size);
			if (victim->data == NULL) {
				return NULL;
			}
			break;
		}
		if (!victim->ref && nx->fetches - victim->used >= APFS_CACHE_RECENT) {
			break;
		}
		victim->ref = false;
		if (++steps >= 2 * APFS_CACHE_BLOCKS) {
			/* every block used that recently: the one used longest ago, then */
			uint32_t k, best = 0;

			for (k = 1; k < APFS_CACHE_BLOCKS; k++) {
				if (nx->fetches - nx->cache[k].used > nx->fetches - nx->cache[best].used) {
					best = k;
				}
			}
			victim = &nx->cache[best];
			break;
		}
	}
	if (nx->unlock != NULL && victim->data != NULL && victim->addr != APFS_BUF_EMPTY) {
		/* somebody may still be reading the block that had it: new memory for the new one */
		uint8_t *fresh = apfs_alloc(nx->block_size);

		if (fresh == NULL) {
			return NULL;
		}
		cache_retire(nx, victim->data);
		victim->data = fresh;
	}
	if (victim->addr != APFS_BUF_EMPTY) {
		/* out of its old chain */
		for (pp = &nx->cache_hash[cache_bucket(victim->addr)]; *pp >= 0;
		    pp = &nx->cache[*pp].next) {
			if (&nx->cache[*pp] == victim) {
				*pp = victim->next;
				break;
			}
		}
		victim->addr = APFS_BUF_EMPTY;
	}
	victim->next = -1;
	return victim;
}

static void
cache_link(struct apfs_nx *nx, struct apfs_buf *b, uint64_t addr)
{
	uint32_t bucket = cache_bucket(addr);

	b->addr = addr;
	b->next = nx->cache_hash[bucket];
	nx->cache_hash[bucket] = (int32_t)(b - nx->cache);
}

static struct apfs_buf *
cache_find(struct apfs_nx *nx, uint64_t addr)
{
	int32_t i;

	for (i = nx->cache_hash[cache_bucket(addr)]; i >= 0; i = nx->cache[i].next) {
		if (nx->cache[i].addr == addr) {
			return &nx->cache[i];
		}
	}
	return NULL;
}

static inline void nx_unlock(struct apfs_nx *nx);
static inline void nx_relock(struct apfs_nx *nx);

/*
 * A block of metadata not in the cache, read without the lock: the block and its aligned
 * neighbours into memory of this call's own, the lock taken back, and put in -- unless
 * somebody put the block in meanwhile, whose copy is used then (and only the neighbours not
 * there yet go in). Cache slots that get a block get new memory (cache_victim), so nothing
 * anyone still reads is overwritten.
 */
static int
nx_block_unlocked(struct apfs_nx *nx, uint64_t addr, const uint8_t **out, bool verify)
{
	uint32_t bs = nx->block_size;
	uint32_t k = nx->meta_cluster != 0 ?
	    (nx->meta_cluster > APFS_META_CLUSTER ? APFS_META_CLUSTER : nx->meta_cluster) : APFS_META_CLUSTER_DEFAULT;
	uint64_t first = addr - addr % k, count = nx->block_count - first < k ? nx->block_count - first : k, i, t0, dt;
	struct apfs_buf *b;
	uint8_t *mine = apfs_alloc((size_t)count * bs);
	int error;

	if (mine == NULL) {
		return ENOMEM;
	}
	nx->unlocked_fills++;
	nx_unlock(nx);
	t0 = apfs_ticks();
	error = nx->dev.read_data(nx->dev.ctx, first * (uint64_t)bs, mine, (size_t)count * bs);
	dt = apfs_ticks() - t0;
	nx_relock(nx);
	nx->dev_ticks += dt;
	nx->dev_calls++;
	nx->dev_bytes += count * bs;
	if (error) {
		apfs_free(mine, (size_t)count * bs);
		return error;
	}
	nx->fetches++;
	b = cache_find(nx, addr);
	if (b != NULL) {
		nx->fill_races++;               /* in already, by somebody else's read */
		if (verify && !b->checked) {
			if (!apfs_checksum_ok(b->data, bs)) {
				apfs_log("block %llu: bad checksum\n", (unsigned long long)addr);
				apfs_free(mine, (size_t)count * bs);
				return EIO;
			}
			b->checked = true;
		}
	} else {
		b = cache_victim(nx);
		if (b == NULL) {
			apfs_free(mine, (size_t)count * bs);
			return ENOMEM;
		}
		memcpy(b->data, mine + (addr - first) * bs, bs);
		nx->reads++;
		if (verify && !apfs_checksum_ok(b->data, bs)) {
			apfs_log("block %llu: bad checksum\n", (unsigned long long)addr);
			apfs_free(mine, (size_t)count * bs);
			return EIO;             /* (the slot stays empty: cache_victim took it out of its chain) */
		}
		b->checked = verify;
		cache_link(nx, b, addr);
	}
	b->ref = true;
	b->used = nx->fetches;
	/* the neighbours not there yet: kept as if long unused, checked when asked for */
	for (i = first; i < first + count; i++) {
		struct apfs_buf *nb;

		if (i == addr || i == 0 || cache_find(nx, i) != NULL) {
			continue;
		}
		nb = cache_victim(nx);
		if (nb == NULL) {
			break;
		}
		memcpy(nb->data, mine + (i - first) * bs, bs);
		nb->checked = false;
		nb->ref = false;
		nb->used = nx->fetches - APFS_CACHE_RECENT;
		cache_link(nx, nb, i);
		nx->prefetched++;
	}
	apfs_free(mine, (size_t)count * bs);
	*out = b->data;
	return 0;
}

static int
nx_block(struct apfs_nx *nx, uint64_t addr, const uint8_t **out, bool verify)
{
	struct apfs_buf *victim, *b;
	uint64_t first = 0, count = 0, i;
	int error;

	if (addr == 0 || addr >= nx->block_count) {
		return EIO;
	}
	if (addr > nx->max_addr) {
		nx->max_addr = addr;
	}
	b = cache_find(nx, addr);
	if (b != NULL) {
		if (verify && !b->checked) {
			/* brought in as a neighbour, asked for now */
			if (!apfs_checksum_ok(b->data, nx->block_size)) {
				apfs_log("block %llu: bad checksum\n", (unsigned long long)addr);
				return EIO;
			}
			b->checked = true;
		}
		if (!b->ref && nx->fetches - b->used >= APFS_CACHE_RECENT) {
			nx->prefetch_used++;    /* (or one used long ago; near enough) */
		}
		b->ref = true;
		b->used = nx->fetches;
		nx->hits++;
		*out = b->data;
		return 0;
	}
	if (nx->unlock != NULL && nx->dev.read_data != NULL && !nx->cheap_requests) {
		return nx_block_unlocked(nx, addr, out, verify);
	}
	victim = cache_victim(nx);
	if (victim == NULL) {
		return ENOMEM;
	}
	nx->fetches++;
	if (nx->cluster == NULL && nx->dev.read_data != NULL) {
		nx->cluster = apfs_alloc((size_t)APFS_META_CLUSTER * nx->block_size);
	}
	{
		uint64_t t0 = apfs_ticks();

		uint32_t k = nx->meta_cluster != 0 ?
		    (nx->meta_cluster > APFS_META_CLUSTER ? APFS_META_CLUSTER : nx->meta_cluster) :
		    nx->cheap_requests ? 1 : APFS_META_CLUSTER_DEFAULT;

		if (nx->cluster != NULL && k > 1) {
			/* the block and its aligned neighbours, in one request */
			first = addr - addr % k;
			count = nx->block_count - first < k ? nx->block_count - first : k;
			error = nx->dev.read_data(nx->dev.ctx, first * (uint64_t)nx->block_size,
			    nx->cluster, (size_t)count * nx->block_size);
			if (error == 0) {
				memcpy(victim->data, nx->cluster + (addr - first) * nx->block_size,
				    nx->block_size);
			}
			nx->dev_bytes += count * nx->block_size;
		} else {
			error = nx->dev.read(nx->dev.ctx, addr * (uint64_t)nx->block_size,
			    victim->data, nx->block_size);
			nx->dev_bytes += nx->block_size;
		}
		nx->dev_ticks += apfs_ticks() - t0;
		nx->dev_calls++;
	}
	if (error) {
		return error;
	}
	nx->reads++;
	if (verify && !apfs_checksum_ok(victim->data, nx->block_size)) {
		apfs_log("block %llu: bad checksum\n", (unsigned long long)addr);
		return EIO;
	}
	victim->checked = verify;
	victim->ref = true;
	victim->used = nx->fetches;
	cache_link(nx, victim, addr);
	/*
	 * The neighbours: kept as if long unused, so that they are the first to go if nobody
	 * asks for them, and checked when somebody does.
	 */
	for (i = first; i < first + count; i++) {
		struct apfs_buf *nb;

		if (i == addr || i == 0 || cache_find(nx, i) != NULL) {
			continue;
		}
		nb = cache_victim(nx);
		if (nb == NULL) {
			break;
		}
		memcpy(nb->data, nx->cluster + (i - first) * nx->block_size, nx->block_size);
		nb->checked = false;
		nb->ref = false;
		nb->used = nx->fetches - APFS_CACHE_RECENT;
		cache_link(nx, nb, i);
		nx->prefetched++;
	}
	*out = victim->data;
	return 0;
}

static inline void
nx_unlock(struct apfs_nx *nx)
{
	if (nx->unlock != NULL) {
		nx->unlock(nx->lock_ctx);
	}
}

static inline void
nx_relock(struct apfs_nx *nx)
{
	if (nx->relock != NULL) {
		nx->relock(nx->lock_ctx);
	}
}

static uint32_t
obj_type(const uint8_t *blk)
{
	return le32(blk + 24) & APFS_OBJECT_TYPE_MASK;
}

static apfs_xid_t
obj_xid(const uint8_t *blk)
{
	return le64(blk + 16);
}

/* ------------------------------------------------------------------ b-tree nodes */

struct node {
	const uint8_t *blk;
	uint16_t       flags;
	uint16_t       level;
	uint32_t       nkeys;
	uint32_t       toc_off;
	uint32_t       toc_len;
	uint32_t       key_base;
	uint32_t       val_end;
	bool           fixed;
	bool           leaf;
};

/*
 * Pick a node block apart and check that every area it describes is inside the block.
 * A node is: the object header, the table of contents, then keys growing forward from the
 * end of the table and values growing backwards from the end of the block -- short of the
 * btree_info that a root node keeps in its last bytes.
 */
static int
node_parse(struct apfs_btree *tree, const uint8_t *blk, struct node *n)
{
	uint32_t bs = tree->nx->block_size;
	uint32_t ts_off, ts_len, ent, need;

	n->blk = blk;
	n->flags = le16(blk + 32);
	n->level = le16(blk + 34);
	n->nkeys = le32(blk + 36);
	ts_off = le16(blk + 40);
	ts_len = le16(blk + 42);
	n->fixed = (n->flags & APFS_BTNODE_FIXED_KV_SIZE) != 0;
	n->leaf = (n->flags & APFS_BTNODE_LEAF) != 0;

	if (n->level >= APFS_MAX_LEVELS) {
		return EIO;
	}
	n->toc_off = 56 + ts_off;
	n->toc_len = ts_len;
	if (n->toc_off < 56 || n->toc_off > bs || n->toc_len > bs - n->toc_off) {
		return EIO;
	}
	n->key_base = n->toc_off + n->toc_len;
	n->val_end = bs;
	if (n->flags & APFS_BTNODE_ROOT) {
		if (bs < 56 + sizeof(struct apfs_btree_info)) {
			return EIO;
		}
		n->val_end = bs - (uint32_t)sizeof(struct apfs_btree_info);
	}
	if (n->key_base > n->val_end) {
		return EIO;
	}
	/* the table of contents must hold the entries the header claims */
	ent = n->fixed ? 4u : 8u;
	if (n->nkeys > (bs / ent)) {
		return EIO;
	}
	need = n->nkeys * ent;
	if (need > n->toc_len) {
		return EIO;
	}
	if (n->fixed && n->leaf && tree->key_size == 0) {
		return EIO;         /* fixed layout with no size to use */
	}
	return 0;
}

/*
 * Entry i of a node.  A value offset of 0xffff marks a key with no value ("a ghost"),
 * which comes back as a zero length rather than an error.
 */
static int
node_entry(struct apfs_btree *tree, const struct node *n, uint32_t i,
    const uint8_t **kp, uint16_t *klen, const uint8_t **vp, uint16_t *vlen)
{
	uint32_t koff, voff, kl, vl;

	if (i >= n->nkeys) {
		return EIO;
	}
	if (n->fixed) {
		koff = le16(n->blk + n->toc_off + 4 * i);
		voff = le16(n->blk + n->toc_off + 4 * i + 2);
		kl = tree->key_size;
		vl = n->leaf ? tree->val_size : 8;
	} else {
		koff = le16(n->blk + n->toc_off + 8 * i);
		kl = le16(n->blk + n->toc_off + 8 * i + 2);
		voff = le16(n->blk + n->toc_off + 8 * i + 4);
		vl = le16(n->blk + n->toc_off + 8 * i + 6);
	}
	/* a key lives at key_base + offset and must end before the value area */
	if (koff > n->val_end - n->key_base || kl > n->val_end - n->key_base - koff) {
		return EIO;
	}
	*kp = n->blk + n->key_base + koff;
	*klen = (uint16_t)kl;

	if (voff == APFS_BTOFF_INVALID && !n->fixed) {
		*vp = NULL;
		*vlen = 0;
		return 0;
	}
	/* a value is measured backwards from the end of the value area */
	if (voff > n->val_end || vl > voff || n->val_end - voff < n->key_base) {
		return EIO;
	}
	*vp = n->blk + n->val_end - voff;
	*vlen = (uint16_t)vl;
	return 0;
}

/* The identifier of child i of a non-leaf node. */
static int
node_child(struct apfs_btree *tree, const struct node *n, uint32_t i, uint64_t *oid)
{
	const uint8_t *k, *v;
	uint16_t kl, vl;
	int error = node_entry(tree, n, i, &k, &kl, &v, &vl);

	if (error) {
		return error;
	}
	if (v == NULL || vl < 8) {
		return EIO;
	}
	*oid = le64(v);
	return 0;
}

static int cfile_header(struct apfs_vol *vol, const struct apfs_inode *ino,
    bool *compressed, uint32_t *type, uint64_t *size, bool *in_rsrc);

/* Where a node identifier actually is: a block address, directly or through a map. */
static int
tree_node_addr(struct apfs_btree *tree, uint64_t oid, uint64_t *addr);

/*
 * Read a node that is already known by block address.
 *
 * Everything that walks a tree keeps block addresses, never object identifiers: the root
 * is only ever an address, so a cursor that mixed the two would resolve the root's
 * address through the object map as though it were an identifier.  That lookup fails with
 * ENOENT, which a caller scanning for records reads as "there are no more" -- the records
 * of the object quietly go missing instead of the read failing.
 */
/*
 * A sealed volume (the iOS cryptexes) keeps its file-system tree in nodes without an object
 * header -- no Fletcher checksum -- and each parent keeps the SHA-256 of each child's whole
 * block instead.  Checked once per block the cache brings in (b->checked, which for such a
 * block means "its hash was right"); `len` is 32 on the way down, 8 when a cursor comes back
 * to a node it had left (apfs_cursor.hash8).  The root has no parent: Apple checks it against
 * the volume's root hash, which is kept outside the image (the IPSW's .root_hash), and
 * nothing here has that.
 */
static void
apfs_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
#ifdef KERNEL
	SHA256_CTX c;

	SHA256_Init(&c);
	SHA256_Update(&c, data, len);
	SHA256_Final(out, &c);
#else
	SHA256(data, len, out);
#endif
}

static int
node_hash_check(struct apfs_nx *nx, uint64_t addr, const uint8_t *blk, const uint8_t *hash, size_t len)
{
	struct apfs_buf *b = cache_find(nx, addr);
	uint8_t d[32];

	if (b != NULL && b->checked) {
		return 0;
	}
	apfs_sha256(blk, nx->block_size, d);
	if (memcmp(d, hash, len) != 0) {
		apfs_log("block %llu: not the node its parent names (SHA-256)\n", (unsigned long long)addr);
		return EIO;
	}
	if (b != NULL) {
		b->checked = true;
	}
	return 0;
}

/* hash8: for a sealed tree's node, the first 8 bytes of its SHA-256 (0 for the root: none) */
static int
node_at_h(struct apfs_btree *tree, uint64_t addr, uint64_t hash8, const uint8_t **blk, struct node *n)
{
	bool noheader = (tree->flags & APFS_BTREE_NOHEADER) != 0;
	int error = nx_block(tree->nx, addr, blk, !noheader);

	if (error) {
		return error;
	}
	if (noheader) {
		uint8_t h[8];

		if ((le16(*blk + 32) & APFS_BTNODE_NOHEADER) == 0) {
			return EIO;
		}
		if (addr != tree->root) {
			if (hash8 == 0) {
				return EIO;     /* (a node below the root always comes with its parent's hash) */
			}
			memcpy(h, &hash8, 8);
			error = node_hash_check(tree->nx, addr, *blk, h, 8);
			if (error) {
				return error;
			}
		}
	} else if (obj_type(*blk) != APFS_OBJECT_TYPE_BTREE &&
	    obj_type(*blk) != APFS_OBJECT_TYPE_BTREE_NODE) {
		return EIO;
	}
	return node_parse(tree, *blk, n);
}

static int
node_at(struct apfs_btree *tree, uint64_t addr, const uint8_t **blk, struct node *n)
{
	return node_at_h(tree, addr, 0, blk, n);
}

/*
 * The block address of child i of a non-leaf node.  In a sealed (hashed) tree the value also
 * holds the child's SHA-256, and the id in it is an offset from the root's id -- apfsck says
 * so as well; Apple's reference does not.  The child's block is checked against the hash
 * here, and the first 8 bytes of it given back for a cursor to keep (hash8, may be NULL).
 */
static int
node_child_addr(struct apfs_btree *tree, const struct node *n, uint32_t i, uint64_t *addr,
    uint64_t *hash8)
{
	const uint8_t *k, *v, *blk;
	uint16_t kl, vl;
	uint8_t hash[32];
	uint64_t oid;
	int error;

	if ((tree->flags & APFS_BTREE_HASHED) == 0) {
		error = node_child(tree, n, i, &oid);
		if (error) {
			return error;
		}
		if (hash8 != NULL) {
			*hash8 = 0;
		}
		return tree_node_addr(tree, oid, addr);
	}
	error = node_entry(tree, n, i, &k, &kl, &v, &vl);
	if (error) {
		return error;
	}
	if (v == NULL || vl != 8 + sizeof(hash)) {
		apfs_log("a sealed tree's node with a %u-byte child entry (only SHA-256 is known here)\n", vl);
		return ENOTSUP;
	}
	oid = le64(v) + tree->root_oid;
	memcpy(hash, v + 8, sizeof(hash));
	error = tree_node_addr(tree, oid, addr);
	if (error) {
		return error;
	}
	error = nx_block(tree->nx, *addr, &blk, (tree->flags & APFS_BTREE_NOHEADER) == 0);
	if (error) {
		return error;
	}
	if (tree->flags & APFS_BTREE_NOHEADER) {
		error = node_hash_check(tree->nx, *addr, blk, hash, sizeof(hash));
	}
	if (error == 0 && hash8 != NULL) {
		memcpy(hash8, hash, 8);
	}
	return error;
}

/* ------------------------------------------------------------------ object maps */

static int omap_find(struct apfs_omap *omap, uint64_t oid, apfs_xid_t want_xid,
    uint64_t *addr);

static int
tree_node_addr(struct apfs_btree *tree, uint64_t oid, uint64_t *addr)
{
	if (tree->omap == NULL || (tree->flags & APFS_BTREE_PHYSICAL)) {
		*addr = oid;
		return 0;
	}
	return omap_find(tree->omap, oid, tree->nx->xid, addr);
}

/* Read a tree's root and remember what its btree_info says about the whole tree. */
static int
btree_open(struct apfs_nx *nx, struct apfs_omap *omap, uint64_t root_oid,
    struct apfs_btree *tree)
{
	const uint8_t *blk;
	uint64_t addr;
	uint32_t bs = nx->block_size;
	const uint8_t *info;
	int error;

	memset(tree, 0, sizeof(*tree));
	tree->nx = nx;
	tree->omap = omap;
	/* the root is found the same way as any node, but its flags are not known yet */
	if (omap == NULL) {
		addr = root_oid;
	} else {
		error = omap_find(omap, root_oid, nx->xid, &addr);
		if (error) {
			return error;
		}
	}
	if (bs < 56 + sizeof(struct apfs_btree_info)) {
		return EIO;
	}
	error = nx_block(nx, addr, &blk, false);
	if (error) {
		return error;
	}
	info = blk + bs - sizeof(struct apfs_btree_info);
	if (le64(blk) == 0 && le64(blk + 8) == 0 && le32(blk + 24) == 0 &&
	    (le16(blk + 32) & APFS_BTNODE_NOHEADER) && (le32(info) & APFS_BTREE_NOHEADER)) {
		/* a sealed tree's root: no header, no checksum (see node_hash_check) */
	} else {
		error = nx_block(nx, addr, &blk, true);
		if (error) {
			return error;
		}
		if (obj_type(blk) != APFS_OBJECT_TYPE_BTREE) {
			apfs_log("block %llu is not a b-tree root\n", (unsigned long long)addr);
			return EIO;
		}
	}
	tree->root = addr;
	tree->root_oid = root_oid;
	tree->flags = le32(info);
	tree->key_size = le32(info + 8);
	tree->val_size = le32(info + 12);
	tree->key_count = le64(info + 24);
	tree->node_count = le64(info + 32);
	return 0;
}

static int
omap_open(struct apfs_nx *nx, uint64_t omap_addr, struct apfs_omap *omap)
{
	const uint8_t *blk;
	uint64_t tree_oid;
	int error;

	memset(omap, 0, sizeof(*omap));
	error = nx_block(nx, omap_addr, &blk, true);
	if (error) {
		return error;
	}
	if (obj_type(blk) != APFS_OBJECT_TYPE_OMAP) {
		apfs_log("block %llu is not an object map\n", (unsigned long long)omap_addr);
		return EIO;
	}
	tree_oid = le64(blk + 48);          /* om_tree_oid, a physical tree */
	error = btree_open(nx, NULL, tree_oid, &omap->tree);
	if (error) {
		return error;
	}
	omap->ready = true;
	return 0;
}

/*
 * Where an object lives.  The map is keyed by (identifier, transaction) in that order, so
 * the versions of one object are a run; we want the newest that is not past want_xid.
 */
static int
omap_find(struct apfs_omap *omap, uint64_t oid, apfs_xid_t want_xid, uint64_t *addr)
{
	struct apfs_btree *tree = &omap->tree;
	uint64_t node_oid = tree->root;
	struct node n;
	const uint8_t *blk;
	int level, error;

	struct apfs_omap_hit *hit = NULL;

	if (!omap->ready) {
		return EIO;
	}
	if (tree->nx->omap_cache != NULL) {
		uint64_t h = (oid ^ (tree->root * 0xff51afd7ed558ccdULL)) * 0x9E3779B97F4A7C15ULL;

		hit = &tree->nx->omap_cache[h >> 53 & (APFS_OMAP_CACHE - 1)];
		if (hit->oid == oid && oid != 0 && hit->xid == want_xid &&
		    hit->map == tree->root) {
			tree->nx->omap_hits++;
			*addr = hit->addr;
			return 0;
		}
		tree->nx->omap_misses++;
	}
	error = nx_block(tree->nx, node_oid, &blk, true);
	if (error) {
		return error;
	}
	error = node_parse(tree, blk, &n);
	if (error) {
		return error;
	}
	for (level = 0; level < APFS_MAX_LEVELS; level++) {
		uint32_t lo = 0, hi, best;

		if (n.nkeys == 0) {
			return ENOENT;
		}
		/* the last entry whose key is at or before what we are looking for: halving */
		hi = n.nkeys;
		while (lo < hi) {
			uint32_t mid = lo + (hi - lo) / 2;
			const uint8_t *k, *v;
			uint16_t kl, vl;
			uint64_t o, x;

			error = node_entry(tree, &n, mid, &k, &kl, &v, &vl);
			if (error) {
				return error;
			}
			if (kl < 16) {
				return EIO;
			}
			o = le64(k);
			x = le64(k + 8);
			if (o < oid || (o == oid && x <= want_xid)) {
				lo = mid + 1;
			} else {
				hi = mid;
			}
		}
		if (lo == 0) {
			return ENOENT;
		}
		best = lo - 1;
		if (n.leaf) {
			const uint8_t *k, *v;
			uint16_t kl, vl;

			error = node_entry(tree, &n, best, &k, &kl, &v, &vl);
			if (error) {
				return error;
			}
			if (kl < 16 || le64(k) != oid) {
				return ENOENT;
			}
			if (v == NULL || vl < 16) {
				return EIO;
			}
			/* omap_val: flags, size, then the block address */
			*addr = le64(v + 8);
			if (*addr == 0 || *addr >= tree->nx->block_count) {
				return EIO;
			}
			if (hit != NULL) {
				hit->map = tree->root;
				hit->oid = oid;
				hit->xid = want_xid;
				hit->addr = *addr;
			}
			return 0;
		}
		error = node_child(tree, &n, best, &node_oid);
		if (error) {
			return error;
		}
		error = nx_block(tree->nx, node_oid, &blk, true);
		if (error) {
			return error;
		}
		error = node_parse(tree, blk, &n);
		if (error) {
			return error;
		}
	}
	return EIO;             /* deeper than any real tree: treat as damage */
}

/* ------------------------------------------------------------------ container */

/* The key of a file-system record: the object id and the kind, packed into eight bytes. */
static inline uint64_t
jkey_id(const uint8_t *k)
{
	return le64(k) & APFS_OBJ_ID_MASK;
}

static inline uint8_t
jkey_type(const uint8_t *k)
{
	return (uint8_t)(le64(k) >> APFS_OBJ_TYPE_SHIFT);
}

int
apfs_nx_open(struct apfs_nx *nx, const struct apfs_dev *dev)
{
	uint8_t *block0;
	const uint8_t *blk;
	uint32_t bs, i, desc_blocks;
	uint64_t desc_base;
	apfs_xid_t best_xid = 0;
	uint64_t best_addr = 0;
	int error;

	memset(nx, 0, sizeof(*nx));
	nx->dev = *dev;
	nx->cache = apfs_alloc(APFS_CACHE_BLOCKS * sizeof(*nx->cache));
	nx->cache_hash = apfs_alloc(APFS_CACHE_HASH * sizeof(*nx->cache_hash));
	if (nx->cache == NULL || nx->cache_hash == NULL) {
		return ENOMEM;                  /* apfs_nx_close() frees what there is */
	}
	for (i = 0; i < APFS_CACHE_BLOCKS; i++) {
		nx->cache[i].addr = APFS_BUF_EMPTY;
		nx->cache[i].next = -1;
	}
	for (i = 0; i < APFS_CACHE_HASH; i++) {
		nx->cache_hash[i] = -1;
	}
	/* zeroed, so every slot is empty; without it the object maps are simply walked */
	nx->omap_cache = apfs_alloc(APFS_OMAP_CACHE * sizeof(*nx->omap_cache));

	/*
	 * Block zero holds a container superblock, but it can be behind: the live one is
	 * whichever copy in the checkpoint descriptor area has the largest transaction
	 * identifier.  Block zero is read only to find that area.
	 */
	block0 = apfs_alloc(APFS_NX_MINIMUM_BLOCK_SIZE);
	if (block0 == NULL) {
		return ENOMEM;
	}
	error = nx->dev.read(nx->dev.ctx, 0, block0, APFS_NX_MINIMUM_BLOCK_SIZE);
	if (error) {
		goto out;
	}
	error = EINVAL;
	if (le32(block0 + 32) != APFS_NX_MAGIC) {
		goto out;           /* not an APFS container; let the caller try something else */
	}
	bs = le32(block0 + 36);
	if (bs < APFS_NX_MINIMUM_BLOCK_SIZE || bs > APFS_NX_MAXIMUM_BLOCK_SIZE ||
	    (bs & (bs - 1)) != 0) {
		apfs_log("block size %u makes no sense\n", bs);
		goto out;
	}
	nx->block_size = bs;
	nx->block_count = le64(block0 + 40);
	if (nx->block_count < 16) {
		goto out;
	}
	if (nx->dev.size != 0 && nx->block_count * (uint64_t)bs > nx->dev.size) {
		apfs_log("container claims %llu blocks, the device holds %llu bytes\n",
		    (unsigned long long)nx->block_count, (unsigned long long)nx->dev.size);
		goto out;
	}
	desc_blocks = le32(block0 + 104);
	desc_base = le64(block0 + 112);
	error = EIO;
	if (desc_base == 0 || desc_blocks == 0 || desc_blocks > nx->block_count ||
	    desc_base >= nx->block_count ||
	    desc_base + desc_blocks > nx->block_count) {
		apfs_log("the checkpoint descriptor area is not inside the container\n");
		goto out;
	}

	for (i = 0; i < desc_blocks; i++) {
		/*
		 * Most of the descriptor area is free or holds older checkpoints, so a block
		 * that fails its checksum here is ordinary and is passed over without a word.
		 */
		if (nx_block(nx, desc_base + i, &blk, false) != 0) {
			continue;
		}
		if (!apfs_checksum_ok(blk, nx->block_size)) {
			continue;
		}
		if (obj_type(blk) != APFS_OBJECT_TYPE_NX_SUPERBLOCK ||
		    le32(blk + 32) != APFS_NX_MAGIC || le64(blk + 8) != APFS_OID_NX_SUPERBLOCK) {
			continue;
		}
		if (obj_xid(blk) > best_xid) {
			best_xid = obj_xid(blk);
			best_addr = desc_base + i;
		}
	}
	if (best_addr == 0) {
		apfs_log("no usable container superblock in the checkpoint area\n");
		goto out;
	}
	error = nx_block(nx, best_addr, &blk, true);
	if (error) {
		goto out;
	}
	nx->xid = obj_xid(blk);
	nx->block_count = le64(blk + 40);
	memcpy(nx->uuid, blk + 72, sizeof(nx->uuid));
	nx->xp_desc_blocks = le32(blk + 104);
	nx->xp_data_blocks = le32(blk + 108);
	nx->xp_desc_base = le64(blk + 112);
	nx->xp_data_base = le64(blk + 120);
	nx->omap_addr = le64(blk + 160);
	{
		uint32_t maxfs = le32(blk + 180);

		if (maxfs > APFS_NX_MAX_FILE_SYSTEMS) {
			maxfs = APFS_NX_MAX_FILE_SYSTEMS;
		}
		nx->fs_count = 0;
		for (i = 0; i < maxfs; i++) {
			uint64_t oid = le64(blk + 184 + 8 * i);

			nx->fs_oid[i] = oid;
			if (oid != 0) {
				nx->fs_count = i + 1;
			}
		}
	}
	error = omap_open(nx, nx->omap_addr, &nx->omap);
	if (error) {
		goto out;
	}
	error = 0;
out:
	apfs_free(block0, APFS_NX_MINIMUM_BLOCK_SIZE);
	if (error) {
		apfs_nx_close(nx);
	}
	return error;
}

static void scratch_drain(struct apfs_nx *nx);

void
apfs_nx_close(struct apfs_nx *nx)
{
	uint32_t i;

	scratch_drain(nx);
	if (nx->omap_cache != NULL) {
		apfs_free(nx->omap_cache, APFS_OMAP_CACHE * sizeof(*nx->omap_cache));
		nx->omap_cache = NULL;
	}

	if (nx->cache != NULL) {
		for (i = 0; i < APFS_CACHE_BLOCKS; i++) {
			if (nx->cache[i].data != NULL) {
				apfs_free(nx->cache[i].data, nx->block_size);
			}
		}
		apfs_free(nx->cache, APFS_CACHE_BLOCKS * sizeof(*nx->cache));
		nx->cache = NULL;
	}
	if (nx->cache_hash != NULL) {
		apfs_free(nx->cache_hash, APFS_CACHE_HASH * sizeof(*nx->cache_hash));
		nx->cache_hash = NULL;
	}
	if (nx->cluster != NULL) {
		apfs_free(nx->cluster, (size_t)APFS_META_CLUSTER * nx->block_size);
		nx->cluster = NULL;
	}
	apfs_nx_reclaim(nx, ~0ULL);
}

int
apfs_vol_open(struct apfs_nx *nx, uint32_t index, struct apfs_vol *vol)
{
	const uint8_t *blk;
	uint64_t addr, omap_oid, root_oid, fext_oid, er_oid;
	uint32_t fext_type, er_type;
	int error;

	if (index >= APFS_NX_MAX_FILE_SYSTEMS || nx->fs_oid[index] == 0) {
		return ENOENT;
	}
	memset(vol, 0, sizeof(*vol));
	vol->nx = nx;
	vol->index = index;
	vol->oid = nx->fs_oid[index];
	error = omap_find(&nx->omap, vol->oid, nx->xid, &addr);
	if (error) {
		return error;
	}
	error = nx_block(nx, addr, &blk, true);
	if (error) {
		return error;
	}
	if (obj_type(blk) != APFS_OBJECT_TYPE_FS || le32(blk + 32) != APFS_MAGIC_VOL) {
		apfs_log("volume %u: block %llu is not a volume superblock\n", index,
		    (unsigned long long)addr);
		return EIO;
	}
	vol->incompat = le64(blk + 56);
	vol->alloc_count = le64(blk + 88);
	er_type = le32(blk + 120);
	omap_oid = le64(blk + 128);
	root_oid = le64(blk + 136);
	er_oid = le64(blk + 144);
	vol->num_files = le64(blk + 184);
	vol->num_dirs = le64(blk + 192);
	vol->num_symlinks = le64(blk + 200);
	vol->num_snapshots = le64(blk + 216);
	vol->integrity_meta_oid = le64(blk + 1024);
	vol->fs_flags = le64(blk + 264);
	memcpy(vol->name, blk + 704, APFS_VOLNAME_LEN);
	vol->name[APFS_VOLNAME_LEN - 1] = '\0';
	vol->role = le16(blk + 964);
	fext_oid = le64(blk + 0x408);           /* apfs_fext_tree_oid, apfs_fext_tree_type */
	fext_type = le32(blk + 0x410);
	vol->hashed_names = (vol->incompat & (APFS_INCOMPAT_CASE_INSENSITIVE |
	    APFS_INCOMPAT_NORMALIZATION_INSENSITIVE)) != 0;

	if (vol->incompat & ~(uint64_t)(APFS_INCOMPAT_CASE_INSENSITIVE |
	    APFS_INCOMPAT_NORMALIZATION_INSENSITIVE | APFS_INCOMPAT_SEALED_VOLUME |
	    APFS_INCOMPAT_DATALESS_SNAPS)) {
		apfs_log("volume %u: incompatible features 0x%llx are not understood\n",
		    index, (unsigned long long)vol->incompat);
		return ENOTSUP;
	}
	if ((vol->fs_flags & APFS_FS_UNENCRYPTED) == 0) {
		apfs_log("volume %u (%s) is encrypted\n", index, vol->name);
		return ENOTSUP;
	}
	/* the volume's own map resolves the nodes of its file-system tree */
	error = omap_open(nx, omap_oid, &vol->omap);
	if (error) {
		return error;
	}
	error = btree_open(nx, &vol->omap, root_oid, &vol->tree);
	if (error) {
		return error;
	}
	if (fext_oid != 0) {
		if ((fext_type & APFS_OBJ_STORAGETYPE_MASK) != APFS_OBJ_PHYSICAL) {
			apfs_log("volume %u (%s): an extent tree that is not physical (0x%x)\n", index, vol->name,
			    fext_type);
			return ENOTSUP;
		}
		error = btree_open(nx, NULL, fext_oid, &vol->fext);
		if (error) {
			return error;
		}
		vol->has_fext = true;
	}
	/*
	 * The extent-reference tree is of no use to a reader, so it is not opened here: only
	 * noted, and only when it is the physical tree it should be.  A volume that mounts
	 * today goes on mounting whatever is wrong with it; not having it means no more than
	 * that apfs_write.c will refuse to write anything here.
	 */
	if (er_oid != 0 && (er_type & APFS_OBJ_STORAGETYPE_MASK) == APFS_OBJ_PHYSICAL) {
		vol->extentref_oid = er_oid;
	}
	return 0;
}

int
apfs_extentref_open(struct apfs_vol *vol, struct apfs_btree *tree)
{
	if (vol->extentref_oid == 0) {
		return ENOENT;
	}
	return btree_open(vol->nx, NULL, vol->extentref_oid, tree);
}

/* ------------------------------------------------------------------ records */

/*
 * Where a seek is going: (obj_id, type), and for a directory record also the word that
 * holds its name's hash and length (with_nlh) -- the records of one directory sort by
 * that word, then by name.
 */
struct seek_target {
	uint64_t obj_id;
	uint8_t  type;
	bool     with_nlh;
	uint32_t nlh;
};

/* Below 0: the key sorts before the target; 0 or more: at it or after. */
static int
seek_cmp(const uint8_t *k, uint16_t kl, const struct seek_target *t)
{
	uint64_t id = jkey_id(k);
	uint8_t type = jkey_type(k);

	if (id != t->obj_id) {
		return id < t->obj_id ? -1 : 1;
	}
	if (type != t->type) {
		return type < t->type ? -1 : 1;
	}
	if (t->with_nlh && kl >= 12 && le32(k + 8) < t->nlh) {
		return -1;
	}
	return 0;
}

static int
seek_to(struct apfs_btree *tree, const struct seek_target *t, struct apfs_cursor *cur)
{
	const uint8_t *blk;
	struct node n;
	uint64_t addr = tree->root;
	int error;

	memset(cur, 0, sizeof(*cur));
	cur->tree = tree;
	error = node_at(tree, addr, &blk, &n);
	if (error) {
		return error;
	}
	cur->depth = 0;
	for (;;) {
		uint32_t i, pick = 0;

		if (cur->depth >= APFS_MAX_LEVELS) {
			return EIO;
		}
		cur->node[cur->depth] = addr;
		if (n.nkeys == 0) {
			cur->done = true;
			return 0;
		}
		/* the first key at or after the target, by halving: keys sort the way seek_cmp does */
		{
			uint32_t lo = 0, hi = n.nkeys;

			while (lo < hi) {
				uint32_t mid = lo + (hi - lo) / 2;
				const uint8_t *k, *v;
				uint16_t kl, vl;

				error = node_entry(tree, &n, mid, &k, &kl, &v, &vl);
				if (error) {
					return error;
				}
				if (kl < 8) {
					return EIO;
				}
				if (seek_cmp(k, kl, t) < 0) {
					lo = mid + 1;
				} else {
					hi = mid;
				}
			}
			i = lo;
		}
		if (n.leaf) {
			cur->index[cur->depth] = i;
			return 0;
		}
		/*
		 * The last child that starts before what we want.  A child's key is the
		 * smallest key in it, so going left on a tie cannot skip a record: the walk
		 * forward will reach it.
		 */
		pick = (i > 0) ? i - 1 : 0;
		cur->index[cur->depth] = pick;
		{
			uint64_t h8 = 0;

			error = node_child_addr(tree, &n, pick, &addr, &h8);
			if (error) {
				return error;
			}
			if (cur->depth + 1 >= APFS_MAX_LEVELS) {
				return EIO;
			}
			cur->depth++;
			cur->hash8[cur->depth] = h8;
			error = node_at_h(tree, addr, h8, &blk, &n);
		}
		if (error) {
			return error;
		}
	}
}

int
apfs_seek(struct apfs_btree *tree, uint64_t obj_id, uint8_t type,
    struct apfs_cursor *cur)
{
	struct seek_target t = { .obj_id = obj_id, .type = type };

	return seek_to(tree, &t, cur);
}

/*
 * The record the cursor is on, then step past it.  The pointers in `rec` point into a
 * cached block and stay good only until the next call that touches this container.
 */
int
apfs_cursor_next(struct apfs_cursor *cur, struct apfs_rec *rec)
{
	struct apfs_btree *tree = cur->tree;
	const uint8_t *blk;
	struct node n;
	int error;

	if (cur->done) {
		return ENOENT;
	}
	for (;;) {
		error = node_at_h(tree, cur->node[cur->depth], cur->hash8[cur->depth], &blk, &n);
		if (error) {
			return error;
		}
		if (!n.leaf) {
			return EIO;
		}
		if (cur->index[cur->depth] < n.nkeys) {
			const uint8_t *k, *v;
			uint16_t kl, vl;

			error = node_entry(tree, &n, cur->index[cur->depth], &k, &kl, &v, &vl);
			if (error) {
				return error;
			}
			if (kl < 8) {
				return EIO;
			}
			cur->index[cur->depth]++;
			rec->key = k;
			rec->key_len = kl;
			rec->val = v;
			rec->val_len = vl;
			rec->obj_id = jkey_id(k);
			rec->type = jkey_type(k);
			return 0;
		}
		/* this leaf is done: step the path to the next one */
		for (;;) {
			if (cur->depth == 0) {
				cur->done = true;
				return ENOENT;
			}
			cur->depth--;
			error = node_at_h(tree, cur->node[cur->depth], cur->hash8[cur->depth], &blk, &n);
			if (error) {
				return error;
			}
			if (n.leaf) {
				return EIO;
			}
			cur->index[cur->depth]++;
			if (cur->index[cur->depth] < n.nkeys) {
				break;
			}
		}
		/* and down the left edge of the subtree we just moved into */
		for (;;) {
			uint64_t child;

			uint64_t h8 = 0;

			error = node_child_addr(tree, &n, cur->index[cur->depth], &child, &h8);
			if (error) {
				return error;
			}
			if (cur->depth + 1 >= APFS_MAX_LEVELS) {
				return EIO;
			}
			cur->depth++;
			cur->node[cur->depth] = child;
			cur->index[cur->depth] = 0;
			cur->hash8[cur->depth] = h8;
			error = node_at_h(tree, child, h8, &blk, &n);
			if (error) {
				return error;
			}
			if (n.leaf) {
				break;
			}
		}
	}
}

/* ------------------------------------------------------------------ inodes */

/*
 * Walk the extended fields at the tail of an inode record.  The blob is a count and a
 * used-byte total, then that many descriptors, then the values, each padded out to a
 * multiple of eight bytes.
 */
static int
xfields_find(const uint8_t *buf, uint16_t len, uint8_t want, const uint8_t **out,
    uint16_t *out_len)
{
	uint16_t num, i, off;

	*out = NULL;
	*out_len = 0;
	if (len < 4) {
		return ENOENT;
	}
	num = le16(buf);
	if ((uint32_t)4 + 4 * (uint32_t)num > len) {
		return EIO;
	}
	off = (uint16_t)(4 + 4 * num);
	for (i = 0; i < num; i++) {
		uint8_t xtype = buf[4 + 4 * i];
		uint16_t xsize = le16(buf + 4 + 4 * i + 2);
		uint32_t pad = ((uint32_t)xsize + 7u) & ~7u;

		if ((uint32_t)off + xsize > len) {
			return EIO;
		}
		if (xtype == want) {
			*out = buf + off;
			*out_len = xsize;
			return 0;
		}
		if ((uint32_t)off + pad > len) {
			return ENOENT;  /* the last field, padding cut short */
		}
		off = (uint16_t)(off + pad);
	}
	return ENOENT;
}

int
apfs_inode_get(struct apfs_vol *vol, uint64_t obj_id, struct apfs_inode *ino)
{
	struct apfs_cursor cur;
	struct apfs_rec rec;
	const uint8_t *xf;
	uint16_t xf_len;
	int error;

	memset(ino, 0, sizeof(*ino));
	error = apfs_seek(&vol->tree, obj_id, APFS_TYPE_INODE, &cur);
	if (error) {
		return error;
	}
	error = apfs_cursor_next(&cur, &rec);
	if (error) {
		return error;
	}
	if (rec.obj_id != obj_id || rec.type != APFS_TYPE_INODE) {
		return ENOENT;
	}
	if (rec.val == NULL || rec.val_len < 92) {
		return EIO;
	}
	ino->obj_id = obj_id;
	ino->parent_id = le64(rec.val);
	ino->private_id = le64(rec.val + 8);
	ino->create_time = le64(rec.val + 16);
	ino->mod_time = le64(rec.val + 24);
	ino->change_time = le64(rec.val + 32);
	ino->access_time = le64(rec.val + 40);
	ino->internal_flags = le64(rec.val + 48);
	ino->nchildren = (int32_t)le32(rec.val + 56);
	ino->bsd_flags = le32(rec.val + 68);
	ino->owner = le32(rec.val + 72);
	ino->group = le32(rec.val + 76);
	ino->mode = le16(rec.val + 80);
	ino->is_sparse = (ino->internal_flags & APFS_INODE_IS_SPARSE) != 0;

	/* the size of the contents is in an extended field, not in the fixed part */
	xf = rec.val + 92;
	xf_len = (uint16_t)(rec.val_len - 92);
	if (xfields_find(xf, xf_len, APFS_INO_EXT_TYPE_DSTREAM, &xf, &xf_len) == 0 &&
	    xf_len >= 16) {
		ino->size = le64(xf);
		ino->alloced_size = le64(xf + 8);
		ino->has_dstream = true;
	}
	xf = rec.val + 92;
	xf_len = (uint16_t)(rec.val_len - 92);
	if (xfields_find(xf, xf_len, APFS_INO_EXT_TYPE_RDEV, &xf, &xf_len) == 0 &&
	    xf_len >= 4) {
		ino->rdev = le32(xf);
	}
	return 0;
}

int
apfs_inode_size(struct apfs_vol *vol, const struct apfs_inode *ino, uint64_t *size)
{
	bool compressed = false, in_rsrc = false;
	uint32_t type = 0;
	uint64_t csize = 0;
	int error;

	if ((ino->mode & APFS_S_IFMT) == APFS_S_IFLNK) {
		char target[1024];
		size_t n = 0;

		error = apfs_readlink(vol, ino->obj_id, target, sizeof(target), &n);
		if (error) {
			return error;
		}
		*size = n;
		return 0;
	}
	/* only the decmpfs header is needed here, not the table of pieces */
	error = cfile_header(vol, ino, &compressed, &type, &csize, &in_rsrc);
	if (error) {
		return error;
	}
	*size = compressed ? csize : ino->size;
	return 0;
}

/* ------------------------------------------------------------------ directories */

int
apfs_readdir(struct apfs_vol *vol, uint64_t dir_id, apfs_dir_cb cb, void *ctx)
{
	struct apfs_cursor cur;
	struct apfs_rec rec;
	int error;

	error = apfs_seek(&vol->tree, dir_id, APFS_TYPE_DIR_REC, &cur);
	if (error) {
		return error;
	}
	for (;;) {
		char name[256];
		uint32_t name_len;
		uint64_t file_id;
		uint8_t d_type;

		error = apfs_cursor_next(&cur, &rec);
		if (error == ENOENT) {
			return 0;
		}
		if (error) {
			return error;
		}
		if (rec.obj_id != dir_id || rec.type != APFS_TYPE_DIR_REC) {
			return 0;
		}
		/*
		 * iOS uses the hashed key: a word holding the name's length and a hash of it,
		 * then the name.  The plain key has a 16-bit length instead; both are handled
		 * so that a volume made elsewhere still reads.
		 */
		if (vol->hashed_names) {
			if (rec.key_len < 12) {
				return EIO;
			}
			name_len = le32(rec.key + 8) & APFS_J_DREC_LEN_MASK;
			if (name_len == 0 || 12 + name_len > rec.key_len) {
				return EIO;
			}
			if (name_len > sizeof(name)) {
				return EIO;
			}
			memcpy(name, rec.key + 12, name_len);
		} else {
			if (rec.key_len < 10) {
				return EIO;
			}
			name_len = le16(rec.key + 8);
			if (name_len == 0 || 10 + name_len > rec.key_len) {
				return EIO;
			}
			if (name_len > sizeof(name)) {
				return EIO;
			}
			memcpy(name, rec.key + 10, name_len);
		}
		name_len--;                     /* the stored length counts the zero byte */
		name[name_len] = '\0';
		if (rec.val == NULL || rec.val_len < 18) {
			return EIO;
		}
		file_id = le64(rec.val);
		d_type = (uint8_t)(le16(rec.val + 16) & APFS_DREC_TYPE_MASK);
		error = cb(ctx, name, name_len, file_id, d_type);
		if (error) {
			return error;
		}
	}
}

struct lookup_ctx {
	const char *name;
	size_t      name_len;
	bool        fold;
	uint64_t    file_id;
	uint8_t     d_type;
	bool        found;
};

static int
ascii_lower(int c)
{
	return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static bool names_equal(const char *a, size_t a_len, const char *b, size_t b_len, bool fold);

static int
lookup_cb(void *ctx, const char *name, size_t name_len, uint64_t file_id, uint8_t d_type)
{
	struct lookup_ctx *lc = ctx;

	if (!names_equal(name, name_len, lc->name, lc->name_len, lc->fold)) {
		return 0;
	}
	lc->file_id = file_id;
	lc->d_type = d_type;
	lc->found = true;
	return EEXIST;                          /* stop the walk; not an error */
}

/*
 * The hash in a directory record's key: CRC-32C over the name as little-endian 32-bit code
 * points, started at all ones and not inverted at the end, low 22 bits -- the name taken after
 * Unicode canonical decomposition (NFD), and on a case-insensitive volume in its canonical
 * caseless form (case-folded NFD), Unicode 9.0 as APFS has it. For the decomposition and folding
 * the kernel's own (bsd/vfs/vfs_unicode.c, utf8_normalizeOptCaseFoldAndHash, which is APFS's);
 * a name of plain ASCII needs neither and is hashed directly. Checked against the hash stored
 * with every name of the iOS 16.5 root volume, ASCII and not (tools/apfs/test_namehash.py,
 * apfs-tool namehash).
 */
static const uint32_t crc32c_nibble[16] = {
	0x00000000, 0x105ec76f, 0x20bd8ede, 0x30e349b1,
	0x417b1dbc, 0x5125dad3, 0x61c69362, 0x7198540d,
	0x82f63b78, 0x92a8fc17, 0xa24bb5a6, 0xb21572c9,
	0xc38d26c4, 0xd3d3e1ab, 0xe330a81a, 0xf36e6f75,
};

static inline uint32_t
crc32c_code_point(uint32_t crc, uint32_t cp)
{
	uint8_t b[4] = { (uint8_t)cp, (uint8_t)(cp >> 8), (uint8_t)(cp >> 16), (uint8_t)(cp >> 24) };
	int j;

	for (j = 0; j < 4; j++) {
		crc ^= b[j];
		crc = (crc >> 4) ^ crc32c_nibble[crc & 15];
		crc = (crc >> 4) ^ crc32c_nibble[crc & 15];
	}
	return crc;
}

static bool
name_is_ascii(const char *name, size_t name_len)
{
	size_t i;

	for (i = 0; i < name_len; i++) {
		if ((uint8_t)name[i] == 0 || (uint8_t)name[i] >= 0x80) {
			return false;
		}
	}
	return true;
}

#if APFS_HAVE_UNICODE
static void
name_hash_feed(void *buf, size_t buf_len, void *ctx)
{
	const int32_t *u = buf;
	uint32_t *crc = ctx;
	size_t i;

	for (i = 0; i < buf_len / sizeof(int32_t); i++) {
		*crc = crc32c_code_point(*crc, (uint32_t)u[i]);
	}
}
#endif

bool
apfs_name_hash(const char *name, size_t name_len, bool fold, uint32_t *hash)
{
	uint32_t crc = 0xffffffffu;
	size_t i;

	if (name_len == 0) {
		return false;
	}
	if (name_is_ascii(name, name_len)) {
		for (i = 0; i < name_len; i++) {
			uint32_t c = (uint8_t)name[i];

			if (fold && c >= 'A' && c <= 'Z') {
				c += 'a' - 'A';         /* the caseless form of an ASCII letter */
			}
			crc = crc32c_code_point(crc, c);
		}
		*hash = crc & 0x3fffff;
		return true;
	}
#if APFS_HAVE_UNICODE
	if (utf8_normalizeOptCaseFoldAndHash(name, name_len, !fold, name_hash_feed, &crc) != 0) {
		return false;                   /* not well-formed UTF-8 (or not stream safe) */
	}
	*hash = crc & 0x3fffff;
	return true;
#else
	return false;
#endif
}

/*
 * Two names the same, as the volume sees names: byte for byte when both are plain ASCII (folding
 * the letters on a case-insensitive volume), else the same after decomposition (and folding).
 */
static bool
names_equal(const char *a, size_t a_len, const char *b, size_t b_len, bool fold)
{
	if (name_is_ascii(a, a_len) && name_is_ascii(b, b_len)) {
		size_t i;

		if (a_len != b_len) {
			return false;
		}
		if (!fold) {
			return memcmp(a, b, a_len) == 0;
		}
		for (i = 0; i < a_len; i++) {
			if (ascii_lower((unsigned char)a[i]) != ascii_lower((unsigned char)b[i])) {
				return false;
			}
		}
		return true;
	}
#if APFS_HAVE_UNICODE
	{
		bool equal = false;

		if (utf8_normalizeOptCaseFoldAndCompare(a, a_len, b, b_len, !fold, &equal) == 0) {
			return equal;
		}
	}
#endif
	return a_len == b_len && memcmp(a, b, a_len) == 0;
}

/*
 * A name in a directory by its hash: straight to the records with that hash and compare
 * only those, instead of reading the directory from its start. Gives ENOENT for a name that
 * is not there, EAGAIN when this way does not apply (not hashed keys, a name that cannot be
 * hashed) and the caller goes through the directory instead.
 */
static int
lookup_hashed(struct apfs_vol *vol, uint64_t dir_id, const char *name, size_t name_len,
    uint64_t *file_id, uint8_t *d_type)
{
	struct seek_target t = { .obj_id = dir_id, .type = APFS_TYPE_DIR_REC,
		                 .with_nlh = true };
	bool fold = (vol->incompat & APFS_INCOMPAT_CASE_INSENSITIVE) != 0;
	struct apfs_cursor cur;
	struct apfs_rec rec;
	uint32_t hash;
	int error;

	if (!vol->hashed_names || name_len == 0 || name_len >= APFS_J_DREC_LEN_MASK ||
	    !apfs_name_hash(name, name_len, fold, &hash)) {
		return EAGAIN;
	}
	t.nlh = hash << APFS_J_DREC_HASH_SHIFT;
	error = seek_to(&vol->tree, &t, &cur);
	if (error) {
		return error;
	}
	for (;;) {
		uint32_t nlh, len;

		error = apfs_cursor_next(&cur, &rec);
		if (error) {
			return error;   /* ENOENT at the end of the tree */
		}
		if (rec.obj_id != dir_id || rec.type != APFS_TYPE_DIR_REC || rec.key_len < 12) {
			return ENOENT;
		}
		nlh = le32(rec.key + 8);
		if ((nlh >> APFS_J_DREC_HASH_SHIFT) != hash) {
			return ENOENT;  /* past the records with this hash */
		}
		len = nlh & APFS_J_DREC_LEN_MASK;
		if (len >= 1 && 12 + len <= rec.key_len &&
		    names_equal((const char *)rec.key + 12, len - 1, name, name_len, fold)) {
			if (rec.val == NULL || rec.val_len < 18) {
				return EIO;
			}
			if (file_id != NULL) {
				*file_id = le64(rec.val);
			}
			if (d_type != NULL) {
				*d_type = (uint8_t)(le16(rec.val + 16) & APFS_DREC_TYPE_MASK);
			}
			return 0;
		}
	}
}

#ifndef KERNEL
int
apfs_dirrec_walk(struct apfs_vol *vol, int (*cb)(void *ctx, uint64_t dir_id, const char *name,
    size_t name_len, uint32_t stored_hash, uint64_t file_id), void *ctx)
{
	struct apfs_cursor cur;
	struct apfs_rec rec;
	int error;

	if (!vol->hashed_names) {
		return ENOTSUP;
	}
	error = apfs_seek(&vol->tree, 0, 0, &cur);
	if (error) {
		return error;
	}
	while ((error = apfs_cursor_next(&cur, &rec)) == 0) {
		uint32_t nlh, len;

		if (rec.type != APFS_TYPE_DIR_REC || rec.key_len < 12) {
			continue;
		}
		nlh = le32(rec.key + 8);
		len = nlh & APFS_J_DREC_LEN_MASK;
		if (len < 1 || 12 + len > rec.key_len) {
			return EIO;
		}
		if (rec.val == NULL || rec.val_len < 18) {
			return EIO;
		}
		error = cb(ctx, rec.obj_id, (const char *)rec.key + 12, len - 1, nlh >> APFS_J_DREC_HASH_SHIFT,
		    le64(rec.val));
		if (error) {
			return error;
		}
	}
	return error == ENOENT ? 0 : error;
}
#endif

int
apfs_lookup(struct apfs_vol *vol, uint64_t dir_id, const char *name, size_t name_len,
    uint64_t *file_id, uint8_t *d_type)
{
	struct lookup_ctx lc;
	int error;

	error = lookup_hashed(vol, dir_id, name, name_len, file_id, d_type);
	if (error != EAGAIN) {
		vol->nx->lookups_hashed++;
		return error;
	}
	vol->nx->lookups_scanned++;
	memset(&lc, 0, sizeof(lc));
	lc.name = name;
	lc.name_len = name_len;
	lc.fold = (vol->incompat & APFS_INCOMPAT_CASE_INSENSITIVE) != 0;
	error = apfs_readdir(vol, dir_id, lookup_cb, &lc);
	if (error && error != EEXIST) {
		return error;
	}
	if (!lc.found) {
		return ENOENT;
	}
	if (file_id != NULL) {
		*file_id = lc.file_id;
	}
	if (d_type != NULL) {
		*d_type = lc.d_type;
	}
	return 0;
}

int
apfs_path_lookup(struct apfs_vol *vol, const char *path, uint64_t *file_id,
    uint8_t *d_type)
{
	uint64_t id = APFS_ROOT_DIR_INO_NUM;
	uint8_t dt = APFS_DT_DIR;
	const char *p = path;

	while (*p != '\0') {
		const char *s;
		size_t n;
		int error;

		while (*p == '/') {
			p++;
		}
		if (*p == '\0') {
			break;
		}
		s = p;
		while (*p != '\0' && *p != '/') {
			p++;
		}
		n = (size_t)(p - s);
		if (n == 1 && s[0] == '.') {
			continue;
		}
		if (dt != APFS_DT_DIR) {
			return ENOTDIR;
		}
		error = apfs_lookup(vol, id, s, n, &id, &dt);
		if (error) {
			return error;
		}
	}
	if (file_id != NULL) {
		*file_id = id;
	}
	if (d_type != NULL) {
		*d_type = dt;
	}
	return 0;
}

/* ------------------------------------------------------------------ attributes */

static int
xattr_parse(const struct apfs_rec *rec, struct apfs_xattr *xa)
{
	uint16_t xdata_len;

	if (rec->val == NULL || rec->val_len < 4) {
		return EIO;
	}
	memset(xa, 0, sizeof(*xa));
	xa->flags = le16(rec->val);
	xdata_len = le16(rec->val + 2);
	if ((uint32_t)4 + xdata_len > rec->val_len) {
		return EIO;
	}
	xa->data = rec->val + 4;
	xa->data_len = xdata_len;
	if (xa->flags & APFS_XATTR_DATA_STREAM) {
		if (xdata_len < 16) {
			return EIO;
		}
		xa->stream_id = le64(xa->data);
		xa->stream_size = le64(xa->data + 8);
	}
	return 0;
}

int
apfs_xattr_list(struct apfs_vol *vol, uint64_t obj_id, apfs_xattr_cb cb, void *ctx)
{
	struct apfs_cursor cur;
	struct apfs_rec rec;
	int error;

	error = apfs_seek(&vol->tree, obj_id, APFS_TYPE_XATTR, &cur);
	if (error) {
		return error;
	}
	for (;;) {
		char name[256];
		uint32_t name_len;
		struct apfs_xattr xa;

		error = apfs_cursor_next(&cur, &rec);
		if (error == ENOENT) {
			return 0;
		}
		if (error) {
			return error;
		}
		if (rec.obj_id != obj_id || rec.type != APFS_TYPE_XATTR) {
			return 0;
		}
		if (rec.key_len < 10) {
			return EIO;
		}
		name_len = le16(rec.key + 8);
		if (name_len == 0 || name_len > sizeof(name) || 10 + name_len > rec.key_len) {
			return EIO;
		}
		memcpy(name, rec.key + 10, name_len);
		name_len--;
		name[name_len] = '\0';
		error = xattr_parse(&rec, &xa);
		if (error) {
			return error;
		}
		error = cb(ctx, name, name_len, &xa);
		if (error) {
			return error;
		}
	}
}

struct xattr_find_ctx {
	const char *want;
	size_t      want_len;
	uint8_t    *buf;                /* NULL when only the metadata is wanted */
	size_t      len;
	size_t      copied;
	uint16_t    flags;
	uint16_t    data_len;
	uint64_t    stream_id;
	uint64_t    stream_size;
	bool        found;
};

static int
xattr_find_cb(void *ctx, const char *name, size_t name_len, const struct apfs_xattr *xa)
{
	struct xattr_find_ctx *fc = ctx;

	if (name_len != fc->want_len || memcmp(name, fc->want, name_len) != 0) {
		return 0;
	}
	fc->found = true;
	fc->flags = xa->flags;
	fc->data_len = xa->data_len;
	fc->stream_id = xa->stream_id;
	fc->stream_size = xa->stream_size;
	/* an embedded value is copied now, while the record is still in a cached block */
	if ((xa->flags & APFS_XATTR_DATA_EMBEDDED) && fc->buf != NULL) {
		size_t n = xa->data_len < fc->len ? xa->data_len : fc->len;

		memcpy(fc->buf, xa->data, n);
		fc->copied = n;
	}
	return EEXIST;                  /* stop the walk; not a failure */
}

static int
xattr_find(struct apfs_vol *vol, uint64_t obj_id, const char *name,
    struct xattr_find_ctx *fc)
{
	int error;

	fc->want = name;
	fc->want_len = strlen(name);
	error = apfs_xattr_list(vol, obj_id, xattr_find_cb, fc);
	if (error == EEXIST) {
		error = 0;
	}
	if (error == 0 && !fc->found) {
		error = ENOENT;
	}
	return error;
}

int
apfs_xattr_lookup(struct apfs_vol *vol, uint64_t obj_id, const char *name,
    struct apfs_xattr *xa)
{
	struct xattr_find_ctx fc;
	int error;

	memset(&fc, 0, sizeof(fc));
	error = xattr_find(vol, obj_id, name, &fc);
	if (error) {
		return error;
	}
	memset(xa, 0, sizeof(*xa));
	xa->flags = fc.flags;
	xa->data = NULL;
	xa->data_len = fc.data_len;
	xa->stream_id = fc.stream_id;
	xa->stream_size = fc.stream_size;
	return 0;
}

int
apfs_xattr_read(struct apfs_vol *vol, uint64_t obj_id, const char *name, void *buf,
    size_t len, size_t *out)
{
	struct xattr_find_ctx fc;
	int error;

	*out = 0;
	memset(&fc, 0, sizeof(fc));
	fc.buf = buf;
	fc.len = len;
	error = xattr_find(vol, obj_id, name, &fc);
	if (error) {
		return error;
	}
	if (fc.flags & APFS_XATTR_DATA_EMBEDDED) {
		*out = fc.copied;
		return 0;
	}
	if (fc.flags & APFS_XATTR_DATA_STREAM) {
		return apfs_read_stream(vol, fc.stream_id, fc.stream_size, 0, buf, len, out);
	}
	return EIO;
}

/* ------------------------------------------------------------------ contents */

/*
 * The extents of one stream, in order: the file-system tree's records of it, or on a sealed
 * volume its entries in the extent tree (key: stream id and logical offset; value: length
 * with flags, and the first block).  ext_begin(..., off, ...) starts at the extent that may
 * reach `off`, or earlier.
 */
struct ext_iter {
	struct apfs_cursor cur;
	uint64_t           id;
	bool               fext;
};

static int
fext_seek(struct apfs_btree *tree, uint64_t id, uint64_t off, struct apfs_cursor *cur)
{
	const uint8_t *blk;
	struct node n;
	uint64_t addr = tree->root;
	int error;

	memset(cur, 0, sizeof(*cur));
	cur->tree = tree;
	error = node_at(tree, addr, &blk, &n);
	if (error) {
		return error;
	}
	for (;;) {
		uint32_t lo = 0, hi = n.nkeys, pick;

		if (cur->depth >= APFS_MAX_LEVELS) {
			return EIO;
		}
		cur->node[cur->depth] = addr;
		if (n.nkeys == 0) {
			cur->done = true;
			return 0;
		}
		/* the last entry at or before (id, off), by halving */
		while (lo < hi) {
			uint32_t mid = lo + (hi - lo) / 2;
			const uint8_t *k, *v;
			uint16_t kl, vl;

			error = node_entry(tree, &n, mid, &k, &kl, &v, &vl);
			if (error) {
				return error;
			}
			if (kl < 16) {
				return EIO;
			}
			if (le64(k) < id || (le64(k) == id && le64(k + 8) <= off)) {
				lo = mid + 1;
			} else {
				hi = mid;
			}
		}
		pick = lo > 0 ? lo - 1 : 0;
		cur->index[cur->depth] = pick;
		if (n.leaf) {
			return 0;
		}
		error = node_child_addr(tree, &n, pick, &addr, NULL);
		if (error) {
			return error;
		}
		if (cur->depth + 1 >= APFS_MAX_LEVELS) {
			return EIO;
		}
		cur->depth++;
		error = node_at(tree, addr, &blk, &n);
		if (error) {
			return error;
		}
	}
}

static int
ext_begin(struct apfs_vol *vol, uint64_t id, uint64_t off, struct ext_iter *it)
{
	it->id = id;
	it->fext = vol->has_fext;
	if (it->fext) {
		return fext_seek(&vol->fext, id, off, &it->cur);
	}
	return apfs_seek(&vol->tree, id, APFS_TYPE_FILE_EXTENT, &it->cur);
}

/* The next extent; ENOENT after the last. */
static int
ext_next(struct ext_iter *it, uint64_t *logical, uint64_t *length, uint64_t *phys)
{
	struct apfs_rec rec;
	int error;

	for (;;) {
		error = apfs_cursor_next(&it->cur, &rec);
		if (error) {
			return error;
		}
		if (it->fext) {
			uint64_t id;

			if (rec.key_len < 16 || rec.val == NULL || rec.val_len < 16) {
				return EIO;
			}
			id = le64(rec.key);
			if (id < it->id) {
				continue;       /* (the seek may land on the stream before) */
			}
			if (id > it->id) {
				return ENOENT;
			}
		} else {
			if (rec.obj_id != it->id || rec.type != APFS_TYPE_FILE_EXTENT) {
				return ENOENT;
			}
			if (rec.key_len < 16 || rec.val == NULL || rec.val_len < 24) {
				return EIO;
			}
		}
		*logical = le64(rec.key + 8);
		*length = le64(rec.val) & APFS_J_FILE_EXTENT_LEN_MASK;
		*phys = le64(rec.val + 8);
		return 0;
	}
}

int
apfs_extent_at(struct apfs_vol *vol, uint64_t stream_id, uint64_t off,
    struct apfs_extent *ext)
{
	struct ext_iter it;
	uint64_t logical, length, phys;
	int error;

	error = ext_begin(vol, stream_id, off, &it);
	if (error) {
		return error;
	}
	/*
	 * The seek lands at or before the extent wanted (a tree keyed by offset cannot be
	 * asked for "the one containing"), so step forward until one covers `off`.
	 */
	for (;;) {
		error = ext_next(&it, &logical, &length, &phys);
		if (error) {
			return error;
		}
		if (length == 0 || (length % vol->nx->block_size) != 0 ||
		    (logical % vol->nx->block_size) != 0) {
			return EIO;     /* an extent of APFS begins and ends on a block */
		}
		if (off < logical) {
			return ENOENT;          /* a gap in the records: not a hole, damage */
		}
		if (off < logical + length) {
			ext->logical = logical;
			ext->length = length;
			ext->phys = phys;
			return 0;
		}
	}
}

int
apfs_file_in_place(struct apfs_vol *vol, const struct apfs_inode *ino, uint64_t *addr)
{
	bool compressed = false, in_rsrc = false;
	uint32_t type = 0;
	uint64_t csize = 0, logical, length, phys;
	struct ext_iter it;
	int error;

	if ((ino->mode & APFS_S_IFMT) != APFS_S_IFREG || !ino->has_dstream || ino->size == 0) {
		return ENOENT;
	}
	error = cfile_header(vol, ino, &compressed, &type, &csize, &in_rsrc);
	if (error) {
		return error;
	}
	if (compressed) {
		return ENOENT;
	}
	error = ext_begin(vol, ino->private_id, 0, &it);
	if (error == 0) {
		error = ext_next(&it, &logical, &length, &phys);
	}
	if (error) {
		return error == ENOENT ? ENOENT : error;
	}
	if (logical != 0 || length < ino->size || phys == 0 || phys >= vol->nx->block_count ||
	    (length / vol->nx->block_size) > vol->nx->block_count - phys) {
		return ENOENT;
	}
	*addr = phys;
	return 0;
}

int
apfs_read_stream(struct apfs_vol *vol, uint64_t stream_id, uint64_t stream_size,
    uint64_t off, void *buf, size_t len, size_t *out)
{
	struct ext_iter it;
	uint8_t *dst = buf;
	uint32_t bs = vol->nx->block_size;
	uint64_t end;
	int error;

	*out = 0;
	if (off >= stream_size) {
		return 0;
	}
	if (len > stream_size - off) {
		len = (size_t)(stream_size - off);
	}
	end = off + len;
	/* holes read as zeros, so start from zeros and fill in what the extents cover */
	memset(dst, 0, len);

	error = ext_begin(vol, stream_id, off, &it);
	if (error) {
		return error;
	}
	for (;;) {
		uint64_t logical, length, phys;

		error = ext_next(&it, &logical, &length, &phys);
		if (error == ENOENT) {
			break;
		}
		if (error) {
			return error;
		}
		if (length == 0 || (length % bs) != 0) {
			return EIO;
		}
		if (logical >= end) {
			break;                  /* extents are in order: past what we need */
		}
		if (logical + length <= off || phys == 0) {
			continue;               /* before what we need, or a hole */
		}
		if (phys >= vol->nx->block_count ||
		    length / bs > vol->nx->block_count - phys) {
			return EIO;
		}
		if (phys + length / bs - 1 > vol->nx->max_data_addr) {
			vol->nx->max_data_addr = phys + length / bs - 1;
		}
		/* the overlap of [logical, logical+length) with [off, end) */
		{
			uint64_t from = logical > off ? logical : off;
			uint64_t to = (logical + length) < end ? (logical + length) : end;
			uint64_t inner = from - logical;
			uint64_t nbytes = to - from;
			uint64_t addr = phys + inner / bs;
			uint64_t skew = inner % bs;

			while (nbytes > 0) {
				const uint8_t *blk;
				uint64_t n = bs - skew;

				if (n > nbytes) {
					n = nbytes;
				}
				/* file data has no object header and so no checksum */
				error = nx_block(vol->nx, addr, &blk, false);
				if (error) {
					return error;
				}
				memcpy(dst + (from - off), blk + skew, (size_t)n);
				from += n;
				nbytes -= n;
				addr++;
				skew = 0;
			}
		}
	}
	*out = len;
	return 0;
}

/*
 * File contents without the lock.  apfs_read_stream above fetches a file's data a block
 * at a time through the block cache, with the caller's lock held all the while: one
 * request to the device per 4096 bytes, each block copied in and copied out again, and
 * the cache filled with data that pushes the B-tree nodes out of it.  Reading a stream is
 * split in two here instead.  stream_plan(), with the lock, turns the extent records into
 * runs of blocks on the device; stream_fetch(), without it, reads those runs with
 * dev.read_data -- one request a run, whole blocks straight into the destination.
 */
struct apfs_run {
	uint64_t at;                    /* bytes into the request */
	uint64_t addr;                  /* the run's first block on the device */
	uint32_t skew;                  /* bytes into that block where the run starts */
	uint64_t len;                   /* bytes */
};

#define APFS_MAX_RUNS 8

/*
 * Where [off, off + len) of a stream lies, as at most `max` runs in order (len already cut
 * to the stream's size).  What the runs leave out of [0, *covered) is holes, read as
 * zeros.  *covered is less than len when there were more than `max` runs: the caller plans
 * again from there.  With the lock held.
 */
static int
stream_plan(struct apfs_vol *vol, uint64_t stream_id, uint64_t off, uint64_t len,
    struct apfs_run *runs, uint32_t max, uint32_t *nruns, uint64_t *covered)
{
	struct ext_iter it;
	uint32_t bs = vol->nx->block_size;
	uint64_t end = off + len;
	int error;

	*nruns = 0;
	*covered = len;
	error = ext_begin(vol, stream_id, off, &it);
	if (error) {
		return error;
	}
	for (;;) {
		uint64_t logical, length, phys, from, to, inner;

		error = ext_next(&it, &logical, &length, &phys);
		if (error == ENOENT) {
			break;
		}
		if (error) {
			return error;
		}
		if (length == 0 || (length % bs) != 0) {
			return EIO;
		}
		if (logical >= end) {
			break;                  /* extents are in order: past what we need */
		}
		if (logical + length <= off || phys == 0) {
			continue;               /* before what we need, or a hole */
		}
		if (phys >= vol->nx->block_count ||
		    length / bs > vol->nx->block_count - phys) {
			return EIO;
		}
		if (phys + length / bs - 1 > vol->nx->max_data_addr) {
			vol->nx->max_data_addr = phys + length / bs - 1;
		}
		/* the overlap of [logical, logical+length) with [off, end) */
		from = logical > off ? logical : off;
		to = (logical + length) < end ? (logical + length) : end;
		inner = from - logical;
		if (*nruns == max) {
			*covered = from - off;
			break;
		}
		runs[*nruns].at = from - off;
		runs[*nruns].addr = phys + inner / bs;
		runs[*nruns].skew = (uint32_t)(inner % bs);
		runs[*nruns].len = to - from;
		(*nruns)++;
	}
	return 0;
}

/* One read of file contents; without the lock. */
static int
dev_read_data(struct apfs_nx *nx, uint64_t addr, void *buf, size_t len, uint64_t *calls,
    uint64_t *bytes)
{
	int error = (nx->dev.read_data != NULL)
	    ? nx->dev.read_data(nx->dev.ctx, addr * (uint64_t)nx->block_size, buf, len)
	    : nx->dev.read(nx->dev.ctx, addr * (uint64_t)nx->block_size, buf, len);

	(*calls)++;
	*bytes += len;
	return error;
}

/*
 * Read what stream_plan found into dst, without the lock: whole blocks straight in, the
 * ends of a run that do not fill a block through `bounce` (one block), zeros for holes.
 */
static int
stream_fetch(struct apfs_nx *nx, const struct apfs_run *runs, uint32_t nruns,
    uint64_t covered, uint8_t *dst, uint8_t *bounce, uint64_t *calls, uint64_t *bytes)
{
	uint32_t bs = nx->block_size;
	uint64_t done = 0;
	uint32_t r;
	int error;

	for (r = 0; r < nruns; r++) {
		uint64_t at = runs[r].at, addr = runs[r].addr, left = runs[r].len;
		uint32_t skew = runs[r].skew;

		if (at > done) {
			memset(dst + done, 0, (size_t)(at - done));
		}
		if (skew != 0 || left < bs) {   /* starts inside a block, or is less than one */
			uint64_t n = bs - skew < left ? bs - skew : left;

			error = dev_read_data(nx, addr, bounce, bs, calls, bytes);
			if (error) {
				return error;
			}
			memcpy(dst + at, bounce + skew, (size_t)n);
			at += n;
			left -= n;
			addr++;
		}
		if (left >= bs) {
			uint64_t n = left - left % bs;

			error = dev_read_data(nx, addr, dst + at, (size_t)n, calls, bytes);
			if (error) {
				return error;
			}
			at += n;
			left -= n;
			addr += n / bs;
		}
		if (left > 0) {                 /* the start of one more block */
			error = dev_read_data(nx, addr, bounce, bs, calls, bytes);
			if (error) {
				return error;
			}
			memcpy(dst + at, bounce, (size_t)left);
			at += left;
		}
		done = at;
	}
	if (covered > done) {
		memset(dst + done, 0, (size_t)(covered - done));
	}
	return 0;
}

int
apfs_readlink(struct apfs_vol *vol, uint64_t obj_id, char *buf, size_t len, size_t *out)
{
	size_t n = 0;
	int error = apfs_xattr_read(vol, obj_id, APFS_SYMLINK_EA_NAME, buf, len, &n);

	if (error) {
		return error;
	}
	/* the stored target ends with a zero byte that is not part of the name */
	while (n > 0 && buf[n - 1] == '\0') {
		n--;
	}
	*out = n;
	return 0;
}

/* Just the decmpfs header: what a caller needs to report a size, and nothing more. */
static int
cfile_header(struct apfs_vol *vol, const struct apfs_inode *ino, bool *compressed,
    uint32_t *type, uint64_t *size, bool *in_rsrc)
{
	uint8_t hdr[16];
	size_t n = 0;
	int error;

	*compressed = false;
	*type = 0;
	*size = 0;
	*in_rsrc = false;
	error = apfs_xattr_read(vol, ino->obj_id, APFS_DECMPFS_XATTR_NAME, hdr, sizeof(hdr),
	    &n);
	if (error == ENOENT) {
		return 0;               /* not compressed */
	}
	if (error) {
		return error;
	}
	if (n < sizeof(hdr) || le32(hdr) != APFS_DECMPFS_MAGIC) {
		return 0;
	}
	*compressed = true;
	*type = le32(hdr + 4);
	*size = le64(hdr + 8);
	switch (*type) {
	case APFS_DECMPFS_TYPE_ZLIB_ATTR:
	case APFS_DECMPFS_TYPE_RAW_ATTR:
	case APFS_DECMPFS_TYPE_LZBITMAP_ATTR:
		*in_rsrc = false;
		return 0;
	case APFS_DECMPFS_TYPE_ZLIB_RSRC:
	case APFS_DECMPFS_TYPE_RAW_RSRC:
	case APFS_DECMPFS_TYPE_LZBITMAP_RSRC:
		*in_rsrc = true;
		return 0;
	default:
		apfs_log("inode %llu: decmpfs type %u is not supported\n",
		    (unsigned long long)ino->obj_id, *type);
		return ENOTSUP;
	}
}

void
apfs_cfile_fini(struct apfs_cfile *cf)
{
	if (cf == NULL) {
		return;
	}
	if (cf->piece_off != NULL) {
		apfs_free(cf->piece_off, (size_t)cf->piece_count * sizeof(uint32_t));
	}
	if (cf->piece_len != NULL) {
		apfs_free(cf->piece_len, (size_t)cf->piece_count * sizeof(uint32_t));
	}
	if (cf->held_data != NULL) {
		apfs_free(cf->held_data, cf->held_cap);
	}
	if (cf->ra_data != NULL) {
		apfs_free(cf->ra_data, (size_t)APFS_RA_PIECES * APFS_DECMPFS_BLOCK_SIZE);
	}
	memset(cf, 0, sizeof(*cf));
	cf->held = APFS_NO_PIECE;
}

/*
 * Read the resource fork's table of pieces, once.  The fork is an HFS-Plus resource fork:
 * sixteen big-endian bytes giving the offset and length of the resource data and of the
 * resource map.  At that data offset sits a four-byte big-endian length, and the table
 * starts right after it -- a little-endian count, then an (offset, length) pair per piece,
 * the offsets measured from the start of the table.  The table is not at a fixed place:
 * the data offset is 256 in every file here, so the table lands at 260, but it has to be
 * read rather than assumed.
 */
/*
 * The table of an LZBITMAP resource fork (type 14): no resource-fork header, but straight
 * from its start n+1 little-endian offsets, piece i lying between the i-th and the next.
 */
static int
cfile_read_offsets(struct apfs_vol *vol, const struct apfs_inode *ino, struct apfs_cfile *cf)
{
	uint32_t count = (uint32_t)((cf->size + APFS_DECMPFS_BLOCK_SIZE - 1) / APFS_DECMPFS_BLOCK_SIZE);
	size_t tbl_len = ((size_t)count + 1) * 4, got = 0;
	uint8_t *tbl;
	uint32_t i;
	int error;

	if (count == 0 || tbl_len > cf->rsrc_size) {
		apfs_log("inode %llu: an LZBITMAP resource fork of %llu bytes for %llu bytes of file\n",
		    (unsigned long long)ino->obj_id, (unsigned long long)cf->rsrc_size,
		    (unsigned long long)cf->size);
		return EIO;
	}
	tbl = apfs_alloc(tbl_len);
	cf->piece_off = apfs_alloc((size_t)count * sizeof(uint32_t));
	cf->piece_len = apfs_alloc((size_t)count * sizeof(uint32_t));
	if (tbl == NULL || cf->piece_off == NULL || cf->piece_len == NULL) {
		error = ENOMEM;
		goto out;
	}
	cf->piece_count = count;
	cf->table_base = 0;
	error = apfs_read_stream(vol, cf->rsrc_id, cf->rsrc_size, 0, tbl, tbl_len, &got);
	if (error == 0 && got < tbl_len) {
		error = EIO;
	}
	for (i = 0; error == 0 && i < count; i++) {
		uint32_t a = le32(tbl + 4 * i), b = le32(tbl + 4 * (i + 1));

		if (b <= a || b > cf->rsrc_size) {
			apfs_log("inode %llu: LZBITMAP piece %u from %u to %u of a %llu-byte resource fork\n",
			    (unsigned long long)ino->obj_id, i, a, b, (unsigned long long)cf->rsrc_size);
			error = EIO;
			break;
		}
		cf->piece_off[i] = a;
		cf->piece_len[i] = b - a;
	}
out:
	if (tbl != NULL) {
		apfs_free(tbl, tbl_len);
	}
	return error;
}

static int
cfile_read_table(struct apfs_vol *vol, const struct apfs_inode *ino,
    struct apfs_cfile *cf)
{
	struct apfs_xattr xa;
	uint8_t head[16];
	uint8_t *tbl = NULL;
	size_t got = 0, tbl_len;
	uint32_t i;
	int error;

	error = apfs_xattr_lookup(vol, ino->obj_id, APFS_RSRC_FORK_XATTR_NAME, &xa);
	if (error) {
		return error;
	}
	if ((xa.flags & APFS_XATTR_DATA_STREAM) == 0) {
		return ENOTSUP;         /* a resource fork small enough to be embedded */
	}
	cf->rsrc_id = xa.stream_id;
	cf->rsrc_size = xa.stream_size;
	if (cf->type == APFS_DECMPFS_TYPE_LZBITMAP_RSRC) {
		return cfile_read_offsets(vol, ino, cf);
	}

	error = apfs_read_stream(vol, cf->rsrc_id, cf->rsrc_size, 0, head, sizeof(head),
	    &got);
	if (error) {
		return error;
	}
	if (got < sizeof(head)) {
		return EIO;
	}
	cf->table_base = (uint64_t)be32(head) + 4;
	if (cf->table_base + 4 > cf->rsrc_size) {
		return EIO;
	}
	error = apfs_read_stream(vol, cf->rsrc_id, cf->rsrc_size, cf->table_base, head, 4,
	    &got);
	if (error) {
		return error;
	}
	if (got < 4) {
		return EIO;
	}
	cf->piece_count = le32(head);
	if (cf->piece_count == 0 ||
	    cf->piece_count != (uint32_t)((cf->size + APFS_DECMPFS_BLOCK_SIZE - 1) /
	    APFS_DECMPFS_BLOCK_SIZE)) {
		apfs_log("inode %llu: resource fork says %u pieces, %llu bytes needs %llu\n",
		    (unsigned long long)ino->obj_id, cf->piece_count,
		    (unsigned long long)cf->size,
		    (unsigned long long)((cf->size + APFS_DECMPFS_BLOCK_SIZE - 1) /
		    APFS_DECMPFS_BLOCK_SIZE));
		return EIO;
	}
	tbl_len = (size_t)cf->piece_count * 8;
	if (cf->table_base + 4 + tbl_len > cf->rsrc_size) {
		return EIO;
	}
	cf->piece_off = apfs_alloc((size_t)cf->piece_count * sizeof(uint32_t));
	cf->piece_len = apfs_alloc((size_t)cf->piece_count * sizeof(uint32_t));
	tbl = apfs_alloc(tbl_len);
	if (cf->piece_off == NULL || cf->piece_len == NULL || tbl == NULL) {
		error = ENOMEM;
		goto out;
	}
	error = apfs_read_stream(vol, cf->rsrc_id, cf->rsrc_size, cf->table_base + 4, tbl,
	    tbl_len, &got);
	if (error) {
		goto out;
	}
	if (got < tbl_len) {
		error = EIO;
		goto out;
	}
	for (i = 0; i < cf->piece_count; i++) {
		uint64_t off = le32(tbl + 8 * i);
		uint64_t len = le32(tbl + 8 * i + 4);

		if (len == 0 || cf->table_base + off + len > cf->rsrc_size) {
			error = EIO;
			goto out;
		}
		cf->piece_off[i] = (uint32_t)off;
		cf->piece_len[i] = (uint32_t)len;
	}
out:
	if (tbl != NULL) {
		apfs_free(tbl, tbl_len);
	}
	return error;
}

static int cfile_setup(struct apfs_vol *vol, const struct apfs_inode *ino, struct apfs_cfile *cf);

/*
 * Worked out into a copy of its own and only then put in place: working it out reads the
 * tree, and a read can let go of the lock (nx_block_unlocked), so another reader of the same
 * file may be doing the same meanwhile -- whoever finishes second throws its copy away.
 */
int
apfs_cfile_init(struct apfs_vol *vol, const struct apfs_inode *ino,
    struct apfs_cfile *cf)
{
	struct apfs_cfile mine;
	int error;

	if (cf->ready && cf->obj_id == ino->obj_id) {
		return 0;               /* already worked out, with its table and piece */
	}
	memset(&mine, 0, sizeof(mine));
	mine.held = APFS_NO_PIECE;
	error = cfile_setup(vol, ino, &mine);
	if (error) {
		return error;           /* (cfile_setup gave back what it had) */
	}
	if (cf->ready && cf->obj_id == ino->obj_id) {
		apfs_cfile_fini(&mine);
		return 0;
	}
	apfs_cfile_fini(cf);
	*cf = mine;
	return 0;
}

static int
cfile_setup(struct apfs_vol *vol, const struct apfs_inode *ino, struct apfs_cfile *cf)
{
	int error;

	cf->obj_id = ino->obj_id;
	cf->held = APFS_NO_PIECE;

	error = cfile_header(vol, ino, &cf->compressed, &cf->type, &cf->size, &cf->in_rsrc);
	if (error) {
		return error;
	}
	if (!cf->compressed) {
		cf->ready = true;
		return 0;
	}
	if (cf->in_rsrc) {
		error = cfile_read_table(vol, ino, cf);
		if (error) {
			apfs_cfile_fini(cf);
			return error;
		}
		cf->held_cap = APFS_DECMPFS_BLOCK_SIZE;
	} else if (cf->size <= APFS_DECMPFS_BLOCK_SIZE) {
		/* the whole thing is in the attribute and fits: hold it as one piece */
		cf->held_cap = (size_t)cf->size;
		cf->piece_count = 1;
	} else {
		cf->held_cap = 0;       /* too big to hold; it is read afresh each time */
	}
	if (cf->held_cap != 0) {
		cf->held_data = apfs_alloc(cf->held_cap);
		if (cf->held_data == NULL) {
			apfs_cfile_fini(cf);
			return ENOMEM;
		}
	}
	cf->ready = true;
	return 0;
}

/*
 * libdeflate allocates its decompressor through these (apfs_deflate.c).  It frees by pointer
 * alone, so each block carries its own length in front of it, as for zlib below.
 */
void *
apfs_deflate_malloc(size_t size)
{
	size_t n = size + sizeof(size_t);
	uint8_t *p;

	if (size > (~(size_t)0) - sizeof(size_t)) {
		return NULL;
	}
	p = apfs_alloc(n);
	if (p == NULL) {
		return NULL;
	}
	memcpy(p, &n, sizeof(n));
	return p + sizeof(size_t);
}

void
apfs_deflate_free(void *ptr)
{
	uint8_t *p;
	size_t n;

	if (ptr == NULL) {
		return;
	}
	p = (uint8_t *)ptr - sizeof(size_t);
	memcpy(&n, p, sizeof(n));
	apfs_free(p, n);
}

/*
 * zlib asks for its own working memory through these.  The kernel's copy of zlib has no
 * allocator of its own -- a zeroed z_stream leaves zalloc NULL and inflateInit_ then calls
 * through it -- so they have to be set, and they are set on both sides so that the kernel
 * and the program on the build machine run the same code.  zfree is told only an address,
 * so each block carries its own length in front of it.
 */
static voidpf
apfs_zalloc(voidpf opaque, unsigned items, unsigned size)
{
	size_t n;
	uint8_t *p;

	(void)opaque;
	if (items == 0 || size == 0) {
		return Z_NULL;
	}
	if ((size_t)items > (~(size_t)0 - sizeof(size_t)) / size) {
		return Z_NULL;
	}
	n = (size_t)items * size + sizeof(size_t);
	p = apfs_alloc(n);
	if (p == NULL) {
		return Z_NULL;
	}
	memcpy(p, &n, sizeof(n));
	return p + sizeof(size_t);
}

static void
apfs_zfree(voidpf opaque, voidpf address)
{
	uint8_t *p;
	size_t n;

	(void)opaque;
	if (address == NULL) {
		return;
	}
	p = (uint8_t *)address - sizeof(size_t);
	memcpy(&n, p, sizeof(n));
	apfs_free(p, n);
}

/*
 * One zlib stream through libdeflate; a leading 0xff means the piece was stored, not
 * compressed.  libdeflate takes the whole stream at once and decodes with large tables:
 * over all 19149 pieces of the restore ramdisk, 740 MB/s on the build machine against
 * zlib's 344, and on the phone 1248 ms against 3834 for one reader (856 once its Adler-32
 * is the NEON one).  It checks the stream's Adler-32 just as zlib does.  A stream it will
 * not take is handed to zlib instead and counted, so that such a thing cannot go by
 * unnoticed -- none of the pieces in either iOS volume is one.  `d` is the caller's
 * decompressor, NULL for one made here for this call alone.
 */
static int apfs_inflate_counted(struct apfs_nx *nx, const uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len, size_t *out);

static int
apfs_inflate_fast(struct libdeflate_decompressor *d, const uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len, size_t *out, bool *fell_back)
{
	struct libdeflate_decompressor *own = NULL;
	enum libdeflate_result r;
	size_t got = 0;

	if (src_len == 0) {
		return EIO;
	}
	if (src[0] == 0xFF) {           /* stored, not compressed */
		size_t n = src_len - 1 < dst_len ? src_len - 1 : dst_len;

		memcpy(dst, src + 1, n);
		*out = n;
		return 0;
	}
	if (d == NULL) {
		own = libdeflate_alloc_decompressor();
		if (own == NULL) {
			return ENOMEM;
		}
		d = own;
	}
	r = libdeflate_zlib_decompress(d, src, src_len, dst, dst_len, &got);
	if (own != NULL) {
		libdeflate_free_decompressor(own);
	}
	if (r == LIBDEFLATE_SUCCESS) {
		*out = got;
		return 0;
	}
	*fell_back = true;              /* counted by the caller, which may not hold the lock now */
	return apfs_inflate_counted(NULL, src, src_len, dst, dst_len, out);
}

static int
apfs_inflate_counted(struct apfs_nx *nx, const uint8_t *src, size_t src_len, uint8_t *dst,
    size_t dst_len, size_t *out)
{
	z_stream zs;
	int zerr;
	uint64_t t0;

	if (src_len == 0) {
		return EIO;
	}
	if (src[0] == 0xFF) {
		size_t n = src_len - 1;

		if (n > dst_len) {
			n = dst_len;
		}
		memcpy(dst, src + 1, n);
		*out = n;
		return 0;
	}
	t0 = apfs_ticks();
	memset(&zs, 0, sizeof(zs));
	zs.zalloc = apfs_zalloc;
	zs.zfree = apfs_zfree;
	zs.opaque = Z_NULL;
	zs.next_in = (Bytef *)(uintptr_t)src;
	zs.avail_in = (uInt)src_len;
	zs.next_out = (Bytef *)dst;
	zs.avail_out = (uInt)dst_len;
	if (inflateInit(&zs) != Z_OK) {
		return ENOMEM;
	}
	zerr = inflate(&zs, Z_FINISH);
	*out = (size_t)zs.total_out;
	inflateEnd(&zs);
	if (nx != NULL) {
		nx->inflate_ticks += apfs_ticks() - t0;
		nx->inflate_calls++;
		nx->inflate_in += src_len;
		nx->inflate_out += *out;
	}
	if (zerr != Z_STREAM_END && zerr != Z_OK && zerr != Z_BUF_ERROR) {
		return EIO;
	}
	return 0;
}

/*
 * The contents of a file whose bytes are in the com.apple.decmpfs attribute, from the
 * bytes after the header.
 *
 * Type 9 means they were stored as they are, with one marker byte in front.  All 23947
 * type-9 files in the iOS 16.5 root file system have 0xcc there and exactly `want` bytes
 * after it, which is how the marker's length was settled; a payload that is already
 * exactly the right length is taken whole.
 */
/*
 * One piece of a compressed file by the file's method: zlib (types 3 and 4) as above;
 * LZBITMAP (13 and 14) -- what the iOS 16 cryptexes use -- by libzbitmap (apfs_zbitmap.c):
 * a piece that starts with 'Z' is a stream of it, one whose first byte has its low four bits
 * set is stored after that byte.
 */
static int
apfs_decode_piece(uint32_t type, struct libdeflate_decompressor *d, const uint8_t *src,
    size_t src_len, uint8_t *dst, size_t dst_len, size_t *out, bool *fell_back)
{
	if (type != APFS_DECMPFS_TYPE_LZBITMAP_ATTR && type != APFS_DECMPFS_TYPE_LZBITMAP_RSRC) {
		return apfs_inflate_fast(d, src, src_len, dst, dst_len, out, fell_back);
	}
	if (src_len == 0) {
		return EIO;
	}
	if (src[0] == 'Z') {
		size_t n = 0;
		int r = zbm_decompress(dst, dst_len, src, src_len, &n);

		if (r < 0 || n > dst_len) {
			apfs_log("an LZBITMAP piece of %zu bytes for %zu: %d (%zu out)\n", src_len, dst_len, r, n);
			return EIO;
		}
		*out = n;
		return 0;
	}
	if ((src[0] & 0x0F) == 0x0F) {
		size_t n = src_len - 1 < dst_len ? src_len - 1 : dst_len;

		memcpy(dst, src + 1, n);
		*out = n;
		return 0;
	}
	apfs_log("an LZBITMAP piece of %zu bytes that starts with %02x\n", src_len, src[0]);
	return EIO;
}

static int
decmpfs_attr(struct apfs_nx *nx, uint32_t type, const uint8_t *src, size_t src_len,
    size_t want, uint8_t *dst, size_t *out)
{
	switch (type) {
	case APFS_DECMPFS_TYPE_RAW_ATTR: {
		const uint8_t *p = src;
		size_t n = src_len;

		if (n != want) {
			if (n == 0) {
				return EIO;
			}
			p++;
			n--;
		}
		if (n > want) {
			n = want;
		}
		memcpy(dst, p, n);
		*out = n;
		return 0;
	}
	case APFS_DECMPFS_TYPE_ZLIB_ATTR:
	case APFS_DECMPFS_TYPE_LZBITMAP_ATTR: {
		bool fb = false;
		int error = apfs_decode_piece(type, NULL, src, src_len, dst, want, out, &fb);

		if (fb && nx != NULL) {
			nx->inflate_fallbacks++;
		}
		return error;
	}
	default:
		return ENOTSUP;
	}
}

/*
 * A compressed file whose bytes are in the attribute: the whole thing is there, after the
 * header, with one marker byte in front when it was stored rather than compressed.  When
 * the context can hold it, it is produced once and kept.
 */
static int
read_cfile_attr(struct apfs_vol *vol, const struct apfs_inode *ino,
    struct apfs_cfile *cf, uint64_t off, uint8_t *buf, size_t len, size_t *out)
{
	struct apfs_xattr xa;
	uint8_t *attr = NULL;
	uint8_t *plain = NULL;
	size_t attr_len = 0, plain_len = 0, alloc = 0;
	bool held = false;
	int error = 0;

	*out = 0;
	if (cf->held_cap != 0 && cf->held == 0) {
		plain = cf->held_data;          /* produced on an earlier call */
		plain_len = cf->held_len;
		held = true;
		goto copy;
	}

	error = apfs_xattr_lookup(vol, ino->obj_id, APFS_DECMPFS_XATTR_NAME, &xa);
	if (error) {
		return error;
	}
	alloc = APFS_XATTR_MAX_EMBEDDED_SIZE + 16;
	if (xa.flags & APFS_XATTR_DATA_STREAM) {
		if (xa.stream_size > 64u * 1024u * 1024u) {
			return EIO;
		}
		alloc = (size_t)xa.stream_size;
	}
	attr = apfs_alloc(alloc);
	if (attr == NULL) {
		return ENOMEM;
	}
	error = apfs_xattr_read(vol, ino->obj_id, APFS_DECMPFS_XATTR_NAME, attr, alloc,
	    &attr_len);
	if (error) {
		goto out;
	}
	if (attr_len < 16) {
		error = EIO;
		goto out;
	}
	if (cf->size > 64u * 1024u * 1024u) {
		error = EIO;
		goto out;
	}
	if (cf->held_cap != 0 && cf->held_cap >= (size_t)cf->size) {
		plain = cf->held_data;
		held = true;
	} else {
		plain = apfs_alloc((size_t)cf->size ? (size_t)cf->size : 1);
		if (plain == NULL) {
			error = ENOMEM;
			goto out;
		}
	}
	error = decmpfs_attr(vol->nx, cf->type, attr + 16, attr_len - 16, (size_t)cf->size,
	    plain, &plain_len);
	if (error) {
		goto out;
	}
	if (held) {
		cf->held = 0;
		cf->held_len = plain_len;
	}
copy:
	if (off < plain_len) {
		size_t n = plain_len - (size_t)off;

		if (n > len) {
			n = len;
		}
		memcpy(buf, plain + off, n);
		*out = n;
	}
out:
	if (attr != NULL) {
		apfs_free(attr, alloc);
	}
	if (plain != NULL && !held) {
		apfs_free(plain, (size_t)cf->size ? (size_t)cf->size : 1);
	}
	return error;
}

/*
 * A compressed file whose bytes are in com.apple.ResourceFork: the contents cut into
 * 64 KB pieces, each its own zlib stream.  The table saying where the pieces are was read
 * once by apfs_cfile_init(); the piece last decompressed is kept, so a reader working
 * through the file -- or a page fault asking for 16 KB of it -- does not decompress the
 * same 64 KB again.
 */
/*
 * What reading a compressed piece needs besides the file's own state: a decompressor, room
 * for the compressed bytes, and room for a piece when it goes to the file's cache.  Made
 * and thrown away on every read before, which was three allocations a read -- with the
 * lock held, and the 64 KB one big enough that the kernel goes to the VM for it.  They are
 * kept on the container now, taken and given back with the lock held, a few at most.
 */
#define APFS_PIECE_CAP    (APFS_DECMPFS_BLOCK_SIZE + 1024)   /* stored: 1 + 64 KB; zlib: a few bytes more */
#define APFS_SCRATCH_KEEP 16

/*
 * A whole piece of a request, planned with the lock and lying in one run on the device:
 * what one thread needs to read it and decompress it into the caller's buffer, with no
 * lock and nothing shared.  Up to APFS_BATCH of them are done together, on as many
 * threads as nx->parallel brings -- one reader's request of many pieces no longer waits
 * on one processor while the others idle.
 */
struct apfs_piece_job {
	uint64_t addr;                  /* the first block the piece lies in */
	uint32_t skew;                  /* where in that block it starts */
	uint32_t plen;                  /* compressed */
	uint8_t *dst;                   /* the caller's, `want` bytes */
	size_t   want;
	bool     ra;                    /* read ahead, not asked for: its failure fails nothing */
	uint32_t type;                  /* the file's decmpfs type: how the piece is decoded */
	int      error;
	const uint8_t *src;             /* its compressed bytes, already read with its neighbours; or NULL */
};

#define APFS_BATCH   32
#define APFS_PAR_MAX 8
#define APFS_GROUP_CAP (1024 * 1024)

struct apfs_scratch {
	struct apfs_scratch            *next;
	struct libdeflate_decompressor *dec;
	uint8_t                        *piece;  /* APFS_PIECE_CAP */
	uint8_t                        *mine;   /* APFS_DECMPFS_BLOCK_SIZE, made when first wanted */
	/*
	 * The blocks a piece lies in, read in one request (APFS_PIECE_CAP and a block either
	 * side); also the bounce block for the ends of runs.
	 */
	uint8_t                        *raw;
	size_t                          raw_cap;
	struct apfs_piece_job          *jobs;   /* APFS_BATCH, made when first wanted */
	uint8_t                        *group;  /* APFS_GROUP_CAP: neighbouring pieces read together */
};

static void
scratch_free(struct apfs_scratch *sc)
{
	if (sc->dec != NULL) {
		libdeflate_free_decompressor(sc->dec);
	}
	if (sc->piece != NULL) {
		apfs_free(sc->piece, APFS_PIECE_CAP);
	}
	if (sc->mine != NULL) {
		apfs_free(sc->mine, APFS_DECMPFS_BLOCK_SIZE);
	}
	if (sc->raw != NULL) {
		apfs_free(sc->raw, sc->raw_cap);
	}
	if (sc->jobs != NULL) {
		apfs_free(sc->jobs, APFS_BATCH * sizeof(*sc->jobs));
	}
	if (sc->group != NULL) {
		apfs_free(sc->group, APFS_GROUP_CAP);
	}
	apfs_free(sc, sizeof(*sc));
}

static struct apfs_scratch *
scratch_get(struct apfs_nx *nx)
{
	struct apfs_scratch *sc = nx->scratch;

	if (sc != NULL) {
		nx->scratch = sc->next;
		nx->scratch_kept--;
		sc->next = NULL;
		return sc;
	}
	sc = apfs_alloc(sizeof(*sc));
	if (sc == NULL) {
		return NULL;
	}
	sc->dec = libdeflate_alloc_decompressor();
	sc->piece = apfs_alloc(APFS_PIECE_CAP);
	sc->raw_cap = APFS_PIECE_CAP + 2 * (size_t)nx->block_size;
	sc->raw = apfs_alloc(sc->raw_cap);
	if (sc->dec == NULL || sc->piece == NULL || sc->raw == NULL) {
		scratch_free(sc);
		return NULL;
	}
	return sc;
}

static void
scratch_put(struct apfs_nx *nx, struct apfs_scratch *sc)
{
	if (nx->scratch_kept >= APFS_SCRATCH_KEEP) {
		scratch_free(sc);
		return;
	}
	sc->next = nx->scratch;
	nx->scratch = sc;
	nx->scratch_kept++;
}

static void
scratch_drain(struct apfs_nx *nx)
{
	while (nx->scratch != NULL) {
		struct apfs_scratch *sc = nx->scratch;

		nx->scratch = sc->next;
		scratch_free(sc);
	}
	nx->scratch_kept = 0;
}

/* One batch on its way: the jobs, a scratch for each worker, what each worker counted. */
struct apfs_batch {
	struct apfs_nx        *nx;
	struct apfs_piece_job *jobs;
	struct apfs_scratch   *sc[APFS_PAR_MAX];
	struct {
		uint64_t calls, bytes, data_ticks, inflate_ticks, out, fallbacks;
	} w[APFS_PAR_MAX];
};

static int
batch_one(void *arg, uint32_t item, uint32_t worker)
{
	struct apfs_batch *b = arg;
	struct apfs_piece_job *j = &b->jobs[item];
	struct apfs_scratch *sc = b->sc[worker];
	uint32_t bs = b->nx->block_size;
	size_t span = ((size_t)j->skew + j->plen + bs - 1) / bs * bs;
	size_t got = 0;
	bool fell_back = false;
	uint64_t t0, t1;
	int error;

	const uint8_t *src = j->src;

	t0 = apfs_ticks();
	if (src == NULL) {
		error = dev_read_data(b->nx, j->addr, sc->raw, span, &b->w[worker].calls,
		    &b->w[worker].bytes);
		src = sc->raw + j->skew;
	} else {
		error = 0;
	}
	t1 = apfs_ticks();
	b->w[worker].data_ticks += t1 - t0;
	if (error) {
		j->error = error;
		return j->ra ? 0 : error;
	}
	error = apfs_decode_piece(j->type, sc->dec, src, j->plen, j->dst, j->want, &got,
	    &fell_back);
	b->w[worker].inflate_ticks += apfs_ticks() - t1;
	b->w[worker].out += got;
	if (fell_back) {
		b->w[worker].fallbacks++;
	}
	if (error == 0 && got != j->want) {
		error = EIO;
	}
	j->error = error;
	return j->ra ? 0 : error;
}

/*
 * Pieces of a batch that lie one right after the other on the device -- the pieces of one
 * file, as a rule -- are read in one request, into sc->group, and the jobs decompress from
 * there.  From the phone's flash a request costs some 25 us besides the bytes (four of
 * 16 KB took 160 us, one of 64 KB 96), and a batch is up to 32 pieces.  A piece that has
 * no neighbour, or does not fit, is left to read itself.  Without the lock.
 */
static void
batch_group(struct apfs_nx *nx, struct apfs_scratch *sc, uint32_t n, uint64_t *calls,
    uint64_t *bytes, uint64_t *ticks)
{
	uint32_t bs = nx->block_size, k = 0;
	size_t used = 0;
	uint64_t t0 = apfs_ticks();

	for (k = 0; k < n; k++) {
		sc->jobs[k].src = NULL;
	}
	if (nx->cheap_requests) {
		return;                 /* each piece reads itself, on its own thread */
	}
	if (sc->group == NULL) {
		sc->group = apfs_alloc(APFS_GROUP_CAP);
		if (sc->group == NULL) {
			return;
		}
	}
	for (k = 0; k < n;) {
		uint64_t start = sc->jobs[k].addr * bs + sc->jobs[k].skew;
		uint64_t end = start + sc->jobs[k].plen, first_block = start / bs, span;
		uint32_t last = k;

		/* as far as the next piece starts where this one ends, and it all fits */
		while (last + 1 < n) {
			uint64_t s2 = sc->jobs[last + 1].addr * bs + sc->jobs[last + 1].skew;
			uint64_t e2 = s2 + sc->jobs[last + 1].plen;

			if (s2 != end || (e2 + bs - 1) / bs * bs - first_block * bs > APFS_GROUP_CAP - used) {
				break;
			}
			end = e2;
			last++;
		}
		span = (end + bs - 1) / bs * bs - first_block * bs;
		if (last > k && span <= APFS_GROUP_CAP - used &&
		    dev_read_data(nx, first_block, sc->group + used, (size_t)span, calls, bytes) == 0) {
			uint32_t m;

			for (m = k; m <= last; m++) {
				sc->jobs[m].src = sc->group + used +
				    (sc->jobs[m].addr * bs + sc->jobs[m].skew - first_block * bs);
			}
			used += (size_t)span;
		}
		k = last + 1;
	}
	*ticks += apfs_ticks() - t0;
}

/*
 * Do the `n` jobs gathered in sc->jobs: let go of the lock, spread them over the threads
 * nx->parallel brings (one scratch each, sc being the caller's), take the lock back and
 * add up what the workers counted.  Inflate time is then the sum over the workers.
 */
static int
batch_run(struct apfs_vol *vol, struct apfs_scratch *sc, uint32_t n)
{
	struct apfs_nx *nx = vol->nx;
	struct apfs_batch b;
	uint32_t workers = 1, w, i;
	uint64_t in = 0;
	int error = 0;

	memset(&b, 0, sizeof(b));
	b.nx = nx;
	b.jobs = sc->jobs;
	b.sc[0] = sc;
	if (nx->parallel != NULL && n > 1) {
		/* the threads shared out among the requests in flight: all of them to one alone */
		workers = nx->requests > 1 ? nx->parallel_max / nx->requests : nx->parallel_max;
		if (workers < 1) {
			workers = 1;
		}
		if (workers > n) {
			workers = n;
		}
		if (workers > APFS_PAR_MAX) {
			workers = APFS_PAR_MAX;
		}
		for (w = 1; w < workers; w++) {
			b.sc[w] = scratch_get(nx);
			if (b.sc[w] == NULL) {
				break;
			}
		}
		workers = w;
	}
	nx_unlock(nx);
	batch_group(nx, sc, n, &b.w[0].calls, &b.w[0].bytes, &b.w[0].data_ticks);
	if (workers > 1) {
		error = nx->parallel(nx->parallel_ctx, n, workers, batch_one, &b);
	} else {
		for (i = 0; i < n; i++) {
			int e = batch_one(&b, i, 0);

			if (e != 0 && error == 0) {
				error = e;
			}
		}
	}
	nx_relock(nx);
	for (i = 0; i < n; i++) {
		in += sc->jobs[i].plen;
	}
	for (w = 0; w < workers; w++) {
		nx->data_calls += b.w[w].calls;
		nx->data_bytes += b.w[w].bytes;
		nx->data_ticks += b.w[w].data_ticks;
		nx->inflate_ticks += b.w[w].inflate_ticks;
		nx->inflate_out += b.w[w].out;
		nx->inflate_fallbacks += b.w[w].fallbacks;
		if (w > 0) {
			scratch_put(nx, b.sc[w]);
		}
	}
	nx->inflate_calls += n;
	nx->inflate_in += in;
	nx->batch_calls++;
	nx->batch_pieces += n;
	return error;
}

static int
read_cfile_rsrc(struct apfs_vol *vol, const struct apfs_inode *ino,
    struct apfs_cfile *cf, uint64_t off, uint8_t *buf, size_t len, size_t *out)
{
	struct apfs_scratch *sc = NULL;
	uint8_t *big = NULL;            /* a piece bigger than APFS_PIECE_CAP, should there be one */
	size_t big_cap = 0;
	uint32_t bs = vol->nx->block_size;
	uint32_t nbatch = 0;            /* whole pieces gathered in sc->jobs, not yet done */
	uint32_t ra_at = 0, nra = 0;    /* read-ahead jobs: sc->jobs[ra_at .. ra_at + nra) */
	uint64_t first, last, i;
	int error = 0;

	*out = 0;
	(void)ino;
	if (cf->piece_count == 0 || cf->piece_off == NULL) {
		return EIO;
	}
	if (off >= cf->size) {
		return 0;
	}
	if (len > cf->size - off) {
		len = (size_t)(cf->size - off);
	}
	first = off / APFS_DECMPFS_BLOCK_SIZE;
	last = (off + len - 1) / APFS_DECMPFS_BLOCK_SIZE;
	if (last >= cf->piece_count) {
		return EIO;
	}

	for (i = first; i <= last; i++) {
		size_t want = APFS_DECMPFS_BLOCK_SIZE;
		size_t from_in_piece, n, got = 0;

		if ((i + 1) * APFS_DECMPFS_BLOCK_SIZE > cf->size) {
			want = (size_t)(cf->size - i * APFS_DECMPFS_BLOCK_SIZE);
		}
		from_in_piece = (off > i * APFS_DECMPFS_BLOCK_SIZE)
		    ? (size_t)(off - i * APFS_DECMPFS_BLOCK_SIZE) : 0;
		if (cf->held != i && cf->ra_count > 0 && !cf->ra_filling &&
		    i >= cf->ra_first && i - cf->ra_first < cf->ra_count &&
		    cf->ra_len[i - cf->ra_first] == want) {
			/* read ahead by an earlier request of this reader's */
			n = want - from_in_piece;
			if (n > len - *out) {
				n = len - *out;
			}
			memcpy(buf + *out, cf->ra_data +
			    (size_t)(i - cf->ra_first) * APFS_DECMPFS_BLOCK_SIZE + from_in_piece, n);
			*out += n;
			vol->nx->ra_hits++;
			continue;
		}
		if (cf->held != i) {
			uint32_t plen = cf->piece_len[i];
			uint64_t poff = cf->table_base + cf->piece_off[i];
			bool raw_type = (cf->type == APFS_DECMPFS_TYPE_RAW_RSRC);
			struct apfs_run runs[APFS_MAX_RUNS];
			uint32_t nruns = 0;
			uint64_t covered = 0, calls = 0, bytes = 0, t0, t1, t2;
			bool planned = false, fell_back = false;
			/*
			 * The whole piece wanted, from its start (a 64 KB read at a 64 KB
			 * offset, which is what read(2) in big chunks and pagein ask for):
			 * straight into the caller's buffer, and the file's cache is left as
			 * it was.  Otherwise into a buffer of our own, which goes into the
			 * file's cache once the lock is back, by swapping buffers.
			 */
			bool direct = !raw_type && from_in_piece == 0 && len - *out >= want;
			const uint8_t *src;
			uint8_t *piece, *dst = NULL;
			size_t got_len = 0;

			if (sc == NULL) {
				sc = scratch_get(vol->nx);
				if (sc == NULL) {
					error = ENOMEM;
					goto out;
				}
			}
			piece = sc->piece;
			if (plen <= APFS_PIECE_CAP && poff < cf->rsrc_size &&
			    plen <= cf->rsrc_size - poff) {
				error = stream_plan(vol, cf->rsrc_id, poff, plen, runs, APFS_MAX_RUNS,
				    &nruns, &covered);
				if (error) {
					goto out;
				}
				planned = (covered == plen);
			}
			if (!planned) {
				/*
				 * Bigger than a piece should be, running past the fork's end, or
				 * in more runs than a plan holds: the old way, with the lock.
				 */
				if (plen > APFS_PIECE_CAP) {
					if (plen > big_cap) {
						if (big != NULL) {
							apfs_free(big, big_cap);
						}
						big_cap = plen;
						big = apfs_alloc(big_cap);
						if (big == NULL) {
							error = ENOMEM;
							goto out;
						}
					}
					piece = big;
				}
				error = apfs_read_stream(vol, cf->rsrc_id, cf->rsrc_size, poff, piece,
				    plen, &got);
				if (error) {
					goto out;
				}
				if (got < plen) {
					error = EIO;
					goto out;
				}
			}
			if (direct && planned && nruns == 1 && runs[0].at == 0 &&
			    runs[0].len == plen) {
				/* its place in the caller's buffer is kept; batch_run fills it */
				struct apfs_piece_job *j;

				if (sc->jobs == NULL) {
					sc->jobs = apfs_alloc(APFS_BATCH * sizeof(*sc->jobs));
					if (sc->jobs == NULL) {
						error = ENOMEM;
						goto out;
					}
				}
				j = &sc->jobs[nbatch++];
				j->addr = runs[0].addr;
				j->skew = runs[0].skew;
				j->plen = plen;
				j->dst = buf + *out;
				j->want = want;
				j->ra = false;
				j->type = cf->type;
				*out += want;
				if (nbatch == APFS_BATCH) {
					error = batch_run(vol, sc, nbatch);
					nbatch = 0;
					if (error) {
						goto out;
					}
				}
				/*
				 * The last piece of this request, the one right after where this
				 * reader's last request ended, and threads to spare for it (the
				 * share batch_run would give it is two or more): the pieces after it
				 * go into this batch as well, into ra_data.
				 */
				if (i == last && i >= 1 && i == cf->ra_expect && !cf->ra_filling &&
				    vol->nx->parallel != NULL && i + 1 < cf->piece_count &&
				    vol->nx->parallel_max / (vol->nx->requests > 1 ?
				    vol->nx->requests : 1) >= 2) {
					uint64_t k;

					/*
					 * Claimed before the pieces are planned: planning reads the tree,
					 * which can let go of the lock, and nobody else may fill ra_data.
					 */
					cf->ra_filling = true;
					if (cf->ra_data == NULL) {
						cf->ra_data = apfs_alloc((size_t)APFS_RA_PIECES *
						    APFS_DECMPFS_BLOCK_SIZE);
					}
					cf->ra_count = 0;
					ra_at = nbatch;
					for (k = i + 1; cf->ra_data != NULL && k < cf->piece_count &&
					    k - i <= APFS_RA_PIECES && nbatch < APFS_BATCH; k++) {
						size_t kwant = APFS_DECMPFS_BLOCK_SIZE;
						uint32_t kplen = cf->piece_len[k];
						uint64_t kpoff = cf->table_base + cf->piece_off[k];
						struct apfs_run kruns[APFS_MAX_RUNS];
						uint32_t knruns = 0;
						uint64_t kcovered = 0;

						if ((k + 1) * APFS_DECMPFS_BLOCK_SIZE > cf->size) {
							kwant = (size_t)(cf->size - k * APFS_DECMPFS_BLOCK_SIZE);
						}
						if (kplen > APFS_PIECE_CAP || kpoff >= cf->rsrc_size ||
						    kplen > cf->rsrc_size - kpoff ||
						    stream_plan(vol, cf->rsrc_id, kpoff, kplen, kruns,
						    APFS_MAX_RUNS, &knruns, &kcovered) != 0 ||
						    kcovered != kplen || knruns != 1 || kruns[0].at != 0 ||
						    kruns[0].len != kplen) {
							break;  /* not one for a batch: the read-ahead stops here */
						}
						j = &sc->jobs[nbatch++];
						j->addr = kruns[0].addr;
						j->skew = kruns[0].skew;
						j->plen = kplen;
						j->dst = cf->ra_data + (size_t)(k - i - 1) * APFS_DECMPFS_BLOCK_SIZE;
						j->want = kwant;
						j->ra = true;
						j->type = cf->type;
						cf->ra_len[k - i - 1] = (uint32_t)kwant;
						nra++;
					}
					if (nra > 0) {
						cf->ra_first = i + 1;
					} else {
						cf->ra_filling = false;
					}
				}
				continue;
			}
			if (!raw_type && !direct && sc->mine == NULL) {
				sc->mine = apfs_alloc(APFS_DECMPFS_BLOCK_SIZE);
				if (sc->mine == NULL) {
					error = ENOMEM;
					goto out;
				}
			}
			dst = direct ? buf + *out : sc->mine;

			/*
			 * Without the lock from here: reading the piece off the device and
			 * decompressing it touch nothing anyone else can see.  Decompressing
			 * is most of the time spent here -- with the lock held through it,
			 * four readers on eight processors kept the lock busy 94% of the
			 * time.  Two readers of the same file may decompress the same piece
			 * once each; that is the price, and the cache stays right either way.
			 */
			t0 = apfs_ticks();
			nx_unlock(vol->nx);
			src = piece;
			if (planned && nruns == 1 && runs[0].at == 0 && runs[0].len == plen) {
				/* the usual case: the blocks the piece lies in, in one request */
				size_t span = ((size_t)runs[0].skew + plen + bs - 1) / bs * bs;

				error = dev_read_data(vol->nx, runs[0].addr, sc->raw, span, &calls,
				    &bytes);
				src = sc->raw + runs[0].skew;
			} else if (planned) {
				error = stream_fetch(vol->nx, runs, nruns, covered, piece, sc->raw,
				    &calls, &bytes);
			}
			t1 = apfs_ticks();
			if (error == 0 && !raw_type) {
				error = apfs_decode_piece(cf->type, sc->dec, src, plen, dst, want, &got_len,
				    &fell_back);
			}
			nx_relock(vol->nx);
			t2 = apfs_ticks();
			vol->nx->data_calls += calls;
			vol->nx->data_bytes += bytes;
			vol->nx->data_ticks += t1 - t0;
			if (!raw_type) {
				if (fell_back) {
					vol->nx->inflate_fallbacks++;
				}
				vol->nx->inflate_ticks += t2 - t1;
				vol->nx->inflate_calls++;
				vol->nx->inflate_in += plen;
				vol->nx->inflate_out += got_len;
			}
			if (error) {
				goto out;
			}
			if (raw_type) {
				size_t m = plen < want ? (size_t)plen : want;

				cf->held = APFS_NO_PIECE;
				memcpy(cf->held_data, src, m);
				cf->held_len = m;
			} else {
				if (got_len != want) {
					error = EIO;
					goto out;
				}
				if (direct) {
					*out += want;
					continue;
				}
				/* in it goes: the old piece's buffer becomes ours */
				{
					uint8_t *t = cf->held_data;

					cf->held_data = sc->mine;
					sc->mine = t;
				}
				cf->held_len = got_len;
			}
			if (cf->held_len != want) {
				error = EIO;
				goto out;
			}
			cf->held = i;
		}
		n = cf->held_len - from_in_piece;
		if (n > len - *out) {
			n = len - *out;
		}
		memcpy(buf + *out, cf->held_data + from_in_piece, n);
		*out += n;
	}
	if (nbatch > 0) {
		error = batch_run(vol, sc, nbatch);
	}
	if (nra > 0) {
		uint32_t k;

		cf->ra_count = nra;
		for (k = 0; k < nra; k++) {
			if (sc->jobs[ra_at + k].error != 0) {
				cf->ra_count = k;       /* what came before the first that failed */
				break;
			}
		}
		vol->nx->ra_batches++;
		vol->nx->ra_pieces += cf->ra_count;
		cf->ra_filling = false;
		nra = 0;
	}
	if (error == 0) {
		cf->ra_expect = ((off + len) % APFS_DECMPFS_BLOCK_SIZE == 0)
		    ? (off + len) / APFS_DECMPFS_BLOCK_SIZE : APFS_NO_PIECE;
	}
out:
	if (nra > 0) {
		/* given up on before the batch ran: nothing in ra_data is good */
		cf->ra_count = 0;
		cf->ra_filling = false;
	}
	if (big != NULL) {
		apfs_free(big, big_cap);
	}
	if (sc != NULL) {
		scratch_put(vol->nx, sc);
	}
	return error;
}

/*
 * An uncompressed file's contents: planned with the lock, read without it, whole blocks
 * straight into the caller's buffer.
 */
static int
read_plain(struct apfs_vol *vol, uint64_t stream_id, uint64_t stream_size, uint64_t off,
    uint8_t *buf, size_t len, size_t *out)
{
	struct apfs_run runs[APFS_MAX_RUNS];
	struct apfs_scratch *sc;
	size_t done = 0;
	int error = 0;

	*out = 0;
	if (off >= stream_size) {
		return 0;
	}
	if (len > stream_size - off) {
		len = (size_t)(stream_size - off);
	}
	sc = scratch_get(vol->nx);
	if (sc == NULL) {
		return ENOMEM;
	}
	while (done < len) {
		uint32_t nruns = 0;
		uint64_t covered = 0, calls = 0, bytes = 0, t0;

		error = stream_plan(vol, stream_id, off + done, len - done, runs, APFS_MAX_RUNS,
		    &nruns, &covered);
		if (error) {
			break;
		}
		if (covered == 0) {
			error = EIO;            /* cannot happen: a plan covers at least its first run */
			break;
		}
		t0 = apfs_ticks();
		nx_unlock(vol->nx);
		error = stream_fetch(vol->nx, runs, nruns, covered, buf + done, sc->raw, &calls,
		    &bytes);
		nx_relock(vol->nx);
		vol->nx->data_calls += calls;
		vol->nx->data_bytes += bytes;
		vol->nx->data_ticks += apfs_ticks() - t0;
		if (error) {
			break;
		}
		done += (size_t)covered;
	}
	scratch_put(vol->nx, sc);
	if (error == 0) {
		*out = done;
	}
	return error;
}

int
apfs_read_file(struct apfs_vol *vol, const struct apfs_inode *ino,
    struct apfs_cfile *cf, uint64_t off, void *buf, size_t len, size_t *out)
{
	struct apfs_cfile own;
	bool borrowed = false;
	int error;

	*out = 0;
	if ((ino->mode & APFS_S_IFMT) == APFS_S_IFLNK) {
		/*
		 * The whole target, then the part asked for.  Reading only `len` bytes of it and
		 * then counting from `off` gave the wrong bytes to anyone reading a link in
		 * pieces smaller than its target (the host tool, reading a byte at a time).
		 */
		size_t n = 0;
		char *target = apfs_alloc(APFS_LINK_MAX);

		if (target == NULL) {
			return ENOMEM;
		}
		error = apfs_readlink(vol, ino->obj_id, target, APFS_LINK_MAX, &n);
		if (error == 0 && off < n) {
			*out = n - (size_t)off < len ? n - (size_t)off : len;
			memcpy(buf, target + off, *out);
		}
		apfs_free(target, APFS_LINK_MAX);
		return error;
	}
	/*
	 * Without a context from the caller, one is made for this call alone -- correct, and
	 * the slow way: the table and the piece are then found again on the next call.
	 */
	if (cf == NULL) {
		memset(&own, 0, sizeof(own));
		own.held = APFS_NO_PIECE;
		cf = &own;
		borrowed = true;
	}
	error = apfs_cfile_init(vol, ino, cf);
	if (error == 0) {
		if (!cf->compressed) {
			error = ino->has_dstream
			    ? read_plain(vol, ino->private_id, ino->size, off, buf, len, out)
			    : 0;
		} else if (cf->in_rsrc) {
			error = read_cfile_rsrc(vol, ino, cf, off, buf, len, out);
		} else {
			error = read_cfile_attr(vol, ino, cf, off, buf, len, out);
		}
	}
	if (borrowed) {
		apfs_cfile_fini(cf);
	}
	return error;
}
