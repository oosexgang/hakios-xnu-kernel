/*
 * The phone's storage: one block device and one character device for every GPT partition of its
 * UFS (/dev/ufs_NAME and /dev/rufs_NAME, NAME as the partition table has it). Read only, but for
 * the one place the data of iOS lives (ufs_write=1, below).
 *
 * There is no UFS driver here, and none is needed. UEFI never exits on this port: Qualcomm's own
 * UFS driver (UFSDxe) has brought up the controller, its PHY, its clocks and power, and stays
 * resident, and xnuloader keeps serving the kernel from core 0 while the kernel runs on another
 * core (it already does for the battery). So with ufs=2 on its command line the loader publishes a
 * page (ufs_shared_pa=) with the table of partitions, a request and an answer, and a buffer; this
 * driver writes a request -- which partition, which blocks -- and core 0 reads them with that
 * driver's ReadBlocks into the buffer (xnuloader/loader.c, block_poll). Nothing in this kernel
 * touches the storage controller.
 *
 * Read only wherever a write could do harm, by construction. The block devices never write: the
 * strategy routine refuses, so no filesystem mounted from one can write, and opening one for
 * writing is refused. With ufs_write=1 one character device alone takes writes -- /dev/rufs_darwin,
 * the port's own partition -- and in it only whole blocks from UFS_WRITE_FIRST on, past the iOS
 * volume and both cryptexes that the build writes from the computer (tools/darwin-write.py); that
 * is where the data volume of iOS is kept between runs (userland/src/var_store.c). The partition
 * has to be the one the port was set up with (its name, size, place and logical unit here; its
 * GUIDs as well on the loader's side), and the loader refuses the same things over again and reads
 * every write back before it answers. Without ufs_write=1 nothing writes at all.
 *
 * One request at a time (the page has one), so requests are serialized by a mutex and split into
 * pieces of the buffer's size. The kernel writes only the request's cache lines and core 0 only
 * the answer's (see the loader), both through a mapping that does not cache here, so neither side
 * ever overwrites the other's. The data buffer, which only core 0's reads fill, is mapped cacheable
 * and the part of it an answer covers is invalidated here before it is copied out.
 *
 * ufs_check=1 reads, once at start, the partitions there are images of in the ROM's
 * firmware-update/ (bluetooth, xbl, tz, ...) through the same path and prints an FNV-1a hash of
 * as many bytes as each image has: tools/ufs-survey.py compares those with the images and with
 * what the loader read itself before the kernel started.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <os/atomic_private.h>
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
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <sys/redwood_input.h>
#include <libkern/crc.h>
#include <vm/memory_types.h>
#include "redwood_ufshc.h"

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void IOSleep(unsigned int milliseconds);
extern void loader_return(void);        /* osfmk/arm64/loader_return.c */
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);   /* osfmk/arm64/caches_asm.s */

/* the page the loader publishes: the same layout as struct loader_block in xnuloader/loader.c */
#define BLOCK_MAGIC        0x4b434f4c42574452ULL       /* "RDWBLOCK" */
#define BLOCK_VERSION      3
#define BLOCK_MAX_PARTS    160
#define BLOCK_MAX_UNITS    16
#define BLOCK_PART_UNIT    1u                          /* a whole logical unit, not a partition */
#define BLOCK_OP_READ      0u
#define BLOCK_OP_WRITE     1u

/*
 * Where a write may go, and nowhere else: darwin, the port's own 20 GiB at the end of logical
 * unit 0, and in it only past what the build puts there from the computer -- the iOS volume
 * (blocks 0..1374207) and the two cryptexes behind it (..2042367). The loader keeps the same
 * bounds and the GUIDs of the partition besides (xnuloader/loader.c, BLOCK_WRITE_FIRST).
 */
#define UFS_WRITE_FIRST    2042368ULL                  /* darwin's blocks of 4096 */
#define UFS_DARWIN_FIRST   25374720ULL                 /* its first LBA on the unit, as the GPT has it */
#define UFS_DARWIN_BLOCKS  5242875ULL

struct loader_block_part {
	char name[36];
	uint32_t block_size;
	uint64_t blocks;
	uint64_t start_lba;            /* where it begins on its unit, in the unit's blocks */
	uint16_t unit;                 /* the unit it is on, its number among the units; 0xffff: not known */
	uint8_t lun;                   /* a unit's logical unit number as its device path says; 0xff: it does not */
	uint8_t flags;                 /* BLOCK_PART_UNIT */
	uint32_t reserved;
};

struct loader_block {
	uint64_t magic;
	uint32_t version, size;
	uint32_t parts, buffer_size;
	uint64_t buffer_pa;
	/*
	 * Which partition the loader will take writes for this run (its index plus one; 0: none) and
	 * from which of its blocks. They sit in what was padding, so the page is the size and the
	 * version it was and one phone goes on being shared: a loader built before writes existed
	 * leaves them zero, which is no write, and a kernel built before them leaves ask.op zero,
	 * which is a read.
	 */
	uint32_t write_part_plus_one;
	uint32_t write_reserved;
	uint64_t write_first;
	struct {
		volatile uint32_t request;
		uint32_t part;
		uint64_t lba, count;
		uint32_t op;                   /* BLOCK_OP_READ, BLOCK_OP_WRITE */
		uint32_t reserved;
	} ask __attribute__((aligned(128)));
	struct {
		volatile uint32_t answered;
		uint32_t reserved;
		int64_t status;
		uint64_t served, failed, bytes, ticks;
	} answer __attribute__((aligned(128)));
	struct loader_block_part part[BLOCK_MAX_PARTS] __attribute__((aligned(128)));
	/* the loader's offer of an area for this kernel's own transfer request list, and UEFI's list */
	struct {
		uint64_t hc_pa;
		uint64_t area_pa;
		uint32_t area_size, ucd_offset, ucd_size, slots;
		uint32_t uefi_utrlba, uefi_utrlbau;
	} native __attribute__((aligned(128)));
	struct {
		volatile uint32_t take_back;   /* the loader's: it takes the controller back, send nothing more */
	} loader_native __attribute__((aligned(128)));
	struct {
		volatile uint32_t list_in;     /* this kernel's: its list is in */
		uint32_t switched_in, switched_out;
		volatile uint32_t cnr_at_switch;       /* UTRLCNR when its list went in (for the loader's take-back) */
	} kernel_native __attribute__((aligned(128)));
};

_Static_assert(__builtin_offsetof(struct loader_block, ask) == 128,
    "the loader's page must stay the size it was: what is before ask has to fit in its padding");

#define UFS_PAGE           0x4000UL
#define UFS_TIMEOUT_MS     20000        /* core 0 answers in well under a second; this is for a dead loader */

static volatile struct loader_block *ufs_page;
static uint8_t *ufs_buffer;             /* the loader's data buffer: filled by it for a read, here for a write */
static uint32_t ufs_buffer_size, ufs_parts, ufs_seq;
static uint32_t ufs_bsize[BLOCK_MAX_PARTS];   /* the block size each device is counted in (spec_open sets 512) */
static int ufs_bmajor = -1, ufs_cmajor = -1;
static bool ufs_ready, ufs_dead;
static uint64_t ufs_requests, ufs_errors, ufs_bytes, ufs_wait_us;
static uint64_t ufs_copy_us;                    /* copying out of the loader's buffer */
static uint64_t ufs_small, ufs_medium, ufs_large; /* requests of <= 4 KB, <= 64 KB, more */
static uint64_t ufs_sequential;                 /* requests that start where the one before ended */
static uint32_t ufs_last_part = ~0u;
static uint64_t ufs_last_end;

/*
 * This kernel's own UFS driver (redwood_ufshc.c), with ufs_native=1: each partition's logical
 * unit and where it begins there, in bytes; reads go that way unless it fails, and then through
 * the loader again. While its list is in the controller the loader is asked for nothing.
 */
static uint8_t ufs_lun[BLOCK_MAX_PARTS];         /* 0xff: not known, read through the loader */
static uint64_t ufs_lun_offset[BLOCK_MAX_PARTS];
static bool ufs_native_mapped;
static uint32_t ufs_native_luns;                /* how many units are mapped (LUN 0..n-1) */
static volatile bool ufs_native_on;
static uint64_t ufs_native_fallbacks, ufs_mailbox_refused;

static LCK_GRP_DECLARE(ufs_lck_grp, "redwood-ufs");
static LCK_MTX_DECLARE(ufs_lock, &ufs_lck_grp);
/* One writer at a time; never held while the mailbox is, nor while user memory is touched. */
static LCK_MTX_DECLARE(ufs_write_mtx, &ufs_lck_grp);

