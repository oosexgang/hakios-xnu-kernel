// SPDX-License-Identifier: GPL-2.0+
/*
 * hakios: start the HakiOS loader from memory.
 *
 * What the loader needs is two things in RAM: the loader itself (an AArch64 UEFI application, loader.efi) and its
 * payload (the "XNUP" container: the kernel, a small root image, the command line), at PAYLOAD_BASE, where the loader
 * looks first (xnuloader/loader.c). Nothing is read from or written to the phone's storage on the way.
 *
 * They come in one "HKUB" container (tools/mkubootimg.py in the common repository), from either of two places:
 *
 *   - the ramdisk of the boot image ABL started U-Boot from (fastboot boot hakios-u-boot.img): ABL put it in RAM
 *     and said where in the device tree it handed over (/chosen linux,initrd-start and -end);
 *   - U-Boot's own fastboot buffer, from the host: fastboot stage x.hkub; fastboot oem run:hakios load fastboot
 *   - anywhere else in RAM: hakios load ADDR [SIZE] (a container or a payload says its own size)
 *
 * A bare XNUP payload or a bare loader.efi is taken too, so that one of them can be changed without the other; a payload
 * with an entry "loader.efi" (the one xl enter makes for Silicium's boot manager) brings its loader with it.
 *
 *   HKUB container, little-endian:
 *     u32 magic "HKUB", u32 version (1), u32 count, u32 header size
 *     count x { char name[16]; u32 offset; u32 size; u32 crc32; u32 reserved; }   offsets from the container's start
 *   entries: "loader" (loader.efi), "payload" (XNUP), "env" (text, name=value lines: what this image makes U-Boot do,
 *   e.g. the menu of an installed HakiOS; imported over the environment built in)
 */

#include <command.h>
#include <cpu_func.h>
#include <env.h>
#include <fdt_support.h>
#include <fdtdec.h>
#include <init.h>
#include <lmb.h>
#include <mapmem.h>
#include <u-boot/crc.h>
#include <asm/cache.h>
#include <asm/system.h>
#include <linux/errno.h>
#include <linux/libfdt.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include "hakios.h"

#define PAYLOAD_BASE		0xF0000000UL
#define PAYLOAD_MAX		(SZ_64M - SZ_1M)	/* the last MiB of the 64 is the kernel's panic log */
#define XNUP_MAGIC		0x50554E58		/* "XNUP" */
#define HKUB_MAGIC		0x42554B48		/* "HKUB" */
#define HKUB_VERSION		1

struct hkub_header {
	u32 magic;
	u32 version;
	u32 count;
	u32 header_size;
};

struct hkub_entry {
	char name[16];
	u32 offset;
	u32 size;
	u32 crc32;
	u32 reserved;
};

static phys_addr_t loader_addr;
static ulong loader_size, loader_room, payload_size;

/* ABL's ramdisk, from the device tree ABL handed over (U-Boot keeps its address: get_prev_bl_fdt_addr()) */
static int abl_ramdisk(ulong *start, ulong *size)
{
	const void *fdt = (const void *)get_prev_bl_fdt_addr();
	u64 lo, hi;
	int node;

	if (!fdt || fdt_check_header(fdt)) {
		printf("[oosexgang] hakios: no device tree from the bootloader before U-Boot\n");
		return -ENOENT;
	}
	node = fdt_path_offset(fdt, "/chosen");
	if (node < 0)
		return -ENOENT;
	lo = fdtdec_get_uint64(fdt, node, "linux,initrd-start", 0);
	if (!lo)
		lo = fdtdec_get_uint(fdt, node, "linux,initrd-start", 0);
	hi = fdtdec_get_uint64(fdt, node, "linux,initrd-end", 0);
	if (!hi)
		hi = fdtdec_get_uint(fdt, node, "linux,initrd-end", 0);
	if (!lo || hi <= lo) {
		printf("[oosexgang] hakios: the boot image has no ramdisk\n");
		return -ENOENT;
	}
	*start = lo;
	*size = hi - lo;
	return 0;
}

static int take_loader(const void *data, ulong size)
{
	if (size < 2 || memcmp(data, "MZ", 2)) {
		printf("[oosexgang] hakios: not a UEFI application\n");
		return -EINVAL;
	}
	if (size > loader_room) {
		loader_room = ALIGN(size, SZ_64K);
		if (lmb_alloc_mem(LMB_MEM_ALLOC_ANY, SZ_64K, &loader_addr, loader_room, LMB_NONE)) {
			printf("[oosexgang] hakios: no memory for a loader of %lu bytes\n", size);
			loader_addr = 0;
			loader_room = loader_size = 0;
			return -ENOMEM;
		}
	}
	memcpy(map_sysmem(loader_addr, size), data, size);
	loader_size = size;
	env_set_hex("hakios_loader", loader_addr);
	env_set_hex("hakios_loader_size", size);
	printf("[oosexgang] hakios: loader of %lu bytes at %#llx\n", size, (unsigned long long)loader_addr);
	return 0;
}

