/*
 * net_test: bring the network interface up the way iOS itself would, and see whether iOS can
 * use it. Each step is one layer of what a stock application needs:
 *
 * usage: net_test STEP...
 *   show                  the interfaces and their addresses, as the kernel has them
 *   store [PATTERN]       what configd has in its dynamic store (default: everything under
 *                         State:/Network and Setup:/Network)
 *   dhcp IF               ask configd's IPConfiguration plugin to configure IF by DHCP
 *   waitaddr IF SECONDS   wait until IF has an IPv4 address
 *   resolve NAME          getaddrinfo(): the question goes to mDNSResponder, which asks the
 *                         servers configd has told it about
 *   tcp HOST PORT         connect a socket, send an HTTP request, show the first line of the answer
 *   fetch URL             the same through CFNetwork (NSURLSession): for https this is TLS, the
 *                         system's trust store and trustd as well. The status, the length, an HTML
 *                         title if there is one, and the answer's own "x-" headers -- where Apple's
 *                         services say what they decided (X-Set-Apple-Store-Front; rc.jobs.store)
 *   reach NAME            what SCNetworkReachability says: the question applications ask first
 *   wait SECONDS
 *   wifi                  what the job com.redwood.wifi runs in every image: if the kernel was
 *                         started with wifi=1, wait for the radio's interface (en0) to appear,
 *                         and ask for it to be configured by DHCP ("dhcp en0") only if configd
 *                         has no network service of its own for it; else nothing
 *   watch SECONDS         for so long, say when configd's store changes what tells an interface's
 *                         state: State:/Network/Service/.../IPv4, State:/Network/Interface/en0/...,
 *                         State:/Network/Global/IPv4 (the key, whether it is there, the names in it;
 *                         no addresses)
 *   wrongjoin             ask the radio to join the network it is on with a passphrase that is
 *                         wrong on purpose, through the request wifid uses (Apple80211's
 *                         ASSOCIATE), and say what the kernel answers: Apple80211's result number
 *                         (21: a wrong passphrase) and how long it took. The network's name is
 *                         the kernel's (its SSID request) and is not printed.
 *
 * The interface QEMU's network gives (en0, bsd/dev/redwood_net.c) has no IOKit object behind it,
 * and configd makes its network services out of those. (The phone's radio has one now:
 * iokit/Kernel/RedwoodWLAN.cpp, and configd makes a "Wi-Fi" service of it by itself.) But the
 * plugin that does the configuring keeps its own list
 * of interfaces, straight from the kernel, and has a Mach interface of its own, the one the
 * "ipconfig" command of macOS uses ("ipconfig set en0 DHCP"; the command itself is not part of
 * iOS). "dhcp" is one of its calls: the plugin makes a service for the interface that exists in
 * no preferences file, runs DHCP, and publishes what it learns; the rest of configd then treats
 * it like any other service.
 * (Not "ipconfig set", the first call one thinks of: the service that one makes is gone at
 * configd's next look at its configuration, half a second later at boot, and the protocol is
 * detached from the interface again. "ipconfig setservice" makes one that stays.)
 */
#import <Foundation/Foundation.h>
#include <SystemConfiguration/SCNetworkReachability.h>
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

/* not in the iOS SDK's headers, in the libraries all the same */
extern mach_port_t bootstrap_port;
extern kern_return_t bootstrap_look_up(mach_port_t bootstrap, const char *name, mach_port_t *service);
typedef const struct __SCDynamicStore *SCDynamicStoreRef;
extern SCDynamicStoreRef SCDynamicStoreCreate(CFAllocatorRef allocator, CFStringRef name, void *callout, void *context);
extern CFArrayRef SCDynamicStoreCopyKeyList(SCDynamicStoreRef store, CFStringRef pattern);
extern CFPropertyListRef SCDynamicStoreCopyValue(SCDynamicStoreRef store, CFStringRef key);
extern Boolean SCDynamicStoreSetNotificationKeys(SCDynamicStoreRef store, CFArrayRef keys, CFArrayRef patterns);
extern CFRunLoopSourceRef SCDynamicStoreCreateRunLoopSource(CFAllocatorRef allocator, SCDynamicStoreRef store, CFIndex order);

static double
seconds(void)
{
	static mach_timebase_info_data_t base;

	if (base.denom == 0) {
		mach_timebase_info(&base);
	}
	return (double)mach_absolute_time() * base.numer / base.denom / 1e9;
}

