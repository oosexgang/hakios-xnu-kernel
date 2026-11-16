/* hakios: stand-in for iig output (as kernel/deps/gen-iig-stubs.py makes for the other classes):
 * IOExtensiblePaniclog is new in xnu-12377 and kernel/deps/iig-impl has no stubs for it. Its RPC
 * dispatch (calls from user-space DriverKit drivers) is not available. */
#include <IOKit/IOLib.h>
#include <IOKit/IOReturn.h>
extern "C" {
kern_return_t __iig_stub_paniclog_0(void) __asm__("__ZN20IOExtensiblePaniclog8DispatchE5IORPC");
kern_return_t __iig_stub_paniclog_0(void) { return kIOReturnUnsupported; }
kern_return_t __iig_stub_paniclog_1(void) __asm__("__ZN20IOExtensiblePaniclog9MetaClass8DispatchE5IORPC");
kern_return_t __iig_stub_paniclog_1(void) { return kIOReturnUnsupported; }
}
