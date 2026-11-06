/*
 * The phone's UFS host controller, driven from this kernel -- read only, next to UEFI's driver.
 *
 * Reading the storage through the loader (redwood_ufs.c) is one request at a time: Qualcomm's
 * UFSDxe reads synchronously on core 0, and its transfer request list has room for one request
 * only (UTRLBA points at a single descriptor, with UFSDxe's command descriptor right behind it,
 * where slots 8.. would be). The controller takes 32 at once. So for the length of a run this
 * kernel puts a list of its own into the controller: with nothing in flight, the list is stopped
 * (UTRLRSR 0), UTRLBA/UTRLBAU are pointed at the new one, the list is started again; at the end
 * (redwood_ufs_quiesce, from loader_return), what is in flight is waited for, the list stopped,
 * UEFI's address written back, the list started. The list and its command descriptors are in
 * memory the loader owns (it offers it in its page, with UEFI's address); the loader checks the
 * address on every way back from a run and puts UEFI's back itself if the kernel did not
 * (xnuloader/loader.c, hc_take_back), and neither reads nor writes for the kernel or for its log
 * while another list is in. While this kernel's list is in, it asks the loader for nothing.
 *
 * Nothing else of the controller is changed: no reset, no link startup, no UIC command, no
 * interrupt enable, aggregation, clock or power setting; the task management list is not
 * touched; IS and UTRLCNR are only read. UTRLCLR is written only to clear this kernel's own
 * slots when they do not finish at the end.
 *
 * Read only by construction: a command is made in a buffer of its own, and only one that passes
 * ufshc_permitted() is ever written into a slot; what the slot then holds is read back and must
 * be the same and pass again before the doorbell is rung. The whitelist checks three things of
 * every command: the SCSI operation (READ(10)/READ(16), TEST UNIT READY, REQUEST SENSE, INQUIRY,
 * READ CAPACITY(10/16), nothing else -- no query, no task management, no NOP), the data
 * direction in the request descriptor (from the device or none, never to it), and the command
 * UPIU's flags (never W); and the logical unit: never a well-known one (REPORT LUNS, BOOT, RPMB,
 * the device), and only those reads are allowed to go to now (ufshc_set_luns: LUN 0, where the
 * iOS volume is, unless a comparison with the loader's reads asks for more). A read's length in
 * the CDB must be what the UPIU expects. It is tried at the probe on made-up commands that must
 * pass or be refused. This file builds READ(10) and TEST UNIT READY only.
 *
 * Every slot of this list holds a whitelisted command at all times: TEST UNIT READY until it is
 * used for a read. Slot 0 is kept for the proof that the controller does read this list: after
 * every switch, before any read, slot 0's TEST UNIT READY is rung and must come back in this
 * list (its status written here). Were the controller still reading UEFI's list, the doorbell
 * would run UEFI's slot 0 instead -- so first UEFI's slot 0 must hold a read too (it does: the
 * loader's last use of the storage before a run is reading back the log it wrote), and the
 * proof costs nothing either way. Reads go in slots 1..31.
 *
 * The doorbell is rung only with interrupts off, after looking that this kernel's list is the
 * one in the controller and running, that the slot is free, and that the loader is not taking
 * the controller back (it says so in its page, then waits before it does).
 *
 * The list and the command descriptors are mapped here without caching (nothing of them is ever
 * in a cache line of this kernel's to be written back over what the controller writes); the data
 * buffers are this kernel's memory, cleaned and invalidated around every read.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <os/atomic_private.h>
#include <libkern/libkern.h>
#include <stdarg.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <vm/vm_kern.h>
#include <vm/memory_types.h>
#include "redwood_ufshc.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */
extern boolean_t pmap_valid_address(uint64_t addr);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern void IOSleep(unsigned int milliseconds);
extern boolean_t ml_set_interrupts_enabled(boolean_t enable);

#define UFSHC_PHYS          0x1d84000ULL        /* ufshc@1d84000, "ufs_mem", in the phone's Android device tree */
#define UFSHC_SIZE          0x3000

/* UFSHCI registers (JESD223). 0x38..0x48, the UIC error codes, clear when read: never read here. */
#define UFSHC_CAP           0x00
#define UFSHC_VER           0x08
#define UFSHC_HCDDID        0x10
#define UFSHC_HCPMID        0x14
#define UFSHC_AHIT          0x18
#define UFSHC_IS            0x20
#define UFSHC_IE            0x24
#define UFSHC_HCS           0x30
#define UFSHC_HCE           0x34
#define UFSHC_UTRIACR       0x4c
#define UFSHC_UTRLBA        0x50
#define UFSHC_UTRLBAU       0x54
#define UFSHC_UTRLDBR       0x58
#define UFSHC_UTRLCLR       0x5c
#define UFSHC_UTRLRSR       0x60
#define UFSHC_UTRLCNR       0x64
#define UFSHC_UTMRLBA       0x70
#define UFSHC_UTMRLBAU      0x74
#define UFSHC_UTMRLDBR      0x78
#define UFSHC_UTMRLRSR      0x80

#define CAP_NUTRS(c)        (((c) & 0x1f) + 1)
#define CAP_64AS            (1u << 24)
#define HCS_DP              (1u << 0)
#define HCS_UTRLRDY         (1u << 1)
#define IS_FATAL            ((1u << 2) | (1u << 11) | (1u << 12) | (1u << 16) | (1u << 17))  /* UE, DFES, UTPES, HCFES, SBFES */

/* the request descriptor's first word: command type, data direction */
#define UTRD_CT_MASK        0xf0000000u
#define UTRD_CT_UFS         (1u << 28)
#define UTRD_DD_MASK        (3u << 25)
#define UTRD_DD_NONE        (0u << 25)
#define UTRD_DD_OUT         (1u << 25)          /* host to device: never */
#define UTRD_DD_IN          (2u << 25)
#define OCS_SUCCESS         0x0
#define OCS_INVALID         0xf

#define UPIU_COMMAND        0x01
#define UPIU_RESPONSE       0x21
#define UPIU_FLAG_READ      0x40
#define UPIU_FLAG_WRITE     0x20
#define LUN_WELL_KNOWN      0x80

#define SLOTS               32                  /* this kernel's list */
#define SLOT_PROOF          0
#define UCD_RESPONSE        512
#define UCD_PRDT            1024
#define PRD_MAX             (256u << 10)        /* the most one table entry describes */
#define SLOT_BUFFER_DEFAULT (256u << 10)
#define BLOCK               4096u               /* the logical block size of every unit here */

#define TIMEOUT_MS          10000

struct hc_cmd {
	uint8_t upiu[32];
	uint32_t dw0;
};

struct hc_slot {
	uint32_t number;                        /* in this kernel's list */
	volatile uint32_t *utrd;                /* its descriptor (the loader's memory, not cached here) */
	volatile uint32_t *ucd;                 /* its command descriptor (likewise) */
	uint64_t ucd_pa;
	uint8_t *data;                          /* this kernel's memory, cached */
	uint64_t data_pa;
};

static vm_offset_t hc;                          /* the registers */
static struct ufshc_loader hc_loader;
static uint32_t hc_list_lo, hc_list_hi;         /* this kernel's list's address */
static volatile uint32_t *hc_uefi_list;         /* UEFI's list, not cached here, only read */
static uint64_t hc_uefi_map_pa, hc_uefi_map_size;
static vm_offset_t hc_uefi_map_va;
static struct hc_slot hc_slots[SLOTS];
static uint32_t hc_nslots;                      /* slots 1..hc_nslots carry reads */
static uint32_t hc_free;                        /* bit i: hc_slots[i] free */
static uint32_t hc_slot_buffer = SLOT_BUFFER_DEFAULT;
static uint32_t hc_lun_mask = 1;                /* the logical units reads may go to: LUN 0 */
static bool hc_usable;
static uint32_t hc_leave;                       /* ufs_native_leave=1 (a test): the list left in at the end, for the loader */
static volatile bool hc_in, hc_closed, hc_dead;
static uint32_t hc_is_in, hc_cnr_in;            /* IS and UTRLCNR when this kernel's list went in */
static uint64_t hc_requests, hc_errors, hc_bytes, hc_wait_us, hc_refused, hc_most_in_flight, hc_proofs;
static uint64_t hc_switch_in_count, hc_switch_out_count;
static uint32_t hc_in_flight;
/* where the time goes: doorbell reads while waiting, waits for a free slot, taking hc_lock, how many in flight */
static uint64_t hc_polls, hc_slot_waits, hc_slot_wait_ticks, hc_lock_ticks, hc_flight_hist[6];

