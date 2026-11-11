#ifndef _COREENTITLEMENTS_V2_API_H
#define _COREENTITLEMENTS_V2_API_H
/*
 * redwood (iOS 26 / xnu-12377): CoreEntitlements was reorganized under V2/, but the open-source
 * EXTERNAL_HEADERS still ships only the flat layout, which carries the same CE* types.  This
 * forwards to them and adds the few V2-only names the kernel's AMFI interface (amfi.h) references
 * only as opaque pointers; the concrete definitions live in the closed CoreEntitlements framework
 * and are provided at runtime by the miniamfi replacement (libkern/amfi/miniamfi*).
 */
#include <CoreEntitlements/CoreEntitlements.h>
#include <CoreEntitlements/Errors.h>
#include <CoreEntitlements/Index.h>
#include <CoreEntitlements/Entitlements.h>
#ifndef CE_VERSION
#define CE_VERSION 2
#endif
typedef struct CEContext CEContext_t;
#endif /* _COREENTITLEMENTS_V2_API_H */
