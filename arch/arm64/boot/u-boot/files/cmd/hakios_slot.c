// SPDX-License-Identifier: GPL-2.0+
/*
 * hakios slot: Qualcomm's A/B slots, as ABL keeps them: read, and the active slot's own state changed. Never which slot
 * is active.
 *
 * ABL boots the slot whose boot partition's GPT entry has the "active" bit (with the highest priority), and keeps the
 * slot's state in the other bits of the same attributes (ABL's PartitionTableUpdate.h): a priority, a retry count
 * that each boot of a slot not yet marked successful uses up, "successful" and "unbootable".
 *
 * WHICH slot is active is not these bits alone. ABL's SetActiveSlot() also swaps the type GUIDs of every _a/_b pair on
 * every unit (the active slot's partitions carry the real types, the other's 77036cd4-..., which the loaders before ABL
 * go by) and switches the UFS boot LUN (1 for a, 2 for b); at every boot ValidateSlotGuids() refuses a slot whose
 * boot_X and abl_X carry the same GUID, or whose LUN does not match, with "Device Error". An earlier version of this
 * changed the active bit alone: the bits and the GUIDs then disagreed, no ABL command could bring them together again
 * (each swaps both), and the phone started nothing until the unit's own table was written back (`fastboot flash
 * partition:4`, from tools/backup-partitions.py's copy). So only ABL and Android's boot control HAL change the active
 * slot. What this does is what ABL itself does to the active slot's bits: "successful" (every start of U-Boot, so that
 * ABL does not give up on it after seven boots), and "unbootable" (the menu's "Android": ABL then finds the active slot
 * unbootable and makes the other one active itself, HandleActiveSlotUnbootable(), GUIDs and LUN with it).
 *
 * What is written is the attributes of the active slot's boot entry, in both copies of the table of the disk it is on,
 * with the CRCs that cover them. Both copies are read and checked first and must agree; everything written is read back
 * and checked; no other byte of the table changes.
 */

#include <blk.h>
#include <command.h>
#include <dm.h>
#include <malloc.h>
#include <memalign.h>
#include <part.h>
#include <part_efi.h>
#include <sysreset.h>
#include <asm/io.h>
#include <power/pmic.h>
#include <u-boot/sha256.h>
#include <u-boot/crc.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <time.h>
#include <vsprintf.h>
#include "hakios.h"

#define SLOT_PRIORITY_SHIFT	48
#define SLOT_PRIORITY		(3ULL << SLOT_PRIORITY_SHIFT)
#define SLOT_ACTIVE		BIT_ULL(50)
#define SLOT_RETRY_SHIFT	51
#define SLOT_RETRY		(7ULL << SLOT_RETRY_SHIFT)
#define SLOT_SUCCESSFUL		BIT_ULL(54)
#define SLOT_UNBOOTABLE		BIT_ULL(55)

struct gpt_copy {
	lbaint_t lba;			/* of the header */
	gpt_header *h;			/* a block */
	gpt_entry *e;
	lbaint_t e_blocks;
};

struct slot_table {
	struct blk_desc *desc;
	struct gpt_copy copy[2];	/* primary, backup */
	int idx[2];			/* boot_a, boot_b */
};

static const char *const slot_part[2] = { "boot_a", "boot_b" };

static void gpt_free(struct gpt_copy *c)
{
	free(c->h);
	free(c->e);
	c->h = NULL;
	c->e = NULL;
}