/* ---- show ------------------------------------------------------------------------------------------ */

static bool
show(const char *only, bool quiet)
{
	struct ifaddrs *list, *a;
	bool has_ipv4 = false;
	char text[64];

	if (getifaddrs(&list) != 0) {
		perror("net_test: getifaddrs");
		return false;
	}
	for (a = list; a != NULL; a = a->ifa_next) {
		if (a->ifa_addr == NULL || (only != NULL && strcmp(a->ifa_name, only) != 0)) {
			continue;
		}
		if (a->ifa_addr->sa_family == AF_LINK) {
			struct sockaddr_dl *dl = (struct sockaddr_dl *)a->ifa_addr;
			const unsigned char *m = (const unsigned char *)LLADDR(dl);

			if (quiet) {
				continue;
			}
			printf("net_test: %s: flags %#x%s%s, type %#x", a->ifa_name, a->ifa_flags, (a->ifa_flags & IFF_UP) ? " up" : "",
			    (a->ifa_flags & IFF_RUNNING) ? " running" : "", dl->sdl_type);
			if (dl->sdl_alen == 6) {
				printf(", address %02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
			}
			printf("\n");
		} else if (a->ifa_addr->sa_family == AF_INET) {
			struct sockaddr_in *in = (struct sockaddr_in *)a->ifa_addr, *mask = (struct sockaddr_in *)a->ifa_netmask;

			has_ipv4 = true;
			if (!quiet) {
				printf("net_test: %s: inet %s", a->ifa_name, inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text)));
				printf(" netmask %s\n", mask ? inet_ntop(AF_INET, &mask->sin_addr, text, sizeof(text)) : "?");
			}
		} else if (a->ifa_addr->sa_family == AF_INET6 && !quiet) {
			struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)a->ifa_addr;

			printf("net_test: %s: inet6 %s\n", a->ifa_name, inet_ntop(AF_INET6, &in6->sin6_addr, text, sizeof(text)));
		}
	}
	freeifaddrs(list);
	return has_ipv4;
}

/* ---- store ----------------------------------------------------------------------------------------- */

