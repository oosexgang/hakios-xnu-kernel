/* Part of the loader (included by loader.c, not compiled alone): ufs=2: the kernel reads the phone's storage through the loader. */
/* ------------------------------------------------------------------------ */
/* UFS block service (ufs=2): the kernel reads the phone's storage through  */
/* this loader, and this loader through Qualcomm's own UFS driver           */

/*
 * The kernel has no driver for the phone's storage, and does not need one: UEFI never exits here,
 * Qualcomm's UFS driver (UFSDxe) has the controller, its PHY, clocks and power up and stays
 * resident, and this loader already serves the kernel from core 0 while the kernel runs on another
 * (the battery). So the kernel asks for blocks through a page both can see, and core 0 reads them
 * with that driver's ReadBlocks into a buffer both can see. The kernel's side is
 * bsd/dev/redwood_ufs.c; it gets the page's address as ufs_shared_pa= on its command line.
 *
 * Reads, and -- with ufs_write=1 on the command line -- writes into one partition and nowhere
 * else: the port's own darwin, proved by its GPT entry (name, both GUIDs, first and last LBA, its
 * logical unit), and in it only past everything the build writes from the computer, so that no
 * answer to the kernel can reach Android's partitions, the iOS volume or the cryptexes. The
 * refusals are here, in the kernel (bsd/dev/redwood_ufs.c) and in UEFI's own driver, which gives
 * out a partition's BlockIo and bounds it: a request has to pass all three.
 *
 * Each side writes its own cache lines only: the kernel the request, core 0 the answer (and the
 * table, once, before the kernel starts). A line one side cleans out of its cache therefore never
 * carries a stale copy of what the other side wrote into it. Into the data buffer core 0 writes
 * only what it has read back after a write of its own, so after the controller's DMA there is
 * nothing of its own in its cache to write back over the data; the kernel, which fills the buffer
 * for a write, cleans its own lines of it before it asks.
 */
#define BLOCK_MAGIC        0x4b434f4c42574452ULL       /* "RDWBLOCK" */
#define BLOCK_VERSION      3
#define BLOCK_MAX_PARTS    160
#define BLOCK_MAX_UNITS    16
#define BLOCK_PART_UNIT    1u                          /* the entry is a whole logical unit, not a partition */
#define BLOCK_PART_DSU     2u                          /* the DSU image: blocks mapped through its extents, read only */
#define BLOCK_OP_READ      0u
#define BLOCK_OP_WRITE     1u

/*
 * The one partition a write may reach, and from which of its blocks. darwin is the port's own
 * 20 GiB at the end of logical unit 0 (README, "The port's partition"); what the build puts there
 * from the computer (tools/darwin-write.py) is the iOS volume at its start and the two cryptexes
 * behind it, and nothing of this port reads or writes the rest -- so the data of iOS lives there
 * and a write is refused before that block. The identity is the whole of darwin-write.py's: the
 * same partition, named the same, of the same size, in the same place, on logical unit 0.
 */
#define BLOCK_WRITE_FIRST  2042368ULL                  /* darwin's blocks of 4096: past both cryptexes */
#define BLOCK_DARWIN_FIRST 25374720ULL                 /* its first and last LBA of unit 0, as the GPT has them */
#define BLOCK_DARWIN_LAST  30617594ULL
#define BLOCK_DARWIN_TYPE  { 0x7c3457ef, 0x0000, 0x11aa, { 0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac } }
#define BLOCK_DARWIN_UNIQ  { 0xfd6854ad, 0x24aa, 0x4fc3, { 0x94, 0xfe, 0x29, 0x01, 0x16, 0x85, 0x8d, 0x68 } }

/*
 * The partitions first, then the logical units they are on (BLOCK_PART_UNIT, named unit0, unit1,
 * ... in the order UEFI lists them). Each partition says which of those it is on and where it
 * begins there, so that a reader of the units themselves -- the kernel's own UFS driver -- finds
 * it; a unit says its logical unit number if its device path does.
 */
struct loader_block_part {
	char name[36];
	uint32_t block_size;
	uint64_t blocks;
	uint64_t start_lba;            /* where it begins on its unit, in the unit's blocks (0 for a unit) */
	uint16_t unit;                 /* the unit it is on, its number among the units; 0xffff: not known */
	uint8_t lun;                   /* a unit's logical unit number as its device path says; 0xff: it does not */
	uint8_t flags;                 /* BLOCK_PART_UNIT */
	uint32_t reserved;
};

