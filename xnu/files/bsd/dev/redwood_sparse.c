/*
 * A block device over an image that holds only some of its blocks (/dev/rsp0).  Only with
 * boot-arg apfs_sparse=<offset>.
 *
 * This exists so that the real iOS root volume can be read on the phone.  The phone has
 * no driver for its own storage, so a volume there has to live in a RAM disk, and that
 * volume is 5368 MB against the 4288 MB its firmware leaves usable.  But a given walk
 * reads only some of those blocks, so tools/apfs/mksparse.py writes out the ones it reads
 * together with a table saying where each went, and this serves them at their original
 * addresses.  The APFS driver above cannot tell the difference: every block it asks for is
 * there, at the address it asks for.
 *
 * The image rides at the end of the resident RAM disk, after the image that is the root -- an
 * APFS container, which ends where its own superblock says, so what follows is nothing to it
 * -- and apfs_sparse is the byte offset where it starts.
 *
 * A block that is not in the image reads as zeros and is counted.  Nothing should ask for
 * one; the count is printed so that it does not go unnoticed if something does.
 *
 * Read only.
 */

#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/redwood_sparse.h>

extern dev_t mdevlookup(int devid);
extern int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size);

#define SPARSE_MAGIC   "REDWOODSPARSE1\0\0"
#define SPARSE_SECTOR  512

struct sparse_header {
	char     sh_magic[16];
	uint32_t sh_block_size;
	uint64_t sh_block_count;        /* of the container this stands in for */
	uint64_t sh_present;            /* how many of them are here */
	uint64_t sh_table_off;          /* from the start of this header */
	uint64_t sh_data_off;
} __attribute__((packed));

static const uint8_t *sparse_table;     /* sh_block_count 32-bit entries */
static const uint8_t *sparse_data;
static uint32_t sparse_block_size;
static uint64_t sparse_block_count;
static uint64_t sparse_present;
static bool sparse_ready;
static int sparse_bmajor = -1, sparse_cmajor = -1;
static uint64_t sparse_hits, sparse_misses;

static int
sparse_open(__unused dev_t dev, int flags, __unused int devtype, __unused proc_t p)
{
	if (!sparse_ready) {
		return ENXIO;
	}
	return (flags & FWRITE) ? EROFS : 0;
}

static int
sparse_close(__unused dev_t dev, __unused int flags, __unused int devtype,
    __unused proc_t p)
{
	return 0;
}

/* Where block `blk` of the container is in the image, or NULL if it is not here. */
static const uint8_t *
sparse_block(uint64_t blk)
{
	uint32_t slot;

	if (blk >= sparse_block_count) {
		return NULL;
	}
	memcpy(&slot, sparse_table + blk * 4, sizeof(slot));
	if (slot == 0) {
		return NULL;
	}
	return sparse_data + (uint64_t)(slot - 1) * sparse_block_size;
}

static void
sparse_strategy(buf_t bp)
{
	uint8_t *dst;
	uint64_t off;
	size_t left;

	if (!sparse_ready) {
		buf_seterror(bp, ENXIO);
		buf_biodone(bp);
		return;
	}
	if (!(buf_flags(bp) & B_READ)) {
		buf_seterror(bp, EROFS);
		buf_biodone(bp);
		return;
	}
	off = (uint64_t)buf_blkno(bp) * SPARSE_SECTOR;
	left = buf_count(bp);
	dst = (uint8_t *)buf_dataptr(bp);

	if (off >= sparse_block_count * sparse_block_size) {
		buf_setresid(bp, buf_count(bp));
		buf_seterror(bp, (off == sparse_block_count * sparse_block_size) ? 0 : EINVAL);
		buf_biodone(bp);
		return;
	}
	while (left > 0) {
		uint64_t blk = off / sparse_block_size;
		uint32_t skew = (uint32_t)(off % sparse_block_size);
		size_t n = sparse_block_size - skew;
		const uint8_t *src;

		if (n > left) {
			n = left;
		}
		src = sparse_block(blk);
		if (src == NULL) {
			/* nothing asked for this when the image was made */
			bzero(dst, n);
			sparse_misses++;
		} else {
			memcpy(dst, src + skew, n);
			sparse_hits++;
		}
		dst += n;
		off += n;
		left -= n;
	}
	buf_setresid(bp, 0);
	buf_biodone(bp);
}

