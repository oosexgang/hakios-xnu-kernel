/*
 * Stand-in for a header Apple omits from the open-source XNU drop.
 * SART is an Apple-SoC DMA filter; there is none on this platform.
 */
#ifndef _ARM64_PPL_SART_H_
#define _ARM64_PPL_SART_H_
static inline void
sart_bootstrap(void)
{
}
#endif /* _ARM64_PPL_SART_H_ */