static int gpt_read(struct blk_desc *desc, lbaint_t lba, struct gpt_copy *c)
{
	ulong bs = desc->blksz;
	u32 crc, bytes;

	c->lba = lba;
	c->h = memalign(ARCH_DMA_MINALIGN, bs);
	c->e = NULL;
	if (!c->h || blk_dread(desc, lba, 1, c->h) != 1)
		return -EIO;
	if (le64_to_cpu(c->h->signature) != GPT_HEADER_SIGNATURE_UBOOT ||
	    le32_to_cpu(c->h->header_size) < sizeof(gpt_header) || le32_to_cpu(c->h->header_size) > bs ||
	    le32_to_cpu(c->h->sizeof_partition_entry) != sizeof(gpt_entry) ||
	    /* (a backup written by ABL's `fastboot flash partition:N` says it is at LBA 1: ABL's PatchGpt() puts 1 there) */
	    (le64_to_cpu(c->h->my_lba) != lba && !(lba != 1 && le64_to_cpu(c->h->my_lba) == 1)))
		return -EINVAL;
	crc = le32_to_cpu(c->h->header_crc32);
	c->h->header_crc32 = 0;
	if (crc32(0, (void *)c->h, le32_to_cpu(c->h->header_size)) != crc)
		return -EBADMSG;
	c->h->header_crc32 = cpu_to_le32(crc);
	bytes = le32_to_cpu(c->h->num_partition_entries) * sizeof(gpt_entry);
	if (!bytes || bytes > 1024 * sizeof(gpt_entry))
		return -EINVAL;
	c->e_blocks = DIV_ROUND_UP(bytes, bs);
	c->e = memalign(ARCH_DMA_MINALIGN, c->e_blocks * bs);
	if (!c->e || blk_dread(desc, le64_to_cpu(c->h->partition_entry_lba), c->e_blocks, c->e) != c->e_blocks)
		return -EIO;
	if (crc32(0, (void *)c->e, bytes) != le32_to_cpu(c->h->partition_entry_array_crc32))
		return -EBADMSG;
	return 0;
}

static bool name_is(const gpt_entry *e, const char *name)
{
	int i;

	for (i = 0; name[i]; i++)
		if (i >= PARTNAME_SZ || le16_to_cpu(e->partition_name[i]) != (u8)name[i])
			return false;
	return i == PARTNAME_SZ || !e->partition_name[i];
}

static void slot_tables_free(struct slot_table *t)
{
	gpt_free(&t->copy[0]);
	gpt_free(&t->copy[1]);
}

/* the disk with boot_a and boot_b, both copies of its table read and checked and the same */
static int slot_tables_find(struct slot_table *t)
{
	struct udevice *dev;

	uclass_foreach_dev_probe(UCLASS_BLK, dev) {
		struct blk_desc *desc = dev_get_uclass_plat(dev);
		gpt_header *p;
		u32 n, bytes;

		memset(t, 0, sizeof(*t));
		t->desc = desc;
		if (gpt_read(desc, 1, &t->copy[0])) {
			slot_tables_free(t);
			continue;
		}
		p = t->copy[0].h;
		n = le32_to_cpu(p->num_partition_entries);
		t->idx[0] = t->idx[1] = -1;
		for (u32 i = 0; i < n; i++)
			for (int s = 0; s < 2; s++)
				if (name_is(&t->copy[0].e[i], slot_part[s]))
					t->idx[s] = i;
		if (t->idx[0] < 0 || t->idx[1] < 0) {
			slot_tables_free(t);
			continue;
		}
		if (gpt_read(desc, le64_to_cpu(p->alternate_lba), &t->copy[1])) {
			printf("hakios: %s: the backup table does not check out: not touched\n", dev->name);
			slot_tables_free(t);
			return -EBADMSG;
		}
		bytes = n * sizeof(gpt_entry);
		if (le32_to_cpu(t->copy[1].h->num_partition_entries) != n ||
		    memcmp(t->copy[0].e, t->copy[1].e, bytes)) {
			printf("hakios: %s: the two copies of the table differ: not touched\n", dev->name);
			slot_tables_free(t);
			return -EBADMSG;
		}
		return 0;
	}
	printf("hakios: no disk with boot_a and boot_b\n");
	return -ENODEV;
}

static u64 attr_get(const gpt_entry *e)
{
	return le64_to_cpu(e->attributes.raw);
}

static void attr_set(gpt_entry *e, u64 v)
{
	e->attributes.raw = cpu_to_le64(v);
}

static void slot_print(const struct slot_table *t)
{
	for (int s = 0; s < 2; s++) {
		u64 a = attr_get(&t->copy[0].e[t->idx[s]]);

		printf("slot %c: %s, priority %llu, retries %llu%s%s\n", 'a' + s, a & SLOT_ACTIVE ? "active" : "inactive",
		       (a & SLOT_PRIORITY) >> SLOT_PRIORITY_SHIFT, (a & SLOT_RETRY) >> SLOT_RETRY_SHIFT,
		       a & SLOT_SUCCESSFUL ? ", successful" : "", a & SLOT_UNBOOTABLE ? ", UNBOOTABLE" : "");
	}
}

