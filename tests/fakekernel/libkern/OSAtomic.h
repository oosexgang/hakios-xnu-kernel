/* The kernel's atomic operations the drivers use, as the compiler's. */
#include "../fakekernel.h"

typedef int32_t SInt32;
typedef uint32_t UInt32;

static inline SInt32
OSIncrementAtomic(volatile SInt32 *address)
{
	return __atomic_fetch_add(address, 1, __ATOMIC_SEQ_CST);
}

static inline UInt32
OSBitOrAtomic(UInt32 mask, volatile UInt32 *address)
{
	return __atomic_fetch_or(address, mask, __ATOMIC_SEQ_CST);
}

static inline UInt32
OSBitAndAtomic(UInt32 mask, volatile UInt32 *address)
{
	return __atomic_fetch_and(address, mask, __ATOMIC_SEQ_CST);
}

static inline void
OSMemoryBarrier(void)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}