static void
store(const char *pattern)
{
	SCDynamicStoreRef session = SCDynamicStoreCreate(NULL, CFSTR("net_test"), NULL, NULL);
	NSString *wanted = pattern ? @(pattern) : @"(State|Setup):/Network/.*";
	NSArray *keys;

	if (session == NULL) {
		printf("net_test: store: no session with configd\n");
		return;
	}
	keys = CFBridgingRelease(SCDynamicStoreCopyKeyList(session, (__bridge CFStringRef)wanted));
	printf("net_test: store: %lu keys match %s\n", (unsigned long)keys.count, wanted.UTF8String);
	for (NSString *key in [keys sortedArrayUsingSelector:@selector(compare:)]) {
		id value = CFBridgingRelease(SCDynamicStoreCopyValue(session, (__bridge CFStringRef)key));
		NSString *text = [[[value description] componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
		    componentsJoinedByString:@" "];

		while ([text containsString:@"  "]) {
			text = [text stringByReplacingOccurrencesOfString:@"  " withString:@" "];
		}
		printf("net_test:   %s = %s\n", key.UTF8String, text.length > 400 ? [text substringToIndex:400].UTF8String : text.UTF8String);
	}
	CFRelease(session);
}

/* ---- dhcp ------------------------------------------------------------------------------------------ */

/*
 * The one call of the plugin's interface that is needed, written out by hand (bootp's
 * ipconfig.defs, subsystem 20000, the twelfth routine):
 *   routine ipconfig_set_service(server; name : array[16] of char; method_data : ^array[] of byte;
 *                                out serviceID : array[128] of char; out status : int)
 * method_data is a property list, { ConfigMethod = DHCP }, sent out of line. Only root may ask.
 */
#pragma pack(push, 4)
struct set_request {
	mach_msg_header_t head;
	mach_msg_body_t body;
	mach_msg_ool_descriptor_t method;
	NDR_record_t ndr;
	char name[16];
	mach_msg_type_number_t method_length;
};
struct set_reply {
	mach_msg_header_t head;
	NDR_record_t ndr;
	kern_return_t result;
	char service[128];
	int status;
	mach_msg_max_trailer_t trailer;
};
#pragma pack(pop)

static void
dhcp(const char *interface)
{
	static const char *const statuses[] = { "done", "permission denied", "no such interface", "invalid parameter",
		                                "invalid operation", "allocation failed", "internal error", "not supported" };
	NSData *method = [NSPropertyListSerialization dataWithPropertyList:@{ @"ConfigMethod": @"DHCP" }
	    format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
	mach_port_t server = MACH_PORT_NULL, reply_port = mig_get_reply_port();
	union {
		struct set_request request;
		struct set_reply reply;
	} message;
	kern_return_t result;

	result = bootstrap_look_up(bootstrap_port, "com.apple.network.IPConfiguration", &server);
	if (result != KERN_SUCCESS) {
		printf("net_test: dhcp: configd's IPConfiguration is not there: %#x\n", result);
		return;
	}
	memset(&message, 0, sizeof(message));
	message.request.head.msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
	message.request.head.msgh_size = sizeof(message.request);
	message.request.head.msgh_remote_port = server;
	message.request.head.msgh_local_port = reply_port;
	message.request.head.msgh_id = 20011;
	message.request.body.msgh_descriptor_count = 1;
	message.request.method.address = (void *)method.bytes;
	message.request.method.size = (mach_msg_size_t)method.length;
	message.request.method.deallocate = FALSE;
	message.request.method.copy = MACH_MSG_VIRTUAL_COPY;
	message.request.method.type = MACH_MSG_OOL_DESCRIPTOR;
	message.request.ndr = NDR_record;
	strlcpy(message.request.name, interface, sizeof(message.request.name));
	message.request.method_length = (mach_msg_type_number_t)method.length;
	result = mach_msg(&message.request.head, MACH_SEND_MSG | MACH_RCV_MSG, sizeof(message.request), sizeof(message), reply_port,
	    MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	if (result != KERN_SUCCESS) {
		printf("net_test: dhcp: the message did not go through: %#x\n", result);
	} else if (message.reply.result != KERN_SUCCESS) {
		printf("net_test: dhcp: IPConfiguration does not take the message: %d\n", message.reply.result);
	} else {
		int status = message.reply.status;

		message.reply.service[sizeof(message.reply.service) - 1] = 0;
		printf("net_test: dhcp: IPConfiguration says of \"setservice %s DHCP\": %d (%s), service %s\n", interface, status,
		    status >= 0 && status < 8 ? statuses[status] : "?", message.reply.service);
	}
}

/* ---- resolve, tcp ---------------------------------------------------------------------------------- */

static bool
resolve(const char *name, char *first, size_t size)
{
	struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *list = NULL, *a;
	double start = seconds();
	int error = getaddrinfo(name, NULL, &hints, &list);
	char text[64];

	if (error != 0) {
		printf("net_test: resolve %s: %s (after %.2f s)\n", name, gai_strerror(error), seconds() - start);
		return false;
	}
	printf("net_test: resolve %s, %.2f s:", name, seconds() - start);
	for (a = list; a != NULL; a = a->ai_next) {
		void *address = a->ai_family == AF_INET ? (void *)&((struct sockaddr_in *)a->ai_addr)->sin_addr :
		    (void *)&((struct sockaddr_in6 *)a->ai_addr)->sin6_addr;

		printf(" %s", inet_ntop(a->ai_family, address, text, sizeof(text)));
		if (a->ai_family == AF_INET && first != NULL && first[0] == 0) {
			strlcpy(first, text, size);
		}
	}
	printf("\n");
	freeaddrinfo(list);
	return true;
}

static void
tcp(const char *host, const char *port)
{
	struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *list = NULL;
	char request[256], answer[2048], *end;
	double start = seconds();
	ssize_t n, total = 0;
	int s, error;

	error = getaddrinfo(host, port, &hints, &list);
	if (error != 0) {
		printf("net_test: tcp %s: %s\n", host, gai_strerror(error));
		return;
	}
	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0 || connect(s, list->ai_addr, list->ai_addrlen) != 0) {
		printf("net_test: tcp %s port %s: %s (after %.2f s)\n", host, port, strerror(errno), seconds() - start);
		freeaddrinfo(list);
		return;
	}
	printf("net_test: tcp %s port %s: connected in %.2f s\n", host, port, seconds() - start);
	snprintf(request, sizeof(request), "GET / HTTP/1.0\r\nHost: %s\r\nUser-Agent: net_test\r\n\r\n", host);
	write(s, request, strlen(request));
	answer[0] = 0;
	while ((n = read(s, total == 0 ? answer : request, total == 0 ? sizeof(answer) - 1 : sizeof(request))) > 0) {
		if (total == 0) {
			answer[n] = 0;
		}
		total += n;
	}
	if ((end = strpbrk(answer, "\r\n")) != NULL) {
		*end = 0;
	}
	printf("net_test: tcp %s: %zd bytes in %.2f s, the answer begins \"%s\"\n", host, total, seconds() - start, answer);
	close(s);
	freeaddrinfo(list);
}

/* ---- fetch, reach ---------------------------------------------------------------------------------- */

static void
fetch(const char *address)
{
	NSURL *url = [NSURL URLWithString:@(address)];
	NSURLSessionConfiguration *configuration = NSURLSessionConfiguration.ephemeralSessionConfiguration;
	dispatch_semaphore_t done = dispatch_semaphore_create(0);
	double start = seconds();

	configuration.timeoutIntervalForRequest = 60;
	[[[NSURLSession sessionWithConfiguration:configuration] dataTaskWithURL:url
	    completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
		if (error != nil) {
			printf("net_test: fetch %s: FAILED after %.2f s: %s (%s %ld)\n", address, seconds() - start,
			    error.localizedDescription.UTF8String, error.domain.UTF8String, (long)error.code);
		} else {
			NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
			NSRange title = [text rangeOfString:@"<title>"], close = [text rangeOfString:@"</title>"];

			NSDictionary *headers = [(NSHTTPURLResponse *)response allHeaderFields];

			printf("net_test: fetch %s: status %ld, %lu bytes in %.2f s", address,
			    (long)((NSHTTPURLResponse *)response).statusCode, (unsigned long)data.length, seconds() - start);
			if (title.location != NSNotFound && close.location != NSNotFound && close.location > title.location) {
				printf(", title \"%s\"", [text substringWithRange:NSMakeRange(NSMaxRange(title),
				    close.location - NSMaxRange(title))].UTF8String);
			}
			printf("\n");
			/* The answer's own "x-" headers, which are where Apple's services say what they decided: a bag
			   request made with no storefront comes back with X-Set-Apple-Store-Front, and that header is
			   the whole question for the App Store (see rc.jobs.store). Nothing else is printed: a header
			   of ours could carry a token. */
			for (NSString *name in headers) {
				if ([name rangeOfString:@"x-" options:NSCaseInsensitiveSearch | NSAnchoredSearch].location == 0) {
					printf("net_test: fetch %s: %s: %.200s\n", address, name.UTF8String,
					    [headers[name] description].UTF8String);
				}
			}
		}
		dispatch_semaphore_signal(done);
	}] resume];
	if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 90 * NSEC_PER_SEC)) != 0) {
		printf("net_test: fetch %s: no answer and no error in 90 s\n", address);
	}
}

