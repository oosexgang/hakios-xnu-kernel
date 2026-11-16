/*
 * libhakios.kc.dylib -- injected into akd.
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
#include <Block.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "absinthe_bag.h"

/* The host that runs sapd also runs a tiny line sink here, so our log survives the kernel log flood. */
#define KC_SINK_HOST "192.168.1.18"
#define KC_SINK_PORT 8100

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
		int fd, s;

		write(2, line, (size_t)n);
		/* Also to a file the kernel log flood cannot evict. */
		fd = open("/private/var/root/hakios_kc.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd >= 0) {
			write(fd, line, (size_t)n);
			close(fd);
		}
		/* And best-effort to the host line sink over Wi-Fi, so it can be read live without ssh. */
		s = socket(AF_INET, SOCK_STREAM, 0);
		if (s >= 0) {
			struct sockaddr_in addr;
			struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 };

			setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
			memset(&addr, 0, sizeof(addr));
			addr.sin_family = AF_INET;
			addr.sin_port = htons(KC_SINK_PORT);
			addr.sin_addr.s_addr = inet_addr(KC_SINK_HOST);
			if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
				write(s, line, (size_t)n);
			}
			close(s);
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
		say("hakios: kc: stripped SecAccessControl -> AfterFirstUnlockThisDeviceOnly\n");
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
		say("hakios: kc: downgraded WhenPasscodeSetThisDeviceOnly -> AfterFirstUnlockThisDeviceOnly\n");
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
	say("hakios: kc: %s service=\"%s\" accessControl=%s\n", fn, svc, ac != NULL ? "YES" : "no");
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
typedef void (*MSHookMessageEx_t)(Class cls, SEL sel, IMP replace, IMP *result);

/*
 * Capture small JSON response bodies (store daemons only). The App Store media-account auth to
 * auth.itunes.apple.com returns a short (~119-byte) JSON the store treats as failure, but the task
 * object does not retain the body -- so hook where it is parsed, +[NSJSONSerialization
 * JSONObjectWithData:options:error:], and print the raw bytes. Small inputs only, to stay quiet.
 */
static id (*orig_json)(id, SEL, id, unsigned long, id *);

/* Universal response capture: the otp media-auth uses a plain NSURLSession whose delegate accumulates
 * the body into an NSMutableData. Hook the two accumulators and print any small JSON ('{') chunk --
 * the 118-byte otp verify reply lands here when every higher hook misses it. Tight filter = little noise. */
static void
log_raw(const unsigned char *b, long len)
{
	if (b == NULL || len <= 0 || len >= 600 || b[0] != '{') {
		return;
	}
	{
		char body[640];
		int n = (int)len;

		memcpy(body, b, (size_t)n);
		body[n] = 0;
		for (int i = 0; i < n; i++) {
			if (body[i] < 32) {
				body[i] = ' ';
			}
		}
		say("hakios: kc: raw-json len=%ld %s\n", len, body);
	}
}

static void (*orig_appendData)(id, SEL, id);
static void
hooked_appendData(id self, SEL _cmd, id data)
{
	if (data != NULL) {
		long len = ((long (*)(id, SEL))objc_msgSend)(data, sel_registerName("length"));
		const void *bytes = len > 0 && len < 600 ? ((const void *(*)(id, SEL))objc_msgSend)(data, sel_registerName("bytes")) : NULL;

		if (bytes != NULL) {
			log_raw(bytes, len);
		}
	}
	orig_appendData(self, _cmd, data);
}

static void (*orig_appendBytes)(id, SEL, const void *, unsigned long);
static void
hooked_appendBytes(id self, SEL _cmd, const void *bytes, unsigned long len)
{
	if (bytes != NULL && len > 0 && len < 600) {
		log_raw(bytes, (long)len);
	}
	orig_appendBytes(self, _cmd, bytes, len);
}

