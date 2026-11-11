#ifndef HOST_MACH_MACHINE_H
#define HOST_MACH_MACHINE_H
#include <stdint.h>
typedef int integer_t;
typedef integer_t cpu_type_t;
typedef integer_t cpu_subtype_t;
typedef integer_t cpu_threadtype_t;
#define CPU_ARCH_ABI64 0x01000000
#define CPU_TYPE_X86 7
#define CPU_TYPE_I386 CPU_TYPE_X86
#define CPU_TYPE_X86_64 (CPU_TYPE_X86 | CPU_ARCH_ABI64)
#define CPU_TYPE_ARM 12
#define CPU_TYPE_ARM64 (CPU_TYPE_ARM | CPU_ARCH_ABI64)
#define CPU_SUBTYPE_X86_64_ALL 3
#define CPU_SUBTYPE_ARM64_ALL 0
#define CPU_SUBTYPE_ARM64E 2
#define CPU_SUBTYPE_MASK 0xff000000
#endif
