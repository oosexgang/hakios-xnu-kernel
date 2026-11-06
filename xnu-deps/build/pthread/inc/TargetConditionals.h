#ifndef _TARGETCONDITIONALS_H_
#define _TARGETCONDITIONALS_H_
/* minimal stand-in for building the pthread kext sources outside Xcode */
#define TARGET_OS_MAC 1
#define TARGET_OS_OSX 0
#define TARGET_OS_IPHONE 1
#define TARGET_OS_IOS 1
#define TARGET_OS_EMBEDDED 1
#define TARGET_OS_SIMULATOR 0
#define TARGET_OS_DRIVERKIT 0
#define TARGET_CPU_ARM64 1
#endif