struct loader_block {
	/* written by the loader, once, before the kernel starts */
	uint64_t magic;
	uint32_t version, size;
	uint32_t parts, buffer_size;
	uint64_t buffer_pa;
	/*
	 * Which partition takes writes this run, as its index among part[] plus one (0: none), and
	 * from which of its blocks. They are here, in what was padding before ask, so that the page
	 * stays the size and the version it was: one phone is shared between working copies, and a
	 * kernel built before writes existed must go on reading through a loader built after them,
	 * and the other way about -- a kernel that does not know these fields leaves ask.op zero,
	 * which is a read, and a loader that does not know them leaves these zero, which is no write.
	 */
	uint32_t write_part_plus_one;
	uint32_t write_reserved;
	uint64_t write_first;
	/* written by the kernel: what it wants, then request is advanced */
	struct {
		volatile uint32_t request;
		uint32_t part;
		uint64_t lba, count;           /* in the partition's own blocks */
		uint32_t op;                   /* BLOCK_OP_READ, BLOCK_OP_WRITE */
		uint32_t reserved;
	} ask __attribute__((aligned(128)));
	/* written by the loader: answered becomes request when the data is in the buffer */
	struct {
		volatile uint32_t answered;
		uint32_t reserved;
		int64_t status;                /* the EFI status of the read, 0 when it went well */
		uint64_t served, failed, bytes, ticks;
	} answer __attribute__((aligned(128)));
	struct loader_block_part part[BLOCK_MAX_PARTS] __attribute__((aligned(128)));
	/*
	 * Written by the loader before the kernel starts, with ufs_native= on the command line: where
	 * the kernel may put a transfer request list of its own into the UFS host controller for the
	 * run (hc_area, below), and UEFI's, which the loader puts back. hc_pa 0: it may not.
	 */
	struct {
		uint64_t hc_pa;                /* the controller's registers */
		uint64_t area_pa;              /* the list (1 KB, at the start) and command descriptors */
		uint32_t area_size, ucd_offset, ucd_size, slots;
		uint32_t uefi_utrlba, uefi_utrlbau;
	} native __attribute__((aligned(128)));
	/* written by the loader during a run: it takes the controller back (a hung kernel); send nothing more */
	struct {
		volatile uint32_t take_back;
	} loader_native __attribute__((aligned(128)));
	/* written by the kernel: its list is in the controller (for the loader's log) */
	struct {
		volatile uint32_t list_in;
		uint32_t switched_in, switched_out;
		volatile uint32_t cnr_at_switch;       /* UTRLCNR when the kernel's list went in */
	} kernel_native __attribute__((aligned(128)));
};

_Static_assert(__builtin_offsetof(struct loader_block, ask) == 128,
    "the page must stay the size it was: the fields before ask have to fit in what was padding");

/* (8 KB is as far as this target aligns; the kernel maps whole 16 KB pages around both) */
static struct loader_block block_shared __attribute__((aligned(0x2000)));
static void block_io2_probe(efi_handle h, efi_block_io *bio);
static int block_probe_requested;       /* ufs_probe=1: block_io2_probe() at block_start() */
static efi_block_io *block_bios[BLOCK_MAX_PARTS];
static int block_enabled;
static int block_write_requested;       /* ufs_write=1 on the command line */
static int block_write_part = -1;       /* the only partition a write may reach; -1: none, writes refused */
static int block_dsu_part = -1;         /* the DSU image's entry (BLOCK_PART_DSU); -1: none */
static uint64_t block_writes, block_writes_refused, block_write_bytes;

/* the bytes of a device path before its end node */
static uint64_t
device_path_bytes(const efi_device_path *dp)
{
	const uint8_t *p = (const uint8_t *)dp;
	uint64_t n = 0;

	for (int nodes = 0; nodes < 64; nodes++) {
		uint64_t len = p[n + 2] | (uint64_t)p[n + 3] << 8;

		if (p[n] == 0x7f || len < 4) {
			break;
		}
		n += len;
	}
	return n;
}

/*
 * A unit's device path, a node at a time ("ufs: unit N: path TYPE.SUBTYPE[data] ..."), and its
 * logical unit number if a UFS node (messaging, subtype 0x19: target, then LUN) says it.
 */
static uint8_t
device_path_print(uint64_t unit, const efi_device_path *dp)
{
	const uint8_t *p = (const uint8_t *)dp;
	uint64_t n = 0, total = device_path_bytes(dp);
	uint8_t lun = 0xff;

	print("ufs: unit %d: path", unit);
	while (n < total) {
		uint64_t len = p[n + 2] | (uint64_t)p[n + 3] << 8;

		print(" %x.%x[", (uint64_t)p[n], (uint64_t)p[n + 1]);
		for (uint64_t i = 4; i < len && i < 4 + 24; i++) {
			print("%s%x", (uint64_t)p[n + i] < 16 ? "0" : "", (uint64_t)p[n + i]);
		}
		print("%s]", len > 4 + 24 ? "..." : "");
		if (p[n] == 3 && p[n + 1] == 0x19 && len >= 6) {
			lun = p[n + 5];
		}
		n += len;
	}
	print(lun == 0xff ? "\n" : " (LUN %d)\n", (uint64_t)lun);
	return lun;
}