static void
reach(const char *name)
{
	SCNetworkReachabilityRef target = SCNetworkReachabilityCreateWithName(NULL, name);
	SCNetworkReachabilityFlags flags = 0;

	if (target == NULL || !SCNetworkReachabilityGetFlags(target, &flags)) {
		printf("net_test: reach %s: no answer\n", name);
	} else {
		printf("net_test: reach %s: flags %#x:%s%s%s%s\n", name, flags, (flags & kSCNetworkReachabilityFlagsReachable) ?
		    " reachable" : " NOT reachable", (flags & kSCNetworkReachabilityFlagsConnectionRequired) ? ", connection required" : "",
		    (flags & kSCNetworkReachabilityFlagsIsWWAN) ? ", cellular" : "",
		    (flags & kSCNetworkReachabilityFlagsIsDirect) ? ", direct" : "");
	}
	if (target != NULL) {
		CFRelease(target);
	}
}

/*
 * With wifi=1 among the kernel's arguments: wait for en0, and for configd's own network service for
 * it ("Wi-Fi", made of the radio's IOKit objects, iokit/Kernel/RedwoodWLAN.cpp). That service runs
 * DHCP by itself, and it has to be the one that does: wifid takes the interface for usable only
 * when the address appears under a service whose settings name en0, and the service "dhcp en0"
 * makes has none (Settings then shows a spinner where the tick belongs). Only a kernel without
 * those IOKit objects gets the call.
 */