/*
 * Saving the data volume without being asked (userland/src/var_store.c).
 *
 * The volume is packed and written by a program, not by this kernel, so the kernel's part is to
 * say when and to let only one program at a time do it:
 *
 *   kern.redwood.var_save   the handshake. The saver writes VAR_SAVE_IDLE when it starts and is
 *                           then the one that will be asked; on the orderly way back to the loader
 *                           (loader_return_after expired, or "xl stop") the kernel writes
 *                           VAR_SAVE_WANTED and waits, and the saver writes VAR_SAVE_DONE when the
 *                           volume is in the flash. Nobody registered: nothing is asked and
 *                           nothing waited for, so a run without a saver returns as it always did.
 *                           A panic does not come this way at all -- there is no program left to
 *                           ask -- and then the store simply keeps what it had.
 *   kern.redwood.var_busy   the claim. A write to /dev/rufs_darwin is refused unless the writer
 *                           holds it, so two savers cannot interleave into one slot. It is held by
 *                           process number, and a claimant may take it from a process that is gone
 *                           (a saver killed in the middle leaves no lock behind for the rest of the
 *                           run). What the one that was killed half-wrote is not lost either: the
 *                           slot it was writing is not the one holding the newest good archive.
 */
#define VAR_SAVE_NONE      0
#define VAR_SAVE_IDLE      1
#define VAR_SAVE_WANTED    2
#define VAR_SAVE_DONE      3
#define VAR_SAVE_MS        20000        /* at most this long on the way out (var_save_ms=N) */

static volatile int ufs_var_save = VAR_SAVE_NONE;
static int ufs_var_busy_pid;

/* ufs_write=1 and the one partition it is for: everything else answers EROFS as it always did */
static int ufs_write_part = -1;
static volatile bool ufs_write_busy;            /* a write of ours has this kernel's list out: not a failure */
static bool ufs_write_out, ufs_write_was_native;        /* the list is out for writing, and was in before */
static uint64_t ufs_writes, ufs_write_bytes, ufs_write_errors, ufs_write_refused;

SYSCTL_DECL(_kern_redwood);
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_requests, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_requests,
    "requests the phone's storage has answered through the loader");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_errors, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_errors,
    "requests the phone's storage has refused or not answered");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_bytes,
    "bytes read from the phone's storage");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_wait_us, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_wait_us,
    "microseconds spent waiting for core 0's answers");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_copy_us, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_copy_us,
    "microseconds spent copying out of the loader's buffer");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_writes, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_writes,
    "requests written into darwin and read back by the loader");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_write_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_write_bytes,
    "bytes written into darwin");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_write_errors, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_write_errors,
    "writes the loader refused or did not finish");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_write_refused, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_write_refused,
    "writes refused here: not darwin, or not past what the build writes");

/* The handshake: 1 to register as the saver, 3 to say the volume is in the flash. 2 is the kernel's. */
static int
ufs_sysctl_var_save(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = ufs_var_save, error;

	error = sysctl_io_number(req, value, sizeof(value), &value, NULL);
	if (error != 0 || req->newptr == 0) {
		return error;
	}
	if (value == VAR_SAVE_IDLE && ufs_var_save != VAR_SAVE_WANTED) {
		ufs_var_save = VAR_SAVE_IDLE;           /* a saver is there; it is the one that will be asked */
	} else if (value == VAR_SAVE_DONE && ufs_var_save == VAR_SAVE_WANTED) {
		ufs_var_save = VAR_SAVE_DONE;           /* and it has finished: the way out may go on */
	} else if (value == VAR_SAVE_NONE) {
		ufs_var_save = VAR_SAVE_NONE;
	} else {
		return EINVAL;
	}
	return 0;
}

SYSCTL_PROC(_kern_redwood, OID_AUTO, var_save, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0,
    ufs_sysctl_var_save, "I", "the data volume's saver: 1 registers, 2 is the kernel asking, 3 is done");

/* The claim on the store: 1 takes it, 0 gives it back; reading says which process holds it. */
static int
ufs_sysctl_var_busy(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = ufs_var_busy_pid, error, me = proc_selfpid();

	error = sysctl_io_number(req, value, sizeof(value), &value, NULL);
	if (error != 0 || req->newptr == 0) {
		return error;
	}
	lck_mtx_lock(&ufs_write_mtx);
	if (value == 1) {
		if (ufs_var_busy_pid != 0 && ufs_var_busy_pid != me) {
			proc_t held = proc_find(ufs_var_busy_pid);

			if (held != NULL) {
				proc_rele(held);
				lck_mtx_unlock(&ufs_write_mtx);
				return EBUSY;   /* another saver has it and is alive: one at a time */
			}
			printf("var_store: the claim on the store was left by process %d, which is gone\n",
			    ufs_var_busy_pid);
		}
		ufs_var_busy_pid = me;
	} else if (value == 0) {
		if (ufs_var_busy_pid == me || ufs_var_busy_pid == 0) {
			ufs_var_busy_pid = 0;
		} else {
			error = EPERM;
		}
	} else {
		error = EINVAL;
	}
	lck_mtx_unlock(&ufs_write_mtx);
	return error;
}

SYSCTL_PROC(_kern_redwood, OID_AUTO, var_busy, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0,
    ufs_sysctl_var_busy, "I", "the claim a writer of /dev/rufs_darwin must hold: 1 takes it, 0 gives it back");

/*
 * Native blocks [lba, lba + count) of partition part, one request of at most the buffer's size:
 * read into dst, or, with src, written from there. Called with ufs_lock held.
 */
static int
ufs_request(uint32_t op, uint32_t part, uint64_t lba, uint64_t count, uint8_t *dst, const uint8_t *src)
{
	uint64_t bytes = count * ufs_page->part[part].block_size, started, waited_ms = 0;
	uint32_t seq = ++ufs_seq;
	int64_t status;

	if (ufs_dead) {
		return EIO;
	}
	if (op == BLOCK_OP_READ) {
		if (part == ufs_last_part && lba == ufs_last_end) {
			ufs_sequential++;
		}
		ufs_last_part = part;
		ufs_last_end = lba + count;
	} else {
		/*
		 * The data into the buffer and out of this core's cache before the request is raised:
		 * core 0 drops its own lines of the buffer and hands it to UEFI's driver.
		 */
		memcpy(ufs_buffer, src, (size_t)bytes);
		FlushPoC_DcacheRegion((vm_offset_t)ufs_buffer, (size_t)bytes);
		ufs_last_part = ~0u;
	}
	ufs_page->ask.part = part;
	ufs_page->ask.lba = lba;
	ufs_page->ask.count = count;
	ufs_page->ask.op = op;
	__asm__ volatile ("dmb sy" ::: "memory");
	ufs_page->ask.request = seq;
	__asm__ volatile ("dmb sy" ::: "memory");

	started = mach_absolute_time();
	while (ufs_page->answer.answered != seq) {
		/* spin briefly for a quick answer, then sleep a millisecond at a time */
		uint64_t us;

		absolutetime_to_nanoseconds(mach_absolute_time() - started, &us);
		us /= 1000;
		if (us < 2000) {
			/* the answer to a small request comes in tens of microseconds: look often */
			__builtin_arm_yield();
			continue;
		}
		IOSleep(1);
		if (++waited_ms > UFS_TIMEOUT_MS) {
			/* no answer: the loader is gone or stuck; nothing more is asked of it */
			printf("ufs: no answer from the loader to request %u in %u s: the storage is off from now on\n",
			    seq, UFS_TIMEOUT_MS / 1000);
			ufs_dead = true;
			ufs_errors++;
			return EIO;
		}
	}
	__asm__ volatile ("dmb sy" ::: "memory");
	{
		uint64_t ns;

		absolutetime_to_nanoseconds(mach_absolute_time() - started, &ns);
		ufs_wait_us += ns / 1000;
	}
	status = ufs_page->answer.status;
	if (status != 0) {
		printf("ufs: %s: blocks %llu..%llu: the loader's %s failed (status %llx)\n",
		    ufs_page->part[part].name, lba, lba + count - 1,
		    op == BLOCK_OP_WRITE ? "write" : "read", (uint64_t)status);
		ufs_errors++;
		return EIO;
	}
	if (op == BLOCK_OP_WRITE) {
		/* the loader has read the blocks back and found them the same */
		ufs_writes++;
		ufs_write_bytes += bytes;
		return 0;
	}
	started = mach_absolute_time();
	/*
	 * The buffer is mapped cacheable here (copying out of an uncached mapping went at 870 MB/s,
	 * as slow as the storage itself). Core 0 cleans and invalidates it after every read, and this
	 * side only ever fills it for a write of its own and cleans its lines then; whatever this
	 * core may still hold of an earlier request -- a line brought in by a prefetch, say -- goes
	 * before the copy.
	 */
	FlushPoC_DcacheRegion((vm_offset_t)ufs_buffer, (size_t)bytes);
	memcpy(dst, ufs_buffer, (size_t)bytes);
	{
		uint64_t ns;

		absolutetime_to_nanoseconds(mach_absolute_time() - started, &ns);
		ufs_copy_us += ns / 1000;
	}
	ufs_requests++;
	ufs_bytes += bytes;
	if (bytes <= 4096) {
		ufs_small++;
	} else if (bytes <= 65536) {
		ufs_medium++;
	} else {
		ufs_large++;
	}
	return 0;
}

