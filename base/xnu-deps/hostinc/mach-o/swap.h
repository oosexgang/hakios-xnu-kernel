/* Host and target are both little-endian: swapping is never needed. */
#ifndef HOST_MACHO_SWAP_H
#define HOST_MACHO_SWAP_H
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/fat.h>
#include <architecture/byte_order.h>
#include <stdlib.h>
#define HOST_SWAP_STUB(name, ...) static inline void name(__VA_ARGS__) { abort(); }
HOST_SWAP_STUB(swap_mach_header, struct mach_header *h, enum NXByteOrder o)
HOST_SWAP_STUB(swap_mach_header_64, struct mach_header_64 *h, enum NXByteOrder o)
HOST_SWAP_STUB(swap_segment_command, struct segment_command *h, enum NXByteOrder o)
HOST_SWAP_STUB(swap_segment_command_64, struct segment_command_64 *h, enum NXByteOrder o)
HOST_SWAP_STUB(swap_section, struct section *h, uint32_t n, enum NXByteOrder o)
HOST_SWAP_STUB(swap_section_64, struct section_64 *h, uint32_t n, enum NXByteOrder o)
HOST_SWAP_STUB(swap_symtab_command, struct symtab_command *h, enum NXByteOrder o)
HOST_SWAP_STUB(swap_uuid_command, struct uuid_command *h, enum NXByteOrder o)
HOST_SWAP_STUB(swap_load_command, struct load_command *h, enum NXByteOrder o)
HOST_SWAP_STUB(swap_nlist, struct nlist *h, uint32_t n, enum NXByteOrder o)
HOST_SWAP_STUB(swap_nlist_64, struct nlist_64 *h, uint32_t n, enum NXByteOrder o)
HOST_SWAP_STUB(swap_fat_header, struct fat_header *h, enum NXByteOrder o)
HOST_SWAP_STUB(swap_fat_arch, struct fat_arch *h, uint32_t n, enum NXByteOrder o)
#endif
