/*
 * keychain_test: write a keychain item (a generic password), read it back, delete it, and print what
 * Security.framework answers each time: 0 is success, -25300 "not found", -25291 "not available"
 * (what the keychain answered while the kernel's AppleKeyStore stand-in did not wrap keys),
 * -34018 "missing entitlement" (built unsigned: it is signed with keychain_test.entitlements).
 *
 * usage: keychain_test [keep|read|add|delete]
 *   (none)   the whole round trip, as before: delete, add, read back, delete
 *   keep     read it; if it is not there, write one that says when -- so a later boot's "read"
 *            says which boot wrote it. For the data volume kept in the flash (var_store=1): the
 *            keychain's database is a file under /private/var like any other, and whether an item
 *            written in one boot can still be decrypted in the next is the question (the class keys
 *            are a reversible transformation in the kernel, iokit/Kernel/MiniAppleKeyStore.cpp, not
 *            a key of a Secure Enclave this phone has not got, so it should be -- and now is).
 *   read     read it and print it, and nothing else
 *   add      write one that says when, over whatever is there
 *   delete   remove it
 */
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static CFMutableDictionaryRef
query(void)
{
	CFMutableDictionaryRef q = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
	CFDictionarySetValue(q, kSecAttrService, CFSTR("hakios-keychain-test"));
	CFDictionarySetValue(q, kSecAttrAccount, CFSTR("test"));
	return q;
}

/* The item's bytes, or NULL; the caller releases. */
static CFDataRef
item_read(OSStatus *status)
{
	CFMutableDictionaryRef q = query();
	CFDataRef back = NULL;

	CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
	*status = SecItemCopyMatching(q, (CFTypeRef *)&back);
	CFRelease(q);
	return back;
}

static void
print_item(const char *what, OSStatus status, CFDataRef back)
{
	char text[128] = "";

	if (back != NULL && CFGetTypeID(back) == CFDataGetTypeID()) {
		CFIndex n = CFDataGetLength(back);

		if (n > (CFIndex)sizeof(text) - 1) {
			n = (CFIndex)sizeof(text) - 1;
		}
		memcpy(text, CFDataGetBytePtr(back), (size_t)n);
		text[n] = 0;
	}
	printf("keychain_test: %s: %d%s%s\n", what, (int)status, back != NULL ? ", " : "", text);
}

static OSStatus
item_add(void)
{
	CFMutableDictionaryRef q = query();
	char secret[64];
	CFDataRef data;
	OSStatus status;

	snprintf(secret, sizeof(secret), "written by run of %lld", (long long)time(NULL));
	data = CFDataCreate(NULL, (const UInt8 *)secret, (CFIndex)strlen(secret));
	CFDictionarySetValue(q, kSecValueData, data);
	CFDictionarySetValue(q, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock);
	status = SecItemAdd(q, NULL);
	printf("keychain_test: add \"%s\": %d\n", secret, (int)status);
	CFRelease(data);
	CFRelease(q);
	return status;
}

int
main(int argc, char **argv)
{
	const char *what = argc > 1 ? argv[1] : "";
	CFMutableDictionaryRef q;
	CFDataRef data, back = NULL;
	OSStatus status;

	setvbuf(stdout, NULL, _IOLBF, 0);

	if (strcmp(what, "keep") == 0) {
		back = item_read(&status);
		print_item("read", status, back);
		if (back != NULL) {
			CFRelease(back);
			return 0;               /* an earlier boot's item, still readable: nothing to do */
		}
		return item_add() == 0 ? 0 : 1;
	}
	if (strcmp(what, "read") == 0) {
		back = item_read(&status);
		print_item("read", status, back);
		if (back != NULL) {
			CFRelease(back);
		}
		return status == 0 ? 0 : 1;
	}
	if (strcmp(what, "add") == 0) {
		return item_add() == 0 ? 0 : 1;
	}
	if (strcmp(what, "delete") == 0) {
		q = query();
		status = SecItemDelete(q);
		printf("keychain_test: delete: %d\n", (int)status);
		CFRelease(q);
		return status == 0 ? 0 : 1;
	}

	/* no argument: the whole round trip, as this program always did */
	{
		static const char secret[] = "hello from the keychain";

		q = query();
		data = CFDataCreate(NULL, (const UInt8 *)secret, sizeof(secret) - 1);
		status = SecItemDelete(q);
		printf("keychain_test: delete before: %d\n", (int)status);
		CFDictionarySetValue(q, kSecValueData, data);
		CFDictionarySetValue(q, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock);
		status = SecItemAdd(q, NULL);
		printf("keychain_test: add: %d\n", (int)status);
		CFRelease(q);
		back = item_read(&status);
		printf("keychain_test: read back: %d, %s\n", (int)status, back != NULL && CFGetTypeID(back) == CFDataGetTypeID() &&
		    CFEqual(back, data) ? "THE SAME BYTES" : back != NULL ? "other bytes" : "nothing");
		if (back != NULL) {
			CFRelease(back);
		}
		q = query();
		status = SecItemDelete(q);
		printf("keychain_test: delete: %d\n", (int)status);
		CFRelease(q);
		CFRelease(data);
	}
	return 0;
}