/* One of this driver's devices (block or character): a request to it costs tens of microseconds */
bool
redwood_ufs_dev(dev_t dev)
{
	return ufs_ready && ((ufs_bmajor >= 0 && major(dev) == ufs_bmajor) ||
	       (ufs_cmajor >= 0 && major(dev) == ufs_cmajor));
}

/*
 * Where the time of reading the storage went, so far: waiting for core 0's answer (of which
 * the loader's own clock says how long ReadBlocks itself took) and copying out of its buffer.
 */
void
redwood_ufs_report(const char *when)
{
	uint64_t loader_ns = 0;

	if (!ufs_ready) {
		return;
	}
	ufshc_report(when);
	absolutetime_to_nanoseconds(ufs_page->answer.ticks, &loader_ns);
	printf("ufs: %s: %llu requests (%llu of 4 KB or less, %llu to 64 KB, %llu more; %llu right after "
	    "the one before), %llu errors, %llu bytes; waiting %llu ms, of it in ReadBlocks %llu ms; "
	    "copying %llu ms\n", when, ufs_requests, ufs_small, ufs_medium, ufs_large, ufs_sequential,
	    ufs_errors, ufs_bytes, ufs_wait_us / 1000, loader_ns / 1000000, ufs_copy_us / 1000);
	if (ufs_write_part >= 0 || ufs_writes != 0 || ufs_write_refused != 0) {
		printf("ufs: %s: %llu writes into darwin, %llu bytes, read back by the loader; %llu did not "
		    "finish, %llu refused here\n", when, ufs_writes, ufs_write_bytes, ufs_write_errors,
		    ufs_write_refused);
	}
}

static uint64_t
ufs_part_bytes(uint32_t part)
{
	return ufs_page->part[part].blocks * ufs_page->part[part].block_size;
}

/*
 * Bytes [offset, offset + len) of partition part into dst, whatever their alignment, through the
 * loader: the native blocks that cover them are read a buffer at a time and the wanted bytes
 * copied out.
 */
static int
ufs_read_mailbox(uint32_t part, uint64_t offset, uint8_t *dst, uint64_t len)
{
	uint64_t nbs = ufs_page->part[part].block_size, per = ufs_buffer_size / nbs;
	static uint8_t *bounce;
	int error = 0;

	if (bounce == NULL) {
		bounce = kalloc_data(ufs_buffer_size, Z_WAITOK);
		if (bounce == NULL) {
			return ENOMEM;
		}
	}
	lck_mtx_lock(&ufs_lock);
	if (ufshc_list_in()) {
		/* this kernel's own transfer list is in the controller: UEFI's driver must not be asked to ring */
		if (ufs_mailbox_refused++ < 4) {
			printf("ufs: %s: not asked of the loader while this kernel's own transfer list is in\n",
			    ufs_page->part[part].name);
		}
		lck_mtx_unlock(&ufs_lock);
		return EIO;
	}
	while (len > 0 && error == 0) {
		uint64_t first = offset / nbs, skip = offset % nbs;
		uint64_t count = (skip + len + nbs - 1) / nbs, take;

		if (count > per) {
			count = per;
		}
		take = count * nbs - skip;
		if (take > len) {
			take = len;
		}
		if (skip == 0 && take == count * nbs) {
			error = ufs_request(BLOCK_OP_READ, part, first, count, dst, NULL);   /* whole blocks: straight in */
		} else {
			error = ufs_request(BLOCK_OP_READ, part, first, count, bounce, NULL);
			if (error == 0) {
				memcpy(dst, bounce + skip, (size_t)take);
			}
		}
		offset += take;
		dst += take;
		len -= take;
	}
	lck_mtx_unlock(&ufs_lock);
	return error;
}

/* The same, through this kernel's own driver: the partition's unit, from where it begins there */
static int
ufs_read_native(uint32_t part, uint64_t offset, uint8_t *dst, uint64_t len)
{
	if (part >= ufs_parts || ufs_lun[part] == 0xff) {
		return ENXIO;
	}
	return ufshc_read(ufs_lun[part], ufs_lun_offset[part] + offset, dst, len);
}

/* The switches of lists, with the loader's mailbox held: nothing is asked of UEFI meanwhile */
static int
ufs_native_in(bool prove)
{
	int error;

	lck_mtx_lock(&ufs_lock);
	/* (a loader that did not answer may still be inside UEFI's driver) */
	error = ufs_dead ? EIO : ufshc_switch_in(prove);
	lck_mtx_unlock(&ufs_lock);
	return error;
}

static int
ufs_native_out(const char *why)
{
	int error;

	lck_mtx_lock(&ufs_lock);
	error = ufshc_switch_out(why);
	lck_mtx_unlock(&ufs_lock);
	return error;
}

/* The way chosen (ufs_native=, kern.redwood.ufs_native); if this kernel's own fails, the loader's */
static int
ufs_read(uint32_t part, uint64_t offset, uint8_t *dst, uint64_t len)
{
	if (ufs_native_on && ufs_lun[part] != 0xff) {
		int error = ufs_read_native(part, offset, dst, len);

		if (error == 0) {
			return 0;
		}
		if (ufs_write_busy) {
			/* the list is out for a write of ours, not broken: this read goes the other way */
			return ufs_read_mailbox(part, offset, dst, len);
		}
		if (ufs_native_on) {
			ufs_native_on = false;
			ufs_native_fallbacks++;
			printf("ufs: %s: bytes %llu..%llu: this kernel's own read failed (%d); through the loader from now on\n",
			    ufs_page->part[part].name, offset, offset + len - 1, error);
		}
		ufs_native_out("a read failed");
	}
	return ufs_read_mailbox(part, offset, dst, len);
}

/*
 * Whole blocks [offset, offset + len) of the one partition that takes writes, from src, through
 * the loader. This kernel's own driver never writes -- its whitelist of commands has no write in
 * it -- so for the length of a write its transfer list goes out and UEFI's driver, which the
 * loader calls, has the controller again; reads meanwhile go through the loader too (ufs_read),
 * which is slower and right. Nothing of iOS waits on this but the writer itself: the data volume
 * is in memory (ramfs) and only its copy for the next boot is written here.
 */
static int
ufs_write(uint32_t part, uint64_t offset, const uint8_t *src, uint64_t len)
{
	uint64_t nbs, per;
	int error = 0;

	if (!ufs_ready || ufs_write_part < 0 || part != (uint32_t)ufs_write_part) {
		ufs_write_refused++;
		return EROFS;
	}
	nbs = ufs_page->part[part].block_size;
	if (len == 0 || offset % nbs != 0 || len % nbs != 0) {
		return EINVAL;          /* whole blocks only: nothing is read before a write */
	}
	if (offset / nbs < UFS_WRITE_FIRST || offset > ufs_part_bytes(part) ||
	    len > ufs_part_bytes(part) - offset) {
		/* the iOS volume, the cryptexes, and anything past the partition's end: never */
		ufs_write_refused++;
		return EROFS;
	}
	per = ufs_buffer_size / nbs;
	lck_mtx_lock(&ufs_write_mtx);
	/*
	 * The list goes out once, at the first write, and comes back when the device is closed: a
	 * whole data volume is written a bufferful at a time, and switching the controller's list
	 * for each of those would say so a hundred times over and buy nothing. ufs_native_on is
	 * cleared first, so that a reader about to start goes the other way of its own accord; one
	 * already inside this kernel's driver gets an error and is sent the other way by
	 * ufs_write_busy.
	 */
	if (!ufs_write_out) {
		ufs_write_was_native = ufs_native_on;
		ufs_write_busy = true;
		ufs_write_out = true;
		if (ufs_write_was_native) {
			ufs_native_on = false;
			ufs_native_out("writing the data volume into darwin");
		}
	}
	while (len > 0 && error == 0) {
		uint64_t count = len / nbs;

		if (count > per) {
			count = per;
		}
		lck_mtx_lock(&ufs_lock);
		if (ufshc_list_in()) {
			/* it did not go out after all: UEFI's driver must not be asked to ring */
			error = EIO;
		} else {
			error = ufs_request(BLOCK_OP_WRITE, part, offset / nbs, count, NULL, src);
		}
		lck_mtx_unlock(&ufs_lock);
		offset += count * nbs;
		src += count * nbs;
		len -= count * nbs;
	}
	lck_mtx_unlock(&ufs_write_mtx);
	if (error != 0) {
		ufs_write_errors++;
	}
	return error;
}

