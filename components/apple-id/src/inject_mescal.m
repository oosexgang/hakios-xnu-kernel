/*
 * libhakios.mescal.dylib: inside appstored / itunesstored / AppStore, this makes the App Store's SAP
 * (Mescal) signature -- the one thing that stands between the storefront token and a 200.
 *
 * Why. The storefront-token request (sf-api-token-service/apiToken) is refused 401 because it carries
 * no X-Apple-ActionSignature: on this phone -[AMSMescalSession signData:bag:error:] cannot produce one
 * (its CoreFP returns -42184, an obfuscated check it will not pass without a real FairPlay
 * provisioning, and fairplayd is dead besides). The signature is NOT rooted in the Secure Enclave,
 * though -- it is Apple's software, and ipatool's engine makes an Apple-accepted one by running the
 * macOS CoreFP under emulation. That engine runs on the host as `sapd` (scratchpad/ipatool-src), and
 * the phone reaches it over Wi-Fi.
 *
 * What this does. It replaces -[AMSMescalSession signData:bag:error:] with one that POSTs the bytes to
 * sapd and returns sapd's signature. AMS then does the rest unchanged -- ams_addMescalHeaderWithType:
 * puts the signature in X-Apple-ActionSignature and sends the request -- so the store signs and talks
 * to Apple exactly as a real device would, with the one step it cannot do here delegated to a machine
 * that can. The host and port are a boot constant (fw-wifi's network is the phone's own Wi-Fi).
 */
#import <Foundation/Foundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#define SAPD_URL "http://192.168.1.18:8099/sign"

static bool ours;

static void
say(const char *format, ...)
{
	char line[320];
	va_list ap;
	int n, fd;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = (int)sizeof(line) - 1;
	}
	fd = open("/dev/console", O_WRONLY);
	if (fd >= 0) {
		write(fd, line, (size_t)n);
		close(fd);
	}
	write(2, line, (size_t)n);
}

/* POST the bytes to sapd and wait for the signature; a straight line, so it blocks on the reply */
static NSData *
sapd_sign(NSData *data)
{
	NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:@SAPD_URL]];
	__block NSData *signature = nil;
	dispatch_semaphore_t done = dispatch_semaphore_create(0);

	request.HTTPMethod = @"POST";
	request.HTTPBody = data;
	[request setValue:@"application/octet-stream" forHTTPHeaderField:@"Content-Type"];
	[[[NSURLSession sharedSession] dataTaskWithRequest:request
	    completionHandler:^(NSData *body, NSURLResponse *response, NSError *error) {
		    long status = [response isKindOfClass:[NSHTTPURLResponse class]] ? ((NSHTTPURLResponse *)response).statusCode : 0;

		    if (status == 200 && body.length > 0) {
			    signature = body;
		    }
		    dispatch_semaphore_signal(done);
	    }] resume];
	dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC));
	return signature;
}

static id (*old_signData)(id, SEL, id, id, NSError **);

static id
new_signData(id self, SEL _cmd, id data, id bag, NSError **error)
{
	NSData *signature = [data isKindOfClass:[NSData class]] ? sapd_sign(data) : nil;

	if (signature.length > 0) {
		say("hakios: mescal: signData %lu bytes -> sapd signature %lu bytes (X-Apple-ActionSignature)\n",
		    (unsigned long)[data length], (unsigned long)signature.length);
		if (error != NULL) {
			*error = nil;
		}
		return signature;
	}
	say("hakios: mescal: signData %lu bytes -> sapd gave nothing, falling back to the device\n",
	    (unsigned long)[data length]);
	return old_signData(self, _cmd, data, bag, error);
}

__attribute__((constructor))
static void
hakios_mescal_init(void)
{
	const char *name = getprogname();
	Class cls;
	Method method;

	ours = name != NULL && (strcmp(name, "appstored") == 0 || strcmp(name, "itunesstored") == 0 ||
	    strcmp(name, "AppStore") == 0 || strcmp(name, "storekitagent") == 0);
	say("hakios: mescal: ours is in, loaded by \"%s\"%s\n", name != NULL ? name : "(null)", ours ? "" : " (watching nothing)");
	if (!ours) {
		return;
	}
	cls = objc_getClass("AMSMescalSession");
	method = cls != NULL ? class_getInstanceMethod(cls, sel_registerName("signData:bag:error:")) : NULL;
	if (method == NULL) {
		say("hakios: mescal: AMSMescalSession -signData:bag:error: not found; nothing replaced\n");
		return;
	}
	old_signData = (id (*)(id, SEL, id, id, NSError **))method_getImplementation(method);
	method_setImplementation(method, (IMP)new_signData);
	say("hakios: mescal: -[AMSMescalSession signData:bag:error:] now delegates to sapd at " SAPD_URL "\n");
}
