/*
 * Stand-in for a header Apple omits from the open-source XNU drop.
 * UAT is the Apple GPU address translator; there is none on this platform.
 */
#ifndef _ARM64_PPL_UAT_H_
#define _ARM64_PPL_UAT_H_
static inline void
uat_bootstrap(void)
{
}
#endif /* _ARM64_PPL_UAT_H_ */
