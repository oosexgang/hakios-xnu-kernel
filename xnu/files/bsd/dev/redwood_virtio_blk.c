/*
 * A virtio block device, so that a disk image on the host can be a block device in the
 * guest (/dev/rvblk0).  Only with boot-arg virtio_blk=1: the bus address is QEMU's and
 * means nothing on the phone.
 *
 * This exists for the APFS driver.  The volume in ios165/rootfs.dmg is 5.6 GB, which is
 * too much to hold in guest memory as a RAM disk the way this port's own images are held, and the
 * point of reading APFS is to read a real image rather than one cut down to fit.  QEMU
 * serves it straight off the host's disk:
 *
 *   -drive file=rootfs.apfs,format=raw,if=none,id=d,readonly=on
 *   -device virtio-blk-device,drive=d
 *
 * Under QEMU's "virt" machine the transport is virtio-mmio and must be the current one,
 * not "legacy" (-global virtio-mmio.force-legacy=false).
 *
 * No interrupt is used and only one request is outstanding at a time: a request is put on
 * the queue, the device is notified, and the used ring is watched until it comes back.
 * That is slow in principle and entirely adequate here -- reads come from the APFS
 * driver's own block cache misses, and the host is serving them out of its page cache.
 *
 * Read only.  A write request is refused: nothing in this port has any business writing
 * to the image, and refusing is one less way to damage it by accident.
 */

#include <kern/clock.h>
#include <kern/locks.h>
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
#include <sys/redwood_virtio_blk.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);

#define VIRTIO_BUS              0x0a000000UL    /* QEMU virt: 32 transports of 0x200 bytes */
#define VIRTIO_SLOTS            32
#define VIRTIO_SLOT_SIZE        0x200

#define VIRTIO_MAGIC            0x000           /* "virt" */
#define VIRTIO_VERSION          0x004
#define VIRTIO_DEVICE_ID        0x008
#define VIRTIO_DRIVER_FEATURES  0x020
#define VIRTIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_QUEUE_SEL        0x030
#define VIRTIO_QUEUE_NUM_MAX    0x034
#define VIRTIO_QUEUE_NUM        0x038
#define VIRTIO_QUEUE_READY      0x044
#define VIRTIO_QUEUE_NOTIFY     0x050
#define VIRTIO_INTERRUPT_ACK    0x064
#define VIRTIO_STATUS           0x070
#define VIRTIO_QUEUE_DESC       0x080
#define VIRTIO_QUEUE_DRIVER     0x090
#define VIRTIO_QUEUE_DEVICE     0x0a0
#define VIRTIO_CONFIG           0x100           /* the device's own configuration */

#define VIRTIO_ID_BLOCK         2
#define STATUS_ACKNOWLEDGE      1
#define STATUS_DRIVER           2
#define STATUS_DRIVER_OK        4
#define STATUS_FEATURES_OK      8

#define QUEUE_SIZE              64
#define DESC_F_NEXT             1
#define DESC_F_WRITE            2
#define AVAIL_F_NO_INTERRUPT    1

/* the request types we use */
#define VIRTIO_BLK_T_IN         0
#define VIRTIO_BLK_S_OK         0

#define VIRTIO_BLK_SECTOR       512             /* virtio-blk counts in these, always */

/*
 * The most one request may carry.  A request needs one descriptor for its header, one for
 * its status byte and one per page of the data, so this bounds the chain well inside the
 * queue.
 */
#define BLK_MAX_XFER            (64 * 1024)
#define BLK_MAX_DATA_DESC       (BLK_MAX_XFER / PAGE_SIZE + 1)

struct vq_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};
struct vq_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[QUEUE_SIZE];
};
struct vq_used {
	uint16_t flags;
	uint16_t idx;
	struct {
		uint32_t id;
		uint32_t len;
	} ring[QUEUE_SIZE];
};
struct blk_req_hdr {
	uint32_t type;
	uint32_t reserved;
	uint64_t sector;
};

/* the ring and the small request pieces share one page */
#define OFFSET_DESC             0x0000
#define OFFSET_AVAIL            0x0400
#define OFFSET_USED             0x0800
#define OFFSET_HDR              0x0c00
#define OFFSET_STATUS           0x0c40

struct blk_device {
	vm_offset_t          regs;
	vm_offset_t          page;
	uint64_t             page_phys;
	struct vq_desc      *desc;
	struct vq_avail     *avail;
	struct vq_used      *used;
	struct blk_req_hdr  *hdr;
	volatile uint8_t    *status;
	uint16_t             seen;      /* how far the used ring has been read */
	uint64_t             capacity;  /* in 512-byte sectors */
	lck_mtx_t            lock;
};

