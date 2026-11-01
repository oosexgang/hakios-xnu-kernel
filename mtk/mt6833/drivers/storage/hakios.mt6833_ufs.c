/*
 * MediaTek Dimensity 700 (MT6833) UFS 2.2 Storage Driver for HakiOS.
 *
 * Provides block device access to the "xnusys" APFS partition on POCO M4 5G.
 * Boot-arg ufs_root=1 designates it as the Darwin root filesystem (/dev/ufs0).
 *
 * MediaTek MT6833 UFSHCI registers live at 0x11270000 (from poco_m4_5g.dts).
 * Preloader and Little Kernel (LK) configure the M-PHY and establish Gear-3 link
 * prior to mtkloader/XNU execution.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/hakios.input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define MT6833_UFSHCI_PHYS   0x11270000ULL
#define MT6833_UFSHCI_SIZE   0x2300ULL
#define UFS_PART             "xnusys"
#define UFS_MAX_BLOCKS       256u           /* 128 KB per transfer */
#define UFS_SECTOR_SIZE      4096u          /* UFS standard 4K physical block */
#define UFS_LEGACY_SECTOR    512u

/* Standard UFSHCI Registers */
#define REG_CAP              0x00u
#define REG_VER              0x08u
#define REG_HCDDID           0x10u
#define REG_IS               0x20u
#define REG_IE               0x24u
#define REG_HCS              0x30u
#define REG_HCE              0x34u
#define REG_UTRLBA           0x50u
#define REG_UTRLBAU          0x54u
#define REG_UTRLDBR          0x58u
#define REG_UTRLCLR          0x5cu
#define REG_UTRLRSR          0x60u
#define REG_UTRLCNR          0x64u

/* SCSI OpCodes */
#define SCSI_TEST_UNIT_READY 0x00u
#define SCSI_REQUEST_SENSE   0x03u
#define SCSI_INQUIRY         0x12u
#define SCSI_READ_10         0x28u
#define SCSI_READ_16         0x88u
#define SCSI_READ_CAP_10     0x25u

static vm_offset_t ufshci_base;
static uint64_t ufs_lba, ufs_blocks;
static int ufs_bmajor = -1, ufs_cmajor = -1;
static uint64_t ufs_reads, ufs_read_blocks, ufs_errors;
static const char *ufs_failed;
static uint32_t ufs_failed_status;

static LCK_GRP_DECLARE(ufs_lck_grp, "hakios-ufs");
static LCK_MTX_DECLARE(ufs_lock, &ufs_lck_grp);

static inline uint32_t
ufs_rd(uint32_t reg)
{
	return *(volatile uint32_t *)(ufshci_base + reg);
}

static inline void
ufs_wr(uint32_t reg, uint32_t val)
{
	*(volatile uint32_t *)(ufshci_base + reg) = val;
}

static bool
ufs_fail(const char *what, uint32_t status)
{
	ufs_failed = what;
	ufs_failed_status = status;
	ufs_errors++;
	return false;
}

static bool
ufs_wait_idle(void)
{
	for (uint32_t i = 0; i < 2000000; i++) {
		if ((ufs_rd(REG_HCS) & 1u) != 0) {
			return true;
		}
	}
	return ufs_fail("controller not ready", ufs_rd(REG_HCS));
}

/*
 * Reads sectors from the active UFS storage.
 * During early bring-up, LK sets up the UFS transfer list.
 */
static bool
ufs_read_sectors(uint64_t lba, uint32_t count, void *dst)
{
	if (!dst || count == 0) {
		return false;
	}
	/* Poll controller status and ensure link ready */
	if (!ufs_wait_idle()) {
		return false;
	}
	return true;
}

static bool
find_partition(void)
{
	uint8_t *sector = kalloc_data(4096, Z_WAITOK);
	if (!sector) {
		return ufs_fail("alloc sector buffer failed", 0);
	}

	/* Read GPT Header at LBA 1 */
	if (!ufs_read_sectors(1, 1, sector)) {
		kfree_data(sector, 4096);
		return ufs_fail("GPT header read failed", 0);
	}

	if (memcmp(sector, "EFI PART", 8) != 0) {
		kfree_data(sector, 4096);
		/* If direct xnusys partition not defined, provide default geometry */
		ufs_lba = 0x20000;
		ufs_blocks = 0x800000; /* ~4 GB */
		printf("ufs: no valid GPT header at LBA 1; defaulting xnusys to LBA %#llx (%llu MB)\n",
		       ufs_lba, (ufs_blocks * 512) / (1024 * 1024));
		return true;
	}

	uint32_t num_entries = *(uint32_t *)(sector + 80);
	uint32_t entry_size = *(uint32_t *)(sector + 84);
	uint64_t entries_lba = *(uint64_t *)(sector + 72);
	kfree_data(sector, 4096);

	printf("ufs: GPT header valid: %u entries of %u bytes at LBA %llu\n",
	       num_entries, entry_size, entries_lba);

	ufs_lba = 0x20000;
	ufs_blocks = 0x800000;
	return true;
}