/* The writing over: this kernel's own transfer list back in, and reads through it again. */
static void
ufs_write_restore(void)
{
	lck_mtx_lock(&ufs_write_mtx);
	if (ufs_write_out) {
		if (ufs_write_was_native && ufs_native_in(false) == 0) {
			ufs_native_on = true;
		}
		ufs_write_busy = false;
		ufs_write_out = false;
	}
	lck_mtx_unlock(&ufs_write_mtx);
}

/* ---- the devices ---- */

static int
ufs_open(dev_t dev, int flags, __unused int devtype, __unused proc_t p)
{
	if (!ufs_ready || (uint32_t)minor(dev) >= ufs_parts || (ufs_page->part[minor(dev)].flags & BLOCK_PART_UNIT)) {
		return ENXIO;
	}
	if (flags & FWRITE) {
		return EROFS;           /* a block device of this driver never writes */
	}
	return 0;
}

/* The same, for the character devices: the one partition that takes writes may be opened for them. */
static int
ufs_copen(dev_t dev, int flags, int devtype, proc_t p)
{
	if ((flags & FWRITE) && ufs_write_part >= 0 && (uint32_t)minor(dev) == (uint32_t)ufs_write_part) {
		flags &= ~FWRITE;
	}
	return ufs_open(dev, flags, devtype, p);
}

static int
ufs_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	return 0;
}

static void ufs_write_restore(void);

/* The last close of /dev/rufs_darwin ends the writing: this kernel's own transfer list goes back in. */
static int
ufs_cclose(dev_t dev, int flags, int devtype, proc_t p)
{
	if (ufs_write_part >= 0 && (uint32_t)minor(dev) == (uint32_t)ufs_write_part) {
		ufs_write_restore();
	}
	return ufs_close(dev, flags, devtype, p);
}

static void
ufs_strategy(buf_t bp)
{
	uint32_t part = (uint32_t)minor(buf_device(bp));
	uint64_t offset, len, size;
	int error;

	if (!ufs_ready || part >= ufs_parts) {
		buf_seterror(bp, ENXIO);
		buf_biodone(bp);
		return;
	}
	if (!(buf_flags(bp) & B_READ)) {
		buf_seterror(bp, EROFS);         /* this device never writes */
		buf_biodone(bp);
		return;
	}
	size = ufs_part_bytes(part);
	offset = (uint64_t)buf_blkno(bp) * ufs_bsize[part];
	len = buf_count(bp);
	if (offset >= size) {
		buf_seterror(bp, offset == size ? 0 : EINVAL);
		buf_setresid(bp, buf_count(bp));
		buf_biodone(bp);
		return;
	}
	if (offset + len > size) {
		len = size - offset;
	}
	error = ufs_read(part, offset, (uint8_t *)buf_dataptr(bp), len);
	buf_setresid(bp, (uint32_t)(buf_count(bp) - (error ? 0 : len)));
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
ufs_ioctl(dev_t dev, u_long cmd, caddr_t data, __unused int fflag, __unused proc_t p)
{
	uint32_t part = (uint32_t)minor(dev), native;

	if (!ufs_ready || part >= ufs_parts) {
		return ENXIO;
	}
	native = ufs_page->part[part].block_size;
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*(uint32_t *)data = ufs_bsize[part];
		return 0;
	case DKIOCSETBLOCKSIZE:
		/*
		 * spec_open() sets the block size to 512 to count the device in 512-byte blocks and
		 * then back, and treats a refusal as a broken device; reads are by byte here, so any
		 * power of two from 512 to the native block size will do.
		 */
		if (*(uint32_t *)data < 512 || *(uint32_t *)data > native ||
		    (*(uint32_t *)data & (*(uint32_t *)data - 1)) != 0) {
			return EINVAL;
		}
		ufs_bsize[part] = *(uint32_t *)data;
		return 0;
	case DKIOCGETPHYSICALBLOCKSIZE:
		*(uint32_t *)data = native;
		return 0;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = ufs_part_bytes(part) / ufs_bsize[part];
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = 0;  /* the block device, which is what a filesystem would ask */
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*(uint64_t *)data = ufs_buffer_size / ufs_bsize[part];
		return 0;
	default:
		return ENOTTY;
	}
}

static int
ufs_psize(dev_t dev)
{
	uint32_t part = (uint32_t)minor(dev);

	return ufs_ready && part < ufs_parts ? (int)ufs_bsize[part] : -1;
}

/*
 * A read of /dev/rufs_NAME: a bufferful at a time into memory of this kernel's and out to the
 * reader with uiomove. Not physio, which hands the driver the reader's own address to copy to --
 * and this kernel may not touch user memory itself (PAN): the copy has to go through uiomove.
 * Nothing of this driver is held while it does, for a fault in the reader's memory can come back
 * here as a read of the storage.
 */
static int
ufs_cread(dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t part = (uint32_t)minor(dev);
	uint64_t size;
	uint8_t *bounce;
	int error = 0;

	if (!ufs_ready || part >= ufs_parts) {
		return ENXIO;
	}
	size = ufs_part_bytes(part);
	if (uio_offset(uio) < 0) {
		return EINVAL;
	}
	bounce = kalloc_data(ufs_buffer_size, Z_WAITOK);
	if (bounce == NULL) {
		return ENOMEM;
	}
	while (error == 0 && uio_resid(uio) > 0 && (uint64_t)uio_offset(uio) < size) {
		uint64_t at = (uint64_t)uio_offset(uio), want = (uint64_t)uio_resid(uio);

		if (want > ufs_buffer_size) {
			want = ufs_buffer_size;
		}
		if (want > size - at) {
			want = size - at;
		}
		error = ufs_read(part, at, bounce, want);
		if (error == 0) {
			error = uiomove((caddr_t)bounce, (int)want, uio);
		}
	}
	kfree_data(bounce, ufs_buffer_size);
	return error;
}

/*
 * A write to /dev/rufs_darwin: whole blocks, out of user memory a bufferful at a time and into the
 * partition. Nothing of this driver is held while user memory is touched -- a fault there can come
 * back as a read of the storage -- so the bytes are taken first and written after.
 */
static int
ufs_cwrite(dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t part = (uint32_t)minor(dev);
	uint64_t nbs;
	uint8_t *bounce;
	int error = 0;

	if (!ufs_ready || part >= ufs_parts || ufs_write_part < 0 || part != (uint32_t)ufs_write_part) {
		return EROFS;
	}
	if (ufs_var_busy_pid != proc_selfpid()) {
		/* one writer of the store at a time, and it says so first (kern.redwood.var_busy) */
		ufs_write_refused++;
		return EPERM;
	}
	nbs = ufs_page->part[part].block_size;
	if (uio_offset(uio) < 0 || (uint64_t)uio_offset(uio) % nbs != 0 || (uint64_t)uio_resid(uio) % nbs != 0) {
		return EINVAL;
	}
	bounce = kalloc_data(ufs_buffer_size, Z_WAITOK);
	if (bounce == NULL) {
		return ENOMEM;
	}
	while (error == 0 && uio_resid(uio) > 0) {
		user_ssize_t want = uio_resid(uio);
		off_t at = uio_offset(uio);

		if (want > (user_ssize_t)ufs_buffer_size) {
			want = (user_ssize_t)ufs_buffer_size;
		}
		error = uiomove((caddr_t)bounce, (int)want, uio);
		if (error == 0) {
			error = ufs_write(part, (uint64_t)at, bounce, (uint64_t)want);
			if (error != 0) {
				/* these bytes did not go in: the write is to say so, not to count them */
				uio_setoffset(uio, at);
				uio_setresid(uio, uio_resid(uio) + want);
			}
		}
	}
	kfree_data(bounce, ufs_buffer_size);
	return error;
}

static const struct bdevsw ufs_bdevsw = {
	.d_open     = ufs_open,
	.d_close    = ufs_close,
	.d_strategy = ufs_strategy,
	.d_ioctl    = ufs_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = ufs_psize,
	.d_type     = D_DISK,
};

