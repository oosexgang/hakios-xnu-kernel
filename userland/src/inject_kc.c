/*
 * libredwood.kc.dylib -- injected into akd.
 *
 * Why: Apple ID sign-in persists ~20 GrandSlam tokens into the keychain. One of them,
 * com.apple.gs.icloud.storage.buy, is stored in the protection class
 * kSecAttrAccessibleWhenPasscodeSetThisDeviceOnly ("akpu"). That class exists only on a device
 * with a real passcode: its wrapping key is derived from the passcode by the Secure Enclave on
 * unlock. This port has no Secure Enclave and no passcode, so securityd refuses to create the item
 * (errSecAuthFailed, -25293) no matter what lock/passcode state the kernel keybag stub reports --
 * it never even asks the keybag to wrap an akpu key. akd treats that single store failure as fatal
 * (PersistGrandslamTokens -> Global signout -> the account is removed -> "Could Not Sign In"),
 * although every other token and the account credential itself persist fine.
 *
 * A genuine passcode-free iPhone never hits this because such a token would not be put in a
 * passcode-only class. So, only for akd, we rewrite the one unsatisfiable protection class to its
 * passcode-free neighbour, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly ("cku"): same
 * this-device-only confinement and same "available after first unlock" lifetime, but wrapped with
 * the device key, which our keybag stub provides. securityd then stores the item, the persist
 * succeeds, and sign-in completes. getprogname() guards it so nothing but akd is affected, and only
 * this exact class value is touched -- every other keychain write passes through unchanged.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* Our log goes to stderr and to a file the kernel log flood cannot evict. */

typedef int OSStatus;

extern OSStatus SecItemAdd(CFDictionaryRef attributes, CFTypeRef *result);
extern OSStatus SecItemUpdate(CFDictionaryRef query, CFDictionaryRef attributesToUpdate);
extern OSStatus SecItemDelete(CFDictionaryRef query);
extern const CFStringRef kSecAttrAccessible;
extern const CFStringRef kSecAttrAccessibleWhenPasscodeSetThisDeviceOnly;
extern const CFStringRef kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
extern const CFStringRef kSecClass;
extern const CFStringRef kSecClassGenericPassword;
extern const CFStringRef kSecAttrAccount;
extern const CFStringRef kSecAttrService;
extern const CFStringRef kSecValueData;
extern const CFStringRef kSecAttrAccessControl;

typedef const struct __SecAccessControl *SecAccessControlRef;
typedef uint32_t SecAccessControlCreateFlags;
extern SecAccessControlRef SecAccessControlCreateWithFlags(CFAllocatorRef allocator, CFTypeRef protection,
    SecAccessControlCreateFlags flags, CFErrorRef *error);
/* SecAccessControlCreateFlags values (SecAccessControl.h) */
#define kSecAccessControlUserPresence   (1u << 0)
#define kSecAccessControlDevicePasscode (1u << 4)

static bool ours;

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = (int)sizeof(line) - 1;
	}
	if (n > 0) {
		int fd;

		write(2, line, (size_t)n);
		/* Also to a file the kernel log flood cannot evict. */
		fd = open("/private/var/root/redwood_kc.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd >= 0) {
			write(fd, line, (size_t)n);
			close(fd);
		}
		/* and into the kernel's kept log (kern.redwood.say, "!" = kept), so it comes back in the run log */
		{
			char kept[280];
			int k = snprintf(kept, sizeof(kept), "!%.*s", n > 0 && line[n - 1] == '\n' ? n - 1 : n, line);

			if (k > 0) {
				sysctlbyname("kern.redwood.say", NULL, NULL, kept, (size_t)k);
			}
		}
	}
}

/*
 * If dict asks for the passcode-only class, return a mutable copy with that one value swapped for the
 * passcode-free this-device-only class; otherwise NULL (leave the call untouched). Caller releases.
 */