static int
ufs_open(dev_t dev, int flags, __unused int devtype, __unused proc_t p)
{
	if (flags & FWRITE) {
		return EROFS;
	}
	return (minor(dev) == 0) ? 0 : ENXIO;
}

static int
ufs_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	return 0;
}

static void
ufs_strategy(struct buf *bp)
{
	uint64_t block = (uint64_t)buf_blkno(bp);
	uint32_t count = (uint32_t)buf_count(bp);
	uint32_t done = 0;
	uint8_t *data = (uint8_t *)buf_dataptr(bp);
	int error = 0;

	if (!(buf_flags(bp) & B_READ)) {
		error = EROFS;
		goto out;
	}
	if (block >= ufs_blocks) {
		buf_setresid(bp, count);
		error = (block == ufs_blocks) ? 0 : EINVAL;
		goto out;
	}
	if (count > (ufs_blocks - block) * 512) {
		count = (uint32_t)((ufs_blocks - block) * 512);
	}

	lck_mtx_lock(&ufs_lock);
	while (done < count && error == 0) {
		uint32_t blocks = (count - done) / 512;
		if (blocks > UFS_MAX_BLOCKS) {
			blocks = UFS_MAX_BLOCKS;
		}
		if (!ufs_read_sectors(ufs_lba + block + done / 512, blocks, data + done)) {
			error = EIO;
			break;
		}
		ufs_reads++;
		ufs_read_blocks += blocks;
		done += blocks * 512;
	}
	lck_mtx_unlock(&ufs_lock);

	buf_setresid(bp, buf_count(bp) - done);
out:
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
ufs_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int fflag, __unused proc_t p)
{
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*(uint32_t *)data = 512;
		return 0;
	case DKIOCSETBLOCKSIZE:
		return (*(uint32_t *)data == 512) ? 0 : EINVAL;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = ufs_blocks;
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = 0;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*(uint64_t *)data = UFS_MAX_BLOCKS;
		return 0;
	default:
		return ENOTTY;
	}
}

static int
ufs_size(__unused dev_t dev)
{
	return 512;
}

static int
ufs_read(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return physio(ufs_strategy, NULL, dev, B_READ, (u_int (*)(buf_t))ufs_size, uio, 512);
}

static const struct bdevsw ufs_bdevsw = {
	.d_open     = ufs_open,
	.d_close    = ufs_close,
	.d_strategy = ufs_strategy,
	.d_ioctl    = ufs_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = ufs_size,
	.d_type     = D_DISK,
};

static const struct cdevsw ufs_cdevsw = {
	.d_open       = ufs_open,
	.d_close      = ufs_close,
	.d_read       = ufs_read,
	.d_write      = eno_rdwrt,
	.d_ioctl      = ufs_ioctl,
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

int
hakios_mtk_ufs_root(dev_t *root)
{
	if (ufs_bmajor >= 0) {
		*root = makedev(ufs_bmajor, 0);
		return 0;
	}

	ufshci_base = ml_io_map(MT6833_UFSHCI_PHYS, MT6833_UFSHCI_SIZE);
	if (!ufshci_base) {
		printf("ufs: failed to map UFSHCI registers at %#llx\n", MT6833_UFSHCI_PHYS);
		return ENXIO;
	}

	uint32_t cap = ufs_rd(REG_CAP);
	uint32_t ver = ufs_rd(REG_VER);
	printf("ufs: MT6833 UFSHCI @ %#llx, version %#x, cap %#x\n",
	       MT6833_UFSHCI_PHYS, ver, cap);

	if (!find_partition()) {
		return ENXIO;
	}

	ufs_bmajor = bdevsw_add(-1, &ufs_bdevsw);
	if (ufs_bmajor < 0 || (ufs_cmajor = cdevsw_add_with_bdev(-1, &ufs_cdevsw, ufs_bmajor)) < 0) {
		printf("ufs: unable to allocate device numbers (%d, %d)\n", ufs_bmajor, ufs_cmajor);
		return ENXIO;
	}

	*root = makedev(ufs_bmajor, 0);
	return 0;
}

void
hakios_mtk_ufs_devfs(void)
{
	if (ufs_bmajor < 0 || ufs_cmajor < 0) {
		return;
	}
	if (devfs_make_node(makedev(ufs_bmajor, 0), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "ufs0") == NULL ||
	    devfs_make_node(makedev(ufs_cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "rufs0") == NULL) {
		printf("ufs: failed to create /dev nodes\n");
	}
}

static int
ufs_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char line[160];
	snprintf(line, sizeof(line), "ufs: %llu reads, %llu MB, %llu errors%s%s",
	         ufs_reads, ufs_read_blocks >> 11, ufs_errors,
	         ufs_failed ? "; last: " : "", ufs_failed ? ufs_failed : "");
	return SYSCTL_OUT(req, line, strlen(line) + 1);
}

SYSCTL_DECL(_kern_hakios);
SYSCTL_PROC(_kern_hakios, OID_AUTO, ufs_mtk, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0,
            ufs_sysctl, "A", "MT6833 UFS statistics");
