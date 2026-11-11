#ifndef _COREENTITLEMENTS_V2_KERNEL_H
#define _COREENTITLEMENTS_V2_KERNEL_H
#include <CoreEntitlements/V2/API.h>
/*
 * The kernel-facing CoreEntitlements implementation table.  amfi.h and amfi.c use it only as an
 * opaque pointer (libCoreEntitlements), so an incomplete type is enough to build; the real struct
 * is in the closed framework.
 */
typedef struct CEKernelAPI CEKernelAPI_t;
#endif /* _COREENTITLEMENTS_V2_KERNEL_H */