/*
 * The loader inside the payload, as an entry "loader.efi": what Silicium's boot manager starts (xl enter puts it
 * there). Entries: char name[16], u32 size, u32 reserved, the data padded to 16 bytes; an empty name ends them.
 */
static int take_loader_from_payload(const u8 *data, ulong size)
{
	ulong off = 8, end = 8 + ((const u32 *)data)[1];

	while (off + 24 <= end && data[off]) {
		u32 n = *(const u32 *)(data + off + 16);

		if (n > end - off - 24)
			break;
		if (!strncmp((const char *)data + off, "loader.efi", 16))
			return take_loader(data + off + 24, n);
		off += 24 + ALIGN(n, 16);
	}
	return -ENOENT;
}

static int take_payload(const void *data, ulong size)
{
	const u32 *head = data;
	phys_addr_t addr = PAYLOAD_BASE;

	if (size < 8 || head[0] != XNUP_MAGIC || head[1] > size - 8) {
		printf("[oosexgang] hakios: not an XNUP payload\n");
		return -EINVAL;
	}
	size = head[1] + 8;
	if (size > PAYLOAD_MAX) {
		printf("[oosexgang] hakios: payload of %lu bytes is over %u\n", size, PAYLOAD_MAX);
		return -E2BIG;
	}
	/*
	 * The device tree keeps PAYLOAD_BASE reserved (hakios-payload@f0000000), so this succeeds only on a U-Boot
	 * that does not: then nothing else took it either.
	 */
	lmb_alloc_mem(LMB_MEM_ALLOC_ADDR, 0, &addr, SZ_64M, LMB_NONE);
	memcpy(map_sysmem(PAYLOAD_BASE, size), data, size);
	/* the loader runs with its own view of the caches (and the kernel with the MMU off at first) */
	flush_dcache_range(PAYLOAD_BASE, PAYLOAD_BASE + ALIGN(size, ARCH_DMA_MINALIGN));
	payload_size = size;
	printf("[oosexgang] hakios: payload of %lu bytes at %#lx\n", size, PAYLOAD_BASE);
	/* a payload that carries its loader needs no other; one that does not keeps the loader there was */
	take_loader_from_payload(data, size);
	return 0;
}

static int take_env(const void *data, ulong size)
{
	char cmd[64];

	snprintf(cmd, sizeof(cmd), "env import -t %lx %lx", (ulong)map_to_sysmem(data), size);
	if (run_command(cmd, 0)) {
		printf("[oosexgang] hakios: the image's environment was not taken\n");
		return -EINVAL;
	}
	printf("[oosexgang] hakios: environment of %lu bytes from the image\n", size);
	return 0;
}

static int take_container(const u8 *data, ulong size)
{
	const struct hkub_header *h = (const void *)data;
	const struct hkub_entry *e = (const void *)(data + sizeof(*h));
	int ret = 0;

	if (h->version != HKUB_VERSION || h->count > 16 ||
	    h->header_size < sizeof(*h) + h->count * sizeof(*e) || h->header_size > size) {
		printf("[oosexgang] hakios: HKUB container version %u that this U-Boot does not read\n", h->version);
		return -EINVAL;
	}
	for (u32 i = 0; i < h->count && !ret; i++, e++) {
		char name[sizeof(e->name) + 1];
		const u8 *p = data + e->offset;

		memcpy(name, e->name, sizeof(e->name));
		name[sizeof(e->name)] = 0;
		if (e->offset > size || e->size > size - e->offset) {
			printf("[oosexgang] hakios: %s runs past the end of the container\n", name);
			return -EINVAL;
		}
		/* ABL's ramdisk sits in memory U-Boot may have used since: a damaged entry is never started */
		if (crc32(0, p, e->size) != e->crc32) {
			printf("[oosexgang] hakios: %s is damaged (crc32 %08x, should be %08x)\n", name, crc32(0, p, e->size), e->crc32);
			return -EIO;
		}
		if (!strcmp(name, "loader"))
			ret = take_loader(p, e->size);
		else if (!strcmp(name, "payload"))
			ret = take_payload(p, e->size);
		else if (!strcmp(name, "env"))
			ret = take_env(p, e->size);
		else
			printf("[oosexgang] hakios: %s skipped\n", name);
	}
	return ret;
}

/* the size an HKUB container or an XNUP payload says it has; 0 for anything else */
static ulong known_size(const void *data)
{
	const struct hkub_header *h = data;
	const struct hkub_entry *e = (const void *)(h + 1);
	const u32 *head = data;
	ulong end = h->header_size;

	if (head[0] == XNUP_MAGIC)
		return head[1] + 8UL;
	if (h->magic != HKUB_MAGIC || h->count > 16)
		return 0;
	for (u32 i = 0; i < h->count; i++)
		end = max(end, (ulong)e[i].offset + e[i].size);
	return end;
}

