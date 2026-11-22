/*
 * XNU loader: a UEFI application that plays the role of iBoot for an
 * open-source arm64 XNU kernel on non-Apple hardware.
 *
 *   - finds the "XNUP" payload in RAM (placed there by the boot shim on the
 *     phone, by `-device loader` under QEMU) and the Mach-O kernel inside it
 *   - picks a block of free RAM to be XNU's [physBase, physBase + memSize)
 *   - copies the Mach-O segments there, builds an Apple-format device tree
 *     and a boot_args structure behind the kernel
 *   - exits boot services, turns the MMU off and jumps to the kernel entry
 */
#include "efi.h"
/* who Darwin shall think we are: from the iPhone's own device tree (mkidentity.py). LOADER_IDENTITY (build.sh) names
 * another such header, for a Darwin whose files are another iPhone's */
#ifdef LOADER_IDENTITY
#include LOADER_IDENTITY
#else
#include "identity.h"
#endif


/*
 * The loader is one compilation unit: the parts below are included here, in this order, and are not
 * compiled alone. Each is a topic; a part may use what the parts before it define. arch/arm64/boot/README.md
 * says what each is for.
 */
#include "src/conventions.h"      /* what the boot shim, the packer and the kernel agree on with the loader */
#include "src/xnu.h"              /* the kernel's own structures the loader fills in or reads */
#include "src/base.c"             /* the loader's globals, memory and strings */
#include "src/console.c"          /* what the loader says, on the screen and into its log; the caches */
#include "src/logstore.c"         /* the log kept across runs: a raw write to a partition (phone only) */
#include "src/devicetree.c"       /* the writer of the Apple-format device tree */
#include "src/identity_given.c"   /* which iPhone Darwin is told it runs on: built in, or from the payload */
#include "src/platform.h"         /* struct platform: what the loader needs to know of a machine; includes the generated platforms.h */
#include "src/return.c"           /* the reset, the way back from the kernel, the jump into it */
#include "src/othercore.c"        /* the kernel on another core: the stub it starts in, its log read while it runs */
#include "src/charger.c"          /* the battery, read through the firmware's charger protocols */
#include "src/ufs_survey.c"       /* ufs=1: what the phone's storage holds, read only */
#include "src/block_page.h"       /* the page through which the kernel asks for the storage's blocks, as both sides lay it out */
#include "src/ufs_block.c"        /* ufs=2: the kernel reads the phone's storage through the loader: the page's place, the controller, the firmware's tables */
#include "src/own_partition.c"    /* the one partition a write may reach: is this it? */
#include "src/partition_named.c"  /* a partition of the phone's storage, by its name */
#include "src/dsu_extents.c"      /* where the DSU image lies in userdata: Android's lp_metadata, read */
#include "src/dsu_place.c"        /* the DSU image found on the phone, and read through its extents */
#include "src/ufs_service.c"      /* ufs=2, the service itself: the storage's units and partitions listed, the kernel's requests answered */
#include "src/charger_start.c"    /* the firmware's charger protocols found, the first reading (charger.c reads) */
#include "src/othercore_start.c"  /* the kernel on another core: started through the firmware's power interface, watched */
#include "src/watchdog.c"         /* the hypervisor's watchdog */
#include "src/exception.c"        /* a fault of the loader into the log instead of onto the screen */
#include "src/payload.c"          /* the payload: kernel, images and boot arguments, found and taken apart */
#include "src/nvram.c"            /* the NVRAM image the kernel is given */
#include "src/unit.c"             /* this unit's identity: serial numbers, ECID, addresses, from one seed */
#include "src/window.c"           /* the kernel's memory: the firmware's map and the block chosen from it */
#include "src/pac.c"              /* pointer authentication of the kernel image: keys, and the image's pointers signed */
#include "src/run.c"              /* the kernel loaded, given its device tree and boot arguments, and run */
#include "src/usb.c"              /* the USB command channel: a small server of the fastboot protocol */
#include "src/dsu_write.c"        /* the system image written through its extents, and nowhere else */
#include "src/dsu.c"              /* the Darwin system image as an Android DSU image ("oem dsu") */
#include "src/download.c"         /* where a download over USB goes, and how much of it fits */
#include "src/commands.c"         /* the commands taken over USB */
#include "src/ramfb.c"            /* QEMU's display in the shape of the phone's screen */

efi_status EFIAPI
efi_main(efi_handle image, efi_system_table *st)
{
	static const efi_guid gop_guid = EFI_GOP_GUID;
	struct payload_info pi;

	ST = st;
	BS = st->bs;
	IMAGE = image;

	BS->set_watchdog_timer(0, 0, 0, NULL);
	console_banner();
	print("XNULOADER v3  |  UEFI recovery\n");
	print("CurrentEL=%x MIDR=%x MMFR0=%x CNTFRQ=%d image=%x\n", READ_SYSREG(CurrentEL) >> 2, READ_SYSREG(MIDR_EL1),
	    READ_SYSREG(ID_AA64MMFR0_EL1), READ_SYSREG(CNTFRQ_EL0), (uint64_t)(uintptr_t)&__ImageBase);

	PLAT = &platform_qemu_virt;
	if (EFI_ERROR(BS->locate_protocol(&gop_guid, NULL, (void **)&GOP)) || GOP->mode == NULL ||
	    GOP->mode->info == NULL || GOP->mode->fb_base == 0) {
		GOP = NULL;                     /* no display (QEMU without -device ramfb) */
	} else {
		SCREEN.base = GOP->mode->fb_base;
		SCREEN.width = GOP->mode->info->width;
		SCREEN.height = GOP->mode->info->height;
		SCREEN.stride = GOP->mode->info->pixels_per_scanline * 4;
		for (unsigned i = 0; i < sizeof(platforms) / sizeof(platforms[0]); i++)
			if (platforms[i]->fb_base == SCREEN.base)
				PLAT = platforms[i];
		/* Only QEMU maps fw_cfg at 0x09020000. A real phone with an
		 * unrecognized GOP base still has a usable firmware framebuffer;
		 * don't probe QEMU MMIO just because no board profile matched. */
		if (PLAT == &platform_qemu_virt && SCREEN.base == platform_qemu_virt.fb_base)
			ramfb_phone_screen();
		else if (PLAT == &platform_qemu_virt)
			print("display: unknown framebuffer; keeping the firmware mode\n");
		print("display %d x %d at %x\n", (uint64_t)SCREEN.width, (uint64_t)SCREEN.height, SCREEN.base);
	}
	print("platform %s\n", PLAT->name);

	install_exception_hook();
	wdt_arm();

	int have_payload = parse_payload(&pi, 0);
	/* ABL/U-Boot may start the loader without an XNUP payload. That is a
	 * recoverable packaging or transfer error, not a reason to cold-reset the
	 * phone: bring up the same fastboot channel used by --usb payloads so the
	 * host can send a payload and run it. */
	if (!have_payload) {
		print("no XNUP payload; entering USB recovery\n");
	}
	if ((!have_payload || pi.usb_mode) && usb_start()) {
		flush_log();
		usb_command_loop();
	}
	if (!have_payload) {
		print("USB recovery unavailable; holding in loader\n");
		flush_log();
		for (;;) {
			wdt_pet();
			BS->stall(100000);
		}
	}

	/* One-shot mode: run the kernel that came with the image, then reset. */
	if (have_payload) {
		run_kernel(0, 0);
	}
	reboot();
	return 0;
}
