/*
 * FreeBSD's linker sets, for a Mach-O kernel. A set is a section of pointers that the linker
 * gathers from every object; FreeBSD's kernel walks many of them, the layer has a use for one:
 * the startup functions (sysinit_set, where a Linux module's module_init() ends up). That one is
 * kept, in a section of ours, for the layer to walk when a file that has one is built: none of
 * those built so far does. The entries of the others (the sysctl tree, module metadata, shutdown
 * functions) are dropped, since nothing here reads them. A set that this file does not know
 * stops the compilation.
 */
#ifndef _SYS_LINKER_SET_H_
#define _SYS_LINKER_SET_H_

#define __LX_SET_KEEP(sect, set, sym, qv)					\
	static void const * qv __set_##set##_sym_##sym				\
	    __attribute__((__section__("__DATA," sect), __used__)) = &(sym)
#define __LX_SET_DROP(set, sym, qv)						\
	static void const * qv __set_##set##_sym_##sym __attribute__((__unused__)) = &(sym)

#define __LX_SET_sysinit_set(set, sym, qv)	__LX_SET_KEEP("lx_sysinit", set, sym, qv)
#define __LX_SET_sysuninit_set(set, sym, qv)	__LX_SET_DROP(set, sym, qv)
#define __LX_SET_sysctl_set(set, sym, qv)	__LX_SET_DROP(set, sym, qv)
#define __LX_SET_modmetadata_set(set, sym, qv)	__LX_SET_DROP(set, sym, qv)

#define __MAKE_SET_QV(set, sym, qv)	__LX_SET_##set(set, sym, qv)
#define __MAKE_SET(set, sym)		__MAKE_SET_QV(set, sym, const)
#define TEXT_SET(set, sym)		__MAKE_SET(set, sym)
#define DATA_SET(set, sym)		__MAKE_SET(set, sym)
#define DATA_WSET(set, sym)		__MAKE_SET_QV(set, sym, )
#define BSS_SET(set, sym)		__MAKE_SET(set, sym)
#define ABS_SET(set, sym)		__MAKE_SET(set, sym)
#define SET_ENTRY(set, sym)		__MAKE_SET(set, sym)

/*
 * Walking a set: FreeBSD's names for its ends are declared, and for the sets that are dropped
 * nothing defines them, so code that walks one shows up when the object is linked.
 */
#define SET_DECLARE(set, ptype)							\
	extern ptype *__CONCAT(__start_set_,set);				\
	extern ptype *__CONCAT(__stop_set_,set)
#define SET_BEGIN(set)		(&__CONCAT(__start_set_,set))
#define SET_LIMIT(set)		(&__CONCAT(__stop_set_,set))
#define SET_FOREACH(pvar, set)	for (pvar = SET_BEGIN(set); pvar < SET_LIMIT(set); pvar++)
#define SET_ITEM(set, i)	((SET_BEGIN(set))[i])
#define SET_COUNT(set)		(SET_LIMIT(set) - SET_BEGIN(set))

#endif
