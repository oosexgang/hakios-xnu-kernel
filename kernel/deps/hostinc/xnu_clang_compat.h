/*
 * Force-included into every XNU translation unit when building with stock
 * (non-Apple) clang. Apple's clang has builtins that describe the pointer/data
 * layout of a type for kalloc_type; upstream clang lacks them, so every type
 * is conservatively described as "mixed pointers and data".
 */
#ifndef XNU_CLANG_COMPAT_H
#define XNU_CLANG_COMPAT_H
#if !defined(ASSEMBLER) && !defined(__ASSEMBLER__)
#if !__has_builtin(__builtin_xnu_type_signature)
#define XNU_NO_TYPE_BUILTINS                     1
#define __builtin_xnu_type_signature(type)       "12"
#define __builtin_xnu_type_summary(type)         ((1U << 1) | (1U << 2))
#ifdef __cplusplus
#define __builtin_xnu_types_compatible(a, b)     __is_same(a, b)
#else
#define __builtin_xnu_types_compatible(a, b)     __builtin_types_compatible_p(a, b)
#endif
#else
/* Apple's clang (macOS hosts) has the builtins; OSData.h still tests the macro */
#define XNU_NO_TYPE_BUILTINS                     0
#endif
#endif
#endif