static void
wifi(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	double start = seconds();
	bool wanted = false, there = false;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	for (const char *word = strtok(arguments, " "); word != NULL; word = strtok(NULL, " ")) {
		wanted |= strcmp(word, "wifi=1") == 0;
	}
	if (!wanted) {
		return;
	}
	while (!(there = if_nametoindex("en0") != 0) && seconds() - start < 120) {
		usleep(500000);
	}
	if (!there) {
		printf("net_test: wifi: no en0 after %.0f s: the radio has not come up\n", seconds() - start);
		return;
	}
	printf("net_test: wifi: en0 is there after %.1f s\n", seconds() - start);
	for (;;) {
		SCDynamicStoreRef session = SCDynamicStoreCreate(NULL, CFSTR("net_test"), NULL, NULL);
		NSArray *keys = session ? CFBridgingRelease(SCDynamicStoreCopyKeyList(session, CFSTR("Setup:/Network/Service/[^/]+/Interface"))) : nil;
		bool configd_has_it = false;

		for (NSString *key in keys) {
			NSDictionary *interface = CFBridgingRelease(SCDynamicStoreCopyValue(session, (__bridge CFStringRef)key));

			configd_has_it |= [interface isKindOfClass:NSDictionary.class] && [interface[@"DeviceName"] isEqual:@"en0"];
		}
		if (session != NULL) {
			CFRelease(session);
		}
		if (configd_has_it) {
			printf("net_test: wifi: configd has a network service for en0 after %.1f s: it configures it itself\n", seconds() - start);
			return;
		}
		if (seconds() - start > 30) {
			break;
		}
		usleep(500000);
	}
	printf("net_test: wifi: configd has no network service for en0 (a kernel without its IOKit objects): DHCP asked for\n");
	dhcp("en0");
}

/* ---- watch ------------------------------------------------------------------------------------------ */

static double watch_start;

static void
watch_changed(SCDynamicStoreRef session, CFArrayRef keys, void *info)
{
	for (CFIndex i = 0; i < CFArrayGetCount(keys); i++) {
		NSString *key = (__bridge NSString *)CFArrayGetValueAtIndex(keys, i);
		id value = CFBridgingRelease(SCDynamicStoreCopyValue(session, (__bridge CFStringRef)key));
		NSString *names = [value isKindOfClass:NSDictionary.class] ?
		    [[[(NSDictionary *)value allKeys] sortedArrayUsingSelector:@selector(compare:)] componentsJoinedByString:@" "] : @"";

		printf("net_test: watch +%.2f s: %s %s%s%s\n", seconds() - watch_start, key.UTF8String, value ? "set" : "REMOVED",
		    value ? ": " : "", names.UTF8String);
	}
}

static void
watch(double limit)
{
	SCDynamicStoreRef session = SCDynamicStoreCreate(NULL, CFSTR("net_test watch"), (void *)watch_changed, NULL);
	NSArray *patterns = @[ @"State:/Network/Service/[^/]+/IPv4", @"State:/Network/Interface/en0/.*", @"State:/Network/Global/IPv4" ];
	CFRunLoopSourceRef source;

	if (session == NULL || !SCDynamicStoreSetNotificationKeys(session, NULL, (__bridge CFArrayRef)patterns) ||
	    (source = SCDynamicStoreCreateRunLoopSource(NULL, session, 0)) == NULL) {
		printf("net_test: watch: no session with configd\n");
		return;
	}
	watch_start = seconds();
	printf("net_test: watch: for %.0f s\n", limit);
	CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, limit, false);
	CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
	CFRelease(source);
	CFRelease(session);
}

/* ---- wrongjoin --------------------------------------------------------------------------------------- */

struct apple80211req {
	char name[16];
	int32_t type, value;
	uint32_t length;
	void *data __attribute__((aligned(8)));
};

#define SIOCSA80211     0x802869c8u
#define SIOCGA80211     0xc02869c9u

