#ifndef HOST_MACH_INIT_H
#define HOST_MACH_INIT_H
#include <unistd.h>
#include <stdint.h>
#define mach_vm_round_page(x) (((uint64_t)(x) + 4095ULL) & ~4095ULL)
#define round_page(x) mach_vm_round_page(x)
#endif
