/*
 * aks_restore: what a restore of iOS leaves for the key store, done once. On a device the system keybag is made when
 * the system is restored, and every boot after that launchd's boot task "keybag" (keybagd --init) only loads it from
 * /private/var/keybags/systembag.kb -- and, finding none, reboots into recovery ("FATAL KEYBAG ERROR: kb_load",
 * run 469). There is no restore here, so this makes it: a bag from the kernel's key store (iokit/Kernel/
 * MiniAppleKeyStore.cpp, boot-arg aks_keys=1; selector 0x02 makes it, 0x03 hands out its bytes, 0x04 lets it go),
 * written where and as keybagd reads it: a binary property list { KeyBagKeys: <the bag> } -- plain, since the kernel
 * says no-effaceable-storage in /defaults of the device tree (keybagd's KBLoadLoadKeyBagFile then loads it with
 * no crypto). The bag holds the device key as it is; see the kernel's file for what that does and does not protect.
 *
 * usage: aks_restore        make the system bag, unless /private/var/keybags/systembag.kb is there already
 * Signed with aks_restore.entitlements (com.apple.keystore.device): the kernel lets nobody else make bags.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DIR     "/private/var/keybags"
#define BAG     DIR "/systembag.kb"

int
main(void)
{
	struct stat st;
	io_service_t service;
	io_connect_t connection = 0;
	uint64_t create[6] = { 0, (uint64_t)-1, 0, 0, 0, 0 };   /* type, -1, no passcode, its length 0, 0, 0 */
	uint64_t handle = 0;
	uint32_t count = 1;
	static uint8_t bytes[0x8000];
	size_t length = sizeof(bytes);
	kern_return_t ret;
	CFDataRef bag, plist;
	CFDictionaryRef dict;
	const void *key, *value;
	char temporary[] = BAG ".writing";
	int fd, ok;

	if (stat(BAG, &st) == 0) {
		printf("aks_restore: %s is there (%lld bytes): nothing to do\n", BAG, (long long)st.st_size);
		return 0;
	}
	service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleKeyStore"));
	if (service == 0 || IOServiceOpen(service, mach_task_self(), 0, &connection) != KERN_SUCCESS) {
		printf("aks_restore: no AppleKeyStore\n");
		return 1;
	}
	ret = IOConnectCallMethod(connection, 0x02, create, 6, NULL, 0, &handle, &count, NULL, NULL);
	if (ret != KERN_SUCCESS) {
		printf("aks_restore: the bag is not made: 0x%x (aks_keys=1? the entitlement?)\n", ret);
		return 1;
	}
	ret = IOConnectCallMethod(connection, 0x03, &handle, 1, NULL, 0, NULL, NULL, bytes, &length);
	IOConnectCallMethod(connection, 0x04, &handle, 1, NULL, 0, NULL, NULL, NULL, NULL);
	if (ret != KERN_SUCCESS) {
		printf("aks_restore: the bag is not handed out: 0x%x\n", ret);
		return 1;
	}

	bag = CFDataCreate(NULL, bytes, (CFIndex)length);
	key = CFSTR("KeyBagKeys");
	value = bag;
	dict = CFDictionaryCreate(NULL, &key, &value, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	plist = CFPropertyListCreateData(NULL, dict, kCFPropertyListBinaryFormat_v1_0, 0, NULL);
	if (plist == NULL) {
		printf("aks_restore: no property list\n");
		return 1;
	}
	mkdir(DIR, 0755);
	fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	ok = fd >= 0 && write(fd, CFDataGetBytePtr(plist), (size_t)CFDataGetLength(plist)) == CFDataGetLength(plist) &&
	    fsync(fd) == 0;
	if (fd >= 0) {
		close(fd);
	}
	if (!ok || rename(temporary, BAG) != 0) {
		printf("aks_restore: %s not written: %s\n", BAG, strerror(errno));
		unlink(temporary);
		return 1;
	}
	printf("aks_restore: the system bag made (handle %llu, %zu bytes), %s written (%ld bytes)\n", handle, length, BAG,
	    (long)CFDataGetLength(plist));
	return 0;
}