static LCK_GRP_DECLARE(blk_lck_grp, "redwood-virtio-blk");
static struct blk_device blk_dev;
static bool blk_ready;
static int blk_bmajor = -1, blk_cmajor = -1;

/* One of this driver's devices: every request is a trip through the hypervisor */
bool
redwood_virtio_blk_dev(dev_t dev)
{
	return (blk_bmajor >= 0 && major(dev) == blk_bmajor) ||
	       (blk_cmajor >= 0 && major(dev) == blk_cmajor);
}

static inline uint32_t
reg_read(struct blk_device *d, uint32_t reg)
{
	return *(volatile uint32_t *)(d->regs + reg);
}

static inline void
reg_write(struct blk_device *d, uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(d->regs + reg) = value;
}

static inline void
reg_write64(struct blk_device *d, uint32_t reg, uint64_t value)
{
	reg_write(d, reg, (uint32_t)value);
	reg_write(d, reg + 4, (uint32_t)(value >> 32));
}

/*
 * One read.  `data` is kernel memory that need not be page aligned and whose pages need
 * not be next to each other in physical memory, so it goes on the queue a page at a time.
 */
static int
blk_read(struct blk_device *d, uint64_t offset, void *data, size_t len)
{
	vm_offset_t va = (vm_offset_t)data;
	uint16_t idx = 0;
	size_t done = 0;
	uint64_t deadline;
	int error = 0;

	if (len == 0 || len > BLK_MAX_XFER) {
		return EINVAL;
	}
	if ((offset % VIRTIO_BLK_SECTOR) != 0) {
		return EINVAL;
	}
	if (offset / VIRTIO_BLK_SECTOR >= d->capacity ||
	    (offset + len) / VIRTIO_BLK_SECTOR > d->capacity) {
		return EINVAL;
	}

	lck_mtx_lock(&d->lock);

	d->hdr->type = VIRTIO_BLK_T_IN;
	d->hdr->reserved = 0;
	d->hdr->sector = offset / VIRTIO_BLK_SECTOR;
	*d->status = 0xff;

	/* descriptor 0: the header, which the device reads */
	d->desc[0].addr = d->page_phys + OFFSET_HDR;
	d->desc[0].len = sizeof(struct blk_req_hdr);
	d->desc[0].flags = DESC_F_NEXT;
	d->desc[0].next = 1;
	idx = 1;

	/* then the data, one descriptor per run of bytes inside a page */
	while (done < len) {
		vm_offset_t at = va + done;
		size_t chunk = PAGE_SIZE - (at & PAGE_MASK);

		if (chunk > len - done) {
			chunk = len - done;
		}
		if (idx >= 1 + BLK_MAX_DATA_DESC) {
			lck_mtx_unlock(&d->lock);
			return EINVAL;
		}
		d->desc[idx].addr = kvtophys(at);
		d->desc[idx].len = (uint32_t)chunk;
		d->desc[idx].flags = DESC_F_WRITE | DESC_F_NEXT;
		d->desc[idx].next = (uint16_t)(idx + 1);
		idx++;
		done += chunk;
	}

	/* last: the status byte, which the device writes */
	d->desc[idx].addr = d->page_phys + OFFSET_STATUS;
	d->desc[idx].len = 1;
	d->desc[idx].flags = DESC_F_WRITE;
	d->desc[idx].next = 0;

	d->avail->flags = AVAIL_F_NO_INTERRUPT;
	d->avail->ring[d->avail->idx % QUEUE_SIZE] = 0;         /* the chain starts at 0 */
	__builtin_arm_dmb(0xb);
	d->avail->idx++;
	__builtin_arm_dmb(0xb);
	reg_write(d, VIRTIO_QUEUE_NOTIFY, 0);

	/* wait for it to come back; a read off the host's disk should take microseconds */
	clock_interval_to_deadline(10, NSEC_PER_SEC, &deadline);
	for (;;) {
		__builtin_arm_dmb(0xb);
		if (d->used->idx != d->seen) {
			d->seen = d->used->idx;
			break;
		}
		if (mach_absolute_time() > deadline) {
			printf("virtio-blk: a read of %lu bytes at %llu went unanswered\n",
			    (unsigned long)len, offset);
			lck_mtx_unlock(&d->lock);
			return EIO;
		}
		delay(10);
	}
	reg_write(d, VIRTIO_INTERRUPT_ACK, reg_read(d, 0x060));  /* keep the device quiet */

	if (*d->status != VIRTIO_BLK_S_OK) {
		printf("virtio-blk: a read of %lu bytes at %llu failed with status %u\n",
		    (unsigned long)len, offset, *d->status);
		error = EIO;
	}
	lck_mtx_unlock(&d->lock);
	return error;
}

