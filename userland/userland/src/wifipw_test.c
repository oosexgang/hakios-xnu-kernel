/*
 * wifipw_test: what wifid does with a network's password when Settings joins it, done here the same
 * way: MobileWiFi's WiFiSecuritySetPassword() into the keychain, WiFiSecurityCopyPassword() back
 * (what Auto-Join asks before it joins a known network on its own), WiFiSecurityRemovePassword()
 * (what "Forget This Network" does). The network is a made-up one ("hakios-wifipw-test") with a
 * made-up password, neither printed in full. Signed with wifipw_test.entitlements: wifid's name
 * and its right to the system keychain, as the keychain answers a program by those.
 * MobileWiFi's own os_log lines ("__WiFiSecuritySetPassword:[...] Error result -25291") say the
 * keychain's answer; they are printed on the console as well (OS_ACTIVITY_DT_MODE).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define SSID	CFSTR("hakios-wifipw-test")
#define SECRET	CFSTR("not-a-real-password")

static CFStringRef (*copy_password)(CFStringRef ssid);

static void
read_back(const char *when)
{
	CFStringRef pw = copy_password(SSID);

	printf("wifipw_test: %s: WiFiSecurityCopyPassword: %s\n", when,
	    pw == NULL ? "nothing" : CFEqual(pw, SECRET) ? "THE SAME PASSWORD" : "another password");
	if (pw != NULL)
		CFRelease(pw);
}

int
main(int argc, char **argv)
{
	void *mw;
	Boolean (*set_password)(CFStringRef ssid, CFStringRef password);
	Boolean (*remove_password)(CFStringRef ssid);
	CFMutableDictionaryRef q;
	CFTypeRef attrs = NULL;
	OSStatus status;

	(void)argc;
	if (getenv("OS_ACTIVITY_DT_MODE") == NULL) {
		/* os_log decides where it prints when the process starts */
		setenv("OS_ACTIVITY_DT_MODE", "YES", 1);
		execv(argv[0], argv);
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	mw = dlopen("/System/Library/PrivateFrameworks/MobileWiFi.framework/MobileWiFi", RTLD_NOW);
	set_password = mw ? dlsym(mw, "WiFiSecuritySetPassword") : NULL;
	copy_password = mw ? dlsym(mw, "WiFiSecurityCopyPassword") : NULL;
	remove_password = mw ? dlsym(mw, "WiFiSecurityRemovePassword") : NULL;
	if (set_password == NULL || copy_password == NULL || remove_password == NULL) {
		printf("wifipw_test: MobileWiFi or its functions not found: %s\n", dlerror());
		return 1;
	}
	read_back("before");
	printf("wifipw_test: WiFiSecuritySetPassword: %s\n", set_password(SSID, SECRET) ? "yes" : "NO");
	read_back("after");

	/* the item itself, as the keychain has it: wifid's service name, the network's name for account */
	q = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
	CFDictionarySetValue(q, kSecAttrService, CFSTR("AirPort"));
	CFDictionarySetValue(q, kSecAttrAccount, SSID);
	CFDictionarySetValue(q, kSecAttrSynchronizable, kSecAttrSynchronizableAny);
	CFDictionarySetValue(q, kSecReturnAttributes, kCFBooleanTrue);
	status = SecItemCopyMatching(q, &attrs);
	printf("wifipw_test: the item (service AirPort): %d\n", (int)status);
	if (attrs != NULL && CFGetTypeID(attrs) == CFDictionaryGetTypeID()) {
		CFTypeRef group = CFDictionaryGetValue(attrs, kSecAttrAccessGroup);
		CFTypeRef cls = CFDictionaryGetValue(attrs, kSecAttrAccessible);
		CFTypeRef sync = CFDictionaryGetValue(attrs, kSecAttrSynchronizable);
		char g[64] = "?", c[16] = "?";

		if (group != NULL && CFGetTypeID(group) == CFStringGetTypeID())
			CFStringGetCString(group, g, sizeof(g), kCFStringEncodingUTF8);
		if (cls != NULL && CFGetTypeID(cls) == CFStringGetTypeID())
			CFStringGetCString(cls, c, sizeof(c), kCFStringEncodingUTF8);
		printf("wifipw_test: its access group %s, protection class %s, synchronizable %s\n", g, c,
		    sync == NULL ? "?" : CFEqual(sync, kCFBooleanTrue) ? "yes" : "no");
	}
	if (attrs != NULL)
		CFRelease(attrs);
	CFRelease(q);

	printf("wifipw_test: WiFiSecurityRemovePassword: %s\n", remove_password(SSID) ? "yes" : "NO");
	read_back("after removing");
	return 0;
}