static LCK_GRP_DECLARE(hc_lck_grp, "redwood-ufshc");
static LCK_MTX_DECLARE(hc_lock, &hc_lck_grp);

SYSCTL_DECL(_kern_redwood);
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_native_requests, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_requests,
    "reads this kernel's own UFS driver has had answered");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_native_errors, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_errors,
    "reads of this kernel's own UFS driver that failed");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_native_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_bytes,
    "bytes this kernel's own UFS driver has read");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_native_wait_us, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_wait_us,
    "microseconds spent waiting for the controller, summed over the reads");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_native_refused, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_refused,
    "commands the whitelist refused (there should never be one)");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_native_most_in_flight, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_most_in_flight,
    "the most reads this kernel's own UFS driver has had in flight at once");

/*
 * What this file says goes to the kernel's log, and the last lines are kept: the start of a long
 * run's log does not come back to the loader (the buffer wraps), so they are said again at the
 * end, into the loader's log directly (redwood_ufs_quiesce; printf is not for that moment).
 */
extern void loader_log_putc(char c);
#define KEPT_LINES          64
static char hc_kept[KEPT_LINES][208];
static uint32_t hc_kept_count;

static void
hc_say(const char *format, ...) __printflike(1, 2);
static void
hc_say(const char *format, ...)
{
	char line[208];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	printf("%s", line);
	strlcpy(hc_kept[hc_kept_count++ % KEPT_LINES], line, sizeof(hc_kept[0]));
}

/* the same, at the end: into the loader's log only */
static void
hc_say_late(const char *format, ...) __printflike(1, 2);
static void
hc_say_late(const char *format, ...)
{
	char line[208];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
}

static inline uint32_t
hc_read(uint32_t reg)
{
	return *(volatile uint32_t *)(hc + reg);
}

static inline void
hc_write(uint32_t reg, uint32_t value)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(hc + reg) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static uint64_t
hc_us_since(uint64_t start)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
	return ns / 1000;
}

/* ---- the whitelist: the one check every command passes before it goes into a slot ---- */

static const char *
hc_refusal(const uint8_t *upiu, uint32_t dw0, uint32_t lun_mask, bool any_command_type)
{
	const uint8_t *cdb = upiu + 16;
	uint32_t dd = dw0 & UTRD_DD_MASK;
	uint64_t expected = (uint64_t)upiu[12] << 24 | (uint64_t)upiu[13] << 16 | (uint64_t)upiu[14] << 8 | upiu[15];
	uint8_t lun = upiu[2];

	/* the data direction: from the device, or none -- never to it */
	if (dd == UTRD_DD_OUT || dd == UTRD_DD_MASK) {
		return "data from host to device";
	}
	if (!any_command_type && (dw0 & UTRD_CT_MASK) != UTRD_CT_UFS) {
		return "not a UFS storage command type";
	}
	/* a SCSI command, and its flags: never W */
	if ((upiu[0] & 0x3f) != UPIU_COMMAND) {
		return "not a SCSI command (a query, task management, NOP, ...)";
	}
	if (upiu[1] & UPIU_FLAG_WRITE) {
		return "the W flag";
	}
	if (upiu[4] & 0x0f) {
		return "not the SCSI command set";
	}
	/* a normal logical unit, and one reads may go to now */
	if (lun & LUN_WELL_KNOWN) {
		return "a well-known logical unit";
	}
	if (lun >= 8 || (lun_mask & (1u << lun)) == 0) {
		return "a logical unit reads do not go to now";
	}
	/* the operation */
	switch (cdb[0]) {
	case 0x28:      /* READ(10) */
	case 0x88: {    /* READ(16) */
		uint64_t blocks = cdb[0] == 0x28 ? ((uint64_t)cdb[7] << 8 | cdb[8]) :
		    ((uint64_t)cdb[10] << 24 | (uint64_t)cdb[11] << 16 | (uint64_t)cdb[12] << 8 | cdb[13]);

		if (dd != UTRD_DD_IN || (upiu[1] & UPIU_FLAG_READ) == 0) {
			return "a read without data from the device";
		}
		if (blocks == 0 || expected != blocks * BLOCK) {
			return "a read whose length is not what the UPIU expects";
		}
		return NULL;
	}
	case 0x00:      /* TEST UNIT READY */
		return dd == UTRD_DD_NONE && expected == 0 && (upiu[1] & UPIU_FLAG_READ) == 0 ? NULL :
		       "TEST UNIT READY with data";
	case 0x03:      /* REQUEST SENSE */
	case 0x12:      /* INQUIRY */
	case 0x25:      /* READ CAPACITY(10) */
		return NULL;
	case 0x9e:      /* SERVICE ACTION IN(16): READ CAPACITY(16) only */
		return (cdb[1] & 0x1f) == 0x10 ? NULL : "a SERVICE ACTION IN other than READ CAPACITY(16)";
	default:
		return "a SCSI operation not on the list";
	}
}

static bool
ufshc_permitted(const uint8_t *upiu, uint32_t dw0)
{
	const char *why = hc_refusal(upiu, dw0, hc_lun_mask, false);

	if (why == NULL) {
		return true;
	}
	hc_refused++;
	hc_say("ufs: native: REFUSED (%s): UPIU %02x %02x %02x %02x, CDB %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x, "
	    "descriptor %08x; it goes into no slot\n", why, upiu[0], upiu[1], upiu[2], upiu[3], upiu[16], upiu[17], upiu[18],
	    upiu[19], upiu[20], upiu[21], upiu[22], upiu[23], upiu[24], upiu[25], dw0);
	return false;
}

static void
hc_make_read(struct hc_cmd *c, uint8_t lun, uint8_t tag, uint32_t lba, uint32_t blocks)
{
	uint32_t bytes = blocks * BLOCK;

	bzero(c, sizeof(*c));
	c->upiu[0] = UPIU_COMMAND;
	c->upiu[1] = UPIU_FLAG_READ;            /* simple task attribute */
	c->upiu[2] = lun;
	c->upiu[3] = tag;
	c->upiu[12] = (uint8_t)(bytes >> 24);
	c->upiu[13] = (uint8_t)(bytes >> 16);
	c->upiu[14] = (uint8_t)(bytes >> 8);
	c->upiu[15] = (uint8_t)bytes;
	c->upiu[16] = 0x28;                     /* READ(10) */
	c->upiu[18] = (uint8_t)(lba >> 24);
	c->upiu[19] = (uint8_t)(lba >> 16);
	c->upiu[20] = (uint8_t)(lba >> 8);
	c->upiu[21] = (uint8_t)lba;
	c->upiu[23] = (uint8_t)(blocks >> 8);
	c->upiu[24] = (uint8_t)blocks;
	c->dw0 = UTRD_CT_UFS | UTRD_DD_IN;      /* no interrupt, no inline encryption */
}

static void
hc_make_tur(struct hc_cmd *c, uint8_t lun, uint8_t tag)
{
	bzero(c, sizeof(*c));
	c->upiu[0] = UPIU_COMMAND;
	c->upiu[2] = lun;
	c->upiu[3] = tag;
	c->upiu[16] = 0x00;                     /* TEST UNIT READY */
	c->dw0 = UTRD_CT_UFS | UTRD_DD_NONE;
}

/*
 * The whitelist tried on commands made up here and never sent: those that must be refused,
 * and those that must pass. Pure bytes; nothing touches the controller.
 */
