/*
 * What AdrenoMetal's frontend (written against the iPadOS 17 SDK) takes from Metal's headers that the iOS 16.5 SDK
 * has not got: forced into each of its files by tools/mk-m2v.sh (-include).
 */
#pragma once
#ifdef __OBJC__
#import <Metal/Metal.h>
#if !defined(__IPHONE_17_0) || __IPHONE_OS_VERSION_MAX_ALLOWED < 170000
typedef MTLArgumentAccess MTLBindingAccess;     /* iOS 17's name for the same values */
#define MTLLanguageVersion3_1 ((MTLLanguageVersion)((3 << 16) + 1))
#endif
#endif