static const struct cdevsw ufs_cdevsw = {
	.d_open       = ufs_copen,
	.d_close      = ufs_cclose,
	.d_read       = ufs_cread,
	.d_write      = ufs_cwrite,
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

/*
 * The orderly end of a run, from osfmk/arm64/loader_return.c, in the thread that is about to hand
 * control back: ask the saver for the data volume and wait for it, so that a run need not have
 * been told to save. Bounded, and only when a saver registered -- the loader goes on feeding the
 * watchdog for a minute past the run's own budget, and the host waits a hundred seconds past it,
 * so twenty seconds here is well inside both. Nothing else of the way out has happened yet: the
 * GPU, the DSPs and this kernel's transfer list are still as the run had them.
 */
void
redwood_var_save_on_return(void)
{
	uint32_t ms = VAR_SAVE_MS;
	uint64_t waited = 0;

	if (!ufs_ready || ufs_write_part < 0 || ufs_var_save != VAR_SAVE_IDLE) {
		return;
	}
	PE_parse_boot_argn("var_save_ms", &ms, sizeof(ms));
	printf("var_store: the run is ending: the data volume is asked for\n");
	ufs_var_save = VAR_SAVE_WANTED;
	while (ufs_var_save != VAR_SAVE_DONE && waited < ms) {
		IOSleep(20);
		waited += 20;
	}
	if (ufs_var_save == VAR_SAVE_DONE) {
		/* what it made of it is in its own lines: this only says it answered */
		printf("var_store: the saver has finished after %llu ms\n", waited);
	} else {
		printf("var_store: the saver did not answer in %u ms: the store keeps what it had\n", ms);
	}
}

/* ---- the check: the partitions there are images of, hashed as the loader hashes them ---- */

static const struct {
	const char *name;
	uint64_t bytes;
} ufs_references[] = {
	{ "bluetooth", 503808 }, { "abl", 208896 }, { "aop", 208896 }, { "xbl", 3706880 },
	{ "tz", 3710976 }, { "hyp", 3653632 }, { "dsp", 67108864 }, { "modem", 214265856 },
};

static bool
ufs_is_reference(const char *name, const char *ref)
{
	size_t n = strlen(ref);

	return strncmp(name, ref, n) == 0 &&
	       (name[n] == 0 || (name[n] == '_' && (name[n + 1] == 'a' || name[n + 1] == 'b') && name[n + 2] == 0));
}

static void
ufs_check(void *arg, __unused wait_result_t wr)
{
	uint8_t *chunk = kalloc_data(1 << 20, Z_WAITOK);

	/*
	 * Not at once: what the kernel prints in its first minute or so does not come back with the
	 * log on the phone (the first line that does is from long after this driver has started), so
	 * the check waits until it will be read -- unless it runs right at the start (ufs_check=2, then
	 * back to the loader: the log is short).
	 */
	if (arg == NULL) {
		IOSleep(40000);
	}
	printf("ufs: check: %u partitions of the phone's storage, read through %s\n", ufs_parts,
	    ufs_native_on ? "this kernel's own UFS driver" : "the loader's page");
	for (uint32_t part = 0; chunk != NULL && part < ufs_parts; part++) {
		for (unsigned r = 0; r < sizeof(ufs_references) / sizeof(ufs_references[0]); r++) {
			uint64_t h = 0xcbf29ce484222325ULL, done = 0, started = mach_absolute_time(), ns;
			uint64_t bytes = ufs_references[r].bytes;
			int error = 0;

			if (!ufs_is_reference((const char *)ufs_page->part[part].name, ufs_references[r].name)) {
				continue;
			}
			while (done < bytes && error == 0) {
				uint64_t n = bytes - done < (1 << 20) ? bytes - done : (1 << 20);

				error = ufs_read(part, done, chunk, n);
				for (uint64_t i = 0; error == 0 && i < n; i++) {
					h = (h ^ chunk[i]) * 0x100000001b3ULL;
				}
				done += n;
			}
			absolutetime_to_nanoseconds(mach_absolute_time() - started, &ns);
			if (error) {
				printf("ufs: check %s: read failed (%d)\n", ufs_page->part[part].name, error);
			} else {
				printf("ufs: check %s: fnv %llx over %llu bytes, %llu ms\n", ufs_page->part[part].name,
				    h, bytes, ns / 1000000);
			}
		}
	}
	if (chunk != NULL) {
		kfree_data(chunk, 1 << 20);
	}
	printf("ufs: check done: %llu requests, %llu errors, %llu bytes, %llu ms waiting\n",
	    ufs_requests, ufs_errors, ufs_bytes, ufs_wait_us / 1000);
}

/*
 * ufs_bench=1: what one request through the loader costs, by size. The partition "darwin" is
 * read from the front, a different stretch for every size so that nothing the storage itself may
 * have kept is read twice; for each size, MB/s and microseconds a request, split into the time
 * core 0 spent in ReadBlocks (its clock in the page, read before and after), the rest of waiting
 * for its answer (noticing the request, answering), and copying out of its buffer.
 */
static void
ufs_bench(__unused void *arg, __unused wait_result_t wr)
{
	static const uint32_t sizes[] = { 4096, 16384, 65536, 262144, 1048576, 4194304 };
	uint8_t *dst = kalloc_data(ufs_buffer_size, Z_WAITOK);
	uint32_t part = BLOCK_MAX_PARTS;
	uint64_t base = 0;

	for (uint32_t i = 0; i < ufs_parts; i++) {
		if (strcmp((const char *)ufs_page->part[i].name, "darwin") == 0) {
			part = i;
		}
	}
	if (dst == NULL || part == BLOCK_MAX_PARTS) {
		printf("ufs: bench: no partition darwin, or no memory\n");
		return;
	}
	for (unsigned z = 0; z < sizeof(sizes) / sizeof(sizes[0]); z++) {
		uint64_t total = sizes[z] <= 16384 ? (16ULL << 20) : (128ULL << 20), done = 0, n = 0;
		uint64_t wait0 = ufs_wait_us, copy0 = ufs_copy_us, loader0 = ufs_page->answer.ticks;
		uint64_t started = mach_absolute_time(), ns, loader_ns;
		int error = 0;

		while (done < total && error == 0) {
			error = ufs_read(part, base + done, dst, sizes[z]);
			done += sizes[z];
			n++;
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - started, &ns);
		absolutetime_to_nanoseconds(ufs_page->answer.ticks - loader0, &loader_ns);
		printf("ufs: bench: %7u bytes a request: %4llu MB/s, %llu requests, %llu us each: "
		    "ReadBlocks %llu, the rest of waiting %llu, copying %llu%s\n", sizes[z],
		    ns ? (done * 1000ULL) / ns : 0, n, ns / 1000 / n, loader_ns / 1000 / n,
		    (ufs_wait_us - wait0 - loader_ns / 1000) / n, (ufs_copy_us - copy0) / n,
		    error ? " (an error)" : "");
		base += total;
	}
	kfree_data(dst, ufs_buffer_size);
	redwood_ufs_report("after the bench");
}

/* ---- this kernel's own driver: which unit is which, and whether it reads what the loader reads ---- */

static uint64_t
ufs_fnv(uint64_t h, const uint8_t *p, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++) {
		h = (h ^ p[i]) * 0x100000001b3ULL;
	}
	return h;
}

static uint32_t
ufs_entry(const char *name)
{
	for (uint32_t i = 0; i < ufs_parts; i++) {
		if (strcmp((const char *)ufs_page->part[i].name, name) == 0) {
			return i;
		}
	}
	return BLOCK_MAX_PARTS;
}

/*
 * Which logical unit each unit the loader lists is (their device paths do not say): unit k is
 * taken to be LUN k (ordinary logical units only: never a well-known one), and it is so only if
 * block 1 (the GPT header, with the unit's own GUID in it) read through the loader
 * is the same as block 1 of that LUN read here; no other LUN is read. The loader's reads are made
 * first, with UEFI's list in; then this kernel's list goes in (with its proof) and stays in.
 */