/*
 * The UFS host controller, for the kernel's own driver (ufs_native= on the command line; without it
 * this loader never touches the controller's registers). The kernel may, for the length of a run,
 * stop the controller's transfer request list, point it at a list of its own in hc_area (memory of
 * this loader, so that putting UEFI's back never depends on the kernel's memory), and start it
 * again; UEFI's list is one slot and its command descriptor, with no room for another request.
 * This loader keeps UEFI's list address (UTRLBA/UTRLBAU) from before the first run and checks it,
 * and puts it back if need be, on every way back from a run (hc_take_back) before anything here
 * touches the storage; it reads for the kernel (block_poll) and writes the log (write_logs) only
 * while UEFI's list is the one in the controller.
 */
#define HC_PA              (PLAT->ufs)         /* the SoC file's [platform] ufs: the device tree's ufshc node, "ufs_mem" */
#define HC_CAP             0x00
#define HC_VER             0x08
#define HC_IS              0x20
#define HC_IE              0x24
#define HC_HCS             0x30
#define HC_HCE             0x34
#define HC_UTRLBA          0x50
#define HC_UTRLBAU         0x54
#define HC_UTRLDBR         0x58
#define HC_UTRLCLR         0x5c
#define HC_UTRLRSR         0x60
#define HC_UTRLCNR         0x64
#define HC_UTMRLDBR        0x78
#define HC_SLOTS           32
#define HC_UCD_OFFSET      0x1000
#define HC_UCD_SIZE        0x800
#define HC_AREA_SIZE       (HC_UCD_OFFSET + HC_SLOTS * HC_UCD_SIZE)

static uint8_t hc_area[HC_AREA_SIZE] __attribute__((aligned(0x2000)));
static int hc_requested;                /* ufs_native= on the command line */
static int hc_known;                    /* UEFI's list address is kept here: check it, put it back */
static uint32_t hc_uefi_lo, hc_uefi_hi;
static int hc_refused_reads;

static inline uint32_t
hc_rd(uint32_t off)
{
	return *(volatile uint32_t *)(uintptr_t)(HC_PA + off);
}

static inline void
hc_wr(uint32_t off, uint32_t v)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(uintptr_t)(HC_PA + off) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static uint64_t
hc_ms_since(uint64_t then)
{
	return (READ_SYSREG(CNTVCT_EL0) - then) * 1000 / READ_SYSREG(CNTFRQ_EL0);
}

/* UEFI's list in, and running: what UEFI's driver needs to read or write */
static int
hc_list_is_uefis(void)
{
	return !hc_known || (hc_rd(HC_UTRLBA) == hc_uefi_lo && hc_rd(HC_UTRLBAU) == hc_uefi_hi &&
	       (hc_rd(HC_UTRLRSR) & 1) != 0);
}

static void
hc_print_state(const char *when)
{
	print("ufs: %s: list %x:%x (UEFI's %x:%x), run %x, doorbell %x, HCS %x, IS %x, IE %x, completion %x, "
	    "task doorbell %x\n", when, (uint64_t)hc_rd(HC_UTRLBAU), (uint64_t)hc_rd(HC_UTRLBA), (uint64_t)hc_uefi_hi,
	    (uint64_t)hc_uefi_lo, (uint64_t)hc_rd(HC_UTRLRSR), (uint64_t)hc_rd(HC_UTRLDBR), (uint64_t)hc_rd(HC_HCS),
	    (uint64_t)hc_rd(HC_IS), (uint64_t)hc_rd(HC_IE), (uint64_t)hc_rd(HC_UTRLCNR), (uint64_t)hc_rd(HC_UTMRLDBR));
}

/*
 * Whatever the kernel left: UEFI's list back in the controller. The kernel is told to send nothing
 * more (it looks before every doorbell, with interrupts off), given 50 ms, then what is in flight
 * is waited for (1 s), cleared if it does not finish (UTRLCLR: every slot of that list is the
 * kernel's), the list stopped, UEFI's address written back, the list started, all read back.
 */
