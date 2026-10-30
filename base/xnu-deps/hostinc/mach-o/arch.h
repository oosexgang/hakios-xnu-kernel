#ifndef HOST_MACHO_ARCH_H
#define HOST_MACHO_ARCH_H
#include <mach/machine.h>
#include <architecture/byte_order.h>
typedef struct { const char *name; cpu_type_t cputype; cpu_subtype_t cpusubtype; enum NXByteOrder byteorder; const char *description; } NXArchInfo;
static inline const NXArchInfo *NXGetLocalArchInfo(void) {
	static const NXArchInfo host = { "x86_64", CPU_TYPE_X86_64, CPU_SUBTYPE_X86_64_ALL, NX_LittleEndian, "host" };
	return &host;
}
#endif