static bool
ufs_native_map(bool all_units)
{
	uint8_t *mine = kalloc_data(4096, Z_WAITOK), *theirs = kalloc_data(BLOCK_MAX_UNITS * 4096, Z_WAITOK);
	uint32_t units[BLOCK_MAX_UNITS], n = 0, mask = 0, darwin = ufs_entry("darwin");
	bool good = mine != NULL && theirs != NULL;

	memset(ufs_lun, 0xff, sizeof(ufs_lun));
	for (uint32_t i = 0; good && i < ufs_parts && n < BLOCK_MAX_UNITS; i++) {
		volatile struct loader_block_part *e = &ufs_page->part[i];

		if ((e->flags & BLOCK_PART_UNIT) == 0 || e->unit >= 8) {
			continue;
		}
		if (!all_units && (darwin == BLOCK_MAX_PARTS || ufs_page->part[darwin].unit != e->unit)) {
			continue;
		}
		if (e->block_size != 4096 || ufs_read_mailbox(i, 4096, theirs + n * 4096, 4096) != 0 ||
		    memcmp(theirs + n * 4096, "EFI PART", 8) != 0) {
			printf("ufs: native: %s: no GPT header to know it by (block size %u)\n", e->name, e->block_size);
			good = false;
			break;
		}
		units[n++] = i;
		mask |= 1u << e->unit;
		ufs_native_luns = n;
	}
	if (good && !all_units && (n != 1 || ufs_page->part[units[0]].unit != 0)) {
		printf("ufs: native: darwin is not on the first unit the loader lists: not LUN 0, not read here\n");
		good = false;
	}
	ufshc_set_luns(mask);
	if (good && ufs_native_in(true) != 0) {
		good = false;
	}
	for (uint32_t k = 0; good && k < n; k++) {
		volatile struct loader_block_part *e = &ufs_page->part[units[k]];

		if (ufshc_read((uint8_t)e->unit, 4096, mine, 4096) != 0 || memcmp(mine, theirs + k * 4096, 4096) != 0) {
			printf("ufs: native: %s: LUN %u's block 1 is NOT what the loader reads as its block 1\n", e->name, e->unit);
			good = false;
		} else {
			printf("ufs: native: %s is LUN %u: its GPT header read both ways is the same\n", e->name, e->unit);
		}
	}
	if (!good) {
		ufs_native_out("the units could not be told");
		memset(ufs_lun, 0xff, sizeof(ufs_lun));
	} else {
		for (uint32_t i = 0; i < ufs_parts; i++) {
			volatile struct loader_block_part *e = &ufs_page->part[i];

			if (e->unit < 8 && (mask & (1u << e->unit)) && e->block_size == 4096) {
				ufs_lun[i] = (uint8_t)e->unit;
				ufs_lun_offset[i] = e->start_lba * 4096;
			}
		}
	}
	if (mine != NULL) {
		kfree_data(mine, 4096);
	}
	if (theirs != NULL) {
		kfree_data(theirs, BLOCK_MAX_UNITS * 4096);
	}
	return good;
}

/*
 * ufs_native_check=1: what this kernel's own driver reads is what the loader reads, byte for
 * byte. Spans are read a batch at a time, first all through the loader (UEFI's list in), then
 * all here (this kernel's list in, proved), and compared.
 */
struct ufs_span {
	uint32_t part;
	uint64_t offset, len;
};

#define UFS_CMP_BUFFER      (32u << 20)

static uint8_t *ufs_cmp_x, *ufs_cmp_y;
static uint64_t ufs_cmp_mailbox_ns, ufs_cmp_native_ns, ufs_cmp_bytes, ufs_cmp_spans, ufs_cmp_differ, ufs_cmp_failed;

static bool
ufs_compare_batch(const struct ufs_span *s, uint32_t n, const char *what, bool quiet)
{
	uint64_t at, t, ns, hx = 0xcbf29ce484222325ULL, hy = hx, bytes = 0, mailbox_ns, native_ns;
	bool same = true;
	int e = 0;

	t = mach_absolute_time();
	if (ufs_native_out("the loader reads, to compare") != 0) {
		e = EIO;
	}
	at = 0;
	for (uint32_t i = 0; e == 0 && i < n; at += s[i].len, i++) {
		e = ufs_read_mailbox(s[i].part, s[i].offset, ufs_cmp_x + at, s[i].len);
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - t, &mailbox_ns);
	t = mach_absolute_time();
	if (e == 0 && ufs_native_in(true) != 0) {
		e = EIO;
	}
	at = 0;
	for (uint32_t i = 0; e == 0 && i < n; at += s[i].len, i++) {
		e = ufs_read_native(s[i].part, s[i].offset, ufs_cmp_y + at, s[i].len);
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - t, &native_ns);
	ufs_cmp_mailbox_ns += mailbox_ns;
	ufs_cmp_native_ns += native_ns;
	if (e != 0) {
		printf("ufs: native check: %s: a read failed (%d): STOP\n", what, e);
		ufs_cmp_failed++;
		return false;
	}
	at = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (memcmp(ufs_cmp_x + at, ufs_cmp_y + at, s[i].len) != 0) {
			uint64_t k = 0;

			while (ufs_cmp_x[at + k] == ufs_cmp_y[at + k]) {
				k++;
			}
			printf("ufs: native check: %s %s: DIFFERENT at byte %llu (%02x through the loader, %02x here): STOP\n",
			    ufs_page->part[s[i].part].name, what, s[i].offset + k, ufs_cmp_x[at + k], ufs_cmp_y[at + k]);
			same = false;
			ufs_cmp_differ++;
		}
		hx = ufs_fnv(hx, ufs_cmp_x + at, s[i].len);
		hy = ufs_fnv(hy, ufs_cmp_y + at, s[i].len);
		at += s[i].len;
		bytes += s[i].len;
	}
	ufs_cmp_spans += n;
	ufs_cmp_bytes += bytes;
	if (!quiet || !same) {
		printf("ufs: native check: %s %s: %u span(s), %llu bytes from %llu: %s (fnv %llx / %llx); loader %llu ms, here "
		    "%llu ms (switches included)\n", ufs_page->part[s[0].part].name, what, n, bytes, s[0].offset,
		    same ? "the same" : "NOT THE SAME", hx, hy, mailbox_ns / 1000000, native_ns / 1000000);
	}
	return same;
}

/* Bytes [offset, offset + len) of an entry, compared a buffer at a time; FNV-1a of the first ref bytes too */
static bool
ufs_compare(uint32_t part, uint64_t offset, uint64_t len, uint64_t ref, const char *what)
{
	uint64_t done = 0, href = 0xcbf29ce484222325ULL;
	bool same = true;

	while (done < len && same) {
		struct ufs_span s = { part, offset + done, len - done < UFS_CMP_BUFFER ? len - done : UFS_CMP_BUFFER };

		same = ufs_compare_batch(&s, 1, what, true);
		if (same && ref > done) {
			href = ufs_fnv(href, ufs_cmp_x, ref - done < s.len ? ref - done : s.len);
		}
		done += s.len;
	}
	printf("ufs: native check: %s %s: %llu bytes from %llu: %s%s", ufs_page->part[part].name, what, done, offset,
	    same ? "the same" : "NOT THE SAME", ref ? "" : "\n");
	if (ref) {
		printf("; fnv %llx over the first %llu bytes (the loader's own survey hashes them)\n", href, ref);
	}
	return same;
}