static bool
hc_whitelist_selftest(void)
{
	static const struct {
		uint8_t type, flags, lun, op, cdb1;
		uint16_t blocks;                /* the CDB's length (READ(10)/(16)) */
		uint32_t expected;              /* the UPIU's */
		uint32_t dd;
		bool pass;
		const char *what;
	} cases[] = {
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0, 0x28, 0, 1, 4096, UTRD_DD_IN, true, "READ(10) of LUN 0" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0, 0x88, 0, 1, 4096, UTRD_DD_IN, true, "READ(16) of LUN 0" },
		{ UPIU_COMMAND, 0, 0, 0x00, 0, 0, 0, UTRD_DD_NONE, true, "TEST UNIT READY of LUN 0" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0, 0x25, 0, 0, 8, UTRD_DD_IN, true, "READ CAPACITY(10)" },
		{ UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0x2a, 0, 1, 4096, UTRD_DD_OUT, false, "WRITE(10)" },
		{ UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0x8a, 0, 1, 4096, UTRD_DD_OUT, false, "WRITE(16)" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0, 0x2a, 0, 1, 4096, UTRD_DD_IN, false, "WRITE(10) dressed as a read" },
		{ UPIU_COMMAND, 0, 0, 0x2a, 0, 0, 0, UTRD_DD_NONE, false, "WRITE(10) with no data" },
		{ UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0x42, 0, 0, 24, UTRD_DD_OUT, false, "UNMAP" },
		{ UPIU_COMMAND, 0, 0, 0x42, 0, 0, 0, UTRD_DD_NONE, false, "UNMAP with no data" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0, 0x28, 0, 1, 4096, UTRD_DD_OUT, false, "READ(10) with data host to device" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0, 0x28, 0, 1, 4096, UTRD_DD_MASK, false, "READ(10) with direction 3" },
		{ UPIU_COMMAND, UPIU_FLAG_READ | UPIU_FLAG_WRITE, 0, 0x28, 0, 1, 4096, UTRD_DD_IN, false, "READ(10) with the W flag" },
		{ UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0x28, 0, 1, 4096, UTRD_DD_IN, false, "READ(10) with only the W flag" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0, 0x28, 0, 1, 8192, UTRD_DD_IN, false, "READ(10) of a length other than expected" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0xc4, 0x28, 0, 1, 4096, UTRD_DD_IN, false, "READ(10) of the RPMB unit" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0xb0, 0x28, 0, 1, 4096, UTRD_DD_IN, false, "READ(10) of the BOOT unit" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0xd0, 0x28, 0, 1, 4096, UTRD_DD_IN, false, "READ(10) of the device unit" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0x81, 0x28, 0, 1, 4096, UTRD_DD_IN, false, "READ(10) of REPORT LUNS" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 3, 0x28, 0, 1, 4096, UTRD_DD_IN, false, "READ(10) of LUN 3 (not allowed now)" },
		{ UPIU_COMMAND, 0, 0, 0x04, 0, 0, 0, UTRD_DD_NONE, false, "FORMAT UNIT" },
		{ UPIU_COMMAND, 0, 0, 0x1b, 0, 0, 0, UTRD_DD_NONE, false, "START STOP UNIT" },
		{ UPIU_COMMAND, 0, 0, 0x35, 0, 0, 0, UTRD_DD_NONE, false, "SYNCHRONIZE CACHE(10)" },
		{ UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0x3b, 0, 0, 4096, UTRD_DD_OUT, false, "WRITE BUFFER" },
		{ UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0xb5, 0, 0, 512, UTRD_DD_OUT, false, "SECURITY PROTOCOL OUT" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0, 0xa2, 0, 0, 512, UTRD_DD_IN, false, "SECURITY PROTOCOL IN" },
		{ UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0x15, 0, 0, 24, UTRD_DD_OUT, false, "MODE SELECT" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0, 0x9e, 0x11, 0, 32, UTRD_DD_IN, false, "SERVICE ACTION IN 0x11" },
		{ UPIU_COMMAND, 0, 0, 0x00, 0, 0, 0, UTRD_DD_IN, false, "TEST UNIT READY with data in" },
		{ 0x16, 0, 0, 0, 0, 0, 0, UTRD_DD_NONE, false, "a QUERY REQUEST" },
		{ 0x04, 0, 0, 0, 0, 0, 0, UTRD_DD_NONE, false, "TASK MANAGEMENT" },
		{ 0x00, 0, 0, 0, 0, 0, 0, UTRD_DD_NONE, false, "NOP OUT" },
	};
	bool right = true;

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		uint8_t upiu[32] = { 0 };
		bool passed;

		upiu[0] = cases[i].type;
		upiu[1] = cases[i].flags;
		upiu[2] = cases[i].lun;
		upiu[12] = (uint8_t)(cases[i].expected >> 24);
		upiu[13] = (uint8_t)(cases[i].expected >> 16);
		upiu[14] = (uint8_t)(cases[i].expected >> 8);
		upiu[15] = (uint8_t)cases[i].expected;
		upiu[16] = cases[i].op;
		upiu[17] = cases[i].cdb1;
		if (cases[i].op == 0x88) {
			upiu[16 + 12] = (uint8_t)(cases[i].blocks >> 8);
			upiu[16 + 13] = (uint8_t)cases[i].blocks;
		} else {
			upiu[16 + 7] = (uint8_t)(cases[i].blocks >> 8);
			upiu[16 + 8] = (uint8_t)cases[i].blocks;
		}
		passed = hc_refusal(upiu, UTRD_CT_UFS | cases[i].dd, 1u << 0, false) == NULL;
		if (passed != cases[i].pass) {
			hc_say("ufs: native: the whitelist %s %s\n", passed ? "LETS THROUGH" : "refuses", cases[i].what);
			right = false;
		}
	}
	/* and what this file makes */
	{
		struct hc_cmd c;

		hc_make_read(&c, 0, 1, 12345, 64);
		right &= hc_refusal(c.upiu, c.dw0, 1u << 0, false) == NULL;
		hc_make_read(&c, 2, 1, 12345, 64);
		right &= hc_refusal(c.upiu, c.dw0, 1u << 0, false) != NULL;
		hc_make_tur(&c, 0, 0);
		right &= hc_refusal(c.upiu, c.dw0, 1u << 0, false) == NULL;
	}
	hc_say("ufs: native: whitelist tried on %u commands made up and never sent -- READ(10/16), TEST UNIT READY, READ "
	    "CAPACITY of LUN 0 pass; WRITE(10/16), UNMAP, a READ with data host to device, a READ with the W flag, the "
	    "RPMB/BOOT/device/REPORT LUNS units, LUN 3, FORMAT, START STOP, SYNCHRONIZE CACHE, WRITE BUFFER, SECURITY "
	    "PROTOCOL, MODE SELECT, query, task management, NOP refused: %s\n",
	    (unsigned)(sizeof(cases) / sizeof(cases[0])) + 3, right ? "as it should" : "WRONG");
	return right;
}

/* ---- one command in one slot ---- */

static void
hc_put(volatile uint32_t *to, const void *from, uint32_t bytes)
{
	for (uint32_t i = 0; i < bytes / 4; i++) {
		uint32_t w;

		memcpy(&w, (const uint8_t *)from + 4 * i, 4);
		to[i] = w;
	}
}

/*
 * Into the slot: the command UPIU, an empty response, the PRD table for bytes of the slot's
 * buffer, the descriptor -- only a command the whitelist passes, and only if what the slot then
 * holds is the same and passes again.
 */