static int do_hakios_load(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	ulong addr, size;
	const void *data;
	u32 magic;

	/* what bootcmd runs once this has taken an image: the loader, unless the image's own environment says otherwise */
	if (!env_get("hakios_start"))
		env_set("hakios_start", "hakios boot");
	if (argc >= 3) {
		addr = hextoul(argv[1], NULL);
		size = hextoul(argv[2], NULL);
	} else if (argc == 2 && !strcmp(argv[1], "fastboot")) {
		/* what the host sent last: fastboot stage FILE */
		addr = env_get_hex("fastboot_addr_r", 0);
		size = env_get_hex("filesize", 0);
		if (!addr || !size) {
			printf("[oosexgang] hakios: nothing received over fastboot\n");
			return CMD_RET_FAILURE;
		}
	} else if (argc == 2) {
		/* a container or a payload says how big it is */
		addr = hextoul(argv[1], NULL);
		size = known_size(map_sysmem(addr, 0));
		if (!size) {
			printf("[oosexgang] hakios: no HKUB container or XNUP payload at %#lx to take the size from\n", addr);
			return CMD_RET_USAGE;
		}
	} else if (abl_ramdisk(&addr, &size)) {
		return CMD_RET_FAILURE;
	}
	if (size < 4)
		return CMD_RET_FAILURE;
	data = map_sysmem(addr, size);
	magic = *(const u32 *)data;
	if (magic == HKUB_MAGIC)
		return take_container(data, size) ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
	if (magic == XNUP_MAGIC)
		return take_payload(data, size) ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
	if (!memcmp(data, "MZ", 2))
		return take_loader(data, size) ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
	printf("[oosexgang] hakios: nothing known at %#lx (%08x): an HKUB container, an XNUP payload or loader.efi\n", addr, magic);
	return CMD_RET_FAILURE;
}

static int do_hakios_boot(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	char cmd[64];

	if (!loader_size) {
		printf("[oosexgang] hakios: no loader in memory (hakios load first)\n");
		return CMD_RET_FAILURE;
	}
	if (*(const u32 *)map_sysmem(PAYLOAD_BASE, 4) != XNUP_MAGIC)
		printf("[oosexgang] hakios: no payload at %#lx: the loader looks for one on its volume\n", PAYLOAD_BASE);
	snprintf(cmd, sizeof(cmd), "bootefi %llx:%lx", (unsigned long long)loader_addr, loader_size);
	return run_command(cmd, 0) ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

static int do_hakios_info(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	const u32 *head = map_sysmem(PAYLOAD_BASE, 8);
	ulong start, size;

	if (loader_size)
		printf("loader:  %lu bytes at %#llx\n", loader_size, (unsigned long long)loader_addr);
	else
		printf("loader:  none\n");
	if (head[0] == XNUP_MAGIC)
		printf("payload: %u bytes at %#lx%s\n", head[1] + 8, PAYLOAD_BASE, payload_size ? "" : " (not put there by U-Boot)");
	else
		printf("payload: none at %#lx\n", PAYLOAD_BASE);
	if (!abl_ramdisk(&start, &size))
		printf("ramdisk: %lu bytes at %#lx (from ABL)\n", size, start);
	return CMD_RET_SUCCESS;
}

static int do_hakios_slot(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	return hakios_slot(argc, argv) ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

static int do_hakios_sum(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	return hakios_sum(argc, argv) ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

static int do_hakios_reboot(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	if (argc > 2 || (argc == 2 && strcmp(argv[1], "bootloader")))
		return CMD_RET_USAGE;
	return hakios_reboot(argc == 2) ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

static struct cmd_tbl cmd_hakios_sub[] = {
	U_BOOT_CMD_MKENT(load, 3, 0, do_hakios_load, "", ""),
	U_BOOT_CMD_MKENT(boot, 1, 0, do_hakios_boot, "", ""),
	U_BOOT_CMD_MKENT(info, 1, 0, do_hakios_info, "", ""),
	U_BOOT_CMD_MKENT(slot, 2, 0, do_hakios_slot, "", ""),
	U_BOOT_CMD_MKENT(sum, 4, 0, do_hakios_sum, "", ""),
	U_BOOT_CMD_MKENT(reboot, 2, 0, do_hakios_reboot, "", ""),
};

static int do_hakios(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	struct cmd_tbl *c;

	if (argc < 2)
		return CMD_RET_USAGE;
	c = find_cmd_tbl(argv[1], cmd_hakios_sub, ARRAY_SIZE(cmd_hakios_sub));
	if (!c)
		return CMD_RET_USAGE;
	return c->cmd(cmdtp, flag, argc - 1, argv + 1);
}

U_BOOT_CMD(hakios, 5, 0, do_hakios,
	"start the HakiOS loader from memory",
	"load [fastboot | addr [size]] - take an HKUB container, an XNUP payload or loader.efi\n"
	"    (default: ABL's ramdisk; fastboot: what fastboot stage sent last)\n"
	"hakios boot - start the loader (bootefi), with the payload at 0xf0000000\n"
	"hakios info - what is in memory\n"
	"hakios slot [successful | unbootable] - Qualcomm's A/B slots: show them; mark the active one successful, or\n"
	"    unbootable (ABL then makes the other one active itself, at the next start)\n"
	"hakios sum PART [BLOCK COUNT] - the sha256 of a partition's blocks (all of them by default)\n"
	"hakios reboot [bootloader] - restart into Android, or into the phone's own fastboot (ABL)"
);
