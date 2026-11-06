/*
 * libredwood.mad.dylib: code of ours inside mobileactivationd.
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
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
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
		int fd = open("/private/var/mobile/redwood_mad.log", O_WRONLY | O_CREAT | O_APPEND, 0644);

		if (fd >= 0) {
			write(fd, line, (size_t)n);
			close(fd);
		}
	}
	/* and into the kernel's kept log (kern.redwood.say, "!" = kept), so it comes back in the run log */
	{
		char kept[520];
		int k = snprintf(kept, sizeof(kept), "!%.*s", n > 0 && line[n - 1] == '\n' ? n - 1 : n, line);

		if (k > 0) {
			sysctlbyname("kern.redwood.say", NULL, NULL, kept, (size_t)k);
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
		say("redwood: mad: MGCopyAnswer(%s) -> forced %d (dev-fused)\n", name, forced);
		return CFNumberCreate(NULL, kCFNumberIntType, &forced);
	}
	return MGCopyAnswer(key);
}

__attribute__((used)) static struct {
	const void *ours, *theirs;
} interposed[] __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)watched_MGCopyAnswer, (const void *)MGCopyAnswer },
};

__attribute__((constructor))
static void
redwood_mad_init(void)
{
	const char *name = getprogname();

	ours = name != NULL && strcmp(name, "mobileactivationd") == 0;
	say("redwood: mad: ours is in, loaded by \"%s\"%s\n", name != NULL ? name : "(null)", ours ? "" : " (watching nothing)");
}