/* ---- the block device ---- */

static int
blk_dev_open(__unused dev_t dev, int flags, __unused int devtype, __unused proc_t p)
{
	if (!blk_ready) {
		return ENXIO;
	}
	if (flags & FWRITE) {
		return EROFS;
	}
	return 0;
}

static int
blk_dev_close(__unused dev_t dev, __unused int flags, __unused int devtype,
    __unused proc_t p)
{
	return 0;
}

static void
blk_dev_strategy(buf_t bp)
{
	struct blk_device *d = &blk_dev;
	uint64_t offset;
	size_t len;
	int error;

	if (!blk_ready) {
		buf_seterror(bp, ENXIO);
		buf_biodone(bp);
		return;
	}
	if (!(buf_flags(bp) & B_READ)) {
		buf_seterror(bp, EROFS);         /* this device never writes */
		buf_biodone(bp);
		return;
	}
	offset = (uint64_t)buf_blkno(bp) * VIRTIO_BLK_SECTOR;
	len = buf_count(bp);

	/* trim a request that runs off the end, and refuse one that starts past it */
	if (offset >= d->capacity * VIRTIO_BLK_SECTOR) {
		buf_seterror(bp, offset == d->capacity * VIRTIO_BLK_SECTOR ? 0 : EINVAL);
		buf_setresid(bp, buf_count(bp));
		buf_biodone(bp);
		return;
	}
	if (offset + len > d->capacity * VIRTIO_BLK_SECTOR) {
		len = (size_t)(d->capacity * VIRTIO_BLK_SECTOR - offset);
	}

	error = 0;
	{
		uint8_t *dst = (uint8_t *)buf_dataptr(bp);
		size_t done = 0;

		while (done < len && error == 0) {
			size_t chunk = len - done;

			if (chunk > BLK_MAX_XFER) {
				chunk = BLK_MAX_XFER;
			}
			error = blk_read(d, offset + done, dst + done, chunk);
			done += chunk;
		}
		buf_setresid(bp, (uint32_t)(buf_count(bp) - (error ? 0 : done)));
	}
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
blk_dev_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int fflag,
    __unused proc_t p)
{
	struct blk_device *d = &blk_dev;

	if (!blk_ready) {
		return ENXIO;
	}
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*(uint32_t *)data = VIRTIO_BLK_SECTOR;
		return 0;
	case DKIOCSETBLOCKSIZE:
		/*
		 * spec_open() sets the block size to 512 to count the device in 512-byte
		 * blocks and then sets it back, and treats a refusal of either as the device
		 * being broken (ENXIO).  virtio-blk counts in 512-byte sectors and nothing
		 * else, so that is the only size to agree to.
		 */
		if (*(uint32_t *)data != VIRTIO_BLK_SECTOR) {
			return EINVAL;
		}
		return 0;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = d->capacity;
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = 0;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*(uint64_t *)data = BLK_MAX_XFER / VIRTIO_BLK_SECTOR;
		return 0;
	default:
		return ENOTTY;
	}
}

static int
blk_dev_size(dev_t dev)
{
	(void)dev;
	return blk_ready ? VIRTIO_BLK_SECTOR : -1;
}

static int
blk_dev_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	/* the character device is here so that tools can see the disk; reads go by buffer */
	return physio(blk_dev_strategy, NULL, dev, B_READ, (u_int (*)(buf_t))blk_dev_size,
	    uio, VIRTIO_BLK_SECTOR);
}

static const struct bdevsw blk_bdevsw = {
	.d_open     = blk_dev_open,
	.d_close    = blk_dev_close,
	.d_strategy = blk_dev_strategy,
	.d_ioctl    = blk_dev_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = blk_dev_size,
	.d_type     = D_DISK,
};