static int
sparse_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int fflag,
    __unused proc_t p)
{
	if (!sparse_ready) {
		return ENXIO;
	}
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*(uint32_t *)data = SPARSE_SECTOR;
		return 0;
	case DKIOCSETBLOCKSIZE:
		/* spec_open() sets 512 and back, and takes a refusal for a broken device */
		return (*(uint32_t *)data == SPARSE_SECTOR) ? 0 : EINVAL;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = sparse_block_count *
		    (sparse_block_size / SPARSE_SECTOR);
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = 0;
		return 0;
	default:
		return ENOTTY;
	}
}

static int
sparse_size(__unused dev_t dev)
{
	return sparse_ready ? SPARSE_SECTOR : -1;
}

static const struct bdevsw sparse_bdevsw = {
	.d_open     = sparse_open,
	.d_close    = sparse_close,
	.d_strategy = sparse_strategy,
	.d_ioctl    = sparse_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = sparse_size,
	.d_type     = D_DISK,
};

static const struct cdevsw sparse_cdevsw = {
	.d_open       = sparse_open,
	.d_close      = sparse_close,
	.d_read       = eno_rdwrt,
	.d_write      = eno_rdwrt,
	.d_ioctl      = sparse_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

void
redwood_sparse_report(void)
{
	if (!sparse_ready) {
		return;
	}
	printf("sparse: %llu blocks served, %llu asked for that are not in the image\n",
	    (unsigned long long)sparse_hits, (unsigned long long)sparse_misses);
}

void
redwood_sparse_init(void)
{
	uint64_t off = 0;
	vm_offset_t base = 0;
	uint64_t size = 0;
	const struct sparse_header *hdr;
	dev_t md;

	if (!PE_parse_boot_argn("apfs_sparse", &off, sizeof(off)) || off == 0) {
		return;
	}
	md = mdevlookup(0);
	if (md == (dev_t)-1 || mdevgetkernelrange(md, &base, &size) != 0) {
		printf("sparse: md0 is not a memory disk this can be read from\n");
		return;
	}
	if (off + sizeof(*hdr) > size) {
		printf("sparse: offset %llu is past the end of md0 (%llu bytes)\n",
		    (unsigned long long)off, (unsigned long long)size);
		return;
	}
	hdr = (const struct sparse_header *)(base + off);
	if (memcmp(hdr->sh_magic, SPARSE_MAGIC, sizeof(hdr->sh_magic)) != 0) {
		printf("sparse: no sparse image at offset %llu in md0\n",
		    (unsigned long long)off);
		return;
	}
	sparse_block_size = hdr->sh_block_size;
	sparse_block_count = hdr->sh_block_count;
	sparse_present = hdr->sh_present;
	if (sparse_block_size < SPARSE_SECTOR || (sparse_block_size % SPARSE_SECTOR) != 0 ||
	    sparse_block_count == 0 || sparse_present == 0 ||
	    sparse_present > sparse_block_count) {
		printf("sparse: the header makes no sense\n");
		return;
	}
	if (off + hdr->sh_data_off + sparse_present * sparse_block_size > size ||
	    off + hdr->sh_table_off + sparse_block_count * 4 > size) {
		printf("sparse: the image does not fit in md0 (%llu bytes)\n",
		    (unsigned long long)size);
		return;
	}
	sparse_table = (const uint8_t *)(base + off + hdr->sh_table_off);
	sparse_data = (const uint8_t *)(base + off + hdr->sh_data_off);

	sparse_bmajor = bdevsw_add(-1, &sparse_bdevsw);
	if (sparse_bmajor < 0) {
		printf("sparse: bdevsw_add failed\n");
		return;
	}
	sparse_cmajor = cdevsw_add_with_bdev(-1, &sparse_cdevsw, sparse_bmajor);
	if (sparse_cmajor < 0) {
		printf("sparse: cdevsw_add failed\n");
		return;
	}
	if (devfs_make_node(makedev(sparse_bmajor, 0), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR,
	    0600, "rsp0") == NULL) {
		printf("sparse: devfs_make_node failed\n");
		return;
	}
	sparse_ready = true;
	printf("sparse: /dev/rsp0 stands in for %llu blocks of %u, %llu of them here "
	    "(%llu MB), at offset %llu of md0\n", (unsigned long long)sparse_block_count,
	    sparse_block_size, (unsigned long long)sparse_present,
	    (unsigned long long)(sparse_present * sparse_block_size / (1024 * 1024)),
	    (unsigned long long)off);
}
