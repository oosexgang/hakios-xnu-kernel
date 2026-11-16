/* Part of the loader (included by loader.c, not compiled alone): struct platform: what the loader needs to know of a machine; includes the generated platforms.h. */
/* ------------------------------------------------------------------------ */
/* Platform description                                                     */

struct platform {
	const char *name;
	uint64_t io_base, io_size;       /* one window covering the SoC peripherals */
	uint64_t gic_dist, gic_redist;   /* GICv3 */
	uint64_t gic_redist_size;        /* the redistributors' region, 0 = 1 MB (8 GICv3 frames of 128 KB) */
	uint64_t gic_redist_stride;      /* the distance between redistributors, 0 = as GICR_TYPER says */
	uint64_t uart;                   /* PL011, 0 if none */
	uint64_t ufs;                    /* the UFS host controller's registers, 0 if the SoC file does not say */
	int psci_hvc;                    /* the firmware's power interface is called with HVC, not SMC */
	int log_other_cpu;               /* keep notes in the log partition while a kernel starts on another core */
	int uefi_charger;                /* the firmware has Qualcomm's charger protocols: the battery is read through them */
	int cores;                       /* at most this many processor cores, numbered from 0 ... */
	int core_shift;                  /* ... in this bit of MPIDR: the phone numbers them in the second affinity field */
	uint32_t timeout_scale;          /* a kernel on several cores gets this much longer to wait for a lock, 0: as it is */
	uint32_t small_cores;            /* a bit for each of the slower cores, by number: "small-core" in the device tree */
	uint64_t fb_base;                /* the framebuffer the firmware reports (GOP): it tells this device apart */
	/*
	 * The one partition of the phone's storage a write may reach, named "darwin": the port's own. Its
	 * GPT entry as it was when the port was set up (the board file's storage { }); last_lba 0: the
	 * board names none, and nothing is ever written.
	 */
	struct {
		efi_guid type, unique;          /* the entry's two GUIDs */
		uint64_t first_lba, last_lba;   /* where it begins and ends on the first logical unit */
		uint64_t write_first;           /* the first of its blocks (of 4096) a write may reach: past what the build put there */
	} own;
};

/*
 * The boards the loader knows: platform_NAME for each, and platforms[], the ones told apart by the
 * framebuffer base their UEFI reports (a display that matches none is QEMU's ramfb: platform_qemu_virt).
 * Generated from the repository's arch/arm64/boot/dts/NAME/board.dts (loader { }) and soc.dtsi (platform { }) by
 * tools/boards.py gen; a port to another device adds its files there (Documentation/porting.md), not code here.
 * The board's name goes to the kernel as /chosen/darwin-board, where the kernel finds its own row
 * (bsd/dev/board/board.c).
 */
#include "platforms.h"