static int
hc_place(struct hc_slot *s, const struct hc_cmd *c, uint32_t bytes)
{
	uint32_t n = 0, back[8];

	if (!ufshc_permitted(c->upiu, c->dw0)) {
		return EPERM;
	}
	hc_put(s->ucd, c->upiu, sizeof(c->upiu));
	for (uint32_t i = sizeof(c->upiu) / 4; i < UCD_PRDT / 4; i++) {
		s->ucd[i] = 0;
	}
	for (uint32_t at = 0; at < bytes; at += PRD_MAX, n++) {
		volatile uint32_t *prd = s->ucd + (UCD_PRDT + 16 * n) / 4;
		uint64_t pa = s->data_pa + at;
		uint32_t len = bytes - at < PRD_MAX ? bytes - at : PRD_MAX;

		prd[0] = (uint32_t)pa;
		prd[1] = (uint32_t)(pa >> 32);
		prd[2] = 0;
		prd[3] = len - 1;
	}
	s->utrd[0] = c->dw0;
	s->utrd[1] = 0;
	s->utrd[2] = OCS_INVALID;
	s->utrd[3] = 0;
	s->utrd[4] = (uint32_t)s->ucd_pa;
	s->utrd[5] = (uint32_t)(s->ucd_pa >> 32);
	s->utrd[6] = ((UCD_RESPONSE / 4) << 16) | (UCD_RESPONSE / 4);   /* response: offset, length in dwords */
	s->utrd[7] = ((UCD_PRDT / 4) << 16) | n;                        /* PRD table: offset in dwords, entries */
	__asm__ volatile ("dsb sy" ::: "memory");
	for (int i = 0; i < 8; i++) {
		back[i] = s->ucd[i];
	}
	if (memcmp(back, c->upiu, sizeof(back)) != 0 || s->utrd[0] != c->dw0 ||
	    !ufshc_permitted((const uint8_t *)back, s->utrd[0])) {
		hc_say("ufs: native: slot %u does not hold what was put in it: not rung\n", s->number);
		hc_refused++;
		return EPERM;
	}
	return 0;
}

/* The doorbell: only while this kernel's list is the one in, running, and the loader not taking it back */
static int
hc_ring(struct hc_slot *s)
{
	uint32_t bit = 1u << s->number;
	boolean_t istate;
	bool ok;

	lck_mtx_lock(&hc_lock);
	istate = ml_set_interrupts_enabled(FALSE);
	ok = hc_in && !hc_closed && !hc_dead && *hc_loader.take_back == 0 &&
	    hc_read(UFSHC_UTRLBA) == hc_list_lo && hc_read(UFSHC_UTRLBAU) == hc_list_hi &&
	    (hc_read(UFSHC_UTRLRSR) & 1) != 0 && (hc_read(UFSHC_UTRLDBR) & bit) == 0;
	if (ok) {
		hc_write(UFSHC_UTRLDBR, bit);
	}
	ml_set_interrupts_enabled(istate);
	lck_mtx_unlock(&hc_lock);
	return ok ? 0 : ENXIO;
}

/*
 * ufs_native_hang=N (a test of the loader's side): once N reads are done, the thread that has just
 * rung a doorbell stops the kernel -- every processor spinning with interrupts off, this one
 * included -- with reads in flight and this kernel's list in. The heartbeat stops, and the
 * loader (or the watchdog) must take it from there.
 */
extern unsigned int cpu_broadcast_xcall(uint32_t *synch, boolean_t self_xcall, void (*func)(void *), void *parm);
static uint32_t hc_hang_after;
static volatile uint32_t hc_hanging;

static void
hc_spin_forever(__unused void *parm)
{
	(void)ml_set_interrupts_enabled(FALSE);
	for (;;) {
		__builtin_arm_yield();
	}
}

static void
hc_hang(uint32_t slot)
{
	static int parm;

	/* (into the loader's log directly as well: a hung kernel's own log never comes back to it) */
	hc_say_late("ufs: native: ufs_native_hang=%u: the kernel stops HERE, every processor spinning with interrupts "
	    "off; slot %u just rung, doorbell %08x, %u reads in flight, this kernel's list in\n", hc_hang_after, slot,
	    hc_read(UFSHC_UTRLDBR), hc_in_flight);
	cpu_broadcast_xcall(NULL, TRUE, hc_spin_forever, &parm);
}

/* Nothing more is sent from here on; whoever waits for a slot is told. */
static void
hc_off(void)
{
	hc_dead = true;
	lck_mtx_lock(&hc_lock);
	wakeup(&hc_free);
	lck_mtx_unlock(&hc_lock);
}

/*
 * Waiting for the controller. Its interrupt stays off (UEFI's setting), so completions are seen
 * by reading UTRLDBR. A waiter looks at its own bit for a short while (hc_spin_us: a small read
 * is done by then), then sleeps; one poller thread looks for all who sleep and wakes each whose
 * slot is done. So the processors are not all taken by threads spinning on the doorbell, which
 * kept others -- one woken because the volume's lock came free, say -- from running.
 */
static volatile uint32_t hc_waiting;            /* slots whose waiters sleep (hc_lock) */
static uint32_t hc_done;                        /* slots the poller has seen done, not yet collected (hc_lock) */
static bool hc_poller_asleep;
static uint32_t hc_spin_us = 40;                /* boot-arg ufs_native_spin */
static uint64_t hc_sleeps, hc_poller_polls, hc_poller_rounds;

static void
hc_poller(__unused void *arg, __unused wait_result_t wr)
{
	for (;;) {
		uint32_t waiting, db, done;

		lck_mtx_lock(&hc_lock);
		while (hc_waiting == 0) {
			hc_poller_asleep = true;
			msleep((void *)&hc_waiting, &hc_lock, PRIBIO, "ufshc-poll", NULL);
		}
		hc_poller_asleep = false;
		waiting = hc_waiting;
		lck_mtx_unlock(&hc_lock);
		hc_poller_rounds++;
		/* until one of them is done, or another waiter comes */
		while (((db = hc_read(UFSHC_UTRLDBR)) & waiting) == waiting && hc_waiting == waiting) {
			hc_poller_polls++;
			__builtin_arm_yield();
		}
		done = waiting & ~db;
		if (done != 0) {
			lck_mtx_lock(&hc_lock);
			hc_done |= done;
			hc_waiting &= ~done;
			for (uint32_t m = done; m != 0; m &= m - 1) {
				wakeup(&hc_slots[__builtin_ctz(m)]);
			}
			lck_mtx_unlock(&hc_lock);
		}
	}
}

