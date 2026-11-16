/* Part of the loader (included by loader.c, not compiled alone): the kernel's own structures the loader fills in or reads. */
/* ------------------------------------------------------------------------ */
/* XNU structures (pexpert/pexpert/arm64/boot.h, mach-o/loader.h)           */

struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

/*
 * The kernel's pexpert/arm64/boot.h, as kernel/patches/0002 leaves it: 608, the layout before xnu-12377 (which has 1024).
 * The two must agree. When they did not, the kernel read bootFlags and memSizeActual past what was written and zeroed here --
 * whatever an earlier run left in that memory, which moved with the payload's size. Some images booted; others gave
 * max_mem_actual garbage, scale_setup a thread_max of 0x7ffff600 and ulock_initialize a 16 GB hash: a panic in the first
 * second, every boot of those images. workbench/tools_test.py compares the two.
 */
#define BOOT_LINE_LENGTH 608

struct boot_args {
	uint16_t revision;
	uint16_t version;
	uint64_t virtBase;
	uint64_t physBase;
	uint64_t memSize;
	uint64_t topOfKernelData;
	struct boot_video video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char commandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags;
	uint64_t memSizeActual;
};

#define MH_MAGIC_64      0xFEEDFACFu
#define LC_SEGMENT_64    0x19
#define LC_UNIXTHREAD    0x5
#define LC_DYLD_CHAINED_FIXUPS 0x80000034u

struct mach_header_64 { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct load_command { uint32_t cmd, cmdsize; };
struct segment_command_64 {
	uint32_t cmd, cmdsize; char segname[16];
	uint64_t vmaddr, vmsize, fileoff, filesize;
	uint32_t maxprot, initprot, nsects, flags;
};
struct section_64 {
	char sectname[16]; char segname[16];
	uint64_t addr, size;
	uint32_t offset, align, reloff, nreloc, flags, reserved1, reserved2, reserved3;
};
struct linkedit_data_command { uint32_t cmd, cmdsize, dataoff, datasize; };
struct chained_fixups_header {
	uint32_t fixups_version, starts_offset, imports_offset, symbols_offset;
	uint32_t imports_count, imports_format, symbols_format;
};
struct chained_starts_in_segment {
	uint32_t size;
	uint16_t page_size, pointer_format;
	uint64_t segment_offset;
	uint32_t max_valid_pointer;
	uint16_t page_count;
	uint16_t page_start[1];
};
#define CHAINED_PTR_START_NONE    0xFFFF
#define CHAINED_PTR_START_MULTI   0x8000  /* page_start[] entry is an index into an overflow list */
#define CHAINED_PTR_START_LAST    0x8000  /* in that overflow list, the last chain start on the page */
#define CHAINED_PTR_ARM64E_KERNEL 7
