/*
 * What of the kernel the drivers use that its headers do not offer to a file of bsd/dev: mapping
 * registers and memory, a physical address, a thread's name and end, the loader's log and its way
 * back, the RAM disk. Declared once here, as XNU defines them.
 */
#ifndef _HAKIOS_KERNEL_H
#define _HAKIOS_KERNEL_H

#include <mach/mach_types.h>
#include <sys/types.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern kern_return_t thread_terminate(thread_t thread);
extern void thread_set_thread_name(thread_t thread, const char *name);
extern void IOSleep(unsigned int milliseconds);
extern void loader_log_putc(char c);
extern void loader_return(void);
extern dev_t mdevlookup(int devid);
extern int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size);

#endif /* _HAKIOS_KERNEL_H */