static const struct cdevsw blk_cdevsw = {
	.d_open       = blk_dev_open,
	.d_close      = blk_dev_close,
	.d_read       = blk_dev_read,
	.d_write      = eno_rdwrt,
	.d_ioctl      = blk_dev_ioctl,
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

/* ---- bringing it up ---- */

static bool
blk_attach(struct blk_device *d, int slot)
{
	uint32_t cap_lo, cap_hi;

	if (kmem_alloc(kernel_map, &d->page, PAGE_SIZE,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	d->page_phys = kvtophys(d->page);
	d->desc = (struct vq_desc *)(d->page + OFFSET_DESC);
	d->avail = (struct vq_avail *)(d->page + OFFSET_AVAIL);
	d->used = (struct vq_used *)(d->page + OFFSET_USED);
	d->hdr = (struct blk_req_hdr *)(d->page + OFFSET_HDR);
	d->status = (volatile uint8_t *)(d->page + OFFSET_STATUS);

	reg_write(d, VIRTIO_STATUS, 0);         /* reset */
	reg_write(d, VIRTIO_STATUS, STATUS_ACKNOWLEDGE);
	reg_write(d, VIRTIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);
	/* nothing in the low half is wanted; bit 32 is VIRTIO_F_VERSION_1 */
	reg_write(d, VIRTIO_DRIVER_FEATURES_SEL, 0);
	reg_write(d, VIRTIO_DRIVER_FEATURES, 0);
	reg_write(d, VIRTIO_DRIVER_FEATURES_SEL, 1);
	reg_write(d, VIRTIO_DRIVER_FEATURES, 1);
	reg_write(d, VIRTIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
	if (!(reg_read(d, VIRTIO_STATUS) & STATUS_FEATURES_OK)) {
		printf("virtio-blk: slot %d: features refused\n", slot);
		return false;
	}

	cap_lo = reg_read(d, VIRTIO_CONFIG);
	cap_hi = reg_read(d, VIRTIO_CONFIG + 4);
	d->capacity = ((uint64_t)cap_hi << 32) | cap_lo;
	if (d->capacity == 0) {
		printf("virtio-blk: slot %d: the disk is empty\n", slot);
		return false;
	}

	reg_write(d, VIRTIO_QUEUE_SEL, 0);
	if (reg_read(d, VIRTIO_QUEUE_NUM_MAX) < QUEUE_SIZE) {
		printf("virtio-blk: slot %d: request queue of %u\n", slot,
		    reg_read(d, VIRTIO_QUEUE_NUM_MAX));
		return false;
	}
	reg_write(d, VIRTIO_QUEUE_NUM, QUEUE_SIZE);
	reg_write64(d, VIRTIO_QUEUE_DESC, d->page_phys + OFFSET_DESC);
	reg_write64(d, VIRTIO_QUEUE_DRIVER, d->page_phys + OFFSET_AVAIL);
	reg_write64(d, VIRTIO_QUEUE_DEVICE, d->page_phys + OFFSET_USED);
	__builtin_arm_dmb(0xb);
	reg_write(d, VIRTIO_QUEUE_READY, 1);
	reg_write(d, VIRTIO_STATUS,
	    STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
	return true;
}

void
redwood_virtio_blk_init(void)
{
	uint32_t enabled = 0;
	vm_offset_t bus;
	struct blk_device *d = &blk_dev;
	int slot;

	if (!PE_parse_boot_argn("virtio_blk", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	bus = ml_io_map(VIRTIO_BUS, VIRTIO_SLOTS * VIRTIO_SLOT_SIZE);
	for (slot = 0; slot < VIRTIO_SLOTS; slot++) {
		d->regs = bus + (vm_offset_t)slot * VIRTIO_SLOT_SIZE;
		if (reg_read(d, VIRTIO_MAGIC) != 0x74726976 ||
		    reg_read(d, VIRTIO_DEVICE_ID) != VIRTIO_ID_BLOCK) {
			continue;
		}
		if (reg_read(d, VIRTIO_VERSION) != 2) {
			printf("virtio-blk: slot %d is a legacy transport; start QEMU with "
			    "-global virtio-mmio.force-legacy=false\n", slot);
			continue;
		}
		lck_mtx_init(&d->lock, &blk_lck_grp, LCK_ATTR_NULL);
		if (!blk_attach(d, slot)) {
			lck_mtx_destroy(&d->lock, &blk_lck_grp);
			continue;
		}
		blk_bmajor = bdevsw_add(-1, &blk_bdevsw);
		if (blk_bmajor < 0) {
			printf("virtio-blk: bdevsw_add failed\n");
			return;
		}
		blk_cmajor = cdevsw_add_with_bdev(-1, &blk_cdevsw, blk_bmajor);
		if (blk_cmajor < 0) {
			printf("virtio-blk: cdevsw_add failed\n");
			return;
		}
		if (devfs_make_node(makedev(blk_bmajor, 0), DEVFS_BLOCK, UID_ROOT,
		    GID_OPERATOR, 0600, "rvblk0") == NULL ||
		    devfs_make_node(makedev(blk_cmajor, 0), DEVFS_CHAR, UID_ROOT,
		    GID_OPERATOR, 0600, "rrvblk0") == NULL) {
			printf("virtio-blk: devfs_make_node failed\n");
			return;
		}
		blk_ready = true;
		printf("virtio-blk: /dev/rvblk0 in slot %d, %llu sectors of %u (%llu MB)\n",
		    slot, d->capacity, VIRTIO_BLK_SECTOR,
		    d->capacity * VIRTIO_BLK_SECTOR / (1024 * 1024));
		return;
	}
	printf("virtio-blk: no block device on the bus\n");
}