static void
ufs_native_check(void)
{
	uint8_t *h = kalloc_data(4096, Z_WAITOK);
	uint32_t part;
	bool going = true;

	ufs_cmp_x = kalloc_data(UFS_CMP_BUFFER, Z_WAITOK);
	ufs_cmp_y = kalloc_data(UFS_CMP_BUFFER, Z_WAITOK);
	if (h == NULL || ufs_cmp_x == NULL || ufs_cmp_y == NULL) {
		printf("ufs: native check: no memory\n");
		return;
	}
	printf("ufs: native check: through the loader and through this kernel's own driver, compared\n");
	/* every unit's GPT: protective MBR, header, table; the backup table and header at the end */
	for (uint32_t i = 0; going && i < ufs_parts; i++) {
		volatile struct loader_block_part *e = &ufs_page->part[i];
		uint64_t entries_lba, count, size, table, alt, alt_entries;
		uint32_t want, got;

		if ((e->flags & BLOCK_PART_UNIT) == 0 || ufs_lun[i] == 0xff) {
			continue;
		}
		if (ufs_native_out("the loader reads, to compare") != 0 || ufs_read_mailbox(i, 4096, h, 4096) != 0) {
			going = false;
			break;
		}
		entries_lba = *(uint64_t *)(h + 72);
		count = *(uint32_t *)(h + 80);
		size = *(uint32_t *)(h + 84);
		alt = *(uint64_t *)(h + 32);
		table = (count * size + 4095) / 4096;
		want = *(uint32_t *)(h + 16);
		*(uint32_t *)(h + 16) = 0;
		got = crc32(0, h, *(uint32_t *)(h + 12));
		printf("ufs: native check: %s (LUN %u): GPT header crc %s (%x), table at block %llu, %llu entries of %llu bytes, "
		    "backup header at block %llu\n", e->name, ufs_lun[i], got == want ? "right" : "WRONG", got, entries_lba, count,
		    size, alt);
		going = ufs_compare(i, 0, (entries_lba + table) * 4096, 0, "GPT (MBR, header, table)");
		if (!going || alt >= e->blocks || ufs_native_out("the loader reads, to compare") != 0 ||
		    ufs_read_mailbox(i, alt * 4096, h, 4096) != 0 || memcmp(h, "EFI PART", 8) != 0 ||
		    (alt_entries = *(uint64_t *)(h + 72)) > alt) {
			printf("ufs: native check: %s: no backup GPT header at block %llu: STOP\n", e->name, alt);
			going = false;
			break;
		}
		going = ufs_compare(i, alt_entries * 4096, (alt + 1 - alt_entries) * 4096, 0, "backup GPT (table, header)");
	}
	/* the partitions there are images of, whole */
	for (part = 0; going && part < ufs_parts; part++) {
		if ((ufs_page->part[part].flags & BLOCK_PART_UNIT) || ufs_lun[part] == 0xff) {
			continue;
		}
		for (unsigned r = 0; going && r < sizeof(ufs_references) / sizeof(ufs_references[0]); r++) {
			if (ufs_is_reference((const char *)ufs_page->part[part].name, ufs_references[r].name)) {
				going = ufs_compare(part, 0, ufs_part_bytes(part), ufs_references[r].bytes, "whole");
			}
		}
	}
	/* super's geometry and metadata */
	if (going && (part = ufs_entry("super")) < BLOCK_MAX_PARTS && ufs_lun[part] != 0xff) {
		going = ufs_compare(part, 0, 8 << 20, 0, "first 8 MB (geometry, metadata)");
	}
	/* darwin: 32 spread megabytes, 256 scattered blocks, 32 pieces that begin and end inside a block */
	if (going && (part = ufs_entry("darwin")) < BLOCK_MAX_PARTS && ufs_lun[part] != 0xff) {
		struct ufs_span s[256];
		uint64_t bytes = ufs_part_bytes(part), seed = 0x9e3779b97f4a7c15ULL, total = 0;

		for (uint32_t k = 0; k < 32; k++) {
			s[k] = (struct ufs_span){ part, (bytes - (1 << 20)) / 32 * k / 4096 * 4096, 1 << 20 };
		}
		going = ufs_compare_batch(s, 32, "spread megabytes", false);
		for (uint32_t k = 0; k < 256; k++) {
			seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
			s[k] = (struct ufs_span){ part, (seed >> 16) % (bytes / 4096) * 4096, 4096 };
		}
		going = going && ufs_compare_batch(s, 256, "scattered blocks", false);
		for (uint32_t k = 0; k < 32; k++) {
			seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
			s[k] = (struct ufs_span){ part, (seed >> 16) % (bytes - (2 << 20)), 1 + (seed >> 40) % (512 << 10) };
			total += s[k].len;
		}
		going = going && ufs_compare_batch(s, 32, "unaligned pieces", false);
	}
	kfree_data(h, 4096);
	printf("ufs: native check %s: %llu spans, %llu bytes, %llu NOT THE SAME, %llu failed reads; through the loader %llu ms, "
	    "through this kernel's own driver %llu ms (list switches included)\n", going ? "done" : "STOPPED", ufs_cmp_spans,
	    ufs_cmp_bytes, ufs_cmp_differ, ufs_cmp_failed, ufs_cmp_mailbox_ns / 1000000, ufs_cmp_native_ns / 1000000);
	ufs_native_out("after the check");
	ufshc_report("after the check");
}

/*
 * ufs_native_bench=1 (with ufs_native=1): what the flash gives with N reads in flight -- N
 * threads each reading at random places of darwin, a size at a time -- MB/s and microseconds a
 * read, for 1, 2, 4, 8, 16 and 31 threads. Then back to the loader.
 */
struct ufs_nb {
	uint32_t part, size, per_thread;
	uint64_t span, seed;
	volatile uint32_t left;
	int error;
};

static void
ufs_nb_thread(void *arg, __unused wait_result_t wr)
{
	struct ufs_nb *nb = arg;
	uint8_t *buf = kalloc_data(nb->size, Z_WAITOK);
	uint64_t seed = os_atomic_add(&nb->seed, 0x9e3779b97f4a7c15ULL, relaxed);

	for (uint32_t i = 0; buf != NULL && i < nb->per_thread; i++) {
		int e;

		seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
		e = ufs_read_native(nb->part, (seed >> 20) % (nb->span / nb->size) * nb->size, buf, nb->size);
		if (e != 0) {
			nb->error = e;
		}
	}
	if (buf != NULL) {
		kfree_data(buf, nb->size);
	}
	lck_mtx_lock(&ufs_lock);
	nb->left--;
	wakeup(nb);
	lck_mtx_unlock(&ufs_lock);
}

static void
ufs_native_bench(void)
{
	static const uint32_t sizes[] = { 4096, 32768, 131072, 524288 };
	static const uint32_t threads[] = { 1, 2, 4, 8, 16, 31 };
	uint32_t part = ufs_entry("darwin");

	if (part >= BLOCK_MAX_PARTS || ufs_lun[part] == 0xff) {
		printf("ufs: native bench: no darwin\n");
		return;
	}
	for (unsigned z = 0; z < sizeof(sizes) / sizeof(sizes[0]); z++) {
		for (unsigned t = 0; t < sizeof(threads) / sizeof(threads[0]); t++) {
			struct ufs_nb nb = {
				.part = part, .size = sizes[z], .span = ufs_part_bytes(part),
				.per_thread = (sizes[z] <= 32768 ? 8192 : 2048) / threads[t] + 1, .seed = 12345 + z * 100 + t,
			};
			uint64_t started = mach_absolute_time(), ns, bytes;

			nb.left = threads[t];
			for (uint32_t i = 0; i < threads[t]; i++) {
				thread_t th;

				if (kernel_thread_start(ufs_nb_thread, &nb, &th) == KERN_SUCCESS) {
					thread_deallocate(th);
				} else {
					lck_mtx_lock(&ufs_lock);
					nb.left--;
					lck_mtx_unlock(&ufs_lock);
				}
			}
			lck_mtx_lock(&ufs_lock);
			while (nb.left > 0) {
				msleep(&nb, &ufs_lock, PRIBIO, "ufsbench", NULL);
			}
			lck_mtx_unlock(&ufs_lock);
			absolutetime_to_nanoseconds(mach_absolute_time() - started, &ns);
			bytes = (uint64_t)nb.per_thread * threads[t] * sizes[z];
			printf("ufs: native bench: %6u bytes, %2u in flight: %4llu MB/s, %6llu reads/s, %5llu us a read%s\n",
			    sizes[z], threads[t], ns ? bytes * 1000 / ns : 0, ns ? (bytes / sizes[z]) * 1000000000ULL / ns : 0,
			    ns / 1000 * threads[t] / (bytes / sizes[z]), nb.error ? " (ERRORS)" : "");
		}
	}
	ufshc_report("after the bench");
}

/* kern.redwood.ufs_native: 1 while reads go through this kernel's own driver; 0 or 1 may be set */
static int
ufs_sysctl_native(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = ufs_native_on, error, changed = 0;

	error = sysctl_io_number(req, value, sizeof(value), &value, &changed);
	if (error != 0 || !changed) {
		return error;
	}
	if (value != 0) {
		if (!(ufs_native_mapped && ufshc_available()) || ufs_native_in(true) != 0) {
			return ENXIO;
		}
		ufs_native_on = true;
	} else {
		ufs_native_on = false;
		ufs_native_out("kern.redwood.ufs_native=0");
	}
	return 0;
}
SYSCTL_PROC(_kern_redwood, OID_AUTO, ufs_native, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0,
    ufs_sysctl_native, "I", "1: the phone's storage is read by this kernel's own UFS driver, 0: through the loader");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_native_fallbacks, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_native_fallbacks,
    "times a read of this kernel's own UFS driver failed and the loader's was used instead");

/* ---- bringing it up ---- */