/* Until the controller is done with the slot's command; then whether it went well. */
static int
hc_complete(struct hc_slot *s, const char *what, bool tur)
{
	uint64_t started = mach_absolute_time(), us;
	volatile const uint8_t *r = (volatile const uint8_t *)s->ucd + UCD_RESPONSE;
	uint32_t ocs, bit = 1u << s->number;

	uint64_t polls = 1;

	while ((hc_read(UFSHC_UTRLDBR) & bit) && hc_us_since(started) < hc_spin_us) {
		polls++;
		__builtin_arm_yield();
	}
	if (hc_read(UFSHC_UTRLDBR) & bit) {
		struct timespec second = { 1, 0 };

		lck_mtx_lock(&hc_lock);
		hc_sleeps++;
		hc_waiting |= bit;
		if (hc_poller_asleep) {
			wakeup((void *)&hc_waiting);
		}
		while ((hc_done & bit) == 0) {
			(void)msleep(s, &hc_lock, PRIBIO, "ufshc-io", &second);
			us = hc_us_since(started);
			if ((hc_done & bit) == 0 && us > TIMEOUT_MS * 1000ULL) {
				hc_waiting &= ~bit;
				lck_mtx_unlock(&hc_lock);
				hc_say("ufs: native: slot %u (%s) not done in %u s (doorbell %08x, IS %08x, HCS %08x): "
				    "this path is off from now on\n", s->number, what, TIMEOUT_MS / 1000, hc_read(UFSHC_UTRLDBR),
				    hc_read(UFSHC_IS), hc_read(UFSHC_HCS));
				hc_off();
				return ETIMEDOUT;
			}
		}
		hc_done &= ~bit;
		lck_mtx_unlock(&hc_lock);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	os_atomic_add(&hc_polls, polls, relaxed);
	if (!tur) {
		os_atomic_add(&hc_wait_us, hc_us_since(started), relaxed);
	}
	ocs = s->utrd[2] & 0xff;
	if (ocs != OCS_SUCCESS || (r[0] & 0x3f) != UPIU_RESPONSE || r[3] != s->number || r[6] != 0 ||
	    (r[7] != 0 && !(tur && r[7] == 0x02))) {
		/* sense data follows the 32 bytes of the header: its length (2), then the fixed format */
		hc_say("ufs: native: slot %u (%s): OCS %x, response %02x tag %02x, response %02x status %02x, "
		    "sense key %x asc %02x ascq %02x\n", s->number, what, ocs, r[0], r[3], r[6], r[7],
		    r[34 + 2] & 0x0f, r[34 + 12], r[34 + 13]);
		if (!tur) {
			os_atomic_inc(&hc_errors, relaxed);
		}
		return EIO;
	}
	return 0;
}

static struct hc_slot *
hc_take(bool wait)
{
	struct hc_slot *s = NULL;
	uint64_t t0 = mach_absolute_time();

	lck_mtx_lock(&hc_lock);
	hc_lock_ticks += mach_absolute_time() - t0;
	if (hc_free == 0 && wait && !hc_dead && hc_in) {
		uint64_t w0 = mach_absolute_time();

		hc_slot_waits++;
		while (hc_free == 0 && wait && !hc_dead && hc_in) {
			msleep(&hc_free, &hc_lock, PRIBIO, "ufshc", NULL);
		}
		hc_slot_wait_ticks += mach_absolute_time() - w0;
	}
	if (hc_free != 0 && hc_in && !hc_dead) {
		uint32_t i = (uint32_t)__builtin_ctz(hc_free), f;

		hc_free &= ~(1u << i);
		s = &hc_slots[i];
		f = ++hc_in_flight;
		if (f > hc_most_in_flight) {
			hc_most_in_flight = f;
		}
		hc_flight_hist[f <= 1 ? 0 : f <= 2 ? 1 : f <= 4 ? 2 : f <= 8 ? 3 : f <= 16 ? 4 : 5]++;
	}
	lck_mtx_unlock(&hc_lock);
	return s;
}

static void
hc_give(struct hc_slot *s)
{
	lck_mtx_lock(&hc_lock);
	hc_free |= 1u << (s - hc_slots);
	hc_in_flight--;
	wakeup(&hc_free);
	lck_mtx_unlock(&hc_lock);
}

/*
 * Bytes [offset, offset + len) of logical unit lun into dst: the blocks that cover them, a slot's
 * buffer at a time, with as many slots in flight at once as are free (at least one), the wanted
 * bytes copied out of each as it is done.
 */
int
ufshc_read(uint8_t lun, uint64_t offset, uint8_t *dst, uint64_t len)
{
	struct {
		struct hc_slot *slot;
		uint32_t lba, blocks, skip, take;
		uint8_t *to;
	} q[SLOTS];
	uint32_t head = 0, tail = 0, per = hc_slot_buffer / BLOCK;
	int error = 0;

	if (!hc_usable || !hc_in || hc_dead || hc_closed) {
		return ENXIO;
	}
	while ((len > 0 && error == 0) || head != tail) {
		/* as many more reads as there are free slots (and one, waiting for it, if none is in flight) */
		while (len > 0 && error == 0 && tail - head < SLOTS) {
			uint64_t first = offset / BLOCK, skip = offset % BLOCK;
			uint64_t blocks = (skip + len + BLOCK - 1) / BLOCK, take;
			struct hc_slot *s = hc_take(head == tail);
			struct hc_cmd c;
			int e;

			if (s == NULL) {
				if (head == tail) {
					error = ENXIO;                  /* the path went off while waiting */
				}
				break;
			}
			if (blocks > per) {
				blocks = per;
			}
			if (first + blocks > 0xffffffffULL) {
				hc_give(s);
				error = EINVAL;
				break;
			}
			take = blocks * BLOCK - skip;
			if (take > len) {
				take = len;
			}
			hc_make_read(&c, lun, (uint8_t)s->number, (uint32_t)first, (uint32_t)blocks);
			/* nothing of the buffer is left in a cache to be written back over what the controller writes */
			FlushPoC_DcacheRegion((vm_offset_t)s->data, (size_t)blocks * BLOCK);
			e = hc_place(s, &c, (uint32_t)blocks * BLOCK);
			if (e == 0) {
				e = hc_ring(s);
			}
			if (e == 0 && hc_hang_after != 0 && hc_requests >= hc_hang_after &&
			    os_atomic_cmpxchg(&hc_hanging, 0, 1, relaxed)) {
				hc_hang(s->number);
			}
			if (e != 0) {
				hc_give(s);
				error = e;
				break;
			}
			q[tail % SLOTS].slot = s;
			q[tail % SLOTS].lba = (uint32_t)first;
			q[tail % SLOTS].blocks = (uint32_t)blocks;
			q[tail % SLOTS].skip = (uint32_t)skip;
			q[tail % SLOTS].take = (uint32_t)take;
			q[tail % SLOTS].to = dst;
			tail++;
			offset += take;
			dst += take;
			len -= take;
		}
		/* the oldest one done, copied out, its slot free again */
		if (head != tail) {
			typeof(q[0]) *e = &q[head % SLOTS];
			char what[48];
			int r;

			snprintf(what, sizeof(what), "LUN %u, blocks %u..%u", lun, e->lba, e->lba + e->blocks - 1);
			r = hc_complete(e->slot, what, false);
			if (r == 0) {
				FlushPoC_DcacheRegion((vm_offset_t)e->slot->data, (size_t)e->blocks * BLOCK);
				os_atomic_inc(&hc_requests, relaxed);
				os_atomic_add(&hc_bytes, (uint64_t)e->blocks * BLOCK, relaxed);
				if (error == 0) {
					memcpy(e->to, e->slot->data + e->skip, e->take);
				}
			} else if (error == 0) {
				error = r;
			}
			if (r != ETIMEDOUT) {
				hc_give(e->slot);                       /* (a slot that never came back is not used again) */
			}
			head++;
		}
	}
	return error;
}

/* ---- the proof, and the switches ---- */

/* UEFI's slot 0 holds a read (what the controller would run were it to take no notice of this list) */
static const char *
hc_uefi_slot0(uint8_t *upiu, uint32_t *dw0)
{
	uint64_t ucd = (uint64_t)hc_uefi_list[5] << 32 | hc_uefi_list[4];
	volatile const uint8_t *u;

	*dw0 = hc_uefi_list[0];
	if (ucd < hc_uefi_map_pa || ucd + 32 > hc_uefi_map_pa + hc_uefi_map_size) {
		return "UEFI's command descriptor is not where it was";
	}
	u = (volatile const uint8_t *)(hc_uefi_map_va + (ucd - hc_uefi_map_pa));
	for (int i = 0; i < 32; i++) {
		upiu[i] = u[i];
	}
	return hc_refusal(upiu, *dw0, 0xff, true) == NULL ? NULL : "UEFI's slot 0 does not hold a read";
}

/* This kernel's slot 0: TEST UNIT READY rung, and its status must come back in this list */
static int
hc_prove(void)
{
	struct hc_slot *s = &hc_slots[SLOT_PROOF];
	struct hc_cmd c;
	uint32_t ocs;
	int e;

	hc_make_tur(&c, (uint8_t)__builtin_ctz(hc_lun_mask), SLOT_PROOF);
	e = hc_place(s, &c, 0);
	if (e == 0) {
		e = hc_ring(s);
	}
	if (e == 0) {
		e = hc_complete(s, "the proof's TEST UNIT READY", true);
	}
	ocs = s->utrd[2] & 0xff;
	hc_proofs++;
	if (e != 0 || ocs != OCS_SUCCESS) {
		hc_say("ufs: native: proof FAILED (%d): this list's slot 0 has OCS %x after its doorbell -- the controller "
		    "did not run it from this list\n", e, ocs);
		return e != 0 ? e : EIO;
	}
	hc_say("ufs: native: proof: TEST UNIT READY in this list's slot 0 came back here (OCS 0, response %02x, status %02x)\n",
	    ((volatile const uint8_t *)s->ucd)[UCD_RESPONSE + 6], ((volatile const uint8_t *)s->ucd)[UCD_RESPONSE + 7]);
	return 0;
}

struct hc_put_back_result {
	uint32_t db_before, db_after_clear, lo, hi, rsr, db, is, cnr, cnr_ours;
	bool cleared, ok;
};

/*
 * UEFI's list back into the controller: what is in flight waited for (1 s), cleared if it does
 * not finish (every slot of this list is this kernel's), the list stopped, UEFI's address
 * written, the list started, read back. Interrupts off, no locks, no printing (the quiesce).
 */
static void
hc_put_back(struct hc_put_back_result *r)
{
	uint64_t t;

	bzero(r, sizeof(*r));
	for (t = mach_absolute_time(); (r->db_before = hc_read(UFSHC_UTRLDBR)) != 0 && hc_us_since(t) < 1000000;) {
		__builtin_arm_yield();
	}
	if (r->db_before != 0 && hc_read(UFSHC_UTRLBA) == hc_list_lo && hc_read(UFSHC_UTRLBAU) == hc_list_hi) {
		r->cleared = true;
		hc_write(UFSHC_UTRLCLR, 0);
		for (t = mach_absolute_time(); (r->db_after_clear = hc_read(UFSHC_UTRLDBR)) != 0 && hc_us_since(t) < 1000000;) {
			__builtin_arm_yield();
		}
	}
	/* the completion notifications this kernel's requests left: exactly those bits, written with ones */
	r->cnr_ours = hc_read(UFSHC_UTRLCNR) & ~hc_cnr_in;
	if (r->cnr_ours != 0) {
		hc_write(UFSHC_UTRLCNR, r->cnr_ours);
	}
	hc_write(UFSHC_UTRLRSR, 0);
	for (t = mach_absolute_time(); (hc_read(UFSHC_UTRLRSR) & 1) && hc_us_since(t) < 5000;) {
		__builtin_arm_yield();
	}
	hc_write(UFSHC_UTRLBA, hc_loader.uefi_lo);
	hc_write(UFSHC_UTRLBAU, hc_loader.uefi_hi);
	hc_write(UFSHC_UTRLRSR, 1);
	r->lo = hc_read(UFSHC_UTRLBA);
	r->hi = hc_read(UFSHC_UTRLBAU);
	r->rsr = hc_read(UFSHC_UTRLRSR);
	r->db = hc_read(UFSHC_UTRLDBR);
	r->is = hc_read(UFSHC_IS);
	r->cnr = hc_read(UFSHC_UTRLCNR);
	r->ok = r->lo == hc_loader.uefi_lo && r->hi == hc_loader.uefi_hi && (r->rsr & 1);
	if (r->ok) {
		*hc_loader.list_in = 0;
		(*hc_loader.switched_out)++;
		hc_switch_out_count++;
	}
}

static void
hc_put_back_print(const char *why, const struct hc_put_back_result *r, bool late)
{
	void (*say)(const char *, ...) = late ? hc_say_late : hc_say;

	if (r->cleared) {
		say("ufs: native: %s: doorbell %08x did not clear in 1 s: this kernel's slots cleared (UTRLCLR), doorbell "
		    "then %08x\n", why, r->db_before, r->db_after_clear);
	}
	say("ufs: native: %s: transfer list %s UEFI's %08x:%08x (run %u, doorbell %08x; IS %08x, at the switch %08x; "
	    "completion %08x after clearing this kernel's %08x, at the switch %08x%s)\n", why, r->ok ? "back to" :
	    "NOT BACK TO", r->hi, r->lo, r->rsr, r->db, r->is, hc_is_in, r->cnr, r->cnr_ours, hc_cnr_in,
	    r->cnr == hc_cnr_in ? ", as it was" : ", NOT AS IT WAS");
}

/*
 * This kernel's list into the controller (the caller holds the loader's mailbox: nothing is asked
 * of UEFI meanwhile). prove: then slot 0's TEST UNIT READY, before anything is read.
 */
int
ufshc_switch_in(bool prove)
{
	uint32_t lo, hi, db, rsr, hcs, uefi_dw0 = 0;
	uint8_t uefi_upiu[32] = { 0 };
	const char *no = NULL;
	struct hc_put_back_result undo;
	boolean_t istate;
	bool undone = false;

	if (!hc_usable || hc_dead || hc_closed) {
		return ENXIO;
	}
	lck_mtx_lock(&hc_lock);
	if (hc_in) {
		lck_mtx_unlock(&hc_lock);
		return 0;
	}
	istate = ml_set_interrupts_enabled(FALSE);
	if (*hc_loader.take_back != 0) {
		no = "the loader has taken the controller back";
	} else {
		no = hc_uefi_slot0(uefi_upiu, &uefi_dw0);
	}
	lo = hc_read(UFSHC_UTRLBA);
	hi = hc_read(UFSHC_UTRLBAU);
	db = hc_read(UFSHC_UTRLDBR);
	rsr = hc_read(UFSHC_UTRLRSR);
	hcs = hc_read(UFSHC_HCS);
	if (no == NULL) {
		if (lo != hc_loader.uefi_lo || hi != hc_loader.uefi_hi) {
			no = "the controller's list is not UEFI's";
		} else if (db != 0) {
			no = "a request in flight";
		} else if ((rsr & 1) == 0 || (hcs & HCS_UTRLRDY) == 0) {
			no = "the list is not running, or not ready";
		}
	}
	if (no == NULL) {
		uint64_t t;

		hc_is_in = hc_read(UFSHC_IS);
		hc_cnr_in = hc_read(UFSHC_UTRLCNR);
		hc_write(UFSHC_UTRLRSR, 0);
		for (t = mach_absolute_time(); (hc_read(UFSHC_UTRLRSR) & 1) && hc_us_since(t) < 5000;) {
			__builtin_arm_yield();
		}
		if (hc_read(UFSHC_UTRLRSR) & 1) {
			no = "the list did not stop";
			hc_write(UFSHC_UTRLRSR, 1);     /* (UEFI's list, running, as it was) */
		} else {
			hc_write(UFSHC_UTRLBA, hc_list_lo);
			hc_write(UFSHC_UTRLBAU, hc_list_hi);
			hc_write(UFSHC_UTRLRSR, 1);
			lo = hc_read(UFSHC_UTRLBA);
			hi = hc_read(UFSHC_UTRLBAU);
			rsr = hc_read(UFSHC_UTRLRSR);
			if (lo != hc_list_lo || hi != hc_list_hi || (rsr & 1) == 0) {
				no = "the controller did not take this list";
				hc_put_back(&undo);
				undone = true;
			} else {
				hc_in = true;
				*hc_loader.cnr_at_switch = hc_cnr_in;
				*hc_loader.list_in = 1;
				(*hc_loader.switched_in)++;
				hc_switch_in_count++;
			}
		}
	}
	ml_set_interrupts_enabled(istate);
	lck_mtx_unlock(&hc_lock);
	if (undone) {
		hc_put_back_print("undoing", &undo, false);
	}
	if (no != NULL) {
		hc_say("ufs: native: this kernel's list not put in: %s (list %08x:%08x, doorbell %08x, run %08x, HCS %08x; UEFI's "
		    "slot 0 %08x, CDB %02x)\n", no, hi, lo, db, rsr, hcs, uefi_dw0, uefi_upiu[16]);
		return EIO;
	}
	hc_say("ufs: native: transfer list switched from UEFI's %08x:%08x to this kernel's %08x:%08x (doorbell 0, run %u; "
	    "IS %08x, completion %08x; UEFI's slot 0 holds a read: %08x, CDB %02x)\n", hc_loader.uefi_hi,
	    hc_loader.uefi_lo, hi, lo, rsr, hc_is_in, hc_cnr_in, uefi_dw0, uefi_upiu[16]);
	if (prove) {
		int e = hc_prove();

		if (e != 0) {
			ufshc_switch_out("after the failed proof");
			hc_off();
			return e;
		}
	}
	return 0;
}

/* UEFI's list back (the caller holds the loader's mailbox) */
int
ufshc_switch_out(const char *why)
{
	struct hc_put_back_result r;
	uint64_t started;
	boolean_t istate;

	lck_mtx_lock(&hc_lock);
	if (!hc_in) {
		lck_mtx_unlock(&hc_lock);
		return 0;
	}
	hc_in = false;                          /* nothing more is rung */
	wakeup(&hc_free);
	lck_mtx_unlock(&hc_lock);
	/* what is in flight finishes (whoever waits for it sees it done) */
	for (started = mach_absolute_time(); hc_read(UFSHC_UTRLDBR) != 0 && hc_us_since(started) < 2000000;) {
		IOSleep(1);
	}
	lck_mtx_lock(&hc_lock);
	istate = ml_set_interrupts_enabled(FALSE);
	hc_put_back(&r);
	ml_set_interrupts_enabled(istate);
	lck_mtx_unlock(&hc_lock);
	hc_put_back_print(why, &r, false);
	if (!r.ok) {
		hc_off();
		return EIO;
	}
	return 0;
}

bool
ufshc_list_in(void)
{
	return hc_in;
}

bool
ufshc_available(void)
{
	return hc_usable && !hc_dead && !hc_closed;
}

void
ufshc_set_luns(uint32_t mask)
{
	hc_lun_mask = mask & 0xff;
}

static void
hc_report(const char *when, bool late)
{
	(late ? hc_say_late : hc_say)("ufs: native: %s: %llu reads, %llu errors, %llu bytes, %llu ms waiting (summed over reads), at most %llu "
	    "in flight, %llu refused, list in %llu times and out %llu times, %llu proofs%s\n", when, hc_requests, hc_errors,
	    hc_bytes, hc_wait_us / 1000, hc_most_in_flight, hc_refused, hc_switch_in_count, hc_switch_out_count, hc_proofs,
	    hc_dead ? "; THE PATH IS OFF" : "");
	{
		uint64_t wait_ns, lock_ns;

		absolutetime_to_nanoseconds(hc_slot_wait_ticks, &wait_ns);
		absolutetime_to_nanoseconds(hc_lock_ticks, &lock_ns);
		(late ? hc_say_late : hc_say)("ufs: native: %s: %llu doorbell reads while waiting (%llu a read), %llu waits for a "
		    "free slot (%llu ms), %llu ms taking the slots' lock; reads put in with 1 in flight %llu, 2 %llu, 3-4 %llu, "
		    "5-8 %llu, 9-16 %llu, more %llu\n", when, hc_polls, hc_requests ? hc_polls / hc_requests : 0, hc_slot_waits,
		    wait_ns / 1000000, lock_ns / 1000000, hc_flight_hist[0], hc_flight_hist[1], hc_flight_hist[2],
		    hc_flight_hist[3], hc_flight_hist[4], hc_flight_hist[5]);
		(late ? hc_say_late : hc_say)("ufs: native: %s: waiters spin %u us, then sleep: %llu slept; the poller %llu "
		    "rounds, %llu doorbell reads\n", when, hc_spin_us, hc_sleeps, hc_poller_rounds, hc_poller_polls);
	}
}

void
ufshc_report(const char *when)
{
	if (hc_usable) {
		hc_report(when, false);
	}
}

/* ---- the probe ---- */

/*
 * What the controller looks like as UEFI left it, and whether this kernel may put a list of its
 * own in (only == true: say so and do not use it).
 */
bool
ufshc_probe(const struct ufshc_loader *loader, bool only)
{
	uint32_t cap, ver, hcs, hce, is, ie, iacr, ahit, base, baseu, db, rsr, cnr, tm_base, tm_baseu, tm_db, tm_rsr;
	uint32_t uefi_dw0 = 0, slots = 31;
	uint8_t uefi_upiu[32] = { 0 };
	uint64_t uefi_pa;
	const char *no = NULL;
	vm_offset_t area;

	if (!hc_whitelist_selftest()) {
		return false;
	}
	hc_loader = *loader;
	PE_parse_boot_argn("ufs_native_buffer", &hc_slot_buffer, sizeof(hc_slot_buffer));
	if (hc_slot_buffer < BLOCK || hc_slot_buffer > (4u << 20) || hc_slot_buffer % BLOCK) {
		hc_slot_buffer = SLOT_BUFFER_DEFAULT;
	}
	PE_parse_boot_argn("ufs_native_slots", &slots, sizeof(slots));
	if (slots < 1 || slots > SLOTS - 1) {
		slots = SLOTS - 1;
	}
	PE_parse_boot_argn("ufs_native_leave", &hc_leave, sizeof(hc_leave));
	PE_parse_boot_argn("ufs_native_hang", &hc_hang_after, sizeof(hc_hang_after));
	if (loader->hc_pa != UFSHC_PHYS || loader->area_pa == 0 || loader->slots != SLOTS || (loader->area_pa & 0x3ff) ||
	    loader->ucd_size < UCD_PRDT + 16 * (hc_slot_buffer / PRD_MAX + 1) || (loader->ucd_offset & 0x7f) ||
	    (loader->ucd_size & 0x7f) || loader->ucd_offset < SLOTS * 32 ||
	    loader->area_size < loader->ucd_offset + SLOTS * loader->ucd_size) {
		hc_say("ufs: native: not used: the loader offers no area for a list (or not one as expected: controller %llx, "
		    "area %llx of %u, command descriptors at %u of %u)\n", loader->hc_pa, loader->area_pa, loader->area_size,
		    loader->ucd_offset, loader->ucd_size);
		return false;
	}
	if (pmap_valid_address(loader->area_pa) || pmap_valid_address(loader->area_pa + loader->area_size - 1)) {
		hc_say("ufs: native: not used: the loader's area %llx is memory this kernel manages\n", loader->area_pa);
		return false;
	}
	hc = ml_io_map(UFSHC_PHYS, UFSHC_SIZE);
	cap = hc_read(UFSHC_CAP);
	ver = hc_read(UFSHC_VER);
	hcs = hc_read(UFSHC_HCS);
	hce = hc_read(UFSHC_HCE);
	is = hc_read(UFSHC_IS);
	ie = hc_read(UFSHC_IE);
	iacr = hc_read(UFSHC_UTRIACR);
	ahit = hc_read(UFSHC_AHIT);
	base = hc_read(UFSHC_UTRLBA);
	baseu = hc_read(UFSHC_UTRLBAU);
	db = hc_read(UFSHC_UTRLDBR);
	rsr = hc_read(UFSHC_UTRLRSR);
	cnr = hc_read(UFSHC_UTRLCNR);
	tm_base = hc_read(UFSHC_UTMRLBA);
	tm_baseu = hc_read(UFSHC_UTMRLBAU);
	tm_db = hc_read(UFSHC_UTMRLDBR);
	tm_rsr = hc_read(UFSHC_UTMRLRSR);
	hc_say("ufs: native: controller at %llx: CAP %08x (%u slots, 64-bit %s), VER %08x, HCDDID %08x, HCPMID %08x\n",
	    UFSHC_PHYS, cap, CAP_NUTRS(cap), (cap & CAP_64AS) ? "yes" : "no", ver, hc_read(UFSHC_HCDDID),
	    hc_read(UFSHC_HCPMID));
	hc_say("ufs: native: HCE %08x, HCS %08x, IS %08x, IE %08x, UTRIACR %08x, AHIT %08x\n", hce, hcs, is, ie, iacr, ahit);
	hc_say("ufs: native: transfer list %08x:%08x (the loader's note of UEFI's: %08x:%08x), doorbell %08x, run %08x, "
	    "completion %08x; task management list %08x:%08x, doorbell %08x, run %08x\n", baseu, base, loader->uefi_hi,
	    loader->uefi_lo, db, rsr, cnr, tm_baseu, tm_base, tm_db, tm_rsr);
	uefi_pa = (uint64_t)baseu << 32 | base;

	if ((hce & 1) == 0) {
		no = "the controller is not enabled";
	} else if ((hcs & (HCS_DP | HCS_UTRLRDY)) != (HCS_DP | HCS_UTRLRDY)) {
		no = "no device present or the transfer list not ready";
	} else if ((rsr & 1) == 0) {
		no = "the transfer list is not running";
	} else if (ver < 0x200 || ver >= 0x400) {
		no = "a controller version other than 2.x or 3.x";
	} else if (CAP_NUTRS(cap) < SLOTS) {
		no = "fewer than 32 slots";
	} else if ((cap & CAP_64AS) == 0) {
		no = "no 64-bit addressing";
	} else if (is & IS_FATAL) {
		no = "an error status";
	} else if (base != loader->uefi_lo || baseu != loader->uefi_hi) {
		no = "the transfer list is not the one the loader noted as UEFI's";
	} else if (db != 0) {
		no = "a request in flight";
	} else if (pmap_valid_address(uefi_pa)) {
		no = "UEFI's transfer list is in memory this kernel manages";
	}
	if (no == NULL) {
		/* UEFI's list and command descriptor (the page they are in), not cached here, only read */
		hc_uefi_map_pa = uefi_pa & ~(uint64_t)PAGE_MASK;
		hc_uefi_map_size = PAGE_SIZE;
		hc_uefi_map_va = ml_io_map_unmappable((vm_offset_t)hc_uefi_map_pa, PAGE_SIZE, VM_WIMG_WCOMB);
		hc_uefi_list = (volatile uint32_t *)(hc_uefi_map_va + (uefi_pa - hc_uefi_map_pa));
		hc_say("ufs: native: UEFI's slot 0: %08x %08x %08x %08x %08x %08x %08x %08x\n", hc_uefi_list[0], hc_uefi_list[1],
		    hc_uefi_list[2], hc_uefi_list[3], hc_uefi_list[4], hc_uefi_list[5], hc_uefi_list[6], hc_uefi_list[7]);
		no = hc_uefi_slot0(uefi_upiu, &uefi_dw0);
		hc_say("ufs: native: UEFI's last command: UPIU %02x %02x %02x %02x, CDB %02x %02x %02x %02x %02x %02x %02x %02x "
		    "%02x %02x\n", uefi_upiu[0], uefi_upiu[1], uefi_upiu[2], uefi_upiu[3], uefi_upiu[16], uefi_upiu[17],
		    uefi_upiu[18], uefi_upiu[19], uefi_upiu[20], uefi_upiu[21], uefi_upiu[22], uefi_upiu[23], uefi_upiu[24],
		    uefi_upiu[25]);
	}
	if (no != NULL) {
		hc_say("ufs: native: not used: %s; the storage is read through the loader\n", no);
		return false;
	}
	if (only) {
		hc_say("ufs: native: as expected; ufs_native=2 asks for no more than this look\n");
		return false;
	}

	/* this kernel's list and command descriptors, in the loader's area; every slot TEST UNIT READY to begin with */
	area = ml_io_map_unmappable((vm_offset_t)(loader->area_pa & ~(uint64_t)PAGE_MASK),
	    (vm_size_t)round_page((loader->area_pa & PAGE_MASK) + loader->area_size), VM_WIMG_WCOMB) +
	    (vm_offset_t)(loader->area_pa & PAGE_MASK);
	hc_list_lo = (uint32_t)loader->area_pa;
	hc_list_hi = (uint32_t)(loader->area_pa >> 32);
	for (uint32_t i = 0; i < SLOTS; i++) {
		struct hc_slot *s = &hc_slots[i];
		struct hc_cmd c;

		s->number = i;
		s->utrd = (volatile uint32_t *)(area + 32 * i);
		s->ucd = (volatile uint32_t *)(area + loader->ucd_offset + i * loader->ucd_size);
		s->ucd_pa = loader->area_pa + loader->ucd_offset + i * loader->ucd_size;
		if (i >= 1 && i <= slots) {
			vm_offset_t data = 0;

			if (kmem_alloc_contig(kernel_map, &data, round_page(hc_slot_buffer), PAGE_MASK, 0, 0,
			    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
				hc_say("ufs: native: not used: no memory for the slots' buffers\n");
				return false;
			}
			s->data = (uint8_t *)data;
			s->data_pa = kvtophys(data);
			FlushPoC_DcacheRegion(data, hc_slot_buffer);
		}
		hc_make_tur(&c, 0, (uint8_t)i);
		if (hc_place(s, &c, 0) != 0) {
			hc_say("ufs: native: not used: slot %u would not take its TEST UNIT READY\n", i);
			return false;
		}
	}
	PE_parse_boot_argn("ufs_native_spin", &hc_spin_us, sizeof(hc_spin_us));
	if (hc_spin_us > 5000) {
		hc_spin_us = 5000;
	}
	{
		thread_t th;

		if (kernel_thread_start(hc_poller, NULL, &th) != KERN_SUCCESS) {
			hc_say("ufs: native: not used: no poller thread\n");
			return false;
		}
		thread_deallocate(th);
	}
	hc_nslots = slots;
	hc_free = (uint32_t)(((1ULL << (slots + 1)) - 1) & ~1ULL);       /* slots 1..n */
	hc_usable = true;
	hc_say("ufs: native: usable: a list of %u slots at %llx (the loader's), reads in %u of them, %u KB each (the logical units reads may go to are set by redwood_ufs.c)%s\n",
	    SLOTS, loader->area_pa, slots, hc_slot_buffer >> 10, hc_leave ? "; ufs_native_leave=1: the list is left in "
	    "at the end, for the loader to take back" : "");
	return true;
}

/*
 * Before the kernel gives the phone back to the loader (loader_return, the other processors
 * already stopped): nothing more is sent, what is in flight is waited for, UEFI's list put back.
 * No locks (a stopped processor may hold one).
 */
void
redwood_ufs_quiesce(void)
{
	struct hc_put_back_result r;

	if (hc_kept_count != 0) {
		hc_say_late("ufs: native: what happened in this run, again:\n");
		for (uint32_t i = hc_kept_count > KEPT_LINES ? hc_kept_count - KEPT_LINES : 0; i < hc_kept_count; i++) {
			const char *line = hc_kept[i % KEPT_LINES];

			hc_say_late("> %s%s", line, line[0] != 0 && line[strlen(line) - 1] == '\n' ? "" : "...\n");
		}
	}
	if (!hc_usable) {
		return;
	}
	hc_closed = true;
	__asm__ volatile ("dsb sy" ::: "memory");
	hc_say_late("ufs: native: at the end: entered with doorbell %08x, %u reads of this kernel's in flight, its list %s\n",
	    hc_read(UFSHC_UTRLDBR), hc_in_flight, hc_in ? "in" : "out");
	if (!hc_in && hc_read(UFSHC_UTRLBA) == hc_loader.uefi_lo && hc_read(UFSHC_UTRLBAU) == hc_loader.uefi_hi &&
	    (hc_read(UFSHC_UTRLRSR) & 1)) {
		hc_say_late("ufs: native: at the end: UEFI's list is in (doorbell %08x)\n", hc_read(UFSHC_UTRLDBR));
		hc_report("at the end", true);
		return;
	}
	hc_in = false;
	if (hc_leave) {
		hc_say_late("ufs: native: at the end: this kernel's list LEFT IN for the loader to take back "
		    "(ufs_native_leave=1; doorbell %08x)\n", hc_read(UFSHC_UTRLDBR));
		hc_report("at the end", true);
		return;
	}
	hc_put_back(&r);
	hc_put_back_print("at the end", &r, true);
	hc_report("at the end", true);
}