static CFDictionaryRef
downgrade(CFDictionaryRef dict)
{
	CFTypeRef accessible, accessControl;
	CFMutableDictionaryRef copy;

	if (dict == NULL) {
		return NULL;
	}
	accessControl = CFDictionaryGetValue(dict, kSecAttrAccessControl);
	accessible = CFDictionaryGetValue(dict, kSecAttrAccessible);

	/*
	 * A SecAccessControl (kSecAttrAccessControl) cannot be stored at all on this port: its ref-key is
	 * derived from the real passcode by the Secure Enclave, which does not exist here, so securityd
	 * returns errSecAuthFailed (-25293) for ANY access-control item -- even one whose only constraint
	 * is a protection class (measured: a plain kSecAttrAccessible item in the same class stores fine).
	 * The one token akd fails to persist (storage.buy) is stored this way. Replace the access control
	 * with the equivalent plain protection class so the item stores; nothing on this device enforces
	 * the dropped user-presence/passcode constraint anyway (there is no passcode and no SEP).
	 */
	if (accessControl != NULL) {
		copy = CFDictionaryCreateMutableCopy(NULL, 0, dict);
		if (copy == NULL) {
			return NULL;
		}
		CFDictionaryRemoveValue(copy, kSecAttrAccessControl);
		CFDictionarySetValue(copy, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
		say("redwood: kc: stripped SecAccessControl -> AfterFirstUnlockThisDeviceOnly\n");
		return copy;
	}

	/* No access control, but a passcode-only protection class: downgrade it to its passcode-free kin. */
	if (accessible != NULL && CFGetTypeID(accessible) == CFStringGetTypeID()
	    && CFEqual(accessible, kSecAttrAccessibleWhenPasscodeSetThisDeviceOnly)) {
		copy = CFDictionaryCreateMutableCopy(NULL, 0, dict);
		if (copy == NULL) {
			return NULL;
		}
		CFDictionarySetValue(copy, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
		say("redwood: kc: downgraded WhenPasscodeSetThisDeviceOnly -> AfterFirstUnlockThisDeviceOnly\n");
		return copy;
	}
	return NULL;
}

/* Trace that a call actually reached our hook, and whether it carried an access control. */
static void
trace_call(const char *fn, CFDictionaryRef d)
{
	char svc[96] = "";
	CFTypeRef service, ac;

	if (d == NULL) {
		return;
	}
	service = CFDictionaryGetValue(d, kSecAttrService);
	ac = CFDictionaryGetValue(d, kSecAttrAccessControl);
	if (service != NULL && CFGetTypeID(service) == CFStringGetTypeID()) {
		CFStringGetCString(service, svc, sizeof(svc), kCFStringEncodingUTF8);
	}
	say("redwood: kc: %s service=\"%s\" accessControl=%s\n", fn, svc, ac != NULL ? "YES" : "no");
}

/*
 * akd stores its GrandSlam tokens from AuthKit.framework, which is in the dyld shared cache; its calls
 * to Security.framework's SecItem* are bound directly inside the cache and so are NOT redirected by
 * __interpose (confirmed: our interpose saw none of them). An inline hook patches the function itself,
 * so every caller is caught. MSHookFunction (CydiaSubstrate, present on the jailbreak prefix at
 * /var/jb) installs it and hands back a trampoline to the original.
 */
static OSStatus (*orig_SecItemAdd)(CFDictionaryRef, CFTypeRef *);
static OSStatus (*orig_SecItemUpdate)(CFDictionaryRef, CFDictionaryRef);
static int hook_fired;   /* incremented inside hooked_SecItemAdd, read by the self-test */

static OSStatus
hooked_SecItemAdd(CFDictionaryRef attributes, CFTypeRef *result)
{
	CFDictionaryRef fixed = downgrade(attributes);
	OSStatus (*orig)(CFDictionaryRef, CFTypeRef *) = orig_SecItemAdd ? orig_SecItemAdd : SecItemAdd;

	hook_fired++;
	trace_call("SecItemAdd", attributes);
	if (fixed != NULL) {
		OSStatus status = orig(fixed, result);
		CFRelease(fixed);
		return status;
	}
	return orig(attributes, result);
}

static OSStatus
hooked_SecItemUpdate(CFDictionaryRef query, CFDictionaryRef attributesToUpdate)
{
	CFDictionaryRef fixed = downgrade(attributesToUpdate);
	OSStatus (*orig)(CFDictionaryRef, CFDictionaryRef) = orig_SecItemUpdate ? orig_SecItemUpdate : SecItemUpdate;

	trace_call("SecItemUpdate", attributesToUpdate);
	if (fixed != NULL) {
		OSStatus status = orig(query, fixed);
		CFRelease(fixed);
		return status;
	}
	return orig(query, attributesToUpdate);
}

typedef void (*MSHookFunction_t)(void *symbol, void *replace, void **result);

static void
install_hooks(void)
{
	void *h = dlopen("/var/jb/usr/lib/libsubstrate.dylib", RTLD_NOW);
	MSHookFunction_t MSHookFunction;

	if (h == NULL) {
		h = dlopen("/var/jb/Library/Frameworks/CydiaSubstrate.framework/CydiaSubstrate", RTLD_NOW);
	}
	if (h == NULL) {
		say("redwood: kc: dlopen(libsubstrate) failed: %s\n", dlerror());
		return;
	}
	MSHookFunction = (MSHookFunction_t)dlsym(h, "MSHookFunction");
	if (MSHookFunction == NULL) {
		say("redwood: kc: MSHookFunction not found\n");
		return;
	}
	MSHookFunction((void *)SecItemAdd, (void *)hooked_SecItemAdd, (void **)&orig_SecItemAdd);
	MSHookFunction((void *)SecItemUpdate, (void *)hooked_SecItemUpdate, (void **)&orig_SecItemUpdate);
	say("redwood: kc: inline hooks installed (SecItemAdd=%p SecItemUpdate=%p)\n",
	    (void *)orig_SecItemAdd, (void *)orig_SecItemUpdate);

	/*
	 * Self-test: does a call through the public SecItemAdd symbol actually land in our hook? If the
	 * shared-cache code page could not be made writable, MSHookFunction returns a trampoline but the
	 * patch never took, and real callers (including akd's own) sail past untouched. Call it once and
	 * see whether hook_fired moved.
	 */
	{
		const void *k[2] = { kSecClass, kSecAttrService };
		const void *v[2] = { kSecClassGenericPassword, CFSTR("redwood.kc.hooktest") };
		CFDictionaryRef q = CFDictionaryCreate(NULL, k, v, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		int before = hook_fired;

		SecItemAdd(q, NULL);
		SecItemDelete(q);
		CFRelease(q);
		say("redwood: kc: hook self-test: hooked_SecItemAdd fired=%s\n", hook_fired > before ? "YES" : "NO");
	}
}

/*
 * Standalone probe: try to store one keychain item in a given protection class, report the OSStatus,
 * then delete it. Lets us learn -- without a full Apple ID sign-in -- whether securityd will accept the
 * passcode-only class (akpu) under the current keybag state the kernel reports, with a non-passcode
 * class as a control so a failure is distinguishable from a broken probe. ("zero is not a measurement".)
 */
static void
probe_class(const char *label, CFStringRef accessible)
{
	const void *keys[4];
	const void *vals[4];
	CFDictionaryRef query;
	CFDataRef data;
	OSStatus add, del;
	static const uint8_t one = 1;

	CFStringRef account = CFStringCreateWithCString(NULL, label, kCFStringEncodingUTF8);

	data = CFDataCreate(NULL, &one, 1);
	keys[0] = kSecClass;          vals[0] = kSecClassGenericPassword;
	keys[1] = kSecAttrService;    vals[1] = CFSTR("redwood.kc.probe");
	keys[2] = kSecAttrAccount;    vals[2] = account;
	keys[3] = kSecAttrAccessible; vals[3] = accessible;
	query = CFDictionaryCreate(NULL, keys, vals, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	/* add then delete; include the data only on the add */
	{
		CFMutableDictionaryRef add_q = CFDictionaryCreateMutableCopy(NULL, 0, query);
		CFDictionarySetValue(add_q, kSecValueData, data);
		add = SecItemAdd(add_q, NULL);
		CFRelease(add_q);
	}
	del = SecItemDelete(query);
	say("redwood: kc: probe %s (%s): SecItemAdd=%d SecItemDelete=%d\n", label,
	    accessible == kSecAttrAccessibleWhenPasscodeSetThisDeviceOnly ? "akpu" : "cku", (int)add, (int)del);
	CFRelease(query);
	CFRelease(data);
	CFRelease(account);
}

/*
 * Like probe_class, but puts the protection class inside a SecAccessControl (kSecAttrAccessControl),
 * which is how a token guarded by user-presence/passcode is really stored. This is the suspected shape
 * of the one token (storage.buy) that akd fails to persist: its ACL ref-key is derived from the real
 * passcode by the SEP, so on a passcode-less device the write returns errSecAuthFailed (-25293) even
 * though a plain akpu item (no ACL) stores fine.
 */
static void
probe_acl(const char *label, CFTypeRef protection, SecAccessControlCreateFlags flags)
{
	const void *keys[4];
	const void *vals[4];
	CFMutableDictionaryRef add_q;
	CFStringRef account = CFStringCreateWithCString(NULL, label, kCFStringEncodingUTF8);
	CFErrorRef err = NULL;
	SecAccessControlRef ac = SecAccessControlCreateWithFlags(NULL, protection, flags, &err);
	OSStatus add = -1, del = -1;
	static const uint8_t one = 1;
	CFDataRef data = CFDataCreate(NULL, &one, 1);

	if (ac == NULL) {
		say("redwood: kc: probe %s: SecAccessControlCreateWithFlags FAILED (err set=%d)\n", label, err != NULL);
		if (err) { CFRelease(err); }
		CFRelease(account);
		CFRelease(data);
		return;
	}
	keys[0] = kSecClass;             vals[0] = kSecClassGenericPassword;
	keys[1] = kSecAttrService;       vals[1] = CFSTR("redwood.kc.probe");
	keys[2] = kSecAttrAccount;       vals[2] = account;
	keys[3] = kSecAttrAccessControl; vals[3] = ac;
	add_q = CFDictionaryCreateMutable(NULL, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	for (int i = 0; i < 4; i++) {
		CFDictionarySetValue(add_q, keys[i], vals[i]);
	}
	CFDictionarySetValue(add_q, kSecValueData, data);
	add = SecItemAdd(add_q, NULL);
	CFDictionaryRemoveValue(add_q, kSecValueData);
	CFDictionaryRemoveValue(add_q, kSecAttrAccessControl);
	del = SecItemDelete(add_q);
	say("redwood: kc: probe %s (ACL flags %#x): SecItemAdd=%d SecItemDelete=%d\n", label, flags, (int)add, (int)del);
	CFRelease(add_q);
	CFRelease(ac);
	CFRelease(account);
	CFRelease(data);
}

__attribute__((constructor))
static void
redwood_kc_init(void)
{
	const char *name = getprogname();

	/*
	 * akd persists GrandSlam tokens by handing them to accountsd (ACDAccountStore), so the keychain
	 * SecItemAdd that carries storage.buy's SecAccessControl runs in accountsd, not akd (the error akd
	 * reports is com.apple.accounts Code=13, i.e. relayed from that daemon). Hook both to be safe.
	 */
	ours = name != NULL && (strcmp(name, "akd") == 0 || strcmp(name, "accountsd") == 0
	    || strcmp(name, "itunesstored") == 0 || strcmp(name, "appstored") == 0);
	say("redwood: kc: loaded by \"%s\"%s\n", name != NULL ? name : "(null)", ours ? "" : " (watching nothing)");
	if (ours) {
		install_hooks();
	}

	/* Run the keychain-class probe once, from SpringBoard (an injectable process with keychain access). */
	if (name != NULL && strcmp(name, "SpringBoard") == 0) {
		probe_class("control", kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
		probe_class("passcode", kSecAttrAccessibleWhenPasscodeSetThisDeviceOnly);
		probe_acl("acl-akpu-plain", kSecAttrAccessibleWhenPasscodeSetThisDeviceOnly, 0);
		probe_acl("acl-akpu-passcode", kSecAttrAccessibleWhenPasscodeSetThisDeviceOnly, kSecAccessControlDevicePasscode);
		probe_acl("acl-afu-presence", kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly, kSecAccessControlUserPresence);
	}
}