/* both copies, entries then header, each read back */
static int slot_tables_write(struct slot_table *t)
{
	struct blk_desc *desc = t->desc;
	u32 bytes = le32_to_cpu(t->copy[0].h->num_partition_entries) * sizeof(gpt_entry);

	for (int c = 0; c < 2; c++) {
		struct gpt_copy *g = &t->copy[c];
		struct gpt_copy check = {};
		int ret;

		if (c)
			memcpy(g->e, t->copy[0].e, bytes);
		g->h->partition_entry_array_crc32 = cpu_to_le32(crc32(0, (void *)g->e, bytes));
		g->h->header_crc32 = 0;
		g->h->header_crc32 = cpu_to_le32(crc32(0, (void *)g->h, le32_to_cpu(g->h->header_size)));
		if (blk_dwrite(desc, le64_to_cpu(g->h->partition_entry_lba), g->e_blocks, g->e) != g->e_blocks ||
		    blk_dwrite(desc, g->lba, 1, g->h) != 1) {
			printf("hakios: writing the %s table failed\n", c ? "backup" : "primary");
			return -EIO;
		}
		ret = gpt_read(desc, g->lba, &check);
		if (!ret && memcmp(check.e, g->e, bytes))
			ret = -EIO;
		gpt_free(&check);
		if (ret) {
			printf("hakios: the %s table read back is not what was written (%d)\n", c ? "backup" : "primary",
			       ret);
			return ret;
		}
	}
	return 0;
}

int hakios_slot(int argc, char *const argv[])
{
	struct slot_table t;
	gpt_entry *e[2];
	u64 a[2], was[2];
	int ret, s;

	ret = slot_tables_find(&t);
	if (ret)
		return ret;
	e[0] = &t.copy[0].e[t.idx[0]];
	e[1] = &t.copy[0].e[t.idx[1]];
	if (argc < 2) {
		slot_print(&t);
		slot_tables_free(&t);
		return 0;
	}
	was[0] = a[0] = attr_get(e[0]);
	was[1] = a[1] = attr_get(e[1]);
	s = a[0] & SLOT_ACTIVE ? 0 : a[1] & SLOT_ACTIVE ? 1 : -1;
	if (s < 0 || (a[!s] & SLOT_ACTIVE)) {
		printf("hakios: not exactly one slot is active: nothing changed\n");
		slot_tables_free(&t);
		return -EINVAL;
	}
	if (!strcmp(argv[1], "successful")) {
		a[s] = (a[s] | SLOT_SUCCESSFUL) & ~SLOT_UNBOOTABLE;
	} else if (!strcmp(argv[1], "unbootable")) {
		/* as ABL's HandleActiveSlotUnbootable(): the next start, ABL makes the other slot active itself */
		if (!(a[!s] & SLOT_SUCCESSFUL) || (a[!s] & SLOT_UNBOOTABLE)) {
			printf("hakios: slot %c is not marked successful: ABL would have no slot to go to; nothing changed\n",
			       'a' + !s);
			slot_tables_free(&t);
			return -EINVAL;
		}
		a[s] = (a[s] | SLOT_UNBOOTABLE) & ~SLOT_SUCCESSFUL;
	} else {
		printf("hakios: which slot is active is ABL's to change (fastboot set_active) or Android's (bootctl)\n");
		slot_tables_free(&t);
		return -EINVAL;
	}
	if (a[0] == was[0] && a[1] == was[1]) {
		slot_print(&t);
		slot_tables_free(&t);
		return 0;
	}
	attr_set(e[0], a[0]);
	attr_set(e[1], a[1]);
	ret = slot_tables_write(&t);
	if (!ret) {
		printf("[oosexgang] hakios: slots changed\n");
		slot_print(&t);
	}
	slot_tables_free(&t);
	return ret;
}

/*
 * hakios sum PART [BLOCK COUNT]: the sha256 of a partition's blocks (all of them by default), read from the disk that
 * has it: what was flashed, checked from the phone itself, with nothing of Android's needed.
 */