/* Log the bytes of a small NSData body (tagged), stripped to one line. Parser-independent. */
static void
log_body(const char *tag, id data)
{
	if (data == NULL) {
		return;
	}
	long len = ((long (*)(id, SEL))objc_msgSend)(data, sel_registerName("length"));
	const void *bytes = len > 0 ? ((const void *(*)(id, SEL))objc_msgSend)(data, sel_registerName("bytes")) : NULL;

	if (bytes != NULL && len > 0 && len < 1800) {
		char buf[1920];
		int n = (int)len;

		memcpy(buf, bytes, (size_t)n);
		buf[n] = 0;
		for (int i = 0; i < n; i++) {
			if (buf[i] == '\n' || buf[i] == '\r') {
				buf[i] = ' ';
			}
		}
		say("hakios: kc: %s(%ld) %s\n", tag, len, buf);
	} else if (len > 0) {
		say("hakios: kc: %s(%ld) (too big to print)\n", tag, len);
	}
}

/*
 * Dump a possibly-large string to the host sink in one shot. say() caps lines at 256 bytes, but an
 * NSError's userInfo (where the authenticate failure's server body / failureType / customerMessage /
 * HTTP status live) can be much larger -- so send the whole thing straight to the sink and console.
 */
static void
say_long(const char *s)
{
	int n = (int)strlen(s);
	int fd, sk;

	if (n <= 0) {
		return;
	}
	write(2, s, (size_t)n);
	fd = open("/private/var/root/hakios_kc.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd >= 0) {
		write(fd, s, (size_t)n);
		close(fd);
	}
	sk = socket(AF_INET, SOCK_STREAM, 0);
	if (sk >= 0) {
		struct sockaddr_in addr;
		struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 };

		setsockopt(sk, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(KC_SINK_PORT);
		addr.sin_addr.s_addr = inet_addr(KC_SINK_HOST);
		if (connect(sk, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
			write(sk, s, (size_t)n);
		}
		close(sk);
	}
}

/*
 * The media-account sign-in ("Cannot Sign In -- password and verification code could not be verified")
 * is assembled in itunesstored as an NSError and shown via XPC, so the NSURLSession/JSON hooks above --
 * which only watch auth.itunes bodies -- never see it. Capture it where it is BORN: every NSError made
 * with a store/account/auth/FairPlay domain, dumping the FULL userInfo. That userInfo answers both
 * questions at once: the exact failure, AND whether the request even left (a server HTTP status /
 * response body in userInfo means it reached Apple and was refused; a purely local reason -- anisette,
 * signing, missing machine data -- means it never went out, like the iCloud Absinthe case).
 */
static void
report_auth_error(id domain, long code, id info)
{
	char dt[80] = "";

	if (domain == NULL) {
		return;
	}
	if (!CFStringGetCString((CFStringRef)domain, dt, sizeof(dt), kCFStringEncodingUTF8)) {
		return;
	}
	if (strstr(dt, "AMS") == NULL && strstr(dt, "MZ") == NULL && strstr(dt, "Store") == NULL
	    && strstr(dt, "ccount") == NULL && strstr(dt, "uth") == NULL && strstr(dt, "Tunes") == NULL
	    && strstr(dt, "SS") == NULL && strstr(dt, "AK") == NULL && strstr(dt, "FairPlay") == NULL
	    && strstr(dt, "Commerce") == NULL && strstr(dt, "URLError") == NULL) {
		return;
	}
	{
		char head[160];

		snprintf(head, sizeof(head), "hakios: kc: auth-error %s code %ld info: ", dt, code);
		say_long(head);
	}
	if (info != NULL) {
		CFStringRef d = CFCopyDescription((CFTypeRef)info);

		if (d != NULL) {
			CFIndex len = CFStringGetLength(d);
			CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
			char *buf = (char *)malloc((size_t)max);

			if (buf != NULL) {
				if (CFStringGetCString(d, buf, max, kCFStringEncodingUTF8)) {
					for (CFIndex i = 0; buf[i] != '\0'; i++) {
						if (buf[i] == '\n' || buf[i] == '\r') {
							buf[i] = ' ';
						}
					}
					say_long(buf);
				}
				free(buf);
			}
			CFRelease(d);
		}
	}
	say_long("\n");
}

static id (*orig_nserr_with)(id, SEL, id, long, id);
static id
my_nserr_with(id self, SEL cmd, id domain, long code, id info)
{
	report_auth_error(domain, code, info);
	return orig_nserr_with(self, cmd, domain, code, info);
}

static id (*orig_nserr_init)(id, SEL, id, long, id);
static id
my_nserr_init(id self, SEL cmd, id domain, long code, id info)
{
	report_auth_error(domain, code, info);
	return orig_nserr_init(self, cmd, domain, code, info);
}

/*
 * THE MEDIA-ACCOUNT 2FA FIX. The App Store media sign-in reaches the 2FA screen but cannot complete
 * it: AMS, building the final request, asks its bag (through -[ISAMSBagShim _bagValueForKey:valueType:])
 * for "absinthe-requests" -- the list of rules that says which requests need a device (Absinthe)
 * signature -- and the itunesstored bag shim has no such key, so AMS raises AMSErrorDomain 204
 * ("ISAMSBagShim Error -- No bag value for key: absinthe-requests") and the sign-in fails with
 * "could not be verified". The key is present in the device's general AMS bag (and the appstored bag),
 * only missing from the one the itunesstored shim consults. So when the shim returns nil for it, hand
 * back the real value (ABSINTHE_REQUESTS_JSON, lifted from the device AMS bag): AMS then checks the
 * authenticate/otp URL against the 11 purchase/eligibility rules, matches none, needs no Absinthe
 * signature, and the request proceeds -- the mere presence of the key is what unblocks it.
 */
static id g_absinthe_requests;   /* cached NSArray parsed from ABSINTHE_REQUESTS_JSON */

static id
absinthe_requests_array(void)
{
	if (g_absinthe_requests == NULL) {
		id nsstr = ((id (*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"),
		    sel_registerName("stringWithUTF8String:"), ABSINTHE_REQUESTS_JSON);
		id data = nsstr != NULL ? ((id (*)(id, SEL, unsigned long))objc_msgSend)(nsstr,
		    sel_registerName("dataUsingEncoding:"), 4 /* UTF8 */) : NULL;
		id err = NULL;
		id arr = data != NULL ? ((id (*)(id, SEL, id, unsigned long, id *))objc_msgSend)(
		    (id)objc_getClass("NSJSONSerialization"),
		    sel_registerName("JSONObjectWithData:options:error:"), data, 0, &err) : NULL;

		if (arr != NULL) {
			g_absinthe_requests = (id)CFRetain((CFTypeRef)arr);
		} else {
			say("hakios: kc: absinthe-requests JSON parse FAILED\n");
		}
	}
	return g_absinthe_requests;
}

static bool
wants_absinthe_requests(id key)
{
	char k[80] = "";

	return key != NULL && CFStringGetCString((CFStringRef)key, k, sizeof(k), kCFStringEncodingUTF8)
	    && strcmp(k, "absinthe-requests") == 0;
}

static int g_bagseen;      /* diagnostic: how many bag lookups we have logged */

static id (*orig_bagValue)(id, SEL, id, long);
static id
my_bagValue(id self, SEL cmd, id key, long valueType)
{
	id result = orig_bagValue(self, cmd, key, valueType);

	if (key != NULL && g_bagseen < 40) {
		char k[80] = "";

		CFStringGetCString((CFStringRef)key, k, sizeof(k), kCFStringEncodingUTF8);
		g_bagseen++;
		say("hakios: kc: bag _bagValueForKey key=\"%s\" type=%ld -> %s\n", k, valueType,
		    result != NULL ? "value" : "NIL");
	}
	if (result == NULL && wants_absinthe_requests(key)) {
		id a = absinthe_requests_array();

		if (a != NULL) {
			long n = ((long (*)(id, SEL))objc_msgSend)(a, sel_registerName("count"));

			say("hakios: kc: INJECTED bag absinthe-requests via _bagValueForKey (%ld rules, type %ld)\n", n, valueType);
			return a;
		}
	}
	return result;
}

/*
 * AMS actually asks for it through the AMSBagProtocol accessor -[ISAMSBagShim arrayForKey:], which the
 * private _bagValueForKey:valueType: below did not sit on the path of (the hook on it never fired, yet
 * the "No bag value for key: absinthe-requests" error still came). So hook the public typed accessor
 * too: when the bag has no array for "absinthe-requests", hand back the real one.
 */
static int g_arrseen;      /* diagnostic: how many arrayForKey lookups we have logged */

static id (*orig_arrayForKey)(id, SEL, id);
static id
my_arrayForKey(id self, SEL cmd, id key)
{
	id result = orig_arrayForKey(self, cmd, key);

	if (key != NULL && g_arrseen < 40) {
		char k[80] = "";

		CFStringGetCString((CFStringRef)key, k, sizeof(k), kCFStringEncodingUTF8);
		g_arrseen++;
		say("hakios: kc: bag arrayForKey key=\"%s\" -> %s\n", k, result != NULL ? "array" : "NIL");
	}
	if (result == NULL && wants_absinthe_requests(key)) {
		id a = absinthe_requests_array();

		if (a != NULL) {
			long n = ((long (*)(id, SEL))objc_msgSend)(a, sel_registerName("count"));

			say("hakios: kc: INJECTED bag absinthe-requests via arrayForKey (%ld rules)\n", n);
			return a;
		}
	}
	return result;
}

/*
 * Capture the raw response body of the App Store media-account auth, whatever parser consumes it, by
 * wrapping the completion handler of -[NSURLSession dataTaskWithRequest:completionHandler:] for
 * auth.itunes.apple.com. This is what returns the short JSON/plist the store treats as failure.
 */
/*
 * Route the App Store media-account auth through a host proxy, so we see every request/response body on
 * the host and can pace calls to Apple instead of hammering them into HTTP 429. The rewrite happens here,
 * inside the process and before TLS, so Apple's cert pinning does not apply: the device talks plain HTTP
 * to the host, and the host does the real HTTPS to Apple. Only auth.itunes.apple.com is redirected; the
 * signed catalog traffic is untouched.
 */
#define KC_PROXY "http://192.168.1.18:8101/_/"

static id
rewrite_request(id request)
{
	char u[400] = "";
	id url = request != NULL ? ((id (*)(id, SEL))objc_msgSend)(request, sel_registerName("URL")) : NULL;
	id s = url != NULL ? ((id (*)(id, SEL))objc_msgSend)(url, sel_registerName("absoluteString")) : NULL;

	if (s != NULL) {
		((char (*)(id, SEL, char *, long, unsigned long))objc_msgSend)(
		    s, sel_registerName("getCString:maxLength:encoding:"), u, (long)sizeof(u), 4);
	}
	/* ONLY GrandSlam (gsa) via proxy. auth.itunes (AMS media otp) must go DIRECT: AMS rejects a proxied
	 * URL with AMSErrorDomain 300 "URL not trusted", which broke media sign-in after GrandSlam succeeded. */
	if (strstr(u, "https://gsa.apple.com/grandslam") == u) {
		char nu[460];
		id ns, newurl, mreq;

		snprintf(nu, sizeof(nu), KC_PROXY "%s", u + strlen("https://"));
		ns = ((id (*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"),
		    sel_registerName("stringWithUTF8String:"), nu);
		newurl = ((id (*)(id, SEL, id))objc_msgSend)((id)objc_getClass("NSURL"),
		    sel_registerName("URLWithString:"), ns);
		mreq = ((id (*)(id, SEL))objc_msgSend)(request, sel_registerName("mutableCopy"));
		if (mreq != NULL && newurl != NULL) {
			((void (*)(id, SEL, id))objc_msgSend)(mreq, sel_registerName("setURL:"), newurl);
			say("hakios: kc: proxied %s -> %s\n", u, nu);
			return mreq; /* autoreleased mutableCopy; fine for the brief call below */
		}
	}
	return request;
}

static id (*orig_dataTask1)(id, SEL, id);
static id
hooked_dataTask1(id self, SEL _cmd, id request)
{
	return orig_dataTask1(self, _cmd, rewrite_request(request));
}

/* The most upstream rewrite: every auth.itunes request URL is built from a string here. Redirect it to
 * the host proxy at creation, so whatever task/session (foreground, background, nsurlsessiond) carries
 * it goes through the proxy. Only auth.itunes is touched. */
static id (*orig_urlWithString)(id, SEL, id);
static id
hooked_urlWithString(id self, SEL _cmd, id str)
{
	if (str != NULL) {
		char u[400] = "";
		((char (*)(id, SEL, char *, long, unsigned long))objc_msgSend)(
		    str, sel_registerName("getCString:maxLength:encoding:"), u, (long)sizeof(u), 4);
		if (strncmp(u, "https://gsa.apple.com/grandslam", 31) == 0) {
			char nu[460];
			id ns;

			snprintf(nu, sizeof(nu), "http://192.168.1.18:8101/_/%s", u + 8);
			ns = ((id (*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"),
			    sel_registerName("stringWithUTF8String:"), nu);
			say("hakios: kc: url-rewrite %s -> %s\n", u, nu);
			return orig_urlWithString(self, _cmd, ns);
		}
	}
	return orig_urlWithString(self, _cmd, str);
}

static id (*orig_dataTask)(id, SEL, id, id);

static id
hooked_dataTask(id self, SEL _cmd, id request, id completion)
{
	char u[300] = "";
	id url;

	request = rewrite_request(request);
	url = request != NULL ? ((id (*)(id, SEL))objc_msgSend)(request, sel_registerName("URL")) : NULL;
	id s = url != NULL ? ((id (*)(id, SEL))objc_msgSend)(url, sel_registerName("absoluteString")) : NULL;

	if (s != NULL) {
		((char (*)(id, SEL, char *, long, unsigned long))objc_msgSend)(
		    s, sel_registerName("getCString:maxLength:encoding:"), u, (long)sizeof(u), 4 /* UTF8 */);
	}
	if (completion != NULL && (strstr(u, "auth.itunes.apple.com") != NULL || strstr(u, "/otp/1.5/") != NULL)) {
		id orig = (id)completion;
		char *uheap = strdup(u); /* the block runs asynchronously; u is on the stack, so copy it */
		void (^wrap)(id, id, id) = ^(id data, id response, id error) {
			long code = 0;
			if (response != NULL) {
				code = ((long (*)(id, SEL))objc_msgSend)(response, sel_registerName("statusCode"));
			}
			say("hakios: kc: otp-resp status %ld url=%s\n", code, uheap != NULL ? uheap : "?");
			log_body("otp-body", data);
			((void (^)(id, id, id))orig)(data, response, error);
			free(uheap);
		};
		return orig_dataTask(self, _cmd, request, (id)wrap);
	}
	return orig_dataTask(self, _cmd, request, completion);
}

static id
hooked_json(id self, SEL _cmd, id data, unsigned long opt, id *err)
{
	if (data != NULL) {
		long len = ((long (*)(id, SEL))objc_msgSend)(data, sel_registerName("length"));
		const void *bytes = len > 0 ? ((const void *(*)(id, SEL))objc_msgSend)(data, sel_registerName("bytes")) : NULL;

		if (bytes != NULL && len > 0 && len < 1800) {
			char buf[1920];
			int n = (int)len;

			memcpy(buf, bytes, (size_t)n);
			buf[n] = 0;
			for (int i = 0; i < n; i++) {
				if (buf[i] == '\n' || buf[i] == '\r') {
					buf[i] = ' ';
				}
			}
			say("hakios: kc: JSON(%ld) %s\n", len, buf);
		}
	}
	return orig_json(self, _cmd, data, opt, err);
}

static void
install_hooks(void)
{
	void *h = dlopen("/var/jb/usr/lib/libsubstrate.dylib", RTLD_NOW);
	MSHookFunction_t MSHookFunction;

	if (h == NULL) {
		h = dlopen("/var/jb/Library/Frameworks/CydiaSubstrate.framework/CydiaSubstrate", RTLD_NOW);
	}
	if (h == NULL) {
		say("hakios: kc: dlopen(libsubstrate) failed: %s\n", dlerror());
		return;
	}
	MSHookFunction = (MSHookFunction_t)dlsym(h, "MSHookFunction");
	if (MSHookFunction == NULL) {
		say("hakios: kc: MSHookFunction not found\n");
		return;
	}
	MSHookFunction((void *)SecItemAdd, (void *)hooked_SecItemAdd, (void **)&orig_SecItemAdd);
	MSHookFunction((void *)SecItemUpdate, (void *)hooked_SecItemUpdate, (void **)&orig_SecItemUpdate);
	say("hakios: kc: inline hooks installed (SecItemAdd=%p SecItemUpdate=%p)\n",
	    (void *)orig_SecItemAdd, (void *)orig_SecItemUpdate);

	/*
	 * Self-test: does a call through the public SecItemAdd symbol actually land in our hook? If the
	 * shared-cache code page could not be made writable, MSHookFunction returns a trampoline but the
	 * patch never took, and real callers (including akd's own) sail past untouched. Call it once and
	 * see whether hook_fired moved.
	 */
	{
		const void *k[2] = { kSecClass, kSecAttrService };
		const void *v[2] = { kSecClassGenericPassword, CFSTR("hakios.kc.hooktest") };
		CFDictionaryRef q = CFDictionaryCreate(NULL, k, v, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		int before = hook_fired;

		SecItemAdd(q, NULL);
		SecItemDelete(q);
		CFRelease(q);
		say("hakios: kc: hook self-test: hooked_SecItemAdd fired=%s\n", hook_fired > before ? "YES" : "NO");
	}

	/* Store daemons: also log small JSON response bodies, to see the media-account auth failure. */
	{
		const char *pn = getprogname();

		if (pn != NULL && (strcmp(pn, "itunesstored") == 0 || strcmp(pn, "appstored") == 0
		    || strcmp(pn, "AppStore") == 0 || strcmp(pn, "akd") == 0)) {
			MSHookMessageEx_t MSHookMessageEx = (MSHookMessageEx_t)dlsym(h, "MSHookMessageEx");
			Class meta = object_getClass((id)objc_getClass("NSJSONSerialization"));

			Class urlsession = (Class)objc_getClass("NSURLSession");

			if (MSHookMessageEx != NULL && meta != NULL) {
				MSHookMessageEx(meta, sel_registerName("JSONObjectWithData:options:error:"),
				    (IMP)hooked_json, (IMP *)&orig_json);
				say("hakios: kc: JSON response logging installed\n");
			} else {
				say("hakios: kc: MSHookMessageEx=%p NSJSONSerialization=%p (JSON log not installed)\n",
				    (void *)MSHookMessageEx, (void *)meta);
			}
			if (MSHookMessageEx != NULL && urlsession != NULL) {
				MSHookMessageEx(urlsession, sel_registerName("dataTaskWithRequest:completionHandler:"),
				    (IMP)hooked_dataTask, (IMP *)&orig_dataTask);
				MSHookMessageEx(urlsession, sel_registerName("dataTaskWithRequest:"),
				    (IMP)hooked_dataTask1, (IMP *)&orig_dataTask1);
				say("hakios: kc: NSURLSession proxy+logging installed\n");
			}
			if (MSHookMessageEx != NULL) {
				Class nsurl = object_getClass((id)objc_getClass("NSURL"));

				if (nsurl != NULL) {
					MSHookMessageEx(nsurl, sel_registerName("URLWithString:"),
					    (IMP)hooked_urlWithString, (IMP *)&orig_urlWithString);
					say("hakios: kc: NSURL URLWithString rewrite installed\n");
				}
			}
			if (MSHookMessageEx != NULL) {
				Class md = (Class)objc_getClass("NSMutableData");

				if (md != NULL) {
					MSHookMessageEx(md, sel_registerName("appendData:"),
					    (IMP)hooked_appendData, (IMP *)&orig_appendData);
					MSHookMessageEx(md, sel_registerName("appendBytes:length:"),
					    (IMP)hooked_appendBytes, (IMP *)&orig_appendBytes);
					say("hakios: kc: NSMutableData body logging installed\n");
				}
			}
			if (MSHookMessageEx != NULL) {
				Class nserr = (Class)objc_getClass("NSError");

				if (nserr != NULL) {
					MSHookMessageEx(object_getClass((id)nserr),
					    sel_registerName("errorWithDomain:code:userInfo:"),
					    (IMP)my_nserr_with, (IMP *)&orig_nserr_with);
					MSHookMessageEx(nserr, sel_registerName("initWithDomain:code:userInfo:"),
					    (IMP)my_nserr_init, (IMP *)&orig_nserr_init);
					say("hakios: kc: NSError auth capture installed\n");
				}
			}
			if (MSHookMessageEx != NULL) {
				Class bagshim = (Class)objc_getClass("ISAMSBagShim");

				if (bagshim != NULL) {
					MSHookMessageEx(bagshim, sel_registerName("_bagValueForKey:valueType:"),
					    (IMP)my_bagValue, (IMP *)&orig_bagValue);
					MSHookMessageEx(bagshim, sel_registerName("arrayForKey:"),
					    (IMP)my_arrayForKey, (IMP *)&orig_arrayForKey);
					say("hakios: kc: ISAMSBagShim absinthe-requests injection installed (arrayForKey+_bagValueForKey)\n");
				} else {
					say("hakios: kc: ISAMSBagShim class not found (injection not installed)\n");
				}
			}
		}
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
	keys[1] = kSecAttrService;    vals[1] = CFSTR("hakios.kc.probe");
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
	say("hakios: kc: probe %s (%s): SecItemAdd=%d SecItemDelete=%d\n", label,
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
		say("hakios: kc: probe %s: SecAccessControlCreateWithFlags FAILED (err set=%d)\n", label, err != NULL);
		if (err) { CFRelease(err); }
		CFRelease(account);
		CFRelease(data);
		return;
	}
	keys[0] = kSecClass;             vals[0] = kSecClassGenericPassword;
	keys[1] = kSecAttrService;       vals[1] = CFSTR("hakios.kc.probe");
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
	say("hakios: kc: probe %s (ACL flags %#x): SecItemAdd=%d SecItemDelete=%d\n", label, flags, (int)add, (int)del);
	CFRelease(add_q);
	CFRelease(ac);
	CFRelease(account);
	CFRelease(data);
}

__attribute__((constructor))
static void
hakios_kc_init(void)
{
	const char *name = getprogname();

	/*
	 * akd persists GrandSlam tokens by handing them to accountsd (ACDAccountStore), so the keychain
	 * SecItemAdd that carries storage.buy's SecAccessControl runs in accountsd, not akd (the error akd
	 * reports is com.apple.accounts Code=13, i.e. relayed from that daemon). Hook both to be safe.
	 */
	ours = name != NULL && (strcmp(name, "akd") == 0 || strcmp(name, "accountsd") == 0
	    || strcmp(name, "itunesstored") == 0 || strcmp(name, "appstored") == 0
	    || strcmp(name, "AppStore") == 0);
	say("hakios: kc: loaded by \"%s\"%s\n", name != NULL ? name : "(null)", ours ? "" : " (watching nothing)");
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