static void
hc_take_back(const char *when)
{
	uint32_t lo, hi, rsr, db;
	uint64_t t;

	if (!hc_known) {
		return;
	}
	lo = hc_rd(HC_UTRLBA);
	hi = hc_rd(HC_UTRLBAU);
	rsr = hc_rd(HC_UTRLRSR);
	db = hc_rd(HC_UTRLDBR);
	if (lo == hc_uefi_lo && hi == hc_uefi_hi && (rsr & 1)) {
		print("ufs: %s: UEFI's transfer list is in the controller (%x), running, doorbell %x\n", when, (uint64_t)lo,
		    (uint64_t)db);
		return;
	}
	print("ufs: %s: the controller has list %x:%x (run %x, doorbell %x), not UEFI's %x:%x: taking it back\n", when,
	    (uint64_t)hi, (uint64_t)lo, (uint64_t)rsr, (uint64_t)db, (uint64_t)hc_uefi_hi, (uint64_t)hc_uefi_lo);
	block_shared.loader_native.take_back = 1;
	clean_dcache((uint64_t)(uintptr_t)&block_shared.loader_native,
	    (uint64_t)(uintptr_t)&block_shared.loader_native + sizeof(block_shared.loader_native));
	for (t = READ_SYSREG(CNTVCT_EL0); hc_ms_since(t) < 50;) {
	}
	for (t = READ_SYSREG(CNTVCT_EL0); (db = hc_rd(HC_UTRLDBR)) != 0 && hc_ms_since(t) < 1000;) {
	}
	if (db != 0) {
		print("ufs: %s: doorbell %x still set after 1 s\n", when, (uint64_t)db);
		if (hc_rd(HC_UTRLBA) != hc_uefi_lo || hc_rd(HC_UTRLBAU) != hc_uefi_hi) {
			print("ufs: %s: clearing the kernel's slots (UTRLCLR)\n", when);
			hc_wr(HC_UTRLCLR, 0);
			for (t = READ_SYSREG(CNTVCT_EL0); (db = hc_rd(HC_UTRLDBR)) != 0 && hc_ms_since(t) < 1000;) {
			}
			print("ufs: %s: doorbell %x after the clear\n", when, (uint64_t)db);
		}
	}
	/* the completion notifications the kernel's requests left (it says what there was when its list went in) */
	flush_dcache((uint64_t)(uintptr_t)&block_shared.kernel_native,
	    (uint64_t)(uintptr_t)&block_shared.kernel_native + sizeof(block_shared.kernel_native));
	if (block_shared.kernel_native.switched_in != 0) {
		uint32_t cnr = hc_rd(HC_UTRLCNR), ours = cnr & ~block_shared.kernel_native.cnr_at_switch;

		if (ours != 0) {
			hc_wr(HC_UTRLCNR, ours);
		}
		print("ufs: %s: completion %x, at the kernel's switch %x: cleared %x, now %x\n", when, (uint64_t)cnr,
		    (uint64_t)block_shared.kernel_native.cnr_at_switch, (uint64_t)ours, (uint64_t)hc_rd(HC_UTRLCNR));
	}
	hc_wr(HC_UTRLRSR, 0);
	for (t = READ_SYSREG(CNTVCT_EL0); (hc_rd(HC_UTRLRSR) & 1) && hc_ms_since(t) < 100;) {
	}
	hc_wr(HC_UTRLBA, hc_uefi_lo);
	hc_wr(HC_UTRLBAU, hc_uefi_hi);
	hc_wr(HC_UTRLRSR, 1);
	for (t = READ_SYSREG(CNTVCT_EL0); hc_ms_since(t) < 50;) {
	}
	hc_print_state(when);
	print("ufs: %s: UEFI's list %s\n", when, hc_list_is_uefis() ? "put back" : "NOT PUT BACK: the storage is not used");
}

/*
 * Before a run (ufs_native=): what the controller looks like, and UEFI's list address kept -- the
 * first time; later it must be the same (or this loader's own area, which is then taken back).
 */