static void
wrongjoin(void)
{
	static const char wrong[] = "not-the-passphrase-0";     /* (a passphrase nobody has: the test is that it fails) */
	uint8_t ssid[32], assoc[592];
	struct apple80211req request = { .name = "en0" };
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	double start;
	uint32_t length;

	request.type = 1;
	request.length = sizeof(ssid);
	request.data = ssid;
	if (s < 0 || ioctl(s, SIOCGA80211, &request) != 0 || request.length == 0 || request.length > sizeof(ssid)) {
		printf("net_test: wrongjoin: no network is joined (or no radio): nothing to try\n");
		if (s >= 0) {
			close(s);
		}
		return;
	}
	length = request.length;
	/* the layout of iOS 16.5 (bsd/dev/redwood_apple80211.c of the kernel): version, mode, authentication lower and
	 * upper, the name's length and the name, the access point (none: any), the key's version, length and kind (10:
	 * a passphrase as typed), the key */
	memset(assoc, 0, sizeof(assoc));
	*(uint32_t *)(assoc + 0x00) = 1;
	*(uint16_t *)(assoc + 0x04) = 1;
	*(uint16_t *)(assoc + 0x06) = 1;
	*(uint16_t *)(assoc + 0x08) = 8;
	*(uint32_t *)(assoc + 0x0c) = length;
	memcpy(assoc + 0x10, ssid, length);
	*(uint32_t *)(assoc + 0x38) = 1;
	*(uint32_t *)(assoc + 0x3c) = sizeof(wrong) - 1;
	*(uint32_t *)(assoc + 0x40) = 10;
	memcpy(assoc + 0x48, wrong, sizeof(wrong) - 1);
	memset(&request, 0, sizeof(request));
	strlcpy(request.name, "en0", sizeof(request.name));
	request.type = 20;
	request.length = sizeof(assoc);
	request.data = assoc;
	start = seconds();
	if (ioctl(s, SIOCSA80211, &request) != 0) {
		printf("net_test: wrongjoin: the kernel refuses the request: %s\n", strerror(errno));
		close(s);
		return;
	}
	printf("net_test: wrongjoin: a join with a wrong passphrase is asked for (a name of %u bytes)\n", length);
	for (;;) {
		memset(&request, 0, sizeof(request));
		strlcpy(request.name, "en0", sizeof(request.name));
		request.type = 21;
		if (ioctl(s, SIOCGA80211, &request) != 0) {
			printf("net_test: wrongjoin: the result cannot be asked for: %s\n", strerror(errno));
			break;
		}
		if (request.value != 0 || seconds() - start > 60) {
			printf("net_test: wrongjoin: the result is %d after %.1f s%s\n", request.value, seconds() - start, request.value == 21 ?
			    " (Apple80211's \"invalid PMK\": a wrong passphrase)" : request.value == 0 ? " (NOT OVER)" : "");
			break;
		}
		usleep(200000);
	}
	close(s);
}

int
main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	@autoreleasepool {
		for (int i = 1; i < argc; i++) {
			const char *step = argv[i];

			if (strcmp(step, "show") == 0) {
				show(NULL, false);
			} else if (strcmp(step, "store") == 0) {
				store(i + 1 < argc && strchr(argv[i + 1], ':') != NULL ? argv[++i] : NULL);
			} else if (strcmp(step, "dhcp") == 0 && i + 1 < argc) {
				dhcp(argv[++i]);
			} else if (strcmp(step, "waitaddr") == 0 && i + 2 < argc) {
				const char *interface = argv[++i];
				double start = seconds(), limit = atof(argv[++i]);

				while (!show(interface, true) && seconds() - start < limit) {
					usleep(250000);
				}
				printf("net_test: waitaddr %s: %s after %.1f s\n", interface, show(interface, true) ? "an address" :
				    "NO address", seconds() - start);
				show(interface, false);
			} else if (strcmp(step, "resolve") == 0 && i + 1 < argc) {
				resolve(argv[++i], NULL, 0);
			} else if (strcmp(step, "tcp") == 0 && i + 2 < argc) {
				tcp(argv[i + 1], argv[i + 2]);
				i += 2;
			} else if (strcmp(step, "fetch") == 0 && i + 1 < argc) {
				fetch(argv[++i]);
			} else if (strcmp(step, "reach") == 0 && i + 1 < argc) {
				reach(argv[++i]);
			} else if (strcmp(step, "wait") == 0 && i + 1 < argc) {
				usleep((useconds_t)(atof(argv[++i]) * 1e6));
			} else if (strcmp(step, "wifi") == 0) {
				wifi();
			} else if (strcmp(step, "watch") == 0 && i + 1 < argc) {
				watch(atof(argv[++i]));
			} else if (strcmp(step, "wrongjoin") == 0) {
				wrongjoin();
			} else {
				fprintf(stderr, "net_test: bad step \"%s\"\n", step);
				return 1;
			}
		}
	}
	return 0;
}