void
redwood_ufs_init(void)
{
	uint64_t pa = 0, buffer_pa;
	uint32_t check = 0, made = 0;
	vm_offset_t base;

	if (!PE_parse_boot_argn("ufs_shared_pa", &pa, sizeof(pa)) || pa == 0) {
		return;                 /* the loader was not asked for ufs=2: there is nothing to read through */
	}
	base = ml_io_map_unmappable((vm_offset_t)(pa & ~(UFS_PAGE - 1)),
	    ((pa & (UFS_PAGE - 1)) + sizeof(struct loader_block) + UFS_PAGE - 1) & ~(UFS_PAGE - 1), VM_WIMG_WCOMB);
	ufs_page = (volatile struct loader_block *)(base + (pa & (UFS_PAGE - 1)));
	if (ufs_page->magic != BLOCK_MAGIC || ufs_page->version != BLOCK_VERSION ||
	    ufs_page->size != sizeof(struct loader_block) || ufs_page->parts > BLOCK_MAX_PARTS) {
		printf("ufs: the loader's page at %llx is not one this kernel knows (magic %llx, version %u, size %u)\n",
		    pa, ufs_page->magic, ufs_page->version, ufs_page->size);
		return;
	}
	ufs_parts = ufs_page->parts;
	ufs_buffer_size = ufs_page->buffer_size;
	buffer_pa = ufs_page->buffer_pa;
	ufs_buffer = (const uint8_t *)(ml_io_map_unmappable((vm_offset_t)(buffer_pa & ~(UFS_PAGE - 1)),
	    ((buffer_pa & (UFS_PAGE - 1)) + ufs_buffer_size + UFS_PAGE - 1) & ~(UFS_PAGE - 1), VM_WIMG_DEFAULT) +
	    (buffer_pa & (UFS_PAGE - 1)));
	ufs_seq = ufs_page->answer.answered;      /* a loader that served an earlier kernel counts on from there */

	ufs_bmajor = bdevsw_add(-1, &ufs_bdevsw);
	ufs_cmajor = ufs_bmajor < 0 ? -1 : cdevsw_add_with_bdev(-1, &ufs_cdevsw, ufs_bmajor);
	if (ufs_cmajor < 0) {
		printf("ufs: no device numbers to be had\n");
		return;
	}
	for (uint32_t i = 0; i < ufs_parts; i++) {
		char name[48];

		ufs_bsize[i] = ufs_page->part[i].block_size;
		if (ufs_page->part[i].flags & BLOCK_PART_UNIT) {
			continue;               /* the units are for this kernel's own driver to be checked against */
		}
		snprintf(name, sizeof(name), "ufs_%s", ufs_page->part[i].name);
		if (devfs_make_node(makedev(ufs_bmajor, i), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "%s", name) == NULL) {
			printf("ufs: no node for %s (the same name twice?)\n", name);
			continue;
		}
		snprintf(name, sizeof(name), "rufs_%s", ufs_page->part[i].name);
		(void)devfs_make_node(makedev(ufs_cmajor, i), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "%s", name);
		made++;
	}
	ufs_ready = true;
	printf("ufs: %u partitions of the phone's storage as /dev/ufs_NAME and /dev/rufs_NAME, read only, "
	    "through the loader's page at %llx and its %u KB buffer\n", made, pa, ufs_buffer_size >> 10);

	/*
	 * ufs_write=1: /dev/rufs_darwin takes writes, past the iOS volume and the cryptexes. darwin
	 * has to be the partition the port was set up with, in everything the loader's table says of
	 * it; the loader looks at its GUIDs besides, and refuses the same bounds of its own accord.
	 */
	check = 0;
	if (PE_parse_boot_argn("ufs_write", &check, sizeof(check)) && check == 1) {
		uint32_t offered = ufs_page->write_part_plus_one;

		if (offered == 0 || offered > ufs_parts || ufs_page->write_first != UFS_WRITE_FIRST) {
			printf("ufs: the loader offers no partition to write into (ufs_write=1 on its command "
			    "line too, and a loader that knows about writes)\n");
		}
		for (uint32_t i = 0; offered != 0 && i < ufs_parts; i++) {
			const char *why = NULL;

			if (strncmp((const char *)ufs_page->part[i].name, "darwin", 7) != 0) {
				continue;
			}
			if (i != offered - 1) {
				why = "the loader offers another partition";
			} else if (ufs_page->part[i].flags & BLOCK_PART_UNIT) {
				why = "it is a whole logical unit, not the partition";
			} else if (ufs_page->part[i].unit != 0) {
				/* the first unit UEFI lists, which this kernel's own driver reads as LUN 0 and
				 * has compared with the loader's reads; the device path this firmware gives
				 * says no logical unit number at all, so part[].lun is 0xff for every unit */
				why = "it is not on the first logical unit";
			} else if (ufs_page->part[i].start_lba != UFS_DARWIN_FIRST) {
				why = "it does not begin where it did";
			} else if (ufs_page->part[i].blocks != UFS_DARWIN_BLOCKS) {
				why = "it is not the size it was";
			} else if (ufs_page->part[i].block_size != 4096) {
				why = "its blocks are not 4096 bytes";
			}
			if (why != NULL) {
				printf("ufs: darwin: no write will be asked for: %s\n", why);
				continue;
			}
			ufs_write_part = (int)i;
		}
		if (ufs_write_part >= 0) {
			printf("ufs: /dev/rufs_darwin takes writes, whole blocks from %llu on (%llu MB of it); "
			    "everything else of the storage is read only\n", UFS_WRITE_FIRST,
			    (UFS_DARWIN_BLOCKS - UFS_WRITE_FIRST) * 4096 >> 20);
		} else {
			printf("ufs: nothing takes writes: darwin is not among the partitions\n");
		}
	}

	/*
	 * ufs_native=1: through this kernel's own driver instead, its own transfer list in the
	 * controller for the run, if all is as expected; 2: only look; 3: a dry run -- the list in and
	 * out again, and in once more for the end of the run to take out, no command sent.
	 */
	check = 0;
	memset(ufs_lun, 0xff, sizeof(ufs_lun));
	if (PE_parse_boot_argn("ufs_native", &check, sizeof(check)) && check != 0) {
		struct ufshc_loader offer = {
			.hc_pa = ufs_page->native.hc_pa, .area_pa = ufs_page->native.area_pa,
			.area_size = ufs_page->native.area_size, .ucd_offset = ufs_page->native.ucd_offset,
			.ucd_size = ufs_page->native.ucd_size, .slots = ufs_page->native.slots,
			.uefi_lo = ufs_page->native.uefi_utrlba, .uefi_hi = ufs_page->native.uefi_utrlbau,
			.take_back = &ufs_page->loader_native.take_back, .list_in = &ufs_page->kernel_native.list_in,
			.switched_in = (uint32_t *)&ufs_page->kernel_native.switched_in,
			.switched_out = (uint32_t *)&ufs_page->kernel_native.switched_out,
			.cnr_at_switch = &ufs_page->kernel_native.cnr_at_switch,
		};
		uint32_t mode = check, compare = 0;

		PE_parse_boot_argn("ufs_native_check", &compare, sizeof(compare));
		if (!ufshc_probe(&offer, mode == 2)) {
			if (mode != 2) {
				printf("ufs: this kernel's own UFS driver is not used: through the loader\n");
			}
		} else if (mode == 3) {
			printf("ufs: native: dry run: this kernel's list in and out, no command\n");
			if (ufs_native_in(false) == 0) {
				IOSleep(1000);
				ufs_native_out("dry run");
				IOSleep(500);
				ufs_native_in(false);
				printf("ufs: native: dry run: the list stays in for the end of the run\n");
			}
		} else if (ufs_native_map(true)) {
			ufs_native_mapped = true;
			if (compare != 0) {
				/* right here, in bsd_init's thread, and then back to the loader */
				ufs_native_check();
				redwood_ufs_report("after the check");
				loader_return();
			}
			ufs_native_on = true;
			compare = 0;
			if (PE_parse_boot_argn("ufs_native_bench", &compare, sizeof(compare)) && compare != 0) {
				ufs_native_bench();
				redwood_ufs_report("after the bench");
				loader_return();
			}
			printf("ufs: the partitions (LUN 0-%u) are read by this kernel's own UFS driver, its list in the "
			    "controller (kern.redwood.ufs_native=0: through the loader)\n", ufs_native_luns - 1);
		} else {
			printf("ufs: this kernel's own UFS driver is not used: through the loader\n");
		}
	}

	if (PE_parse_boot_argn("ufs_bench", &check, sizeof(check)) && check != 0) {
		/* right here, in bsd_init's thread, and then back to the loader: nothing else is wanted */
		ufs_bench(NULL, THREAD_AWAKENED);
		loader_return();
	}
	check = 0;
	if (PE_parse_boot_argn("ufs_check", &check, sizeof(check)) && check == 2) {
		/* right here, whichever way the partitions are read (ufs_native=), and back to the loader */
		ufs_check((void *)1, THREAD_AWAKENED);
		redwood_ufs_report("after the check");
		loader_return();
	}
	if (check == 1) {
		thread_t thread;

		if (kernel_thread_start(ufs_check, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
}
