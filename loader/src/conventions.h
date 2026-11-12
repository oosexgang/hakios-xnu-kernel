/* Part of the loader (included by loader.c, not compiled alone): what the boot shim, the packer and the kernel agree on with the loader. */
/* ------------------------------------------------------------------------ */
/* Fixed conventions shared with the boot shim, the packer and the kernel   */

/* The device's reserved payload RAM (UEFI memory map). Overridable at build time with
 * -DPAYLOAD_BASE=... for a device whose reservation is elsewhere. */
#ifndef PAYLOAD_BASE
#define PAYLOAD_BASE        0xF0000000ULL
#endif
#define PAYLOAD_MAGIC       0x50554E58u /* "XNUP" */

struct payload_header { uint32_t magic; uint32_t size; };
struct payload_entry { char name[16]; uint32_t size; uint32_t reserved; };

#define KERNEL_VIRT_BASE    0xFFFFFFF004000000ULL /* boot_args.virtBase */
#define KERNEL_LINK_OFFSET  0x03000000ULL         /* kernel text sits 48 MB above the base */
#define BASE_ALIGN          0x02000000ULL         /* 32 MB: L2 block size with 16K pages */
#define MAX_MEM_SIZE        0x1F0000000ULL        /* 7.75 GB; the kernel's bootstrap page tables cover 8 */
/*
 * A root image too big for the payload area is placed here, at a fixed
 * offset inside the kernel's memory window, once; it then survives any
 * number of kernel runs (nothing writes to it: the root is read-only).
 */
#define RESIDENT_RD_OFFSET  0x08000000ULL
#define RESIDENT_RD_RESERVE 0x30000000ULL         /* RAM that must remain for the kernel itself */
#define PAGE_SIZE_MAX       0x4000ULL

#define LOG_PARTITION       u"logdump"
#define CHECKPOINT_SELFTEST 999
#define PANIC_LOG_SIZE      0x80000u
#define NVRAM_BANK_SIZE     0x2000u
/*
 * The panic log: the last MB of XNU's window, which the kernel is told is not its memory. (It used
 * to be the tail of the payload reservation, but under the Qualcomm hypervisor that RAM behaves as
 * Device memory -- DC ZVA and unaligned loads fault there -- while the window is ordinary DRAM.)
 */
#define PANIC_LOG_RESERVE   0x100000ULL
#define PANIC_LOG_BASE      (win_base + win_size - PANIC_LOG_RESERVE)
/* RAM reserved for the payload in the UEFI memory map. -DPAYLOAD_MAX_SIZE=... where the reservation is
 * bigger. */
#ifndef PAYLOAD_MAX_SIZE
#define PAYLOAD_MAX_SIZE    0x04000000ULL
#endif
