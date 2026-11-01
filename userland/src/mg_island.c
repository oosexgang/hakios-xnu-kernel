/*
 * mg_island: the Dynamic Island on (boot-arg island=1) or off, in MobileGestalt's cache on the phone's flash.
 *
 * iOS 16's SpringBoard turns its "system aperture" on by the device's artwork subtype, which MobileGestalt answers from
 * its cache (CacheExtra, under the key's obfuscated name oPeik/9e8lQWMszEjbPzng): 2436 is the iPhone X's, 2556 the
 * iPhone 14 Pro's. Changing it there is what Cowabunga did on older phones in 2023. The frameworks ask from inside the
 * shared cache, so libhakios.fps's interposing does not reach them; the cache file does.
 *
 * Run as root by com.hakios.demo (rc.jobs). With island=1 it writes 2556 if the cache says otherwise; without it, it puts
 * back 2436 if it finds 2556. When it changed something it restarts SpringBoard, which then reads the new answer.
 */
#include <CoreFoundation/CoreFoundation.h>
/* (libproc.h is not in this SDK: libsystem_kernel's own) */
#define PROC_ALL_PIDS 1
extern int proc_listpids(uint32_t type, uint32_t typeinfo, void *buffer, int buffersize);
extern int proc_name(int pid, void *buffer, uint32_t buffersize);
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

#define CACHE   "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist"
#define KEY     "oPeik/9e8lQWMszEjbPzng"
#define ISLAND  2556
#define IPHONE_X 2436

static void
respring(void)
{
	pid_t pids[4096];
	int n = proc_listpids(PROC_ALL_PIDS, 0, pids, sizeof(pids)) / (int)sizeof(pids[0]);

	for (int i = 0; i < n; i++) {
		char name[64] = "";

		if (pids[i] > 1 && proc_name(pids[i], name, sizeof(name)) > 0 && strcmp(name, "SpringBoard") == 0) {
			printf("mg_island: SpringBoard (%d) restarted to read it\n", pids[i]);
			kill(pids[i], SIGTERM);
		}
	}
}

int
main(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	bool want = sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "island=1") != NULL;
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)CACHE, strlen(CACHE), false);
	CFReadStreamRef in = CFReadStreamCreateWithFile(NULL, url);
	CFPropertyListFormat format = kCFPropertyListBinaryFormat_v1_0;
	CFMutableDictionaryRef cache = NULL, extra;
	int32_t now = 0, wanted;
	CFNumberRef value;

	if (in != NULL && CFReadStreamOpen(in)) {
		cache = (CFMutableDictionaryRef)CFPropertyListCreateWithStream(NULL, in, 0, kCFPropertyListMutableContainersAndLeaves,
		    &format, NULL);
		CFReadStreamClose(in);
	}
	if (cache == NULL || CFGetTypeID(cache) != CFDictionaryGetTypeID() ||
	    (extra = (CFMutableDictionaryRef)CFDictionaryGetValue(cache, CFSTR("CacheExtra"))) == NULL ||
	    CFGetTypeID(extra) != CFDictionaryGetTypeID()) {
		printf("mg_island: no MobileGestalt cache with CacheExtra at %s (it is made at the first boot): nothing done\n", CACHE);
		return 0;
	}
	value = CFDictionaryGetValue(extra, CFSTR(KEY));
	if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
		CFNumberGetValue(value, kCFNumberSInt32Type, &now);
	}
	wanted = want ? ISLAND : (now == ISLAND ? IPHONE_X : now);
	printf("mg_island: the artwork subtype in the cache is %d; the Dynamic Island %s (island=%d)\n", now, want ? "wanted" : "not wanted",
	    want);
	if (wanted == now) {
		return 0;
	}
	value = CFNumberCreate(NULL, kCFNumberSInt32Type, &wanted);
	CFDictionarySetValue(extra, CFSTR(KEY), value);
	CFDataRef data = CFPropertyListCreateData(NULL, cache, format, 0, NULL);
	FILE *out = data != NULL ? fopen(CACHE, "wb") : NULL;

	if (out == NULL || fwrite(CFDataGetBytePtr(data), 1, (size_t)CFDataGetLength(data), out) != (size_t)CFDataGetLength(data)) {
		printf("mg_island: COULD NOT WRITE the cache\n");
		return 1;
	}
	fclose(out);
	printf("mg_island: the artwork subtype is now %d (%s)\n", wanted, wanted == ISLAND ? "the iPhone 14 Pro's: the Dynamic Island" :
	    "the iPhone X's");
	respring();
	return 0;
}