int hakios_sum(int argc, char *const argv[])
{
	const ulong chunk = SZ_4M;
	struct disk_partition info;
	struct udevice *dev;
	struct blk_desc *desc = NULL;
	u8 digest[SHA256_SUM_LEN];
	sha256_context ctx;
	lbaint_t first, count, done = 0;
	ulong t0 = get_timer(0), ms;
	void *buf;

	if (argc != 2 && argc != 4)
		return -EINVAL;
	uclass_foreach_dev_probe(UCLASS_BLK, dev) {
		struct blk_desc *d = dev_get_uclass_plat(dev);

		part_init(d);		/* (a disk no command has looked at yet has no partition type) */
		if (part_get_info_by_name(d, argv[1], &info) >= 0) {
			desc = d;
			break;
		}
	}
	if (!desc) {
		printf("hakios: no partition %s\n", argv[1]);
		return -ENODEV;
	}
	first = argc == 4 ? simple_strtoul(argv[2], NULL, 0) : 0;
	count = argc == 4 ? simple_strtoul(argv[3], NULL, 0) : info.size;
	if (first > info.size || count > info.size - first) {
		printf("hakios: blocks %lu..+%lu are not inside %s (%lu blocks)\n", (ulong)first, (ulong)count, argv[1],
		       (ulong)info.size);
		return -EINVAL;
	}
	buf = memalign(ARCH_DMA_MINALIGN, chunk);
	if (!buf) {
		printf("hakios: no memory for a buffer of %lu bytes\n", chunk);
		return -ENOMEM;
	}
	sha256_starts(&ctx);
	while (done < count) {
		lbaint_t n = min_t(lbaint_t, count - done, chunk / info.blksz);

		if (blk_dread(desc, info.start + first + done, n, buf) != n) {
			printf("hakios: %s: reading block %lu failed\n", argv[1], (ulong)(first + done));
			free(buf);
			return -EIO;
		}
		sha256_update(&ctx, buf, n * info.blksz);
		done += n;
	}
	free(buf);
	sha256_finish(&ctx, digest);
	ms = max(get_timer(t0), 1UL);
	printf("[oosexgang] hakios: %s blocks %lu..%lu: sha256 ", argv[1], (ulong)first, (ulong)(first + count - 1));
	for (int i = 0; i < SHA256_SUM_LEN; i++)
		printf("%02x", digest[i]);
	printf(" (%lu MiB in %lu ms)\n", (ulong)((u64)count * info.blksz >> 20), ms);
	return 0;
}

/*
 * hakios reboot [bootloader]: restart the way Linux does on Qualcomm phones, saying why where ABL looks: the restart
 * reason in IMEM (0x146AA65C, downstream's qcom,msm-imem-restart_reason) and the reboot-mode cell of the PMK8350 (SDAM 2
 * + 0x48, bits 1..7, upstream's pmk8350.dtsi). U-Boot's own reset says nothing: a reason left there by an earlier
 * `reboot bootloader` stayed, and ABL went on into its fastboot instead of starting Android (seen on the phone).
 */
#define IMEM_RESTART_REASON	0x146AA65C
#define RESTART_NORMAL		0x77665501
#define RESTART_BOOTLOADER	0x77665500
#define REBOOT_REASON_REG	0x7148
#define REBOOT_BOOTLOADER	0x02

int hakios_reboot(bool bootloader)
{
	struct udevice *pmic;
	int v, ret;

	if (IS_ENABLED(CONFIG_ARCH_SNAPDRAGON))
		writel(bootloader ? RESTART_BOOTLOADER : RESTART_NORMAL, (void *)IMEM_RESTART_REASON);
	if (IS_ENABLED(CONFIG_ARCH_SNAPDRAGON) && IS_ENABLED(CONFIG_DM_PMIC)) {
		ret = uclass_get_device_by_name(UCLASS_PMIC, "pmic@0", &pmic);
		v = ret ? ret : pmic_reg_read(pmic, REBOOT_REASON_REG);
		if (v < 0 || pmic_reg_write(pmic, REBOOT_REASON_REG, (v & 1) | (bootloader ? REBOOT_BOOTLOADER << 1 : 0)))
			printf("hakios: the PMK8350's reboot reason was not written (%d)\n", v);
	}
	printf("[oosexgang] hakios: restarting%s\n", bootloader ? " into the bootloader's fastboot" : "");
	sysreset_walk_halt(SYSRESET_COLD);
	return -EIO;
}