static int
hc_init(void)
{
	uint32_t lo, hi, area = (uint32_t)(uintptr_t)hc_area;

	if (!hc_requested) {
		return 0;
	}
	if (HC_PA == 0) {
		print("ufs: native: this board's SoC file does not say where the UFS host controller is ([platform] ufs)\n");
		return 0;
	}
	lo = hc_rd(HC_UTRLBA);
	hi = hc_rd(HC_UTRLBAU);
	print("ufs: controller at %x: CAP %x, VER %x, HCE %x, HCS %x, IS %x, IE %x, list %x:%x, run %x, doorbell %x\n",
	    HC_PA, (uint64_t)hc_rd(HC_CAP), (uint64_t)hc_rd(HC_VER), (uint64_t)hc_rd(HC_HCE), (uint64_t)hc_rd(HC_HCS),
	    (uint64_t)hc_rd(HC_IS), (uint64_t)hc_rd(HC_IE), (uint64_t)hi, (uint64_t)lo, (uint64_t)hc_rd(HC_UTRLRSR),
	    (uint64_t)hc_rd(HC_UTRLDBR));
	if (!hc_known) {
		if (hi == 0 && lo == area) {
			print("ufs: the controller has this loader's own area for its list before any run: left alone\n");
			return 0;
		}
		hc_uefi_lo = lo;
		hc_uefi_hi = hi;
		hc_known = 1;
		print("ufs: UEFI's transfer list is at %x:%x; it is put back after every run\n", (uint64_t)hi, (uint64_t)lo);
	} else if (lo != hc_uefi_lo || hi != hc_uefi_hi) {
		if (hi == 0 && lo == area) {
			hc_take_back("before the run");
		} else {
			print("ufs: UEFI's list moved from %x:%x to %x:%x: that is UEFI's now\n", (uint64_t)hc_uefi_hi,
			    (uint64_t)hc_uefi_lo, (uint64_t)hi, (uint64_t)lo);
			hc_uefi_lo = lo;
			hc_uefi_hi = hi;
		}
	}
	return hc_list_is_uefis();
}

/*
 * The firmware's configuration tables, by GUID (an ACPI or device tree table among them would say
 * where the storage controller is).
 */
static void
block_tables(void)
{
	for (uint64_t i = 0; ST->tables != NULL && i < ST->nr_tables && i < 32; i++) {
		const efi_guid *g = &ST->tables[i].guid;

		print("ufs: firmware table %d: %x-%x-%x-%x%x-%x%x%x%x%x%x at %x\n", i, (uint64_t)g->a, (uint64_t)g->b,
		    (uint64_t)g->c, (uint64_t)g->d[0], (uint64_t)g->d[1], (uint64_t)g->d[2], (uint64_t)g->d[3],
		    (uint64_t)g->d[4], (uint64_t)g->d[5], (uint64_t)g->d[6], (uint64_t)g->d[7],
		    (uint64_t)(uintptr_t)ST->tables[i].table);
	}
}

/*
 * Is this partition the one a write may reach? Everything about it has to be what it was when the
 * port was set up; one field out of place and nothing is written this run. The name UEFI reports
 * is already in bp->name (ufs_name), in ASCII.
 */
static int
block_is_darwin(const efi_partition_info *info, const efi_block_io *bio, const struct loader_block_part *bp)
{
	static const efi_guid type = BLOCK_DARWIN_TYPE, uniq = BLOCK_DARWIN_UNIQ;
	static const char name[] = "darwin";
	const char *why = NULL;

	for (unsigned i = 0; i < sizeof(name); i++) {
		if (bp->name[i] != name[i]) {
			return 0;               /* another partition: nothing to say about it */
		}
	}
	if (memcmp(&info->type_guid, &type, sizeof(type)) != 0) {
		why = "its type GUID is not the one it had";
	} else if (memcmp(&info->unique_guid, &uniq, sizeof(uniq)) != 0) {
		why = "its unique GUID is not the one it had";
	} else if (info->start_lba != BLOCK_DARWIN_FIRST || info->end_lba != BLOCK_DARWIN_LAST) {
		why = "it does not begin and end where it did";
	} else if (bp->unit != 0) {
		/* the first unit UEFI lists: this firmware's device paths say no logical unit number
		 * (device_path_print finds no 3.19 node), so bp->lun is 0xff for every unit */
		why = "it is not on the first logical unit";
	} else if (bp->block_size != 4096 || bp->blocks != BLOCK_DARWIN_LAST - BLOCK_DARWIN_FIRST + 1) {
		why = "its blocks are not what they were";
	} else if (bio->media->read_only) {
		why = "UEFI gives it out read only";
	}
	if (why != NULL) {
		print("ufs: darwin: no write will be served: %s\n", why);
		return 0;
	}
	print("ufs: darwin (LBA %d..%d of unit 0): its blocks from %d on may be written this run\n",
	    info->start_lba, info->end_lba, BLOCK_WRITE_FIRST);
	return 1;
}

static efi_block_io *
partition_named(const char *want)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count;

	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		return NULL;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		char name[40];
		unsigned k = 0;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2 ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
			continue;
		}
		ufs_name(info, name, sizeof(name));
		while (want[k] != 0 && name[k] == want[k]) {
			k++;
		}
		if (want[k] == 0 && name[k] == 0) {
			return bio;
		}
	}
	return NULL;
}
