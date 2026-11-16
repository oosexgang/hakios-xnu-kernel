/*
 * libhakios.mad.dylib: code of ours inside mobileactivationd.
 *
 * mobileactivationd's DeviceType::init reads four MobileGestalt fusing answers --
 * CertificateProductionStatus, EffectiveProductionStatusAp, CertificateSecurityMode and
 * EffectiveSecurityModeSEP -- and, when they read as a development-fused part (0, 0, 1, 1) or a
 * production-fused-demoted one, sets should_hactivate. dealwith_activation then short-circuits the
 * activation state straight to "Activated" ("Hactivation is enabled, short circuiting ...") with no
 * record and no certificate at all -- which is the state the App Store's copyActivationRecord insists
 * on (the forged-record path reaches only "FactoryActivated").
 *
 * This phone has no Secure Enclave to fuse, so those answers are ours to give. Through dyld's
 * interposing (__DATA,__interpose) this returns the development-fused answer for those four keys and
 * passes every other key through untouched. It is the mobileactivationd process alone that is told
 * this (getprogname guards it), so nothing else on the system sees a changed fusing state.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static bool ours;

static void
say(const char *format, ...)
{
	char line[512];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = (int)sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
	{
		int fd = open("/private/var/mobile/hakios_mad.log", O_WRONLY | O_CREAT | O_APPEND, 0644);

		if (fd >= 0) {
			write(fd, line, (size_t)n);
			close(fd);
		}
	}
}

extern CFTypeRef MGCopyAnswer(CFStringRef key);

/* the development-fused answer: the key's forced integer, or INT_MIN to leave the key alone */
static int
forced_answer(CFStringRef key)
{
	char name[64] = "";

	if (key == NULL || !CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8)) {
		return -2147483647 - 1;
	}
	if (strcmp(name, "CertificateProductionStatus") == 0) {
		return 0;
	}
	if (strcmp(name, "EffectiveProductionStatusAp") == 0) {
		return 0;
	}
	if (strcmp(name, "CertificateSecurityMode") == 0) {
		return 1;
	}
	if (strcmp(name, "EffectiveSecurityModeSEP") == 0) {
		return 1;
	}
	return -2147483647 - 1;
}

static CFTypeRef
watched_MGCopyAnswer(CFStringRef key)
{
	int forced = ours ? forced_answer(key) : (-2147483647 - 1);

	if (forced != (-2147483647 - 1)) {
		char name[64] = "";

		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		say("hakios: mad: MGCopyAnswer(%s) -> forced %d (dev-fused)\n", name, forced);
		return CFNumberCreate(NULL, kCFNumberIntType, &forced);
	}
	return MGCopyAnswer(key);
}

__attribute__((used)) static struct {
	const void *ours, *theirs;
} interposed[] __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)watched_MGCopyAnswer, (const void *)MGCopyAnswer },
};

/*
 * ... and, because iOS 27's mobileactivationd reads the fusing keys under OBFUSCATED MobileGestalt names
 * (MGByqGSM and the like), the MGCopyAnswer interpose above for the plain key names never matches. So the
 * decision itself is forced instead: -[DeviceType should_hactivate] is hooked to answer YES, which is what
 * the development-fused answers would have produced, and dealwith_activation then short-circuits the state
 * straight to "Activated" ("Hactivation is enabled, short circuiting ..."). API-agnostic.
 */
static char (*old_should_hactivate)(id, SEL);

static char
new_should_hactivate(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	say("hakios: mad: -[DeviceType should_hactivate] -> forced YES\n");
	return 1;
}

/*
 * ... and the state the daemon actually hands back to clients: wrap
 * -[MobileActivationDaemon getActivationStateWithCompletionBlock:] to say the string it reports (and, if
 * it is not already "Activated", force it), so there is no doubt what the App Store receives.
 */
static void (*old_get_state)(id, SEL, void (^)(id, id));

static void
new_get_state(id self, SEL _cmd, void (^block)(id, id))
{
	void (^wrapped)(id, id) = ^(id state, id err) {
		char s[128] = "(nil)";

		if (state != NULL) {
			CFStringGetCString((CFStringRef)state, s, sizeof(s), kCFStringEncodingUTF8);
		}
		say("hakios: mad: getActivationState -> \"%s\" (err %p); forcing \"Activated\"\n", s, (void *)err);
		block((id)CFSTR("Activated"), NULL);
	};
	old_get_state(self, _cmd, wrapped);
}

__attribute__((constructor))
static void
hakios_mad_init(void)
{
	const char *name = getprogname();

	ours = name != NULL && strcmp(name, "mobileactivationd") == 0;
	say("hakios: mad: ours is in, loaded by \"%s\"%s\n", name != NULL ? name : "(null)", ours ? "" : " (watching nothing)");
	if (ours) {
		Class device_type = objc_getClass("DeviceType");
		Method m = device_type != Nil ? class_getInstanceMethod(device_type,
		    sel_registerName("should_hactivate")) : NULL;

		if (m != NULL) {
			old_should_hactivate = (char (*)(id, SEL))method_getImplementation(m);
			method_setImplementation(m, (IMP)new_should_hactivate);
			say("hakios: mad: -[DeviceType should_hactivate] hooked to answer YES\n");
		} else {
			say("hakios: mad: DeviceType/should_hactivate NOT found (class %s)\n",
			    device_type != Nil ? "there" : "MISSING");
		}
		{
			Class daemon = objc_getClass("MobileActivationDaemon");
			Method g = daemon != Nil ? class_getInstanceMethod(daemon,
			    sel_registerName("getActivationStateWithCompletionBlock:")) : NULL;

			if (g != NULL) {
				old_get_state = (void (*)(id, SEL, void (^)(id, id)))method_getImplementation(g);
				method_setImplementation(g, (IMP)new_get_state);
				say("hakios: mad: -[MobileActivationDaemon getActivationStateWithCompletionBlock:] hooked\n");
			} else {
				say("hakios: mad: getActivationStateWithCompletionBlock: NOT found (daemon %s)\n",
				    daemon != Nil ? "there" : "MISSING");
			}
		}
	}
}
